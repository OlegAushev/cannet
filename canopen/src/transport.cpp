#include <canopen/transport.hpp>

#include <utility>

namespace cannet::canopen {

std::string_view to_string(transport_error e)
{
  switch (e) {
  case transport_error::closed: return "transport closed";
  case transport_error::send_failed: return "send failed";
  case transport_error::tx_queue_full: return "TX queue full (frame dropped)";
  }
  return "unknown error";
}

subscription::subscription(std::move_only_function<void()> cancel)
    : cancel_(std::move(cancel))
{
}

subscription::~subscription()
{
  reset();
}

subscription::subscription(subscription&& other) noexcept
    : cancel_(std::exchange(other.cancel_, nullptr))
{
}

subscription& subscription::operator=(subscription&& other) noexcept
{
  if (this != &other) {
    reset();
    cancel_ = std::exchange(other.cancel_, nullptr);
  }
  return *this;
}

void subscription::reset()
{
  // Cleared before the call, so a cancel that re-enters reset() is a no-op.
  if (auto cancel = std::exchange(cancel_, nullptr)) {
    cancel();
  }
}

} // namespace cannet::canopen
