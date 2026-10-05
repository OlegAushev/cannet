#pragma once

// cannet::canopen::raw_transport — the transport over a CAN_RAW socket
// (cannet::raw::async_socket) on an already-up SocketCAN interface.
//
// Protocol plane, unprivileged. It binds canopen's transport interface to
// the transport plane, and lives here rather than in cansocket because a
// plane never reaches upward. The socket's kernel filters follow the live
// subscriptions, so the kernel drops frames nobody subscribed to; which
// handler gets a frame is still decided by the subscriptions' own filters.
// A failed receive (an interface going down, say) is retried after a pause,
// so a transient fault does not end reception.
//
// Thread model: as for every transport (transport.hpp) — every call on the
// executor given at construction; handlers and completions run there.

#include <canopen/transport.hpp>
#include <cansocket/raw/socket.hpp>

#include <expected>
#include <memory>
#include <string_view>

namespace cannet::canopen {

class raw_transport final : public transport {
public:
  explicit raw_transport(executor_type const& executor);
  ~raw_transport() override;
  raw_transport(raw_transport const&) = delete;
  raw_transport& operator=(raw_transport const&) = delete;

  // Opens a CAN_RAW socket on an already-up interface and starts receiving.
  // Reopening closes the previous socket first; subscriptions carry over.
  std::expected<void, raw::socket_error> open(std::string_view iface);

  // Stops receiving. Sends still queued, the one in flight included,
  // complete with transport_error::closed. Subscriptions stay, for the next
  // open().
  void close();

  bool is_open() const;

  executor_type get_executor() override;
  void send(can_frame const& frame, send_handler done) override;
  [[nodiscard]] subscription subscribe(can_filter filter,
                                       frame_handler on_frame) override;

private:
  struct state;
  std::shared_ptr<state> state_;
};

} // namespace cannet::canopen
