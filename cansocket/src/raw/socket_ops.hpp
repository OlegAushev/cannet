#pragma once

// fd-level CAN_RAW operations shared by cannet::raw::socket and
// cannet::raw::async_socket, so that both open, bind and configure a socket
// the same way and report the same errors. Private to cansocket.

#include <cansocket/raw/socket.hpp>

#include <linux/can.h>

#include <expected>
#include <span>
#include <string_view>

namespace cannet::raw::detail {

// Creates a CAN_RAW socket and binds it to `iface`; returns the new fd.
std::expected<int, socket_error> open_bound(std::string_view iface);

// CAN_RAW_FILTER; an empty span makes the socket receive nothing.
std::expected<void, socket_error>
set_filters(int fd, std::span<can_filter const> filters);

// An int-valued boolean SOL_CAN_RAW option (loopback, recv_own_msgs).
std::expected<void, socket_error> set_flag(int fd, int optname, bool enabled);

} // namespace cannet::raw::detail
