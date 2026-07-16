#include <cansocket/socket.h>

#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace cannet {

socket::~socket() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

socket::socket(socket&& other) noexcept : fd_{other.fd_} {
  other.fd_ = -1;
}

socket& socket::operator=(socket&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

std::expected<void, socket_error> socket::open(std::string_view iface) {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }

  ifreq ifr{};
  if (iface.size() >= IFNAMSIZ) {
    return std::unexpected(socket_error::interface_not_found);
  }
  std::memcpy(
      ifr.ifr_name,
      iface.data(),
      iface.size()
  ); // zero-init'ed => NUL-terminated

  int const fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (fd < 0) {
    return std::unexpected(socket_error::create_failed);
  }

  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    ::close(fd);
    return std::unexpected(socket_error::interface_not_found);
  }

  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return std::unexpected(socket_error::bind_failed);
  }

  fd_ = fd;
  return {};
}

std::expected<void, socket_error> socket::close() {
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

std::expected<void, socket_error> socket::send(can_frame const& frame) {
  if (fd_ < 0) {
    return std::unexpected(socket_error::not_open);
  }

  auto const written = ::write(fd_, &frame, sizeof(can_frame));
  if (written != static_cast<ssize_t>(sizeof(can_frame))) {
    return std::unexpected(socket_error::send_failed);
  }
  return {};
}

std::expected<can_frame, socket_error>
socket::recv(std::chrono::milliseconds timeout) {
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
    return std::unexpected(socket_error::recv_failed);
  }
  return frame;
}

std::string_view to_string(socket_error e) {
  switch (e) {
  case socket_error::not_open: return "socket is not open";
  case socket_error::create_failed: return "failed to create socket";
  case socket_error::interface_not_found: return "interface not found";
  case socket_error::bind_failed: return "failed to bind socket";
  case socket_error::close_failed: return "failed to close socket";
  case socket_error::send_failed: return "send failed";
  case socket_error::recv_timeout: return "receive timeout";
  case socket_error::recv_failed: return "receive failed";
  }
  return "unknown error";
}

} // namespace cannet
