#pragma once

// cannet::socket — raw CAN frame I/O over an already-up SocketCAN interface.
//
// Transport plane only: open/bind, send, recv. Unprivileged — binding a RAW
// CAN socket requires no capabilities. Interface configuration (bitrate,
// up/down) is a separate concern; see cannet::canup.
//
// Thread model: NOT thread-safe. Serialize all access externally — a single
// thread, or an asio executor/strand (the protocol layer runs everything on
// one io_context). Frame integrity of concurrent send()/recv() is guaranteed
// by the kernel (one datagram per syscall), but open()/close() racing
// against I/O is not: don't do it.

#include <linux/can.h>

#include <chrono>
#include <expected>
#include <string_view>

namespace cannet {

enum class socket_error {
  not_open,            // open() not called yet or socket already closed
  create_failed,       // socket(PF_CAN, SOCK_RAW, CAN_RAW) failed
  interface_not_found, // no such interface (name too long or SIOCGIFINDEX)
  bind_failed,
  close_failed,
  send_failed,
  recv_timeout, // no frame within the timeout; not a fault
  recv_failed,
};

class socket {
public:
  socket() = default;
  ~socket();
  socket(socket const&) = delete;
  socket& operator=(socket const&) = delete;
  socket(socket&& other) noexcept;
  socket& operator=(socket&& other) noexcept;

  // Opens a RAW CAN socket and binds it to an already-up interface.
  // Reopening an open socket closes the previous one first.
  std::expected<void, socket_error> open(std::string_view iface);

  std::expected<void, socket_error> close();

  bool is_open() const {
    return fd_ >= 0;
  }

  // Raw fd for integration with poll()-based loops or asio (e.g. wrapping
  // in asio::posix::stream_descriptor); -1 when the socket is not open.
  int native_handle() const {
    return fd_;
  }

  std::expected<void, socket_error> send(can_frame const& frame);

  // Waits up to `timeout` for one frame.
  std::expected<can_frame, socket_error>
  recv(std::chrono::milliseconds timeout);

private:
  int fd_ = -1;
};

// Returns human-readable name for a `socket_error` (for logging / CLI output).
std::string_view to_string(socket_error e);

} // namespace cannet
