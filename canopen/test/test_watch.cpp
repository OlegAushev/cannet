#include <canopen/client.hpp>
#include <canopen/loopback.hpp>

#include "emulated_device.hpp"
#include "support.hpp"

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::test::emulated_device;
using cannet::canopen::test::run_for;
using clock_type = std::chrono::steady_clock;

namespace {

constexpr auto host_id = node_id::literal(127);
constexpr auto device_id = node_id::literal(1);

constexpr od_key uptime_key{0x5000, 0x01}; // float32
constexpr od_key vdc_key{0x5000, 0x02};    // float32
constexpr od_key state_key{0x5000, 0x03};  // uint16
constexpr od_key label_key{0x5000, 0x04};  // a string: not polled
constexpr od_key poke_key{0x5000, 0x05};   // write-only: not polled
constexpr od_key speed_key{0x3000, 0x01};  // a parameter, not a watch object

// Declared out of key order, as a firmware's table may be.
constexpr auto drive_dictionary = dictionary{
    {.watch_category = "watch", .config_category = "config"},
    {{state_key,
      {"watch", "drive", "state", "", od_access::ro, od_value_type::uint16}},
     {vdc_key,
      {"watch", "elec", "Vdc", "V", od_access::ro, od_value_type::float32}},
     {uptime_key,
      {"watch", "sys", "uptime", "s", od_access::ro, od_value_type::float32}},
     {label_key,
      {"watch", "sys", "label", "", od_access::ro, od_value_type::string}},
     {poke_key,
      {"watch", "sys", "poke", "", od_access::wo, od_value_type::uint8}},
     {speed_key,
      {"config",
       "drive",
       "speed",
       "rpm",
       od_access::rw,
       od_value_type::uint16}}}};

void add_objects(emulated_device& device, float vdc = 540.0f)
{
  using enum od_value_type;
  device.add_object(
      uptime_key,
      {.type = float32, .access = od_access::ro, .value = to_raw(12.5f)});
  device.add_object(
      vdc_key,
      {.type = float32, .access = od_access::ro, .value = to_raw(vdc)});
  device.add_object(state_key,
                    {.type = uint16,
                     .access = od_access::ro,
                     .value = to_raw(std::uint16_t{3})});
  device.add_object(label_key,
                    {.type = string, .access = od_access::ro, .text = "x"});
  device.add_object(poke_key, {.type = uint8, .access = od_access::wo});
  device.add_object(speed_key,
                    {.type = uint16,
                     .access = od_access::rw,
                     .value = to_raw(std::uint16_t{1500})});
}

// A client with one node, "drive" at node id 1 with the dictionary above,
// the emulated device behind it on the loopback bus, and every reading of
// the node's watch.
struct fixture {
  fixture()
  {
    add_objects(device);
  }

  void run(std::chrono::milliseconds duration = 20ms)
  {
    run_for(io, duration);
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
  std::vector<object_reading> readings;
  subscription values = node->watch.on_value(
      [this](object_reading const& r) { readings.push_back(r); });
};

std::vector<od_key> keys_of(std::vector<object_reading> const& readings)
{
  std::vector<od_key> keys;
  for (auto const& r : readings) {
    keys.push_back(r.entry->key);
  }
  return keys;
}

} // namespace

TEST_CASE("the watch polls the readable scalars of the watch category",
          "[watch]")
{
  fixture f;
  auto const objects = f.node->watch.objects();
  REQUIRE(objects.size() == 3);
  CHECK(objects[0]->key == uptime_key);
  CHECK(objects[1]->key == vdc_key);
  CHECK(objects[2]->key == state_key);

  f.node->watch.set_period(1s);
  f.run();

  // One pass, in key order.
  CHECK(f.device.sdo_requests()
        == std::vector<od_key>{uptime_key, vdc_key, state_key});
  REQUIRE(f.readings.size() == 3);
  CHECK(f.readings[0].entry == objects[0]);
  CHECK(f.readings[0].value == od_value{12.5f});
  CHECK(f.readings[1].value == od_value{540.0f});
  CHECK(f.readings[2].value == od_value{std::uint16_t{3}});
  CHECK(f.readings[0].time > clock_type::time_point{});
  CHECK(f.readings[0].time <= f.readings[2].time);
}

TEST_CASE("nothing is polled before a period is set, nor after zero", "[watch]")
{
  fixture f;
  CHECK(f.node->watch.period() == 0ms);
  CHECK(f.node->watch.enabled());
  f.run(40ms);
  CHECK(f.device.sdo_requests().empty());

  f.node->watch.set_period(10ms);
  CHECK(f.node->watch.period() == 10ms);
  f.run(35ms);
  CHECK(f.device.sdo_requests().size() >= 6);

  f.node->watch.set_period(0ms);
  f.run(10ms); // a request already on its way still arrives
  auto const settled = f.device.sdo_requests().size();
  f.run(50ms);
  CHECK(f.device.sdo_requests().size() == settled);
}

TEST_CASE("passes start a period apart, and a late one at once", "[watch]")
{
  SECTION("a pass shorter than the period")
  {
    fixture f;
    f.node->watch.set_period(30ms);
    f.run(200ms);

    // The first reading of each pass marks its start.
    std::vector<clock_type::time_point> starts;
    for (auto const& r : f.readings) {
      if (r.entry->key == uptime_key) {
        starts.push_back(r.time);
      }
    }
    REQUIRE(starts.size() >= 4);
    for (std::size_t i = 1; i < starts.size(); ++i) {
      CHECK(starts[i] - starts[i - 1] >= 25ms); // no burst
    }
  }

  SECTION("a pass longer than the period")
  {
    fixture f;
    f.device.answer_delay = 20ms; // a pass takes 60 ms
    f.node->watch.set_period(30ms);
    f.run(250ms);

    // Back to back: no wait between the last reading of a pass and the
    // first of the next.
    REQUIRE(f.readings.size() >= 6);
    for (std::size_t i = 1; i < f.readings.size(); ++i) {
      CHECK(f.readings[i].time - f.readings[i - 1].time < 35ms);
    }
  }
}

TEST_CASE("disabling the watch stops it at once and drops the read in flight",
          "[watch]")
{
  fixture f;
  f.device.answer_delay = 30ms;
  f.node->watch.set_period(10ms);
  f.run(10ms);
  REQUIRE(f.device.sdo_requests().size() == 1); // its answer on the way

  f.node->watch.disable();
  CHECK_FALSE(f.node->watch.enabled());
  f.run(100ms);
  CHECK(f.readings.empty()); // the answer came, and was dropped
  CHECK(f.device.sdo_requests().size() == 1);

  f.device.answer_delay = 0ms;
  f.node->watch.enable(); // a pass at once
  f.run(5ms);
  CHECK(f.device.sdo_requests().size() == 4);
  CHECK(keys_of(f.readings)
        == std::vector<od_key>{uptime_key, vdc_key, state_key});
}

TEST_CASE("single objects can be left out of the watch", "[watch]")
{
  fixture f;
  auto& watch = f.node->watch;
  CHECK(watch.enabled(vdc_key));
  REQUIRE(watch.disable(vdc_key));
  CHECK_FALSE(watch.enabled(vdc_key));
  CHECK(watch.disable(speed_key).error() == setup_error::no_such_object);
  CHECK(watch.enable(label_key).error() == setup_error::no_such_object);
  CHECK(watch.enable(poke_key).error() == setup_error::no_such_object);
  CHECK_FALSE(watch.enabled(speed_key));

  watch.set_period(1s);
  f.run();
  CHECK(f.device.sdo_requests() == std::vector<od_key>{uptime_key, state_key});

  REQUIRE(watch.enable(vdc_key));
  watch.set_period(10ms); // the next pass is due at once
  f.run(5ms);
  CHECK(keys_of(f.readings)
        == std::vector<od_key>{uptime_key,
                               state_key,
                               uptime_key,
                               vdc_key,
                               state_key});
}

TEST_CASE("the watch keeps one request in flight while the device is silent, "
          "and recovers when it answers again",
          "[watch]")
{
  fixture f; // SDO timeout 30 ms
  bool silent = false;
  f.device.lose_answer = [&silent](std::size_t) { return silent; };
  f.node->watch.set_period(10ms);
  f.run(50ms);
  REQUIRE(f.readings.size() >= 3);
  CHECK(std::ranges::all_of(f.readings, [](object_reading const& r) {
    return r.value.has_value();
  }));

  silent = true;
  f.readings.clear();
  auto const before = f.device.sdo_requests().size();
  // Another request for the node, made while the watch's time out.
  std::optional<sdo_result<od_value>> other;
  clock_type::time_point other_done;
  auto const asked = clock_type::now();
  f.node->sdo.async_read(speed_key,
                         od_value_type::uint16,
                         [&](sdo_result<od_value> r) {
                           other = r;
                           other_done = clock_type::now();
                         });
  f.run(300ms);

  // One request per timeout, never a flood.
  auto const sent = f.device.sdo_requests().size() - before;
  CHECK(sent >= 6);
  CHECK(sent <= 300 / 30 + 2);
  // Every timeout is reported, and the passes go on through every object.
  REQUIRE_FALSE(f.readings.empty());
  CHECK(std::ranges::all_of(f.readings, [](object_reading const& r) {
    return !r.value && r.value.error().reason == sdo_error::kind::timeout;
  }));
  auto const keys = keys_of(f.readings);
  CHECK(std::set<od_key>(keys.begin(), keys.end()).size() == 3);
  // The other request waited behind one read of the watch, not a pass.
  REQUIRE(other);
  CHECK(other->error().reason == sdo_error::kind::timeout);
  CHECK(other_done - asked < 90ms);

  silent = false;
  f.readings.clear();
  f.run(100ms);
  auto const answered = keys_of(f.readings);
  CHECK(std::set<od_key>(answered.begin(), answered.end()).size() == 3);
  CHECK(f.readings.back().value.has_value());
}

TEST_CASE("a request the transport cannot send ends the pass", "[watch]")
{
  fixture f;
  f.host_bus.fail_next_send(transport_error::tx_queue_full);
  f.node->watch.set_period(50ms);
  f.run(20ms);
  REQUIRE(f.readings.size() == 1);
  CHECK(f.readings[0].entry->key == uptime_key);
  CHECK(f.readings[0].value.error()
        == sdo_error{.reason = sdo_error::kind::transport,
                     .send_error = transport_error::tx_queue_full});
  CHECK(f.device.sdo_requests().empty());

  f.run(50ms); // the next pass
  CHECK(f.device.sdo_requests()
        == std::vector<od_key>{uptime_key, vdc_key, state_key});
}

TEST_CASE("a node id change drops the read in flight, and the watch goes on "
          "at the new id",
          "[watch]")
{
  fixture f;
  loopback_transport other_bus{f.bus};
  emulated_device other{other_bus, node_id::literal(2), 0ms};
  add_objects(other, 230.0f);

  f.device.answer_delay = 20ms;
  f.node->watch.set_period(1s);
  f.run(5ms);
  REQUIRE(f.device.sdo_requests().size() == 1);
  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  f.run(50ms);

  CHECK(f.device.sdo_requests() == std::vector<od_key>{uptime_key});
  CHECK(other.sdo_requests() == std::vector<od_key>{vdc_key, state_key});
  REQUIRE(keys_of(f.readings) == std::vector<od_key>{vdc_key, state_key});
  CHECK(f.readings[0].value == od_value{230.0f});
}

TEST_CASE("the watch stops for good when its client goes", "[watch]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  emulated_device device{device_bus, device_id, 0ms};
  add_objects(device);
  auto host = std::make_unique<client>(
      host_bus,
      client_options{.id = host_id, .heartbeat_period = 0ms});
  auto const node = *host->add_node(
      {.name = "drive", .id = device_id, .dictionary = drive_dictionary});
  std::size_t readings = 0;
  auto const sub = node->watch.on_value(
      [&readings](object_reading const&) { ++readings; });
  node->watch.set_period(10ms);
  run_for(io, 30ms);
  REQUIRE(readings > 0);

  host.reset();
  run_for(io, 10ms);
  auto const settled = device.sdo_requests().size();
  auto const reported = readings;
  node->watch.set_period(5ms);
  node->watch.enable();
  run_for(io, 50ms);
  CHECK(device.sdo_requests().size() == settled);
  CHECK(readings == reported);
}

TEST_CASE("a watch handler may stop the watch or drop the node", "[watch]")
{
  SECTION("stop the watch")
  {
    fixture f;
    auto const stop = f.node->watch.on_value(
        [&f](object_reading const&) { f.node->watch.disable(); });
    f.node->watch.set_period(10ms);
    f.run(50ms);
    CHECK(f.readings.size() == 1);
    CHECK(f.device.sdo_requests().size() == 1);
  }

  SECTION("drop the node and its client")
  {
    boost::asio::io_context io;
    loopback_bus bus{io.get_executor()};
    loopback_transport host_bus{bus};
    loopback_transport device_bus{bus};
    emulated_device device{device_bus, device_id, 0ms};
    add_objects(device);
    auto host = std::make_unique<client>(
        host_bus,
        client_options{.id = host_id, .heartbeat_period = 0ms});
    auto node = *host->add_node(
        {.name = "drive", .id = device_id, .dictionary = drive_dictionary});
    int readings = 0;
    auto const sub = node->watch.on_value([&](object_reading const&) {
      ++readings;
      host.reset();
      node.reset();
    });
    node->watch.set_period(10ms);
    run_for(io, 50ms);
    CHECK(readings == 1);
    CHECK_FALSE(node);
    CHECK(device.sdo_requests().size() == 1);
  }
}

TEST_CASE("a node without a watch category polls nothing", "[watch]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  emulated_device device{device_bus, device_id, 0ms};
  add_objects(device);
  client host{host_bus, {.id = host_id, .heartbeat_period = 0ms}};
  auto const node = *host.add_node({.name = "drive", .id = device_id});
  CHECK(node->watch.objects().empty());
  node->watch.set_period(10ms);
  run_for(io, 30ms);
  CHECK(device.sdo_requests().empty());
}
