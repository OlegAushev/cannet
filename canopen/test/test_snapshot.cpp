#include <canopen/snapshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <thread>
#include <vector>

using cannet::canopen::snapshot;

TEST_CASE("a snapshot reads the newest value published", "[snapshot]")
{
  snapshot<std::vector<int>> s{{1, 2, 3}};
  CHECK(s.read() == std::vector<int>{1, 2, 3});

  s.publish({4});
  CHECK(s.read() == std::vector<int>{4});
  CHECK(s.read() == std::vector<int>{4}); // nothing newer: the same value

  s.publish({5});
  s.publish({6});
  s.publish({7}); // the reader missed two
  CHECK(s.read() == std::vector<int>{7});
}

TEST_CASE("a value read stays unchanged while the writer goes on", "[snapshot]")
{
  snapshot<int> s{0};
  s.publish(1);
  auto const& held = s.read();
  for (int i = 2; i < 10; ++i) {
    s.publish(i);
  }
  CHECK(held == 1);
  CHECK(s.read() == 9);
}

TEST_CASE("a reader on another thread sees whole values, never older ones",
          "[snapshot]")
{
  // Every element carries the version: a torn read would mix two.
  struct versioned {
    std::array<std::uint64_t, 64> v{};
  };
  constexpr std::uint64_t last = 200'000;
  snapshot<versioned> s;
  std::jthread writer{[&s] {
    versioned next;
    for (std::uint64_t k = 1; k <= last; ++k) {
      next.v.fill(k);
      s.publish(next);
    }
  }};

  std::uint64_t seen = 0;
  bool whole = true;
  bool ordered = true;
  while (seen < last) {
    auto const& value = s.read();
    auto const k = value.v[0];
    whole = whole && std::ranges::all_of(value.v, [k](std::uint64_t x) {
              return x == k;
            });
    ordered = ordered && k >= seen;
    seen = k;
  }
  CHECK(whole);
  CHECK(ordered);
}
