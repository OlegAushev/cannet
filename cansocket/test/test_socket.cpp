#include <cansocket/raw/socket.hpp>

#include <cannet_test/vcan.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace cannet::raw;
using namespace std::chrono_literals;

TEST_CASE("every socket_error has a description and a name", "[socket]")
{
  for (auto const e : {socket_error::not_open,
                       socket_error::create_failed,
                       socket_error::interface_not_found,
                       socket_error::interface_down,
                       socket_error::bind_failed,
                       socket_error::set_option_failed,
                       socket_error::close_failed,
                       socket_error::send_failed,
                       socket_error::tx_queue_full,
                       socket_error::recv_timeout,
                       socket_error::recv_failed,
                       socket_error::cancelled}) {
    CHECK(to_string(e) != "unknown error");
    CHECK(name(e) != "unknown");
  }
  CHECK(name(socket_error::tx_queue_full) == "tx_queue_full");
  CHECK(name(static_cast<socket_error>(-1)) == "unknown");
}

TEST_CASE("socket reports a missing interface", "[socket]")
{
  cannet::raw::socket s;

  auto const missing = s.open("cannet-nx0");
  REQUIRE_FALSE(missing);
  CHECK(missing.error() == socket_error::interface_not_found);

  auto const too_long = s.open("an-interface-name-too-long");
  REQUIRE_FALSE(too_long);
  CHECK(too_long.error() == socket_error::interface_not_found);

  CHECK_FALSE(s.is_open());
}

TEST_CASE("operations on a closed socket report not_open", "[socket]")
{
  cannet::raw::socket s;

  auto const sent = s.send(can_frame{});
  REQUIRE_FALSE(sent);
  CHECK(sent.error() == socket_error::not_open);

  auto const received = s.recv(0ms);
  REQUIRE_FALSE(received);
  CHECK(received.error() == socket_error::not_open);

  CHECK(s.close().error() == socket_error::not_open);
  CHECK(s.set_filters({}).error() == socket_error::not_open);
  CHECK(s.set_loopback(true).error() == socket_error::not_open);
  CHECK(s.set_recv_own_msgs(true).error() == socket_error::not_open);
  CHECK(s.set_error_filter(CAN_ERR_MASK).error() == socket_error::not_open);
}

TEST_CASE("interface_up reports a missing interface", "[socket]")
{
  auto const missing = interface_up("cannet-nx0");
  REQUIRE_FALSE(missing);
  CHECK(missing.error() == socket_error::interface_not_found);

  auto const too_long = interface_up("an-interface-name-too-long");
  REQUIRE_FALSE(too_long);
  CHECK(too_long.error() == socket_error::interface_not_found);
}

TEST_CASE("interface_up tells an interface that is up", "[socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }
  auto const up = interface_up(cannet::test::vcan_iface);
  REQUIRE(up);
  CHECK(*up);
}

TEST_CASE("recv times out when nothing arrives", "[socket][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  cannet::raw::socket s;
  REQUIRE(s.open(cannet::test::vcan_iface));
  REQUIRE(s.set_filters({})); // receive nothing

  auto const received = s.recv(20ms);
  REQUIRE_FALSE(received);
  CHECK(received.error() == socket_error::recv_timeout);
}
