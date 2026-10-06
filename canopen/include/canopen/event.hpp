#pragma once

// cannet::canopen::event — a notification with any number of subscribers.
// Services report what happens through events (a node's heartbeat
// changing, an emergency arriving), and whoever needs current state builds
// it from them; an application may use events to fan out values of its own.
//
// Protocol plane, unprivileged, no I/O.
//
// Thread model: NOT thread-safe. Subscribe, end subscriptions and emit on
// one executor — for a service's events, the client's. Handlers run inside
// emit(), on that executor.

#include <canopen/detail/handler_list.hpp>
#include <canopen/subscription.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>

namespace cannet::canopen {

template<typename... Args>
class event {
public:
  using handler_type = std::move_only_function<void(Args const&...)>;

  event() = default;
  event(event const&) = delete;
  event& operator=(event const&) = delete;

  // Calls `handler` on every emission until the subscription ends.
  // Subscribed from inside a handler, it first sees the next emission.
  [[nodiscard]] subscription subscribe(handler_type handler)
  {
    auto const id = state_->handlers.add(std::move(handler));
    return subscription{[weak = std::weak_ptr{state_}, id] {
      if (auto const s = weak.lock()) {
        s->handlers.remove(id);
      }
    }};
  }

  // Calls every handler with `args`. Emissions keep their order: emit()
  // from inside a handler queues its arguments and returns, and they go out
  // once the current emission has reached every handler, so each handler
  // sees every emission, in the order they were made. A handler may destroy
  // the event; the emission still reaches the remaining handlers.
  void emit(Args const&... args)
  {
    auto const s = state_;
    if (s->emitting) {
      s->pending.emplace_back(args...);
      return;
    }
    emission const scope{*s};
    s->deliver(args...);
    while (!s->pending.empty()) {
      auto const values = std::move(s->pending.front());
      s->pending.pop_front();
      std::apply([&s](Args const&... a) { s->deliver(a...); }, values);
    }
  }

private:
  struct state {
    detail::handler_list<handler_type> handlers;
    std::deque<std::tuple<Args...>> pending;
    bool emitting = false;

    void deliver(Args const&... args)
    {
      handlers.for_each([&](handler_type& handler) { handler(args...); });
    }
  };

  struct emission {
    explicit emission(state& s) : st(s)
    {
      st.emitting = true;
    }

    // Normally the queue is empty by now; after a handler threw, what it
    // held is dropped.
    ~emission()
    {
      st.emitting = false;
      st.pending.clear();
    }

    emission(emission const&) = delete;
    emission& operator=(emission const&) = delete;

    state& st;
  };

  // Shared with the subscriptions, which may outlive the event, and kept
  // alive by emit() while a handler destroys the event.
  std::shared_ptr<state> state_ = std::make_shared<state>();
};

} // namespace cannet::canopen
