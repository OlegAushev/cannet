#pragma once

// cannet::raw::async_socket — asynchronous raw CAN frame I/O (CAN_RAW) over
// an already-up SocketCAN interface, on a Boost.Asio executor.
//
// Transport plane only: the asynchronous sibling of cannet::raw::socket, with
// the same open/bind, kernel-side options and error enum. Frames move through
// Asio asynchronous operations that take any completion token: a callback,
// asio::deferred (the default) or use_awaitable in a coroutine, use_future,
// cancel_after. Their completion signatures carry std::expected — no
// error_code, no exceptions. Unprivileged, like the blocking socket.
//
// The executor comes from outside: the application owns the io_context. The
// socket opens and binds its fd itself and is the fd's only owner.
//
// Thread model: NOT thread-safe, like any Asio I/O object. Make every call on
// the socket's executor (or one strand of it); a completion runs on its
// handler's associated executor, by default the socket's. Each operation
// owns the frame it moves, so the socket may be closed, reopened or
// destroyed with operations outstanding: they complete with
// socket_error::cancelled.

#include <cansocket/raw/socket.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/default_completion_token.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/generic/raw_protocol.hpp>
#include <boost/system/error_code.hpp>

#include <linux/can.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace cannet::raw {

namespace detail {

// Map an Asio completion onto the result an async_socket operation reports.
std::expected<can_frame, socket_error>
receive_result(boost::system::error_code ec,
               std::size_t size,
               can_frame const& frame);
std::expected<void, socket_error> send_result(boost::system::error_code ec,
                                              std::size_t size);

} // namespace detail

class async_socket {
public:
  using executor_type = boost::asio::any_io_executor;

  // Completion signatures of async_receive() and async_send().
  using receive_signature = void(std::expected<can_frame, socket_error>);
  using send_signature = void(std::expected<void, socket_error>);

  explicit async_socket(executor_type const& executor);

  executor_type get_executor() noexcept;

  // Opens a RAW CAN socket and binds it to an already-up interface.
  // Reopening an open socket closes the previous one first; kernel options
  // (filters, loopback) are back at their defaults after every open().
  std::expected<void, socket_error> open(std::string_view iface);

  std::expected<void, socket_error> close();

  bool is_open() const;

  // Completes every outstanding operation with socket_error::cancelled; the
  // socket stays open. A no-op on a closed socket.
  void cancel();

  // Kernel-side options, with the semantics of their cannet::raw::socket
  // namesakes.
  std::expected<void, socket_error>
  set_filters(std::span<can_filter const> filters);
  std::expected<void, socket_error> set_loopback(bool enabled);
  std::expected<void, socket_error> set_recv_own_msgs(bool enabled);
  std::expected<void, socket_error> set_error_filter(can_err_mask_t mask);

  // Receives one frame. Supports per-operation cancellation (cancel_after,
  // a bound cancellation slot): a cancelled receive completes with
  // socket_error::cancelled.
  template<boost::asio::completion_token_for<receive_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_receive(
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return boost::asio::async_initiate<Token, receive_signature>(
        initiate_receive{&socket_},
        token);
  }

  // Sends one frame, copied: the caller's frame need not outlive the call.
  // A full interface TX queue (ENOBUFS) completes with
  // socket_error::tx_queue_full. Supports per-operation cancellation.
  template<boost::asio::completion_token_for<send_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_send(
      can_frame const& frame,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return boost::asio::async_initiate<Token, send_signature>(
        initiate_send{&socket_},
        token,
        frame);
  }

private:
  using socket_type = boost::asio::generic::raw_protocol::socket;

  // Initiations carry the socket's executor, which adapters such as
  // cancel_after use for their timers. Each operation keeps its frame on the
  // heap, owned by its completion step, so moving or destroying the socket
  // cannot invalidate the buffer.
  struct initiate_receive {
    using executor_type = async_socket::executor_type;

    socket_type* socket;

    executor_type get_executor() const noexcept
    {
      return socket->get_executor();
    }

    template<typename Handler>
    void operator()(Handler&& handler) const
    {
      auto storage = std::make_unique<can_frame>();
      auto const buffer = boost::asio::buffer(storage.get(), sizeof(can_frame));
      socket->async_receive(
          buffer,
          boost::asio::deferred(
              [frame = std::move(storage)](boost::system::error_code ec,
                                           std::size_t size) {
                return boost::asio::deferred.values(
                    detail::receive_result(ec, size, *frame));
              }))(std::forward<Handler>(handler));
    }
  };

  struct initiate_send {
    using executor_type = async_socket::executor_type;

    socket_type* socket;

    executor_type get_executor() const noexcept
    {
      return socket->get_executor();
    }

    template<typename Handler>
    void operator()(Handler&& handler, can_frame const& frame) const
    {
      auto storage = std::make_unique<can_frame>(frame);
      auto const buffer = boost::asio::buffer(storage.get(), sizeof(can_frame));
      socket->async_send(
          buffer,
          boost::asio::deferred(
              // `copy` keeps the bytes being sent alive until completion.
              [copy = std::move(storage)](boost::system::error_code ec,
                                          std::size_t size) {
                return boost::asio::deferred.values(
                    detail::send_result(ec, size));
              }))(std::forward<Handler>(handler));
    }
  };

  socket_type socket_;
};

} // namespace cannet::raw
