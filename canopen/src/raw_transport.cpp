#include <canopen/raw_transport.hpp>

#include "subscribers.hpp"

#include <canopen/event.hpp>
#include <cansocket/raw/async_socket.hpp>
#include <cansocket/raw/error_frame.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <linux/can/error.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <utility>

namespace cannet::canopen {

namespace {

using namespace std::chrono_literals;

// The pause before a failed receive is retried.
constexpr auto receive_retry_delay = 100ms;

// How often an interface that is down or gone is looked at.
constexpr auto interface_check_period = 250ms;

// The error frames the status is made of: all but lost arbitration and
// transmit timeouts, which it does not count.
constexpr can_err_mask_t reported_errors = CAN_ERR_MASK
                                         & ~can_err_mask_t{
                                             CAN_ERR_LOSTARB
                                             | CAN_ERR_TX_TIMEOUT};

bool absent(bus_state s)
{
  return s == bus_state::down || s == bus_state::no_interface;
}

std::expected<void, transport_error>
to_send_result(std::expected<void, raw::socket_error> result)
{
  if (result) {
    return {};
  }
  switch (result.error()) {
  case raw::socket_error::tx_queue_full:
    return std::unexpected(transport_error::tx_queue_full);
  case raw::socket_error::interface_down:
  case raw::socket_error::interface_not_found:
    return std::unexpected(transport_error::interface_down);
  case raw::socket_error::not_open:
  case raw::socket_error::cancelled:
    return std::unexpected(transport_error::closed);
  default: return std::unexpected(transport_error::send_failed);
  }
}

bus_state to_bus_state(raw::controller_state s)
{
  switch (s) {
  case raw::controller_state::error_active: return bus_state::error_active;
  case raw::controller_state::error_warning: return bus_state::error_warning;
  case raw::controller_state::error_passive: return bus_state::error_passive;
  case raw::controller_state::bus_off: return bus_state::bus_off;
  }
  return bus_state::error_active;
}

} // namespace

struct raw_transport::state : std::enable_shared_from_this<state> {
  struct queued_send {
    can_frame frame;
    send_handler done;
  };

  explicit state(executor_type const& executor)
      : socket(executor), retry_timer(executor), check_timer(executor)
  {
  }

  raw::async_socket socket;
  boost::asio::steady_timer retry_timer; // a failed receive's retry
  boost::asio::steady_timer check_timer; // an interface down or gone
  detail::subscribers subscribers;
  std::deque<queued_send> queue; // front() is in flight while `sending`
  bool sending = false;
  bool checking = false; // check_timer runs
  // The interface open() was given; empty while the transport is closed.
  std::string iface;
  bus_status status{.state = bus_state::no_interface};
  event<bus_status> changed;
  // Bumped whenever a socket goes: completions of an earlier socket see a
  // stale generation and stop.
  std::uint64_t generation = 0;

  void apply_filters()
  {
    if (!socket.is_open()) {
      return;
    }
    if (socket.set_filters(subscribers.filters())) {
      return;
    }
    // The kernel caps the filter count. Past the cap, receive everything
    // and let the subscriptions' filters sort it out.
    static constexpr can_filter everything{.can_id = 0, .can_mask = 0};
    static_cast<void>(socket.set_filters(std::span{&everything, 1}));
  }

  // Tells the subscribers of the status when it changed.
  void update(bus_status const& next)
  {
    if (next == status) {
      return;
    }
    status = next;
    changed.emit(status);
  }

  void set_state(bus_state s)
  {
    auto next = status;
    next.state = s;
    update(next);
  }

  // What an error frame reports, counted.
  void reported(raw::error_report const& report)
  {
    auto next = status;
    if (report.state) {
      next.state = to_bus_state(*report.state);
    }
    if (report.protocol_violation
        || report.no_ack
        || report.bus_error
        || report.transceiver) {
      ++next.bus_errors;
    }
    if (report.rx_overflow || report.tx_overflow) {
      ++next.overflows;
    }
    if (report.counters) {
      next.counters = report.counters;
    }
    update(next);
  }

  // The socket of a fresh open(), or of one made again: error frames, the
  // subscriptions' filters, reception. Returns the interface's state.
  bus_state start(std::uint64_t gen)
  {
    // A driver that reports nothing leaves the state to the interface.
    static_cast<void>(socket.set_error_filter(reported_errors));
    apply_filters();
    receive(gen);
    return interface_state();
  }

  bus_state interface_state() const
  {
    auto const up = raw::interface_up(iface);
    if (!up) {
      return bus_state::no_interface;
    }
    return *up ? bus_state::error_active : bus_state::down;
  }

  // The state the interface is in; one down or gone is looked at until it
  // is back.
  void enter(bus_state s)
  {
    set_state(s);
    if (absent(s)) {
      arm_check();
    }
  }

  void arm_check()
  {
    // A handler of the status may have closed the transport.
    if (checking || iface.empty()) {
      return;
    }
    checking = true;
    check_timer.expires_after(interface_check_period);
    check_timer.async_wait([weak = weak_from_this(),
                            gen = generation](boost::system::error_code ec) {
      auto const self = weak.lock();
      if (!self || gen != self->generation) {
        return; // shut() has reset `checking`
      }
      self->checking = false;
      if (!ec) {
        self->check();
      }
    });
  }

  void check()
  {
    if (status.state == bus_state::no_interface) {
      reopen();
    }
    else if (status.state == bus_state::down) {
      enter(interface_state());
    }
    // Else an error frame has told already that the interface is up.
  }

  // A socket that its interface's going left unbound for good is replaced
  // by a new one, once the interface is back.
  void reopen()
  {
    shut(transport_error::interface_down);
    if (!socket.open(iface)) {
      arm_check();
      return;
    }
    enter(start(generation));
  }

  // Ends the socket: what it still had to send fails with `error`.
  void shut(transport_error error)
  {
    ++generation;
    retry_timer.cancel();
    check_timer.cancel();
    checking = false;
    if (socket.is_open()) {
      static_cast<void>(socket.close());
    }
    auto queued = std::exchange(queue, {});
    sending = false;
    for (auto& q : queued) {
      boost::asio::post(socket.get_executor(),
                        [done = std::move(q.done), error]() mutable {
                          done(std::unexpected(error));
                        });
    }
  }

  void receive(std::uint64_t gen)
  {
    socket.async_receive(
        [weak = weak_from_this(),
         gen](std::expected<can_frame, raw::socket_error> result) {
          if (auto const self = weak.lock()) {
            self->on_receive(gen, result);
          }
        });
  }

  void on_receive(std::uint64_t gen,
                  std::expected<can_frame, raw::socket_error> const& result)
  {
    if (gen != generation) {
      return;
    }
    if (result) {
      if (auto const report = raw::decode_error_frame(*result)) {
        reported(*report);
      }
      else {
        subscribers.dispatch(*result);
      }
      if (gen == generation) { // a handler may have closed the transport
        receive(gen);
      }
      return;
    }
    switch (result.error()) {
    case raw::socket_error::cancelled:
    case raw::socket_error::not_open: return;
    case raw::socket_error::interface_down:
      // Reported once: the next receive waits until frames come again.
      enter(bus_state::down);
      if (gen == generation) {
        receive(gen);
      }
      return;
    case raw::socket_error::interface_not_found:
      // The socket is unbound for good: reopen() makes another.
      enter(bus_state::no_interface);
      return;
    default: break;
    }
    retry_timer.expires_after(receive_retry_delay);
    retry_timer.async_wait(
        [weak = weak_from_this(), gen](boost::system::error_code ec) {
          auto const self = weak.lock();
          if (!ec && self && gen == self->generation) {
            self->receive(gen);
          }
        });
  }

  void send_next()
  {
    if (queue.empty()) {
      sending = false;
      return;
    }
    sending = true;
    socket.async_send(queue.front().frame,
                      [weak = weak_from_this(), gen = generation](
                          std::expected<void, raw::socket_error> result) {
                        auto const self = weak.lock();
                        if (!self || gen != self->generation) {
                          return; // shut() has completed the queue already
                        }
                        self->sent(gen, result);
                      });
  }

  void sent(std::uint64_t gen, std::expected<void, raw::socket_error> result)
  {
    auto done = std::move(queue.front().done);
    queue.pop_front();
    done(to_send_result(result));
    if (gen != generation) {
      return; // the handler closed the transport
    }
    // A send can be the first to hear of the interface's going.
    if (!result && result.error() == raw::socket_error::interface_not_found) {
      enter(bus_state::no_interface);
    }
    else if (!result
             && result.error() == raw::socket_error::interface_down
             && status.state != bus_state::no_interface) {
      enter(bus_state::down);
    }
    if (gen == generation) {
      send_next();
    }
  }
};

raw_transport::raw_transport(executor_type const& executor)
    : state_(std::make_shared<state>(executor))
{
}

raw_transport::~raw_transport()
{
  // Silently: nobody is told of a transport that goes.
  state_->shut(transport_error::closed);
}

std::expected<void, raw::socket_error>
raw_transport::open(std::string_view iface)
{
  auto& s = *state_;
  s.shut(transport_error::closed);
  if (auto const opened = s.socket.open(iface); !opened) {
    s.iface.clear();
    s.update({.state = bus_state::no_interface});
    return opened;
  }
  s.iface = iface;
  auto const interface = s.start(s.generation);
  // Counted afresh.
  s.update({.state = interface});
  if (absent(interface)) {
    s.arm_check();
  }
  return {};
}

void raw_transport::close()
{
  auto& s = *state_;
  s.shut(transport_error::closed);
  s.iface.clear();
  s.update({.state = bus_state::no_interface});
}

bool raw_transport::is_open() const
{
  return !state_->iface.empty();
}

raw_transport::executor_type raw_transport::get_executor()
{
  return state_->socket.get_executor();
}

void raw_transport::send(can_frame const& frame, send_handler done)
{
  auto& s = *state_;
  if (!s.socket.is_open()) {
    // Between a vanished interface's socket and the next, the transport is
    // open but has nowhere to send.
    auto const error = s.iface.empty() ? transport_error::closed
                                       : transport_error::interface_down;
    boost::asio::post(s.socket.get_executor(),
                      [done = std::move(done), error]() mutable {
                        done(std::unexpected(error));
                      });
    return;
  }
  s.queue.push_back({.frame = frame, .done = std::move(done)});
  if (!s.sending) {
    s.send_next();
  }
}

subscription raw_transport::subscribe(can_filter filter, frame_handler on_frame)
{
  auto const id = state_->subscribers.add(filter, std::move(on_frame));
  state_->apply_filters();
  return subscription{[weak = std::weak_ptr{state_}, id] {
    if (auto const s = weak.lock()) {
      s->subscribers.remove(id);
      s->apply_filters();
    }
  }};
}

bus_status raw_transport::status() const
{
  return state_->status;
}

subscription raw_transport::on_status(status_handler on_status)
{
  return state_->changed.subscribe(std::move(on_status));
}

} // namespace cannet::canopen
