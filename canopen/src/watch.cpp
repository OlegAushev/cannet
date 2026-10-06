#include <canopen/service/watch.hpp>

#include <canopen/event.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

namespace cannet::canopen::service {

namespace {

namespace asio = boost::asio;
using clock_type = std::chrono::steady_clock;

// What one expedited read returns as a value: not a string, which takes a
// read per 4 bytes, and not a command, which has no value.
bool pollable(od_object const& object)
{
  return object.readable()
      && object.type != od_value_type::string
      && object.type != od_value_type::exec;
}

std::vector<od_entry const*> watch_objects(dictionary_view dictionary)
{
  std::vector<od_entry const*> objects;
  auto const category = dictionary.config().watch_category;
  if (category.empty()) {
    return objects;
  }
  for (auto const& entry : dictionary.entries()) {
    if (entry.object.category == category && pollable(entry.object)) {
      objects.push_back(&entry);
    }
  }
  return objects;
}

} // namespace

struct watch::state : std::enable_shared_from_this<state> {
  state(detail::sdo_client& client, dictionary_view dictionary)
      : sdo(&client),
        objects(watch_objects(dictionary)),
        polled(objects.size(), true),
        timer(client.get_executor())
  {
  }

  detail::sdo_client* sdo; // null once the client is gone
  std::vector<od_entry const*> objects;
  std::vector<bool> polled;
  std::chrono::milliseconds period{0};
  bool enabled = true;
  bool running = false;                // the polling coroutine is alive
  asio::steady_timer timer;            // the wait for the next pass
  asio::cancellation_signal stop_read; // drops the read in flight
  event<object_reading> values;

  bool active() const
  {
    return sdo != nullptr
        && enabled
        && period > std::chrono::milliseconds::zero()
        && !objects.empty();
  }

  std::optional<std::size_t> find(od_key key) const
  {
    auto const it = std::ranges::lower_bound(
        objects,
        key,
        std::less{},
        [](od_entry const* e) { return e->key; });
    if (it == objects.end() || (*it)->key != key) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(it - objects.begin());
  }

  // After every change of what to poll and when: starts polling, stops it,
  // or has the wait for the next pass look at the period again.
  void update()
  {
    timer.cancel();
    if (!active()) {
      stop_read.emit(asio::cancellation_type::terminal);
      return;
    }
    if (!running) {
      running = true;
      asio::co_spawn(timer.get_executor(),
                     poll(shared_from_this()),
                     [](std::exception_ptr e) {
                       if (e) {
                         std::rethrow_exception(e);
                       }
                     });
    }
  }

  static asio::awaitable<void> poll(std::shared_ptr<state> self)
  {
    while (self->active()) {
      auto const started = clock_type::now();
      co_await self->pass();
      while (self->active() && clock_type::now() < started + self->period) {
        self->timer.expires_at(started + self->period);
        co_await self->timer.async_wait(asio::as_tuple(asio::use_awaitable));
      }
    }
    self->running = false;
  }

  asio::awaitable<void> pass()
  {
    for (std::size_t i = 0; i < objects.size() && active(); ++i) {
      if (!polled[i]) {
        continue;
      }
      auto const* entry = objects[i];
      auto value = co_await sdo->async_read(
          entry->key,
          entry->object.type,
          asio::bind_cancellation_slot(stop_read.slot(), asio::use_awaitable));
      if (!active()) {
        co_return; // stopped meanwhile: the read is dropped
      }
      auto const failure = value ? std::nullopt
                                 : std::optional{value.error().reason};
      if (failure == sdo_error::kind::cancelled) {
        continue; // cut short by a change of the node's id
      }
      values.emit({.entry = entry,
                   .value = std::move(value),
                   .time = clock_type::now()});
      if (failure == sdo_error::kind::transport) {
        co_return; // the other objects would fail as well
      }
    }
  }
};

watch::watch(detail::sdo_client& sdo, dictionary_view dictionary)
    : state_(std::make_shared<state>(sdo, dictionary))
{
}

watch::~watch()
{
  close();
}

std::span<od_entry const* const> watch::objects() const
{
  return state_->objects;
}

void watch::set_period(std::chrono::milliseconds period)
{
  state_->period = std::max(period, std::chrono::milliseconds::zero());
  state_->update();
}

std::chrono::milliseconds watch::period() const
{
  return state_->period;
}

void watch::enable()
{
  state_->enabled = true;
  state_->update();
}

void watch::disable()
{
  state_->enabled = false;
  state_->update();
}

bool watch::enabled() const
{
  return state_->enabled;
}

std::expected<void, setup_error> watch::enable(od_key key)
{
  auto const i = state_->find(key);
  if (!i) {
    return std::unexpected(setup_error::no_such_object);
  }
  state_->polled[*i] = true;
  return {};
}

std::expected<void, setup_error> watch::disable(od_key key)
{
  auto const i = state_->find(key);
  if (!i) {
    return std::unexpected(setup_error::no_such_object);
  }
  state_->polled[*i] = false;
  return {};
}

bool watch::enabled(od_key key) const
{
  auto const i = state_->find(key);
  return i && state_->polled[*i];
}

subscription watch::on_value(value_handler handler)
{
  return state_->values.subscribe(std::move(handler));
}

void watch::close()
{
  state_->sdo = nullptr;
  state_->update();
}

} // namespace cannet::canopen::service
