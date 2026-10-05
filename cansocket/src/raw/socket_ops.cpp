#include "socket_ops.hpp"

#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace cannet::raw::detail {

std::expected<int, socket_error> open_bound(std::string_view iface)
{
  ifreq ifr{};
  if (iface.size() >= IFNAMSIZ) {
    return std::unexpected(socket_error::interface_not_found);
  }
  std::memcpy(ifr.ifr_name,
              iface.data(),
              iface.size()); // zero-init'ed => NUL-terminated

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

  return fd;
}

std::expected<void, socket_error>
set_filters(int fd, std::span<can_filter const> filters)
{
  // The kernel caps the filter count (512 as of Linux 6.x) and rejects an
  // oversized set with EINVAL.
  if (setsockopt(fd,
                 SOL_CAN_RAW,
                 CAN_RAW_FILTER,
                 filters.data(),
                 static_cast<socklen_t>(filters.size_bytes()))
      < 0) {
    return std::unexpected(socket_error::set_option_failed);
  }
  return {};
}

std::expected<void, socket_error> set_flag(int fd, int optname, bool enabled)
{
  int const value = enabled ? 1 : 0;
  if (setsockopt(fd, SOL_CAN_RAW, optname, &value, sizeof(value)) < 0) {
    return std::unexpected(socket_error::set_option_failed);
  }
  return {};
}

} // namespace cannet::raw::detail
