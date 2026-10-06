#include <canopen/detail/sdo_client.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <format>
#include <optional>

namespace cannet::canopen {

std::string_view to_string(sdo_error::kind k)
{
  switch (k) {
  case sdo_error::kind::aborted: return "SDO aborted by the device";
  case sdo_error::kind::timeout: return "no SDO answer in time";
  case sdo_error::kind::cancelled: return "SDO request cancelled";
  case sdo_error::kind::type_mismatch:
    return "the device's object has another size";
  case sdo_error::kind::malformed: return "malformed SDO answer";
  case sdo_error::kind::transport: return "SDO request not sent";
  }
  return "unknown error";
}

std::string_view name(sdo_error::kind k)
{
  switch (k) {
  case sdo_error::kind::aborted: return "aborted";
  case sdo_error::kind::timeout: return "timeout";
  case sdo_error::kind::cancelled: return "cancelled";
  case sdo_error::kind::type_mismatch: return "type_mismatch";
  case sdo_error::kind::malformed: return "malformed";
  case sdo_error::kind::transport: return "transport";
  }
  return "unknown";
}

std::string to_string(sdo_error const& e)
{
  switch (e.reason) {
  case sdo_error::kind::aborted:
    return std::format("{}: {} (0x{:08X})",
                       to_string(e.reason),
                       to_string(e.abort),
                       std::to_underlying(e.abort));
  case sdo_error::kind::transport:
    return std::format("{}: {}", to_string(e.reason), to_string(e.send_error));
  default: return std::string{to_string(e.reason)};
  }
}

namespace detail {

namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;

// A device that never sends the NUL would keep a string read going for
// ever; no string the stack serves comes near this.
constexpr std::size_t max_string_size = 1024;

using answer = std::expected<expedited_sdo, sdo_error>;

constexpr sdo_error error(sdo_error::kind reason)
{
  return {.reason = reason};
}

can_frame request_frame(node_id id, expedited_sdo const& sdo)
{
  return make_frame(cob_id(cob_type::rsdo, id), CAN_MAX_DLEN, to_payload(sdo));
}

// The data field of a read's answer, checked against the size of `type`.
// Not inlined: inlined into the coroutines that await the answer, GCC 16 at
// -O3 takes the expected's value for maybe uninitialized.
[[gnu::noinline]] sdo_result<expedited_sdo_data> read_data(answer const& a,
                                                           od_value_type type)
{
  if (!a) {
    return std::unexpected(a.error());
  }
  if (a->cs != sdo_cs_codes::server_init_read || a->expedited_transfer == 0) {
    return std::unexpected(error(sdo_error::kind::malformed));
  }
  if (a->data_size_indicated != 0 && a->data_size() != od_value_size(type)) {
    return std::unexpected(error(sdo_error::kind::type_mismatch));
  }
  return a->data;
}

sdo_result<void> write_done(answer const& a)
{
  if (!a) {
    return std::unexpected(a.error());
  }
  if (a->cs != sdo_cs_codes::server_init_write) {
    return std::unexpected(error(sdo_error::kind::malformed));
  }
  return {};
}

} // namespace

struct sdo_state : std::enable_shared_from_this<sdo_state> {
  struct entry {
    std::uint64_t id;
    sdo_request request;
    sdo_completion complete; // empty once completed
  };

  sdo_state(transport& b, node_id i, std::chrono::milliseconds t)
      : bus(b), id(i), timeout(t), wake(b.get_executor())
  {
  }

  transport& bus;
  node_id id;
  std::chrono::milliseconds timeout;
  subscription answers;
  std::deque<entry> queue;
  entry* in_flight = nullptr; // the worker's current entry
  bool working = false;       // a worker is running
  bool closed = false;
  std::uint64_t next_id = 1;
  // A string read that stopped short, with the node id it went to: the
  // device's cursor may sit in the middle of it.
  std::optional<std::pair<node_id, od_key>> mid_string;

  // The exchange in progress: the key it awaits an answer for, the answer,
  // and what cut it short (a node id change, the client gone).
  asio::steady_timer wake;
  std::uint64_t exchange_seq = 0;
  std::optional<od_key> awaiting;
  std::optional<answer> received;
  std::optional<sdo_error> interrupted;

  void bind()
  {
    answers = bus.subscribe(cob_filter(cob_id(cob_type::tsdo, id)),
                            [this](can_frame const& f) { on_answer(f); });
  }

  void on_answer(can_frame const& frame)
  {
    if (!awaiting || received || frame.len < 4) {
      return;
    }
    payload data{};
    std::copy_n(frame.data,
                std::min<std::size_t>(frame.len, CAN_MAX_DLEN),
                data.begin());
    auto const sdo = from_payload<expedited_sdo>(data);
    od_key const key{static_cast<std::uint16_t>(sdo.index),
                     static_cast<std::uint8_t>(sdo.subindex)};
    if (key != *awaiting) {
      return; // a late answer to an earlier request
    }
    if (sdo_cs_code(data) == sdo_cs_codes::abort) {
      auto const abort = from_payload<abort_sdo>(data);
      received = std::unexpected(
          sdo_error{.reason = sdo_error::kind::aborted,
                    .abort = static_cast<sdo_abort_code>(abort.error_code)});
    }
    else {
      received = sdo;
    }
    wake.cancel();
  }

  void submit(entry e)
  {
    queue.push_back(std::move(e));
    if (working || closed) {
      return;
    }
    working = true;
    asio::co_spawn(wake.get_executor(),
                   serve(shared_from_this()),
                   [](std::exception_ptr ex) {
                     if (ex) {
                       std::rethrow_exception(ex);
                     }
                   });
  }

  // A queued request never went out and just leaves the queue. One in
  // flight answers its caller now, while the worker keeps the channel until
  // the answer or the timeout: a late answer must not pass for the next
  // request's.
  void cancel(std::uint64_t entry_id)
  {
    if (in_flight != nullptr && in_flight->id == entry_id) {
      if (auto complete = std::exchange(in_flight->complete, nullptr)) {
        complete(std::unexpected(error(sdo_error::kind::cancelled)));
      }
      return;
    }
    auto const it = std::ranges::find(queue, entry_id, &entry::id);
    if (it == queue.end()) {
      return;
    }
    auto complete = std::move(it->complete);
    queue.erase(it);
    complete(std::unexpected(error(sdo_error::kind::cancelled)));
  }

  void interrupt(sdo_error why)
  {
    if (awaiting) {
      interrupted = why;
      wake.cancel();
    }
  }

  static asio::awaitable<void> serve(std::shared_ptr<sdo_state> self)
  {
    while (!self->closed && !self->queue.empty()) {
      auto current = std::move(self->queue.front());
      self->queue.pop_front();
      self->in_flight = &current;
      auto outcome = co_await self->run(current.request);
      self->in_flight = nullptr;
      if (auto complete = std::exchange(current.complete, nullptr)) {
        complete(std::move(outcome)); // may submit, or destroy the node
      }
    }
    self->working = false;
  }

  asio::awaitable<sdo_outcome> run(sdo_request const& request)
  {
    switch (request.what) {
    case sdo_request::kind::read: {
      auto const a = co_await exchange(
          request.key,
          make_sdo_read_request(request.key.index, request.key.subindex));
      auto const data = read_data(a, request.type);
      if (!data) {
        co_return std::unexpected(data.error());
      }
      co_return make_od_value(*data, request.type);
    }
    case sdo_request::kind::write: {
      auto const a = co_await exchange(
          request.key,
          make_sdo_write_request(request.key.index,
                                 request.key.subindex,
                                 request.data,
                                 request.size));
      co_return write_done(a).transform(
          [] { return std::variant<std::monostate, od_value, std::string>{}; });
    }
    case sdo_request::kind::read_string: {
      if (mid_string == std::pair{id, request.key}) {
        // Run the cursor to the end first, so this read starts over.
        if (auto const drained = co_await read_text(request.key); !drained) {
          co_return std::unexpected(drained.error());
        }
      }
      auto text = co_await read_text(request.key);
      if (!text) {
        co_return std::unexpected(text.error());
      }
      co_return std::move(*text);
    }
    }
    co_return std::unexpected(error(sdo_error::kind::malformed));
  }

  asio::awaitable<sdo_result<std::string>> read_text(od_key key)
  {
    auto const target = id; // a node id change cuts the read short
    std::string text;
    for (std::size_t word = 0;; ++word) {
      auto const chunk = read_data(
          co_await exchange(key,
                            make_sdo_read_request(key.index, key.subindex)),
          od_value_type::string);
      if (!chunk) {
        // A request that never left, or that the device refused at the
        // first word, did not move the cursor; anything else may have.
        auto const reason = chunk.error().reason;
        bool const untouched = word == 0
                            && (reason == sdo_error::kind::transport
                                || reason == sdo_error::kind::aborted);
        if (!untouched) {
          mid_string = std::pair{target, key};
        }
        co_return std::unexpected(chunk.error());
      }
      if (mid_string && mid_string->first == target) {
        mid_string.reset(); // the cursor is on this string now
      }
      for (auto const byte : *chunk) {
        if (byte == 0) {
          co_return text;
        }
        text.push_back(static_cast<char>(byte));
      }
      if (text.size() >= max_string_size) {
        mid_string = std::pair{target, key};
        co_return std::unexpected(error(sdo_error::kind::malformed));
      }
    }
  }

  // Sends `request` and waits for its answer: the device's, or why there is
  // none.
  asio::awaitable<answer> exchange(od_key key, expedited_sdo const& request)
  {
    if (closed) {
      co_return std::unexpected(
          sdo_error{.reason = sdo_error::kind::transport,
                    .send_error = transport_error::closed});
    }
    auto const seq = ++exchange_seq;
    awaiting = key;
    received.reset();
    interrupted.reset();
    bus.send(request_frame(id, request),
             [weak = weak_from_this(),
              seq](std::expected<void, transport_error> sent) {
               auto const self = weak.lock();
               if (sent
                   || !self
                   || self->exchange_seq != seq
                   || !self->awaiting
                   || self->received) {
                 return;
               }
               self->received = std::unexpected(
                   sdo_error{.reason = sdo_error::kind::transport,
                             .send_error = sent.error()});
               self->wake.cancel();
             });
    wake.expires_after(timeout);
    co_await wake.async_wait(asio::as_tuple(asio::use_awaitable));
    awaiting.reset();
    if (interrupted) {
      co_return std::unexpected(*std::exchange(interrupted, std::nullopt));
    }
    if (received) {
      co_return *std::exchange(received, std::nullopt);
    }
    co_return std::unexpected(error(sdo_error::kind::timeout));
  }
};

void sdo_submit(std::weak_ptr<sdo_state> const& state,
                sdo_request const& request,
                asio::cancellation_slot slot,
                sdo_completion complete)
{
  auto const s = state.lock();
  if (!s || s->closed) {
    complete(std::unexpected(sdo_error{.reason = sdo_error::kind::transport,
                                       .send_error = transport_error::closed}));
    return;
  }
  auto const entry_id = s->next_id++;
  if (slot.is_connected()) {
    slot.assign([weak = state, entry_id, ex = s->wake.get_executor()](
                    asio::cancellation_type) {
      // Not from inside the signal: completing the operation clears the
      // slot, and with it this handler.
      asio::post(ex, [weak, entry_id] {
        if (auto const self = weak.lock()) {
          self->cancel(entry_id);
        }
      });
    });
  }
  s->submit(
      {.id = entry_id, .request = request, .complete = std::move(complete)});
}

sdo_client::sdo_client(transport& bus,
                       node_id id,
                       std::chrono::milliseconds timeout)
    : executor_(bus.get_executor()),
      state_(std::make_shared<sdo_state>(bus, id, timeout))
{
  state_->bind();
}

sdo_client::~sdo_client()
{
  close();
}

void sdo_client::rebind(node_id id)
{
  auto& s = *state_;
  s.id = id;
  s.bind();
  s.interrupt(error(sdo_error::kind::cancelled));
}

void sdo_client::close()
{
  auto& s = *state_;
  if (s.closed) {
    return;
  }
  s.closed = true;
  s.answers.reset();
  sdo_error const gone{.reason = sdo_error::kind::transport,
                       .send_error = transport_error::closed};
  s.interrupt(gone);
  // Completed from the executor, not from inside whatever destructor
  // closes the client.
  asio::post(s.wake.get_executor(),
             [queued = std::exchange(s.queue, {}), gone]() mutable {
               for (auto& e : queued) {
                 if (auto complete = std::exchange(e.complete, nullptr)) {
                   complete(std::unexpected(gone));
                 }
               }
             });
}

} // namespace detail
} // namespace cannet::canopen
