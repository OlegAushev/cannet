#include <cansocket/raw/socket.hpp>

#include "socket_ops.hpp"

#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>

namespace cannet::raw {

socket::~socket()
{
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

socket::socket(socket&& other) noexcept : fd_(other.fd_)
{
  other.fd_ = -1;
}

socket& socket::operator=(socket&& other) noexcept
{
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

std::expected<void, socket_error> socket::open(std::string_view iface)
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }

  auto const fd = detail::open_bound(iface);
  if (!fd) {
    return std::unexpected(fd.error());
  }
  fd_ = *fd;
  return {};
}

std::expected<void, socket_error> socket::close()
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }

  int const rc = ::close(fd_);
  fd_ = -1; // the fd is gone even when close() reports an error
  if (rc < 0) {
    return std::unexpected(socket_error::close_failed);
  }
  return {};
}

std::expected<void, socket_error>
socket::set_filters(std::span<can_filter const> filters)
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_filters(fd_, filters);
}

std::expected<void, socket_error> socket::set_loopback(bool enabled)
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_flag(fd_, CAN_RAW_LOOPBACK, enabled);
}

std::expected<void, socket_error> socket::set_recv_own_msgs(bool enabled)
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_flag(fd_, CAN_RAW_RECV_OWN_MSGS, enabled);
}

std::expected<void, socket_error> socket::set_error_filter(can_err_mask_t mask)
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_error_filter(fd_, mask);
}

std::expected<void, socket_error> socket::send(can_frame const& frame)
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }

  auto const written = ::write(fd_, &frame, sizeof(can_frame));
  if (written < 0) {
    // ENOBUFS: the interface TX queue is full — the kernel reports it even
    // on a blocking socket. EAGAIN can appear only once the fd is switched
    // to non-blocking; both mean "retry later".
    return std::unexpected(
        (errno == ENOBUFS || errno == EAGAIN)
            ? socket_error::tx_queue_full
            : detail::io_error(errno, socket_error::send_failed));
  }
  if (written != static_cast<ssize_t>(sizeof(can_frame))) {
    return std::unexpected(socket_error::send_failed);
  }
  return {};
}

std::expected<can_frame, socket_error>
socket::recv(std::chrono::milliseconds timeout)
{
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }

  pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
  int const rc = poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (rc < 0) {
    return std::unexpected(socket_error::recv_failed);
  }
  if (rc == 0) {
    return std::unexpected(socket_error::recv_timeout);
  }

  can_frame frame{};
  if (::read(fd_, &frame, sizeof(can_frame)) < 0) {
    return std::unexpected(detail::io_error(errno, socket_error::recv_failed));
  }
  return frame;
}

std::expected<bool, socket_error> interface_up(std::string_view iface)
{
  auto const flags = detail::interface_flags(iface);
  if (!flags) {
    return std::unexpected(flags.error());
  }
  return (*flags & IFF_UP) != 0;
}

std::string_view to_string(socket_error e)
{
  switch (e) {
  case socket_error::not_open: return "socket is not open";
  case socket_error::create_failed: return "failed to create socket";
  case socket_error::interface_not_found: return "interface not found";
  case socket_error::interface_down: return "interface is down";
  case socket_error::bind_failed: return "failed to bind socket";
  case socket_error::set_option_failed: return "failed to set socket option";
  case socket_error::close_failed: return "failed to close socket";
  case socket_error::send_failed: return "send failed";
  case socket_error::tx_queue_full: return "TX queue full (retry later)";
  case socket_error::recv_timeout: return "receive timeout";
  case socket_error::recv_failed: return "receive failed";
  case socket_error::cancelled: return "operation cancelled";
  }
  return "unknown error";
}

std::string_view name(socket_error e)
{
  switch (e) {
  case socket_error::not_open: return "not_open";
  case socket_error::create_failed: return "create_failed";
  case socket_error::interface_not_found: return "interface_not_found";
  case socket_error::interface_down: return "interface_down";
  case socket_error::bind_failed: return "bind_failed";
  case socket_error::set_option_failed: return "set_option_failed";
  case socket_error::close_failed: return "close_failed";
  case socket_error::send_failed: return "send_failed";
  case socket_error::tx_queue_full: return "tx_queue_full";
  case socket_error::recv_timeout: return "recv_timeout";
  case socket_error::recv_failed: return "recv_failed";
  case socket_error::cancelled: return "cancelled";
  }
  return "unknown";
}

} // namespace cannet::raw
