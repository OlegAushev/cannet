#pragma once

// cannet::canup — CAN interface configuration (bring-up / bring-down / status).
//
// Thin wrapper over libsocketcan. up()/down() perform rtnetlink writes and
// require CAP_NET_ADMIN in the calling process; status() is a read and needs
// no privileges. The library enforces no interface/bitrate whitelist — that is
// application policy and belongs to the caller.

#include <cstdint>
#include <expected>
#include <string_view>

namespace cannet::canup {

enum class error {
  invalid_bitrate,   // bitrate == 0 or rejected by the driver
  device_not_found,  // no such CAN interface
  permission_denied, // caller lacks CAP_NET_ADMIN
  netlink_error,     // any other rtnetlink failure
};

struct network_state {
  bool up{false}; // interface is started (controller not STOPPED/SLEEPING)
  std::uint32_t bitrate{0}; // configured bitrate in bit/s, 0 if unknown
};

// Brings the interface down, sets bitrate (+ auto-restart), brings it up.
// Requires CAP_NET_ADMIN. restart_ms == 0 leaves auto-restart untouched.
std::expected<void, error>
up(std::string_view iface,
   std::uint32_t bitrate,
   std::uint32_t restart_ms = 100);

// Brings the interface down. Requires CAP_NET_ADMIN.
std::expected<void, error> down(std::string_view iface);

// Reads the interface state and configured bitrate. No privileges required.
std::expected<network_state, error> status(std::string_view iface);

// Returns human-readable name for an `error` (for logging / CLI output).
std::string_view to_string(error e);

} // namespace cannet::canup
