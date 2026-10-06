#include "timers.hpp"

#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <utility>

namespace cannet::canopen::detail {

namespace {

using clock = std::chrono::steady_clock;

} // namespace

// Waits complete through weak references and carry the generation they
// were started in: stop() bumps it, so a wait that had already fired when
// the timer was stopped, restarted or destroyed finds itself stale.
struct periodic::state : std::enable_shared_from_this<state> {
  state(executor_type const& executor, std::move_only_function<void()> t)
      : timer(executor), tick(std::move(t))
  {
  }

  boost::asio::steady_timer timer;
  std::move_only_function<void()> tick;
  std::chrono::milliseconds period{0};
  std::uint64_t generation = 0;
  bool running = false;

  void schedule(clock::time_point deadline)
  {
    timer.expires_at(deadline);
    timer.async_wait([weak = weak_from_this(), gen = generation, deadline](
                         boost::system::error_code ec) {
      auto const self = weak.lock();
      if (!self || ec || gen != self->generation) {
        return;
      }
      self->fire(deadline);
    });
  }

  void fire(clock::time_point deadline)
  {
    auto const gen = generation;
    tick();
    if (gen != generation) {
      return; // the tick stopped, restarted or destroyed the periodic
    }
    auto next = deadline + period;
    if (auto const now = clock::now(); next <= now) {
      next = now + period; // late: skip the missed ticks
    }
    schedule(next);
  }

  void stop()
  {
    ++generation;
    running = false;
    timer.cancel();
  }
};

periodic::periodic(executor_type const& executor,
                   std::move_only_function<void()> tick)
    : state_(std::make_shared<state>(executor, std::move(tick)))
{
}

periodic::~periodic()
{
  state_->stop();
}

void periodic::start(std::chrono::milliseconds period)
{
  auto& s = *state_;
  s.stop();
  if (period <= std::chrono::milliseconds::zero()) {
    return;
  }
  s.period = period;
  s.running = true;
  s.schedule(clock::now());
}

void periodic::stop()
{
  state_->stop();
}

bool periodic::running() const
{
  return state_->running;
}

struct watchdog::state : std::enable_shared_from_this<state> {
  state(executor_type const& executor,
        std::chrono::milliseconds t,
        std::move_only_function<void()> expired)
      : timer(executor), timeout(t), on_expired(std::move(expired))
  {
  }

  boost::asio::steady_timer timer;
  std::chrono::milliseconds timeout;
  std::move_only_function<void()> on_expired;
  clock::time_point last_kick{};
  std::uint64_t generation = 0;
  bool counting = false; // kicked, not yet expired or stopped
  bool armed = false;    // a wait is pending

  void kick()
  {
    if (timeout <= std::chrono::milliseconds::zero()) {
      return;
    }
    last_kick = clock::now();
    counting = true;
    if (!armed) {
      arm(last_kick + timeout);
    }
  }

  void arm(clock::time_point deadline)
  {
    armed = true;
    timer.expires_at(deadline);
    timer.async_wait([weak = weak_from_this(),
                      gen = generation](boost::system::error_code ec) {
      auto const self = weak.lock();
      if (!self || ec || gen != self->generation) {
        return;
      }
      self->fire();
    });
  }

  void fire()
  {
    armed = false;
    if (!counting) {
      return;
    }
    // Kicks since the timer was armed moved the deadline.
    if (auto const deadline = last_kick + timeout; clock::now() < deadline) {
      arm(deadline);
      return;
    }
    counting = false;
    on_expired(); // last: it may destroy the watchdog
  }

  void stop()
  {
    ++generation;
    counting = false;
    armed = false;
    timer.cancel();
  }
};

watchdog::watchdog(executor_type const& executor,
                   std::chrono::milliseconds timeout,
                   std::move_only_function<void()> on_expired)
    : state_(std::make_shared<state>(executor, timeout, std::move(on_expired)))
{
}

watchdog::~watchdog()
{
  state_->stop();
}

void watchdog::kick()
{
  state_->kick();
}

void watchdog::stop()
{
  state_->stop();
}

bool watchdog::running() const
{
  return state_->counting;
}

periodic_sender::periodic_sender(transport& bus,
                                 std::move_only_function<can_frame()> make)
    : bus_(bus),
      make_(std::move(make)),
      periodic_(bus.get_executor(), [this] { tick(); })
{
}

void periodic_sender::start(std::chrono::milliseconds period)
{
  periodic_.start(period);
}

void periodic_sender::stop()
{
  periodic_.stop();
}

bool periodic_sender::running() const
{
  return periodic_.running();
}

void periodic_sender::tick()
{
  if (*in_flight_) {
    return; // the last frame is still queued: skip this one
  }
  auto const frame = make_();
  *in_flight_ = true;
  bus_.send(frame,
            [in_flight = in_flight_](auto const&) { *in_flight = false; });
}

} // namespace cannet::canopen::detail
