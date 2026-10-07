#include <cansocket/raw/async_socket.hpp>
#include <cansocket/raw/error_frame.hpp>
#include <cansocket/raw/socket.hpp>

#include <cannet_test/vcan.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <linux/can/error.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

using namespace cannet::raw;
using namespace std::chrono_literals;

namespace {

using receive_result = std::expected<can_frame, socket_error>;
using send_result = std::expected<void, socket_error>;

can_frame test_frame(canid_t id, std::uint8_t byte)
{
  can_frame frame{};
  frame.can_id = id;
  frame.len = 1;
  frame.data[0] = byte;
  return frame;
}

} // namespace

TEST_CASE("async_socket reports a missing interface", "[async_socket]")
{
  boost::asio::io_context io;
  async_socket s{io.get_executor()};

  auto const missing = s.open("cannet-nx0");
  REQUIRE_FALSE(missing);
  CHECK(missing.error() == socket_error::interface_not_found);
  CHECK_FALSE(s.is_open());
}

TEST_CASE("operations on a closed async_socket report not_open",
          "[async_socket]")
{
  boost::asio::io_context io;
  async_socket s{io.get_executor()};

  std::optional<receive_result> received;
  std::optional<send_result> sent;
  s.async_receive([&](receive_result r) { received = r; });
  s.async_send(test_frame(0x123, 0xA5), [&](send_result r) { sent = r; });
  io.run();

  REQUIRE(received);
  CHECK(received->error() == socket_error::not_open);
  REQUIRE(sent);
  CHECK(sent->error() == socket_error::not_open);

  CHECK(s.close().error() == socket_error::not_open);
  CHECK(s.set_filters({}).error() == socket_error::not_open);
  CHECK(s.set_loopback(true).error() == socket_error::not_open);
  CHECK(s.set_recv_own_msgs(true).error() == socket_error::not_open);
  CHECK(s.set_error_filter(CAN_ERR_MASK).error() == socket_error::not_open);
  s.cancel(); // a no-op, not an error
}

TEST_CASE("a downed or vanished interface fails sends and receives by name",
          "[async_socket]")
{
  namespace detail = cannet::raw::detail;
  auto const system = [](int e) {
    return boost::system::error_code{e, boost::system::system_category()};
  };
  can_frame const frame{};

  CHECK(detail::receive_result(system(ENETDOWN), 0, frame).error()
        == socket_error::interface_down);
  CHECK(detail::receive_result(system(ENODEV), 0, frame).error()
        == socket_error::interface_not_found);
  CHECK(detail::receive_result(system(EIO), 0, frame).error()
        == socket_error::recv_failed);
  CHECK(detail::receive_result({}, 3, frame).error()
        == socket_error::recv_failed);

  CHECK(detail::send_result(system(ENETDOWN), 0).error()
        == socket_error::interface_down);
  CHECK(detail::send_result(system(ENXIO), 0).error()
        == socket_error::interface_not_found);
  CHECK(detail::send_result(system(ENOBUFS), 0).error()
        == socket_error::tx_queue_full);
  CHECK(detail::send_result(system(EIO), 0).error()
        == socket_error::send_failed);
}

TEST_CASE("frames travel between async sockets", "[async_socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  async_socket tx{io.get_executor()};
  async_socket rx{io.get_executor()};
  REQUIRE(tx.open(cannet::test::vcan_iface));
  REQUIRE(rx.open(cannet::test::vcan_iface));

  SECTION("with callbacks")
  {
    std::optional<receive_result> received;
    std::optional<send_result> sent;
    rx.async_receive([&](receive_result r) { received = r; });
    tx.async_send(test_frame(0x123, 0xA5), [&](send_result r) { sent = r; });
    io.run_for(1s);

    REQUIRE(sent);
    CHECK(sent->has_value());
    REQUIRE(received);
    REQUIRE(received->has_value());
    CHECK((*received)->can_id == 0x123);
    CHECK((*received)->len == 1);
    CHECK((*received)->data[0] == 0xA5);
  }

  SECTION("in a coroutine, with the default token")
  {
    auto exchange = [&]() -> boost::asio::awaitable<receive_result> {
      auto const sent = co_await tx.async_send(test_frame(0x7FF, 0x5A));
      if (!sent) {
        co_return std::unexpected(sent.error());
      }
      co_return co_await rx.async_receive();
    };
    auto result = boost::asio::co_spawn(io,
                                        exchange(),
                                        boost::asio::use_future);
    io.run_for(1s);

    REQUIRE(result.wait_for(0s) == std::future_status::ready);
    auto const received = result.get();
    REQUIRE(received);
    CHECK(received->can_id == 0x7FF);
    CHECK(received->data[0] == 0x5A);
  }
}

TEST_CASE("kernel filters pass matching frames only", "[async_socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  async_socket tx{io.get_executor()};
  async_socket rx{io.get_executor()};
  REQUIRE(tx.open(cannet::test::vcan_iface));
  REQUIRE(rx.open(cannet::test::vcan_iface));
  REQUIRE(rx.set_filters(
      std::array{can_filter{.can_id = 0x123, .can_mask = CAN_SFF_MASK}}));

  std::optional<receive_result> received;
  rx.async_receive([&](receive_result r) { received = r; });
  tx.async_send(test_frame(0x100, 0x01), [](send_result) {});
  tx.async_send(test_frame(0x123, 0x02), [](send_result) {});
  io.run_for(1s);

  REQUIRE(received);
  REQUIRE(received->has_value());
  CHECK((*received)->can_id == 0x123);
}

TEST_CASE("the blocking and the asynchronous socket share a bus",
          "[async_socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  cannet::raw::socket tx;
  async_socket rx{io.get_executor()};
  REQUIRE(tx.open(cannet::test::vcan_iface));
  REQUIRE(rx.open(cannet::test::vcan_iface));

  REQUIRE(tx.send(test_frame(0x321, 0x33)));

  std::optional<receive_result> received;
  rx.async_receive([&](receive_result r) { received = r; });
  io.run_for(1s);

  REQUIRE(received);
  REQUIRE(received->has_value());
  CHECK((*received)->can_id == 0x321);
}

TEST_CASE("a pending receive completes with cancelled", "[async_socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  std::optional<receive_result> received;
  auto on_receive = [&](receive_result r) { received = r; };

  // The sockets receive nothing, so only cancellation ends a receive.
  SECTION("after cancel_after's timeout")
  {
    async_socket s{io.get_executor()};
    REQUIRE(s.open(cannet::test::vcan_iface));
    REQUIRE(s.set_filters({}));
    s.async_receive(boost::asio::cancel_after(20ms, on_receive));
    io.run_for(1s);
  }

  SECTION("on cancel()")
  {
    async_socket s{io.get_executor()};
    REQUIRE(s.open(cannet::test::vcan_iface));
    REQUIRE(s.set_filters({}));
    s.async_receive(on_receive);
    boost::asio::post(io, [&] { s.cancel(); });
    io.run_for(1s);
    CHECK(s.is_open());
  }

  SECTION("on close()")
  {
    async_socket s{io.get_executor()};
    REQUIRE(s.open(cannet::test::vcan_iface));
    REQUIRE(s.set_filters({}));
    s.async_receive(on_receive);
    boost::asio::post(io, [&] { CHECK(s.close()); });
    io.run_for(1s);
  }

  SECTION("when the socket is destroyed")
  {
    {
      async_socket s{io.get_executor()};
      REQUIRE(s.open(cannet::test::vcan_iface));
      REQUIRE(s.set_filters({}));
      s.async_receive(on_receive);
    }
    io.run_for(1s);
  }

  REQUIRE(received);
  CHECK(received->error() == socket_error::cancelled);
}

TEST_CASE("error frames come through the error filter alone",
          "[async_socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  async_socket tx{io.get_executor()};
  async_socket errors{io.get_executor()};
  async_socket frames{io.get_executor()};
  REQUIRE(tx.open(cannet::test::vcan_iface));
  REQUIRE(errors.open(cannet::test::vcan_iface));
  REQUIRE(frames.open(cannet::test::vcan_iface));
  // No frame filter lets anything through: the error filter is separate.
  REQUIRE(errors.set_filters({}));
  REQUIRE(errors.set_error_filter(CAN_ERR_MASK));

  // vcan has no controller, but delivers an error frame a socket sends.
  can_frame bus_off{};
  bus_off.can_id = CAN_ERR_FLAG | CAN_ERR_BUSOFF;
  bus_off.len = CAN_ERR_DLC;

  std::optional<receive_result> with_filter;
  std::optional<receive_result> without_filter;
  errors.async_receive([&](receive_result r) { with_filter = r; });
  frames.async_receive(boost::asio::cancel_after(100ms, [&](receive_result r) {
    without_filter = r;
  }));
  tx.async_send(bus_off, [](send_result) {});
  io.run_for(1s);

  REQUIRE(with_filter);
  REQUIRE(with_filter->has_value());
  auto const report = decode_error_frame(**with_filter);
  REQUIRE(report);
  CHECK(report->state == cannet::raw::controller_state::bus_off);

  REQUIRE(without_filter);
  CHECK(without_filter->error() == socket_error::cancelled);
}
