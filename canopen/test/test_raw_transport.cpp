#include <canopen/raw_transport.hpp>

#include <cannet_test/vcan.hpp>

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
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
