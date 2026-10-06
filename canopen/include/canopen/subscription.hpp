#pragma once

// cannet::canopen::subscription — the handle that keeps a handler
// subscribed, to a transport's frames (transport.hpp) or to an event
// (event.hpp). Delivery ends when the handle is destroyed or reset().
//
// Protocol plane, unprivileged, no I/O.
//
// Thread model: NOT thread-safe. Destroy or reset a subscription on the
// executor of whatever it subscribes to.

#include <functional>

namespace cannet::canopen {

// Delivery to one handler, in force until the subscription is destroyed or
// reset(). Move-only; a default-constructed subscription is empty. Outliving
// what it subscribes to is harmless.
class subscription {
public:
  subscription() = default;
  explicit subscription(std::move_only_function<void()> cancel);
  ~subscription();
  subscription(subscription&& other) noexcept;
  subscription& operator=(subscription&& other) noexcept;
  subscription(subscription const&) = delete;
  subscription& operator=(subscription const&) = delete;

  // Ends delivery now; safe from inside the subscription's own handler.
  void reset();

private:
  std::move_only_function<void()> cancel_;
};

} // namespace cannet::canopen
