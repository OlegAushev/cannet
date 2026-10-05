#pragma once

// cannet::canopen::loopback_bus, loopback_transport — an in-memory CAN bus.
// A frame sent through one endpoint reaches the matching subscribers of
// every other endpoint on the same bus, as frames travel between sockets on
// vcan; the sender does not get its own frames back.
//
// Protocol plane, unprivileged, no I/O. For tests: protocol code, and the
// per-device applications built on it, run against an emulated device on an
// io_context, with no interface at all. fail_next_send() injects send
// failures.
//
// Thread model: as for every transport (transport.hpp). The bus and all its
// endpoints share one executor; deliveries and completions run there.

#include <canopen/transport.hpp>

#include <memory>

namespace cannet::canopen {

namespace detail {
struct loopback_bus_state;
struct loopback_endpoint;
} // namespace detail

class loopback_bus {
public:
  using executor_type = boost::asio::any_io_executor;

  explicit loopback_bus(executor_type const& executor);
  ~loopback_bus();
  loopback_bus(loopback_bus const&) = delete;
  loopback_bus& operator=(loopback_bus const&) = delete;

  executor_type get_executor() const;

private:
  friend class loopback_transport;
  // Shared with the endpoints, which may outlive this object.
  std::shared_ptr<detail::loopback_bus_state> state_;
};

class loopback_transport final : public transport {
public:
  explicit loopback_transport(loopback_bus& bus);
  ~loopback_transport() override;
  loopback_transport(loopback_transport const&) = delete;
  loopback_transport& operator=(loopback_transport const&) = delete;

  executor_type get_executor() override;

  // Delivers the frame to the other endpoints, then completes `done`. A send
  // still pending when its endpoint is destroyed completes with
  // transport_error::closed and delivers nothing.
  void send(can_frame const& frame, send_handler done) override;

  [[nodiscard]] subscription subscribe(can_filter filter,
                                       frame_handler on_frame) override;

  // The next send() completes with `error` and delivers nothing.
  void fail_next_send(transport_error error);

private:
  std::shared_ptr<detail::loopback_bus_state> bus_;
  std::shared_ptr<detail::loopback_endpoint> endpoint_;
};

} // namespace cannet::canopen
