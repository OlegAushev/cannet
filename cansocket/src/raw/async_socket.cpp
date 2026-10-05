#include <cansocket/raw/async_socket.hpp>

#include "socket_ops.hpp"

#include <boost/asio/error.hpp>

#include <linux/can/raw.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cannet::raw {

namespace detail {

std::expected<can_frame, socket_error>
receive_result(boost::system::error_code ec,
               std::size_t size,
               can_frame const& frame)
{
  if (ec == boost::asio::error::operation_aborted) {
    return std::unexpected(socket_error::cancelled);
  }
  if (ec == boost::asio::error::bad_descriptor) {
    return std::unexpected(socket_error::not_open);
  }
  if (ec || size != sizeof(can_frame)) {
    return std::unexpected(socket_error::recv_failed);
  }
  return frame;
}

std::expected<void, socket_error> send_result(boost::system::error_code ec,
                                              std::size_t size)
{
  if (ec == boost::asio::error::operation_aborted) {
    return std::unexpected(socket_error::cancelled);
  }
  if (ec == boost::asio::error::bad_descriptor) {
    return std::unexpected(socket_error::not_open);
  }
  // ENOBUFS: the interface TX queue is full. Asio waits out EAGAIN by
  // itself, so this is the one "retry later" a completion can carry.
  if (ec == boost::asio::error::no_buffer_space) {
    return std::unexpected(socket_error::tx_queue_full);
  }
  if (ec || size != sizeof(can_frame)) {
    return std::unexpected(socket_error::send_failed);
  }
  return {};
}

} // namespace detail

// Asio's error_code overloads also return the error_code they fill in; the
// casts to void below discard that duplicate.

async_socket::async_socket(executor_type const& executor) : socket_(executor) {}

async_socket::executor_type async_socket::get_executor() noexcept
{
  return socket_.get_executor();
}

std::expected<void, socket_error> async_socket::open(std::string_view iface)
{
  boost::system::error_code ignored; // the old fd is gone either way
  static_cast<void>(socket_.close(ignored));

  auto const fd = detail::open_bound(iface);
  if (!fd) {
    return std::unexpected(fd.error());
  }

  // Hands the fd to Asio, which registers it with its reactor.
  boost::system::error_code ec;
  static_cast<void>(
      socket_.assign(boost::asio::generic::raw_protocol(PF_CAN, CAN_RAW),
                     *fd,
                     ec));
  if (ec) {
    ::close(*fd);
    return std::unexpected(socket_error::create_failed);
  }
  return {};
}

std::expected<void, socket_error> async_socket::close()
{
  if (!socket_.is_open()) {
    return std::unexpected(socket_error::not_open);
  }

  boost::system::error_code ec;
  // The fd is gone even when close() reports an error.
  static_cast<void>(socket_.close(ec));
  if (ec) {
    return std::unexpected(socket_error::close_failed);
  }
  return {};
}

bool async_socket::is_open() const
{
  return socket_.is_open();
}

void async_socket::cancel()
{
  boost::system::error_code ignored; // fails only on a closed socket
  static_cast<void>(socket_.cancel(ignored));
}

std::expected<void, socket_error>
async_socket::set_filters(std::span<can_filter const> filters)
{
  if (!socket_.is_open()) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_filters(socket_.native_handle(), filters);
}

std::expected<void, socket_error> async_socket::set_loopback(bool enabled)
{
  if (!socket_.is_open()) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_flag(socket_.native_handle(), CAN_RAW_LOOPBACK, enabled);
}

std::expected<void, socket_error> async_socket::set_recv_own_msgs(bool enabled)
{
  if (!socket_.is_open()) {
    return std::unexpected(socket_error::not_open);
  }
  return detail::set_flag(socket_.native_handle(),
                          CAN_RAW_RECV_OWN_MSGS,
                          enabled);
}

} // namespace cannet::raw
