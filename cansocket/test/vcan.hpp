#pragma once

// The interface for tests that need a live CAN interface: vcan0. Such tests
// skip when it is not up. Creating it is privileged and left to the user:
//
//   sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
//
// Tests never touch a real interface: a frame sent there reaches a real bus.

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string_view>

namespace cannet::test {

inline constexpr std::string_view vcan_iface = "vcan0";

inline bool vcan_up()
{
  int const fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  ifreq ifr{};
  std::memcpy(ifr.ifr_name, vcan_iface.data(), vcan_iface.size());
  bool const up = ioctl(fd, SIOCGIFFLAGS, &ifr) == 0
               && (ifr.ifr_flags & IFF_UP) != 0;
  ::close(fd);
  return up;
}

} // namespace cannet::test
