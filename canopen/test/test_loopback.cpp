#include <canopen/loopback.hpp>

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace cannet::canopen;

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

// A frame handler that records the ids it receives.
auto record_into(ids& received)
{
  return
      [&received](can_frame const& frame) { received.push_back(frame.can_id); };
}

void ignore(send_result) {}

} // namespace

TEST_CASE("a frame reaches the matching subscribers of the other endpoints",
          "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};
  loopback_transport c{bus};

  ids at_a;
  ids at_b;
  ids at_b_wide;
  ids at_c;
  auto const sub_a = a.subscribe(exactly(0x123), record_into(at_a));
  auto const sub_b = b.subscribe(exactly(0x123), record_into(at_b));
  auto const sub_b_wide = b.subscribe({.can_id = 0x100, .can_mask = 0x700},
                                      record_into(at_b_wide));
  auto const sub_c = c.subscribe(exactly(0x124), record_into(at_c));

  std::optional<send_result> sent;
  a.send(frame_with_id(0x123), [&](send_result r) { sent = r; });
  io.run();

  REQUIRE(sent);
  CHECK(sent->has_value());
  CHECK(at_a.empty()); // the sender does not hear itself
  CHECK(at_b == ids{0x123});
  CHECK(at_b_wide == ids{0x123});
  CHECK(at_c.empty());
}

TEST_CASE("a send completes after delivery, never inside send()", "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};

  bool delivered = false;
  std::optional<bool> delivered_first;
  auto const sub = b.subscribe(exactly(0x1),
                               [&](can_frame const&) { delivered = true; });
  a.send(frame_with_id(0x1), [&](send_result) { delivered_first = delivered; });
  CHECK_FALSE(delivered_first);

  io.run();
  REQUIRE(delivered_first);
  CHECK(*delivered_first);
}

TEST_CASE("frames arrive in send order", "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};

  ids received;
  auto const sub = b.subscribe({.can_id = 0, .can_mask = 0},
                               record_into(received));
  for (canid_t const id : {0x3u, 0x1u, 0x2u}) {
    a.send(frame_with_id(id), ignore);
  }
  io.run();

  CHECK(received == ids{0x3, 0x1, 0x2});
}

TEST_CASE("an ended subscription receives nothing", "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};
  ids received;

  SECTION("after reset()")
  {
    auto sub = b.subscribe(exactly(0x1), record_into(received));
    sub.reset();
    a.send(frame_with_id(0x1), ignore);
    io.run();
  }

  SECTION("after destruction")
  {
    {
      auto const sub = b.subscribe(exactly(0x1), record_into(received));
    }
    a.send(frame_with_id(0x1), ignore);
    io.run();
  }

  CHECK(received.empty());
}

TEST_CASE("a moved subscription keeps delivering", "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};

  ids received;
  subscription moved;
  {
    auto sub = b.subscribe(exactly(0x1), record_into(received));
    moved = std::move(sub);
  }
  a.send(frame_with_id(0x1), ignore);
  io.run();

  CHECK(received == ids{0x1});
}

TEST_CASE("handlers may change subscriptions while frames are dispatched",
          "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};

  SECTION("a handler ends its own subscription")
  {
    int calls = 0;
    subscription sub;
    sub = b.subscribe(exactly(0x1), [&](can_frame const&) {
      ++calls;
      sub.reset();
    });
    a.send(frame_with_id(0x1), ignore);
    a.send(frame_with_id(0x1), ignore);
    io.run();
    CHECK(calls == 1);
  }

  SECTION("a handler ends another subscription the frame also matches")
  {
    int second_calls = 0;
    subscription second;
    auto const first = b.subscribe(exactly(0x1),
                                   [&](can_frame const&) { second.reset(); });
    second = b.subscribe(exactly(0x1),
                         [&](can_frame const&) { ++second_calls; });
    a.send(frame_with_id(0x1), ignore);
    io.run();
    CHECK(second_calls == 0);
  }

  SECTION("a subscription made inside a handler first sees the next frame")
  {
    int late_calls = 0;
    std::vector<subscription> late;
    auto const first = b.subscribe(exactly(0x1), [&](can_frame const&) {
      if (!late.empty()) {
        return;
      }
      // Enough subscriptions to make the registry reallocate under the
      // running handler.
      for (int i = 0; i < 64; ++i) {
        late.push_back(
            b.subscribe(exactly(0x1), [&](can_frame const&) { ++late_calls; }));
      }
    });
    a.send(frame_with_id(0x1), ignore);
    io.run();
    CHECK(late_calls == 0);

    io.restart();
    a.send(frame_with_id(0x1), ignore);
    io.run();
    CHECK(late_calls == 64);
  }
}

TEST_CASE("fail_next_send fails one send and delivers nothing", "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport a{bus};
  loopback_transport b{bus};

  ids received;
  auto const sub = b.subscribe({.can_id = 0, .can_mask = 0},
                               record_into(received));
  std::optional<send_result> first;
  std::optional<send_result> second;
  a.fail_next_send(transport_error::tx_queue_full);
  a.send(frame_with_id(0x1), [&](send_result r) { first = r; });
  a.send(frame_with_id(0x2), [&](send_result r) { second = r; });
  io.run();

  REQUIRE(first);
  REQUIRE_FALSE(*first);
  CHECK(first->error() == transport_error::tx_queue_full);
  REQUIRE(second);
  CHECK(second->has_value());
  CHECK(received == ids{0x2});
}

TEST_CASE("a send pending when its endpoint dies completes with closed",
          "[loopback]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport b{bus};

  ids received;
  auto const sub = b.subscribe({.can_id = 0, .can_mask = 0},
                               record_into(received));
  std::optional<send_result> sent;
  {
    loopback_transport a{bus};
    a.send(frame_with_id(0x1), [&](send_result r) { sent = r; });
  }
  io.run();

  REQUIRE(sent);
  REQUIRE_FALSE(*sent);
  CHECK(sent->error() == transport_error::closed);
  CHECK(received.empty());
}

TEST_CASE("endpoints outlive the bus object and subscriptions their transport",
          "[loopback]")
{
  boost::asio::io_context io;
  auto bus = std::make_unique<loopback_bus>(io.get_executor());
  loopback_transport a{*bus};
  subscription outliving;
  ids received;
  {
    loopback_transport b{*bus};
    auto const sub = b.subscribe(exactly(0x1), record_into(received));
    outliving = b.subscribe(exactly(0x2), record_into(received));
    bus.reset();

    a.send(frame_with_id(0x1), ignore);
    io.run();
  }
  outliving.reset(); // its transport is gone: a no-op

  CHECK(received == ids{0x1});
}

TEST_CASE("every transport_error has a description and a name", "[transport]")
{
  for (auto const e : {transport_error::closed,
                       transport_error::send_failed,
                       transport_error::tx_queue_full}) {
    CHECK(to_string(e) != "unknown error");
    CHECK(name(e) != "unknown");
  }
  CHECK(name(transport_error::tx_queue_full) == "tx_queue_full");
}
