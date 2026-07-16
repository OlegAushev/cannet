#include <canup/canup.h>

#include <libsocketcan.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

namespace cannet::canup {

namespace {

// libsocketcan prints raw "RTNETLINK answers: ..." lines to stderr on every
// failed request, bypassing our error reporting. Redirect fd 2 to /dev/null
// for the duration of a libsocketcan call so callers see only `failure_report`.
// Note: fd redirection is process-wide while an instance is alive.
class stderr_silencer {
public:
  stderr_silencer() {
    std::fflush(stderr);
    saved_fd_ = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (saved_fd_ < 0) {
      return; // can't restore later, so don't redirect at all
    }
    int const devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
  }

  ~stderr_silencer() {
    if (saved_fd_ >= 0) {
      std::fflush(stderr);
      dup2(saved_fd_, STDERR_FILENO);
      close(saved_fd_);
    }
  }

  stderr_silencer(stderr_silencer const&) = delete;
  stderr_silencer& operator=(stderr_silencer const&) = delete;

private:
  int saved_fd_{-1};
};

// Runs one libsocketcan call with stderr silenced; returns the errno
// captured right after the call (0 on success).
template<typename F>
int silenced(F&& call) {
  stderr_silencer const silencer;
  errno = 0;
  if (call() < 0) {
    return (errno != 0) ? errno : EIO;
  }
  return 0;
}

error error_from(int err) {
  switch (err) {
  case EPERM:
  case EACCES: return error::permission_denied;
  case ENODEV:
  case ENXIO: return error::device_not_found;
  case EOPNOTSUPP: return error::not_supported;
  default: return error::netlink_error;
  }
}

std::unexpected<failure_report> fail(operation op, int err) {
  return std::unexpected(failure_report{op, error_from(err)});
}

} // namespace

std::expected<up_report, failure_report>
up(std::string_view iface, std::uint32_t bitrate, std::uint32_t restart_ms) {
  if (bitrate == 0) {
    return std::unexpected(
        failure_report{operation::set_bitrate, error::invalid_bitrate}
    );
  }

  std::string const name(iface);

  // Bittiming can only be changed while the interface is down.
  if (int const err = silenced([&] { return can_do_stop(name.c_str()); })) {
    return fail(operation::stop, err);
  }

  if (int const err = silenced([&] {
        return can_set_bitrate(name.c_str(), bitrate);
      })) {
    // The driver rejects an unsupported bitrate value with EINVAL.
    return (err == EINVAL) ? std::unexpected(
                                 failure_report{
                                     operation::set_bitrate,
                                     error::invalid_bitrate
                                 }
                             )
                           : fail(operation::set_bitrate, err);
  }

  up_report report;
  if (restart_ms > 0) {
    // Non-fatal: some drivers/virtual interfaces don't support auto-restart.
    int const err = silenced([&] {
      return can_set_restart_ms(name.c_str(), restart_ms);
    });
    report.restart_ms_applied = (err == 0);
  }

  if (int const err = silenced([&] { return can_do_start(name.c_str()); })) {
    return fail(operation::start, err);
  }

  return report;
}

std::expected<void, failure_report> down(std::string_view iface) {
  std::string const name(iface);
  if (int const err = silenced([&] { return can_do_stop(name.c_str()); })) {
    return fail(operation::stop, err);
  }
  return {};
}

std::expected<network_state, failure_report> status(std::string_view iface) {
  std::string const name(iface);

  int state = 0;
  if (int const err = silenced([&] {
        return can_get_state(name.c_str(), &state);
      })) {
    return fail(operation::get_state, err);
  }

  network_state st;
  st.up = (state != CAN_STATE_STOPPED && state != CAN_STATE_SLEEPING);

  can_bittiming bt{};
  if (silenced([&] { return can_get_bittiming(name.c_str(), &bt); }) == 0) {
    st.bitrate = bt.bitrate;
  }

  return st;
}

std::string_view to_string(operation op) {
  switch (op) {
  case operation::stop: return "stop";
  case operation::set_bitrate: return "set bitrate";
  case operation::set_restart_ms: return "set restart-ms";
  case operation::start: return "start";
  case operation::get_state: return "get state";
  case operation::get_bittiming: return "get bittiming";
  }
  return "unknown operation";
}

std::string_view to_string(error e) {
  switch (e) {
  case error::invalid_bitrate: return "invalid bitrate";
  case error::device_not_found: return "device not found";
  case error::permission_denied:
    return "permission denied (need CAP_NET_ADMIN)";
  case error::not_supported: return "operation not supported by driver";
  case error::netlink_error: return "netlink error";
  }
  return "unknown error";
}

} // namespace cannet::canup
