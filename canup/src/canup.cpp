#include <canup/canup.h>

#include <libsocketcan.h>

#include <cerrno>
#include <string>

namespace cannet::canup {

namespace {

error error_from_errno() {
  switch (errno) {
  case EPERM:
  case EACCES: return error::permission_denied;
  case ENODEV:
  case ENXIO: return error::device_not_found;
  default: return error::netlink_error;
  }
}

} // namespace

std::expected<void, error>
up(std::string_view iface, std::uint32_t bitrate, std::uint32_t restart_ms) {
  if (bitrate == 0) {
    return std::unexpected(error::invalid_bitrate);
  }

  std::string const name(iface);

  // Bittiming can only be changed while the interface is down.
  if (can_do_stop(name.c_str()) < 0) {
    return std::unexpected(error_from_errno());
  }

  if (can_set_bitrate(name.c_str(), bitrate) < 0) {
    // libsocketcan reports both "no device" and "unsupported bitrate" via errno.
    return (errno == EINVAL) ? std::unexpected(error::invalid_bitrate)
                             : std::unexpected(error_from_errno());
  }

  if (restart_ms > 0) {
    // Non-fatal: some drivers/virtual interfaces don't support auto-restart.
    can_set_restart_ms(name.c_str(), restart_ms);
  }

  if (can_do_start(name.c_str()) < 0) {
    return std::unexpected(error_from_errno());
  }

  return {};
}

std::expected<void, error> down(std::string_view iface) {
  std::string const name(iface);
  if (can_do_stop(name.c_str()) < 0) {
    return std::unexpected(error_from_errno());
  }
  return {};
}

std::expected<network_state, error> status(std::string_view iface) {
  std::string const name(iface);

  int state = 0;
  if (can_get_state(name.c_str(), &state) < 0) {
    return std::unexpected(error_from_errno());
  }

  network_state st;
  st.up = (state != CAN_STATE_STOPPED && state != CAN_STATE_SLEEPING);

  can_bittiming bt{};
  if (can_get_bittiming(name.c_str(), &bt) == 0) {
    st.bitrate = bt.bitrate;
  }

  return st;
}

std::string_view to_string(error e) {
  switch (e) {
  case error::invalid_bitrate: return "invalid bitrate";
  case error::device_not_found: return "device not found";
  case error::permission_denied:
    return "permission denied (need CAP_NET_ADMIN)";
  case error::netlink_error: return "netlink error";
  }
  return "unknown error";
}

} // namespace cannet::canup
