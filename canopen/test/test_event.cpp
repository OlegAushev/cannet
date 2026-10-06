#include <canopen/event.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using cannet::canopen::event;
using cannet::canopen::subscription;

TEST_CASE("an emission reaches every subscriber until its subscription ends",
          "[event]")
{
  event<int> e;
  std::vector<int> a;
  std::vector<int> b;
  auto sub_a = e.subscribe([&a](int v) { a.push_back(v); });
  auto const sub_b = e.subscribe([&b](int v) { b.push_back(v); });

  e.emit(1);
  sub_a.reset();
  e.emit(2);

  CHECK(a == std::vector{1});
  CHECK(b == std::vector{1, 2});
}

TEST_CASE("an event carries any number of arguments", "[event]")
{
  event<> tick;
  int ticks = 0;
  auto const sub_tick = tick.subscribe([&ticks] { ++ticks; });
  tick.emit();
  tick.emit();
  CHECK(ticks == 2);

  event<int, std::string> pair;
  std::vector<std::string> seen;
  auto const sub_pair = pair.subscribe([&seen](int n, std::string const& s) {
    seen.push_back(std::to_string(n) + s);
  });
  pair.emit(7, "x");
  CHECK(seen == std::vector<std::string>{"7x"});
}

TEST_CASE("a handler subscribed during an emission first sees the next one",
          "[event]")
{
  event<int> e;
  std::vector<int> late;
  std::optional<subscription> late_sub;
  auto const first = e.subscribe([&](int) {
    if (!late_sub) {
      late_sub = e.subscribe([&late](int v) { late.push_back(v); });
    }
  });

  e.emit(1);
  e.emit(2);

  CHECK(late == std::vector{2});
}

TEST_CASE("a handler removed during an emission is not called again", "[event]")
{
  event<int> e;
  std::vector<int> calls;
  subscription second;
  subscription third;
  auto const first = e.subscribe([&](int v) {
    calls.push_back(v * 10 + 1);
    second.reset(); // a handler that has not run yet in this emission
  });
  second = e.subscribe([&](int v) { calls.push_back(v * 10 + 2); });
  third = e.subscribe([&](int v) {
    calls.push_back(v * 10 + 3);
    third.reset(); // itself
  });

  e.emit(1);
  e.emit(2);

  CHECK(calls == std::vector{11, 13, 21});
}

TEST_CASE("emissions from inside a handler keep their order", "[event]")
{
  event<int> e;
  std::vector<std::string> log;
  auto const a = e.subscribe([&](int v) {
    log.push_back("a" + std::to_string(v));
    if (v == 1) {
      e.emit(2); // queued: b has not seen 1 yet
      log.push_back("a emitted 2");
    }
  });
  auto const b = e.subscribe(
      [&](int v) { log.push_back("b" + std::to_string(v)); });

  e.emit(1);

  CHECK(log == std::vector<std::string>{"a1", "a emitted 2", "b1", "a2", "b2"});
}

TEST_CASE("a handler may destroy the event", "[event]")
{
  auto e = std::make_unique<event<int>>();
  std::vector<int> calls;
  auto const first = e->subscribe([&](int v) {
    calls.push_back(v);
    e.reset();
  });
  auto const second = e->subscribe([&](int v) { calls.push_back(v + 100); });

  e->emit(1);

  CHECK(e == nullptr);
  CHECK(calls == std::vector{1, 101}); // the emission still went out whole
}

TEST_CASE("a subscription may outlive its event", "[event]")
{
  subscription outliving;
  {
    event<int> e;
    outliving = e.subscribe([](int) {});
  }
  outliving.reset(); // the event is gone: a no-op
}
