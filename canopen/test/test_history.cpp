#include <canopen/client.hpp>
#include <canopen/history.hpp>
#include <canopen/loopback.hpp>

#include <canopen/testing/emulated_device.hpp>
#include <canopen/testing/run.hpp>

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::testing::emulated_device;
using cannet::canopen::testing::run_for;
using cannet::canopen::testing::run_until;

namespace {

constexpr auto host_id = node_id::literal(127);
constexpr auto device_id = node_id::literal(1);

constexpr od_key uptime_key{0x5000, 0x01}; // float32
constexpr od_key state_key{0x5000, 0x02};  // uint16

constexpr auto drive_dictionary = dictionary{
    {.watch_category = "watch", .config_category = ""},
    {{uptime_key,
      {"watch", "sys", "uptime", "s", od_access::ro, od_value_type::float32}},
     {state_key,
      {"watch", "drive", "state", "", od_access::ro, od_value_type::uint16}}}};

std::vector<double> values_of(sample_view samples)
{
  std::vector<double> values;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    values.push_back(samples[i].value);
  }
  return values;
}

std::size_t count(history const& h, od_key key)
{
  return h.read().find("drive", key).size();
}

// A client with one node, "drive", the emulated device behind it on the
// loopback bus, and a history.
struct fixture {
  fixture()
  {
    using enum od_value_type;
    device.add_object(
        uptime_key,
        {.type = float32, .access = od_access::ro, .value = to_raw(12.5f)});
    device.add_object(state_key,
                      {.type = uint16,
                       .access = od_access::ro,
                       .value = to_raw(std::uint16_t{3})});
  }

  template<typename Done>
  bool run_until(Done done)
  {
    return testing::run_until(io, done);
  }

  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  emulated_device device{device_bus, device_id, 0ms};
  client host{host_bus, {.id = host_id, .heartbeat_period = 0ms}};
  std::shared_ptr<remote_node> node = *host.add_node(
      {.name = "drive",
       .id = device_id,
       .dictionary = drive_dictionary,
       .sdo_timeout = 30ms});
  history signals;
};

} // namespace

TEST_CASE("a history keeps each signal's samples, oldest first", "[history]")
{
  history h;
  auto const t0 = h.origin();
  h.push("drive", uptime_key, 1.0, t0 + 1s);
  h.push("drive", uptime_key, 2.0, t0 + 1500ms);
  h.push("drive", state_key, 3.0, t0 + 2s);
  h.push("pump", uptime_key, 7.0, t0 + 3s);

  auto const r = h.read();
  auto const uptime = r.find("drive", uptime_key);
  REQUIRE(uptime.size() == 2);
  CHECK(uptime[0].t == 1.0);
  CHECK(uptime[0].value == 1.0);
  CHECK(uptime[1].t == 1.5);
  CHECK(uptime[1].value == 2.0);
  CHECK(r.find("drive", state_key).size() == 1);
  REQUIRE(r.find("pump", uptime_key).size() == 1);
  CHECK(r.find("pump", uptime_key)[0].value == 7.0);
  CHECK(r.find("pump", state_key).empty());
  CHECK(r.find("nobody", uptime_key).empty());
}

TEST_CASE("a full signal drops its oldest samples, and shrinking keeps the "
          "newest",
          "[history]")
{
  history h{{.capacity = 4}};
  CHECK(h.capacity() == 4);
  for (int i = 0; i < 6; ++i) {
    h.push("drive", uptime_key, static_cast<double>(i), h.origin() + i * 1s);
  }
  {
    auto const r = h.read();
    auto const samples = r.find("drive", uptime_key);
    CHECK(values_of(samples) == std::vector<double>{2, 3, 4, 5});
    CHECK(samples.first().size() + samples.second().size() == 4);
    CHECK(samples[0].t == 2.0);
  }

  h.set_capacity(2);
  CHECK(h.capacity() == 2);
  CHECK(values_of(h.read().find("drive", uptime_key))
        == std::vector<double>{4, 5});

  h.set_capacity(10);
  h.push("drive", uptime_key, 6.0);
  CHECK(values_of(h.read().find("drive", uptime_key))
        == std::vector<double>{4, 5, 6});
}

TEST_CASE("clearing a history forgets every sample", "[history]")
{
  history h;
  h.push("drive", uptime_key, 1.0);
  h.push("drive", state_key, 2.0);
  h.clear();
  CHECK(h.read().find("drive", uptime_key).empty());
  CHECK(h.read().find("drive", state_key).empty());
  h.push("drive", uptime_key, 3.0);
  CHECK(values_of(h.read().find("drive", uptime_key))
        == std::vector<double>{3});
}

TEST_CASE("an attached node's watch readings are recorded, a failed one as "
          "NaN",
          "[history]")
{
  fixture f;
  f.signals.attach(*f.node);
  f.node->watch.set_period(10ms);
  REQUIRE(f.run_until([&f] { return count(f.signals, state_key) >= 2; }));
  {
    auto const r = f.signals.read();
    auto const uptime = r.find("drive", uptime_key);
    REQUIRE(uptime.size() >= 2);
    CHECK(uptime[0].value == 12.5);
    CHECK(uptime[0].t > 0.0);
    CHECK(uptime[1].t > uptime[0].t);
    CHECK(r.find("drive", state_key)[0].value == 3.0);
  }

  // The device falls silent: the plot gets a gap.
  f.device.lose_answer = [](std::size_t) { return true; };
  REQUIRE(f.run_until([&f] {
    auto const r = f.signals.read();
    auto const uptime = r.find("drive", uptime_key);
    return std::isnan(uptime[uptime.size() - 1].value);
  }));

  // Detached, nothing more is recorded; what was stays.
  f.signals.detach(*f.node);
  f.device.lose_answer = nullptr;
  auto const kept = count(f.signals, uptime_key);
  run_for(f.io, 60ms);
  CHECK(count(f.signals, uptime_key) == kept);
}

TEST_CASE("attaching a node again records each reading once", "[history]")
{
  fixture f;
  f.signals.attach(*f.node);
  f.signals.attach(*f.node);
  f.node->watch.set_period(1s); // one pass
  REQUIRE(f.run_until([&f] { return count(f.signals, state_key) == 1; }));
  run_for(f.io, 20ms);
  CHECK(count(f.signals, uptime_key) == 1);
  CHECK(count(f.signals, state_key) == 1);
}

TEST_CASE("a history records on one thread while another reads", "[history]")
{
  constexpr int pushes = 100'000;
  history h{{.capacity = 1000}};
  std::jthread writer{[&h] {
    for (int i = 0; i < pushes; ++i) {
      h.push("drive", uptime_key, static_cast<double>(i));
    }
  }};

  // A view at every frame, as a GUI reads: whole, as many samples as the
  // ring holds at most, and in the order they came.
  bool consistent = true;
  double last = -1;
  auto const deadline = std::chrono::steady_clock::now() + 20s;
  while (last < pushes - 1 && std::chrono::steady_clock::now() < deadline) {
    {
      auto const r = h.read();
      auto const samples = r.find("drive", uptime_key);
      consistent = consistent && samples.size() <= 1000;
      for (std::size_t i = 1; i < samples.size(); ++i) {
        consistent = consistent && samples[i].value == samples[i - 1].value + 1;
      }
      if (!samples.empty()) {
        last = samples[samples.size() - 1].value;
      }
    }
    std::this_thread::sleep_for(200us);
  }
  CHECK(consistent);
  CHECK(last == pushes - 1);
}
