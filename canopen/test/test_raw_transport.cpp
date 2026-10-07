#include <canopen/raw_transport.hpp>
#include <canopen/types.hpp>
#include <cansocket/raw/socket.hpp>

#include <linux/can/error.h>

#include <cannet_test/vcan.hpp>

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;

namespace {

using send_result = std::expected<void, transport_error>;
using ids = std::vector<canid_t>;

can_frame frame_with_id(canid_t id)
{
  can_frame frame{};
  frame.can_id = id;
  frame.len = 1;
  return frame;
}

constexpr can_filter exactly(canid_t id)
{
  return {.can_id = id, .can_mask = CAN_SFF_MASK};
}

auto record_into(ids& received)
{
  return
      [&received](can_frame const& frame) { received.push_back(frame.can_id); };
}

void ignore(send_result) {}

// An error frame as a controller's driver makes one; vcan has no
// controller, but delivers an error frame a socket sends.
can_frame error_frame(canid_t classes, std::array<std::uint8_t, 8> data = {})
{
  can_frame frame{};
  frame.can_id = CAN_ERR_FLAG | classes;
  frame.len = CAN_ERR_DLC;
  for (std::size_t i = 0; i < data.size(); ++i) {
    frame.data[i] = data[i];
  }
  return frame;
}

// Runs handlers until `done()` holds or `timeout` passes; a raw transport
// always has a receive pending, so io_context::run() would never return.
template<typename Predicate>
bool run_until(boost::asio::io_context& io,
               Predicate done,
               std::chrono::milliseconds timeout = 1s)
{
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    io.run_one_for(10ms);
  }
  return done();
}

} // namespace

TEST_CASE("raw_transport reports a missing interface", "[raw_transport]")
{
  boost::asio::io_context io;
  raw_transport t{io.get_executor()};

  auto const opened = t.open("cannet-nx0");
  REQUIRE_FALSE(opened);
  CHECK(opened.error() == cannet::raw::socket_error::interface_not_found);
  CHECK_FALSE(t.is_open());
  CHECK(t.status().state == bus_state::no_interface);
}

TEST_CASE("a send on a closed raw_transport completes with closed",
          "[raw_transport]")
{
  boost::asio::io_context io;
  raw_transport t{io.get_executor()};
  auto const sub = t.subscribe(exactly(0x1), [](can_frame const&) {});

  std::optional<send_result> sent;
  t.send(frame_with_id(0x1), [&](send_result r) { sent = r; });
  CHECK_FALSE(sent); // never inside send()
  io.run();

  REQUIRE(sent);
  REQUIRE_FALSE(*sent);
  CHECK(sent->error() == transport_error::closed);
}

TEST_CASE("frames travel between raw transports", "[raw_transport][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  raw_transport a{io.get_executor()};
  raw_transport b{io.get_executor()};
  REQUIRE(a.open(cannet::test::vcan_iface));
  REQUIRE(b.open(cannet::test::vcan_iface));

  ids received;
  auto const sub = b.subscribe(exactly(0x123), record_into(received));
  std::optional<send_result> sent;
  a.send(frame_with_id(0x100), ignore); // filtered out
  a.send(frame_with_id(0x123), [&](send_result r) { sent = r; });

  REQUIRE(run_until(io, [&] { return sent && !received.empty(); }));
  CHECK(sent->has_value());
  io.run_for(50ms); // nothing else may arrive
  CHECK(received == ids{0x123});
}

TEST_CASE("close() completes queued sends with closed", "[raw_transport][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  raw_transport t{io.get_executor()};
  REQUIRE(t.open(cannet::test::vcan_iface));

  std::vector<send_result> results;
  t.send(frame_with_id(0x1), [&](send_result r) { results.push_back(r); });
  t.send(frame_with_id(0x2), [&](send_result r) { results.push_back(r); });
  t.close();

  REQUIRE(run_until(io, [&] { return results.size() == 2; }));
  for (auto const& r : results) {
    REQUIRE_FALSE(r);
    CHECK(r.error() == transport_error::closed);
  }
}

TEST_CASE("subscriptions survive close() and reopen", "[raw_transport][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  raw_transport a{io.get_executor()};
  raw_transport b{io.get_executor()};
  REQUIRE(a.open(cannet::test::vcan_iface));
  REQUIRE(b.open(cannet::test::vcan_iface));

  ids received;
  auto const sub = b.subscribe(exactly(0x1), record_into(received));
  b.close();
  a.send(frame_with_id(0x1), ignore);
  io.run_for(50ms);
  CHECK(received.empty()); // closed: nothing delivered

  REQUIRE(b.open(cannet::test::vcan_iface));
  a.send(frame_with_id(0x1), ignore);
  REQUIRE(run_until(io, [&] { return !received.empty(); }));
  CHECK(received == ids{0x1});
}

TEST_CASE("the kernel applies a COB-ID filter the same way",
          "[raw_transport][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  cannet::raw::socket tx;
  cannet::raw::socket rx;
  REQUIRE(tx.open(cannet::test::vcan_iface));
  REQUIRE(rx.open(cannet::test::vcan_iface));
  auto const filter = cob_filter(0x181);
  REQUIRE(rx.set_filters(std::span{&filter, 1}));

  REQUIRE(tx.send(frame_with_id(0x181 | CAN_RTR_FLAG)));
  REQUIRE(tx.send(frame_with_id(0x181 | CAN_EFF_FLAG)));
  REQUIRE(tx.send(frame_with_id(0x181)));

  ids received;
  while (auto const frame = rx.recv(50ms)) {
    received.push_back(frame->can_id);
  }
  CHECK(received == ids{0x181});
}

TEST_CASE("a raw_transport is on no interface until it opens one and after "
          "it closes",
          "[raw_transport][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  raw_transport t{io.get_executor()};
  CHECK(t.status().state == bus_state::no_interface);
  std::vector<bus_state> seen;
  auto const changes = t.on_status(
      [&seen](bus_status const& status) { seen.push_back(status.state); });

  REQUIRE(t.open(cannet::test::vcan_iface));
  CHECK(t.is_open());
  CHECK(t.status() == bus_status{});
  t.close();
  CHECK_FALSE(t.is_open());
  CHECK(t.status().state == bus_state::no_interface);
  CHECK(seen == std::vector{bus_state::error_active, bus_state::no_interface});
}

TEST_CASE("error frames make the bus's status and reach no subscription",
          "[raw_transport][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  raw_transport t{io.get_executor()};
  REQUIRE(t.open(cannet::test::vcan_iface));
  cannet::raw::socket controller;
  REQUIRE(controller.open(cannet::test::vcan_iface));

  // Every frame, were error frames frames.
  ids received;
  auto const sub = t.subscribe({.can_id = 0, .can_mask = 0},
                               record_into(received));
  std::vector<bus_status> seen;
  auto const changes = t.on_status(
      [&seen](bus_status const& status) { seen.push_back(status); });

  REQUIRE(controller.send(
      error_frame(CAN_ERR_CRTL | CAN_ERR_CNT,
                  {0, CAN_ERR_CRTL_TX_PASSIVE, 0, 0, 0, 0, 130, 4})));
  REQUIRE(controller.send(error_frame(CAN_ERR_PROT | CAN_ERR_ACK,
                                      {0, 0, 0, CAN_ERR_PROT_LOC_ACK})));
  REQUIRE(controller.send(error_frame(CAN_ERR_BUSOFF)));
  REQUIRE(controller.send(
      error_frame(CAN_ERR_CRTL, {0, CAN_ERR_CRTL_RX_OVERFLOW})));
  // Lost arbitration is counted nowhere.
  REQUIRE(controller.send(error_frame(CAN_ERR_LOSTARB)));
  REQUIRE(controller.send(error_frame(CAN_ERR_RESTARTED)));
  REQUIRE(controller.send(frame_with_id(0x123)));
  REQUIRE(run_until(io, [&] { return !received.empty(); }));

  using counters = cannet::raw::error_counters;
  bus_status const passive{.state = bus_state::error_passive,
                           .bus_errors = 0,
                           .overflows = 0,
                           .counters = counters{.tx = 130, .rx = 4}};
  auto with_error = passive;
  with_error.bus_errors = 1;
  auto off = with_error;
  off.state = bus_state::bus_off;
  auto overflowed = off;
  overflowed.overflows = 1;
  // A restart zeroes the counters, which the controller has not reported
  // since.
  auto restarted = overflowed;
  restarted.state = bus_state::error_active;
  restarted.counters.reset();
  CHECK(seen == std::vector{passive, with_error, off, overflowed, restarted});
  CHECK(t.status() == restarted);
  // The error frames reached no subscription.
  CHECK(received == ids{0x123});

  // A new open() counts afresh.
  REQUIRE(t.open(cannet::test::vcan_iface));
  CHECK(t.status() == bus_status{});
}
