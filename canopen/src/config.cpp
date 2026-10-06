#include <canopen/service/config.hpp>

#include <canopen/event.hpp>

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <exception>

namespace cannet::canopen {

namespace detail {

namespace {

namespace asio = boost::asio;
using clock_type = std::chrono::steady_clock;

// A read of every parameter gives up after this many timeouts in a row.
constexpr int max_unanswered = 3;

constexpr sdo_error error(sdo_error::kind reason)
{
  return {.reason = reason};
}

constexpr sdo_error closed{.reason = sdo_error::kind::transport,
                           .send_error = transport_error::closed};

// What one expedited read returns as a value: not a string, which takes a
// read per 4 bytes, and not a command, which has no value.
bool scalar(od_value_type type)
{
  return type != od_value_type::string && type != od_value_type::exec;
}

std::vector<od_entry const*> config_parameters(dictionary_view dictionary)
{
  std::vector<od_entry const*> parameters;
  auto const category = dictionary.config().config_category;
  if (category.empty()) {
    return parameters;
  }
  for (auto const& entry : dictionary.entries()) {
    if (entry.object.category == category) {
      parameters.push_back(&entry);
    }
  }
  return parameters;
}

// One operation in progress: whether its caller cancelled it, and the
// signal that cancels its SDO request in flight.
struct operation {
  config_completion complete;
  asio::cancellation_signal stop_request;
  bool cancelled = false;
};

} // namespace

struct config_state {
  config_state(sdo_client& client, dictionary_view dictionary)
      : sdo(&client), parameters(config_parameters(dictionary))
  {
  }

  sdo_client* sdo; // null once the client is gone
  std::vector<od_entry const*> parameters;
  event<object_reading> values;

  // Queues `request` with the node's SDO client at once, keeping the order
  // of the operations that asked, and awaits its outcome.
  static asio::awaitable<sdo_outcome> exchange(sdo_client& client,
                                               sdo_request const& request,
                                               asio::cancellation_slot slot)
  {
    co_return co_await asio::async_initiate<asio::use_awaitable_t<> const&,
                                            void(sdo_outcome)>(
        [&client, &request, slot](auto handler) {
          client.queue(request, slot, std::move(handler));
        },
        asio::use_awaitable);
  }

  static asio::awaitable<void> serve(std::shared_ptr<config_state> self,
                                     std::shared_ptr<operation> op,
                                     config_request request)
  {
    auto outcome = co_await self->run(*op, request);
    if (op->cancelled) {
      outcome = std::unexpected(error(sdo_error::kind::cancelled));
    }
    op->complete(std::move(outcome));
  }

  asio::awaitable<config_outcome> run(operation& op,
                                      config_request const& request)
  {
    switch (request.what) {
    case config_request::kind::read: {
      auto reading = co_await read(op, *request.parameter);
      if (!reading.value) {
        co_return std::unexpected(reading.value.error());
      }
      co_return *reading.value;
    }
    case config_request::kind::write: co_return co_await write(op, request);
    case config_request::kind::read_all: co_return co_await read_all(op);
    }
    co_return std::unexpected(error(sdo_error::kind::malformed));
  }

  // Reads one object and reports the reading, unless it was cut short.
  asio::awaitable<object_reading> read(operation& op, od_entry const& entry)
  {
    object_reading reading{.entry = &entry};
    if (!scalar(entry.object.type)) {
      reading.value = std::unexpected(error(sdo_error::kind::type_mismatch));
      co_return reading;
    }
    if (sdo == nullptr) {
      reading.value = std::unexpected(closed);
      co_return reading;
    }
    reading.value = narrow<od_value>(
        co_await exchange(*sdo,
                          {.what = sdo_request::kind::read,
                           .key = entry.key,
                           .type = entry.object.type},
                          op.stop_request.slot()));
    reading.time = clock_type::now();
    bool const cut_short = op.cancelled
                        || (!reading.value
                            && reading.value.error().reason
                                   == sdo_error::kind::cancelled);
    if (!cut_short && sdo != nullptr) {
      values.emit(reading); // may close the service, or destroy the node
    }
    co_return reading;
  }

  asio::awaitable<config_outcome> write(operation& op,
                                        config_request const& request)
  {
    auto const& entry = *request.parameter;
    if (type_of(request.value) != entry.object.type) {
      co_return std::unexpected(error(sdo_error::kind::type_mismatch));
    }
    if (sdo == nullptr) {
      co_return std::unexpected(closed);
    }
    auto const written = narrow<void>(co_await exchange(
        *sdo,
        {.what = sdo_request::kind::write,
         .key = entry.key,
         .data = to_raw(request.value),
         .size = static_cast<std::uint8_t>(od_value_size(entry.object.type))},
        op.stop_request.slot()));
    if (!written) {
      co_return std::unexpected(written.error());
    }
    co_return std::monostate{};
  }

  asio::awaitable<config_outcome> read_all(operation& op)
  {
    std::vector<object_reading> readings;
    int unanswered = 0;
    for (auto const* entry : parameters) {
      if (!entry->object.readable() || !scalar(entry->object.type)) {
        continue;
      }
      if (op.cancelled) {
        break;
      }
      auto reading = co_await read(op, *entry);
      if (op.cancelled) {
        break;
      }
      if (!reading.value) {
        auto const reason = reading.value.error().reason;
        if (reason == sdo_error::kind::cancelled
            || reason == sdo_error::kind::transport) {
          co_return std::unexpected(reading.value.error());
        }
      }
      bool const timed_out = !reading.value
                          && reading.value.error().reason
                                 == sdo_error::kind::timeout;
      unanswered = timed_out ? unanswered + 1 : 0;
      readings.push_back(std::move(reading));
      if (unanswered == max_unanswered) {
        co_return std::unexpected(error(sdo_error::kind::timeout));
      }
    }
    co_return readings; // serve() turns a cancelled one into `cancelled`
  }
};

void config_submit(std::weak_ptr<config_state> const& state,
                   config_request const& request,
                   asio::cancellation_slot slot,
                   config_completion complete)
{
  auto const s = state.lock();
  if (!s || s->sdo == nullptr) {
    complete(std::unexpected(closed));
    return;
  }
  auto const op = std::make_shared<operation>();
  op->complete = std::move(complete);
  if (slot.is_connected()) {
    // Completes nothing itself: the request in flight completes with
    // `cancelled`, and the operation after it.
    slot.assign([weak = std::weak_ptr{op}](asio::cancellation_type) {
      if (auto const o = weak.lock()) {
        o->cancelled = true;
        o->stop_request.emit(asio::cancellation_type::terminal);
      }
    });
  }
  asio::co_spawn(s->sdo->get_executor(),
                 config_state::serve(s, op, request),
                 [](std::exception_ptr e) {
                   if (e) {
                     std::rethrow_exception(e);
                   }
                 });
}

} // namespace detail

namespace service {

config::config(detail::sdo_client& sdo, dictionary_view dictionary)
    : sdo_(sdo), state_(std::make_shared<detail::config_state>(sdo, dictionary))
{
}

config::~config()
{
  close();
}

std::span<od_entry const* const> config::parameters() const
{
  return state_->parameters;
}

subscription config::on_value(value_handler handler)
{
  return state_->values.subscribe(std::move(handler));
}

void config::close()
{
  state_->sdo = nullptr;
}

} // namespace service
} // namespace cannet::canopen
