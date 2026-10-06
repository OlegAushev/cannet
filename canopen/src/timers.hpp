#pragma once

// Timing for the client and its services, on the transport's executor:
// periodic work, timeouts, periodic frames. Private to canopen.
//
// Thread model: NOT thread-safe; every call on the executor given at
// construction, where the callbacks run too. Each object may be destroyed
// from inside its own callback, and no callback runs after destruction.

#include <canopen/transport.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace cannet::canopen::detail {

// Calls `tick` every period, at a fixed rate: ticks missed while the
// executor was busy are skipped, not made up in a burst.
class periodic {
public:
  using executor_type = boost::asio::any_io_executor;

  periodic(executor_type const& executor, std::move_only_function<void()> tick);
  ~periodic();
  periodic(periodic const&) = delete;
  periodic& operator=(periodic const&) = delete;

  // (Re)starts with the first tick at once; a zero period stops.
  void start(std::chrono::milliseconds period);
  void stop();
  bool running() const;

private:
  struct state;
  std::shared_ptr<state> state_;
};

// Calls `on_expired` once `timeout` passes without a kick(), then stays
// quiet until the next kick() starts the countdown again. A kick() costs a
// clock read: the timer is re-armed only when it fires. A zero timeout
// disables the watchdog.
class watchdog {
public:
  using executor_type = boost::asio::any_io_executor;

  watchdog(executor_type const& executor,
           std::chrono::milliseconds timeout,
           std::move_only_function<void()> on_expired);
  ~watchdog();
  watchdog(watchdog const&) = delete;
  watchdog& operator=(watchdog const&) = delete;

  void kick();
  // No on_expired until the next kick().
  void stop();
  // Kicked, and neither expired nor stopped since.
  bool running() const;

private:
  struct state;
  std::shared_ptr<state> state_;
};

// Sends the frame `make` builds every period, at a fixed rate, with at most
// one frame in flight: while the previous one still waits in the
// transport's queue, a tick is skipped and `make` is not called. A failed
// send is dropped; the next period sends anew. Unlike the timers above, the
// sender must not be destroyed from inside `make`.
class periodic_sender {
public:
  periodic_sender(transport& bus, std::move_only_function<can_frame()> make);
  periodic_sender(periodic_sender const&) = delete;
  periodic_sender& operator=(periodic_sender const&) = delete;

  // (Re)starts with the first frame at once; a zero period stops.
  void start(std::chrono::milliseconds period);
  void stop();
  bool running() const;

private:
  void tick();

  transport& bus_;
  std::move_only_function<can_frame()> make_;
  // Shared with the pending send's completion, which may outlive this.
  std::shared_ptr<bool> in_flight_ = std::make_shared<bool>(false);
  periodic periodic_;
};

} // namespace cannet::canopen::detail
