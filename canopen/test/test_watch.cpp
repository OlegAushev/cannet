#include <canopen/client.hpp>
#include <canopen/loopback.hpp>
#include <canopen/watch_snapshot.hpp>

#include <canopen/testing/emulated_device.hpp>
#include <canopen/testing/run.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::testing::emulated_device;
using cannet::canopen::testing::run_for;
using cannet::canopen::testing::run_until;
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

// The device's objects; one board has no Vdc.
void add_objects(emulated_device& device,
                 float vdc = 540.0f,
                 bool has_vdc = true)
{
  using enum od_value_type;
  device.add_object(
      uptime_key,
      {.type = float32, .access = od_access::ro, .value = to_raw(12.5f)});
  if (has_vdc) {
    device.add_object(
        vdc_key,
        {.type = float32, .access = od_access::ro, .value = to_raw(vdc)});
  }
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
  explicit fixture(bool has_vdc = true)
  {
    add_objects(device, 540.0f, has_vdc);
  }

  void run(std::chrono::milliseconds duration = 20ms)
  {
    run_for(io, duration);
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

// The objects some reading has a value of.
std::set<od_key> answered(std::vector<object_reading> const& readings)
{
  std::set<od_key> keys;
  for (auto const& r : readings) {
    if (r.value) {
      keys.insert(r.entry->key);
    }
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
  REQUIRE(f.run_until([&f] { return f.readings.size() == 3; }));
  f.run(); // the next pass is a second away

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
  REQUIRE(f.run_until([&f] { return f.device.sdo_requests().size() >= 6; }));

  // A request already on its way still arrives; no other goes out.
  auto const at_stop = f.device.sdo_requests().size();
  f.node->watch.set_period(0ms);
  f.run(60ms);
  CHECK(f.device.sdo_requests().size() <= at_stop + 1);
}

TEST_CASE("passes start a period apart, and a late one at once", "[watch]")
{
  SECTION("a pass shorter than the period")
  {
    fixture f;
    auto const began = clock_type::now();
    f.node->watch.set_period(30ms);
    REQUIRE(f.run_until([&f] {
      return std::ranges::count(keys_of(f.readings), uptime_key) == 5;
    }));
    // Each pass reads uptime first; five passes span four periods at least.
    CHECK(clock_type::now() - began >= 4 * 30ms);
  }

  SECTION("a pass longer than the period")
  {
    fixture f;
    f.device.answer_delay = 20ms; // a pass takes 60 ms
    f.node->watch.set_period(30ms);
    REQUIRE(f.run_until([&f] { return f.readings.size() >= 15; }));

    // From the last reading of a pass to the first of the next lies one
    // answer, and no wait. The median shrugs off a stall of the machine.
    std::vector<clock_type::duration> gaps;
    for (std::size_t i = 1; i < f.readings.size(); ++i) {
      if (f.readings[i].entry->key == uptime_key) {
        gaps.push_back(f.readings[i].time - f.readings[i - 1].time);
      }
    }
    REQUIRE(gaps.size() >= 4);
    std::ranges::sort(gaps);
    CHECK(gaps[gaps.size() / 2] < 35ms);
  }
}

TEST_CASE("disabling the watch stops it at once and drops the read in flight",
          "[watch]")
{
  fixture f;
  f.device.answer_delay = 30ms;
  f.node->watch.set_period(10ms);
  REQUIRE(f.run_until([&f] { return f.device.sdo_requests().size() == 1; }));

  // Its answer on the way.
  f.node->watch.disable();
  CHECK_FALSE(f.node->watch.enabled());
  f.run(100ms);
  CHECK(f.readings.empty());
  CHECK(f.device.sdo_requests().size() == 1);

  f.device.answer_delay = 0ms;
  f.node->watch.enable(); // a pass at once
  REQUIRE(f.run_until([&f] { return f.readings.size() == 3; }));
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
  REQUIRE(f.run_until([&f] { return f.readings.size() == 2; }));
  f.run();
  CHECK(f.device.sdo_requests() == std::vector<od_key>{uptime_key, state_key});

  REQUIRE(watch.enable(vdc_key));
  watch.set_period(10ms); // the next pass is due at once
  REQUIRE(f.run_until([&f] { return f.readings.size() == 5; }));
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
  REQUIRE(f.run_until([&f] { return f.readings.size() >= 3; }));
  CHECK(answered(f.readings).size() == 3);

  silent = true;
  f.readings.clear();
  auto const before = f.device.sdo_requests().size();
  auto const began = clock_type::now();
  // Another request for the node, made while the watch's time out.
  std::optional<sdo_result<od_value>> other;
  f.node->sdo.async_read(speed_key,
                         od_value_type::uint16,
                         [&other](sdo_result<od_value> r) { other = r; });
  REQUIRE(f.run_until(
      [&f, before] { return f.device.sdo_requests().size() >= before + 7; }));
  auto const elapsed = clock_type::now() - began;

  // One request per timeout, never a flood.
  auto const& requests = f.device.sdo_requests();
  CHECK(requests.size() - before
        <= static_cast<std::size_t>(elapsed / 30ms) + 2);
  // The other request waited behind one read of the watch at most.
  auto const since = std::next(requests.begin(),
                               static_cast<std::ptrdiff_t>(before));
  auto const at = std::find(since, requests.end(), speed_key);
  REQUIRE(at != requests.end());
  CHECK(at - since <= 1);
  REQUIRE(other);
  CHECK(other->error().reason == sdo_error::kind::timeout);
  // Every timeout is reported, and the passes go on through every object.
  REQUIRE(f.readings.size() >= 3);
  CHECK(std::ranges::all_of(f.readings, [](object_reading const& r) {
    return !r.value && r.value.error().reason == sdo_error::kind::timeout;
  }));
  auto const keys = keys_of(f.readings);
  CHECK(std::set<od_key>(keys.begin(), keys.end()).size() == 3);

  silent = false;
  f.readings.clear();
  CHECK(f.run_until([&f] { return answered(f.readings).size() == 3; }));
}

TEST_CASE("a request the transport cannot send ends the pass", "[watch]")
{
  fixture f;
  f.host_bus.fail_next_send(transport_error::tx_queue_full);
  f.node->watch.set_period(300ms);
  REQUIRE(f.run_until([&f] { return f.readings.size() == 1; }));
  CHECK(f.readings[0].entry->key == uptime_key);
  CHECK(f.readings[0].value.error()
        == sdo_error{.reason = sdo_error::kind::transport,
                     .send_error = transport_error::tx_queue_full});
  f.run(30ms); // the next pass is due 300 ms after this one began
  CHECK(f.readings.size() == 1);
  CHECK(f.device.sdo_requests().empty());

  REQUIRE(f.run_until([&f] { return f.readings.size() == 4; }));
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
  REQUIRE(f.run_until([&f] { return f.device.sdo_requests().size() == 1; }));
  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  REQUIRE(f.run_until([&f] { return f.readings.size() == 2; }));
  f.run(40ms); // the old node's late answer

  CHECK(f.device.sdo_requests() == std::vector<od_key>{uptime_key});
  CHECK(other.sdo_requests() == std::vector<od_key>{vdc_key, state_key});
  REQUIRE(keys_of(f.readings) == std::vector<od_key>{vdc_key, state_key});
  CHECK(f.readings[0].value == od_value{230.0f});
}

TEST_CASE("an object the device lacks is polled no more, until enabled again",
          "[watch]")
{
  fixture f{false};
  auto& watch = f.node->watch;
  watch.set_period(10ms);
  // A pass of three readings, then passes of two.
  REQUIRE(f.run_until([&f] { return f.readings.size() == 7; }));
  CHECK(std::ranges::count(f.device.sdo_requests(), vdc_key) == 1);
  auto const lacking = std::ranges::find(
      f.readings,
      vdc_key,
      [](object_reading const& r) { return r.entry->key; });
  REQUIRE(lacking != f.readings.end());
  CHECK(lacking->value.error()
        == sdo_error{.reason = sdo_error::kind::aborted,
                     .abort = sdo_abort_code::object_not_found});
  CHECK(watch.missing(vdc_key));
  CHECK(watch.enabled(vdc_key)); // still the caller's choice
  CHECK_FALSE(watch.missing(uptime_key));

  // A firmware update may have added it.
  REQUIRE(watch.enable(vdc_key));
  CHECK_FALSE(watch.missing(vdc_key));
  REQUIRE(f.run_until([&f] {
    return std::ranges::count(f.device.sdo_requests(), vdc_key) == 2;
  }));
  REQUIRE(f.run_until([&watch] { return watch.missing(vdc_key); }));
}

TEST_CASE("an object refused for another reason is still polled", "[watch]")
{
  fixture f;
  // The device has it write-only, as the dictionary does not.
  f.device.add_object(
      vdc_key,
      {.type = od_value_type::float32, .access = od_access::wo});
  f.node->watch.set_period(10ms);
  REQUIRE(f.run_until([&f] {
    return std::ranges::count(f.device.sdo_requests(), vdc_key) == 3;
  }));
  CHECK_FALSE(f.node->watch.missing(vdc_key));
}

TEST_CASE("a node id change forgets what the last device lacked", "[watch]")
{
  fixture f{false};
  loopback_transport other_bus{f.bus};
  emulated_device other{other_bus, node_id::literal(2), 0ms};
  add_objects(other, 230.0f);

  f.node->watch.set_period(10ms);
  REQUIRE(f.run_until([&f] { return f.node->watch.missing(vdc_key); }));
  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  CHECK_FALSE(f.node->watch.missing(vdc_key));
  REQUIRE(f.run_until([&other] {
    return std::ranges::contains(other.sdo_requests(), vdc_key);
  }));
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
  REQUIRE(run_until(io, [&readings] { return readings > 0; }));

  auto const at_reset = device.sdo_requests().size();
  auto const reported = readings;
  host.reset();
  node->watch.set_period(5ms);
  node->watch.enable();
  run_for(io, 50ms);
  // A request already on its way may still arrive; nothing else goes out,
  // and nothing more is reported.
  CHECK(device.sdo_requests().size() <= at_reset + 1);
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
    REQUIRE(f.run_until([&f] { return !f.readings.empty(); }));
    f.run(30ms);
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
    REQUIRE(run_until(io, [&readings] { return readings > 0; }));
    run_for(io, 30ms);
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

TEST_CASE("a watch snapshot keeps the last reading of every object", "[watch]")
{
  fixture f;
  watch_snapshot snap{f.node->watch};
  {
    auto const table = snap.read();
    REQUIRE(table.size() == 3);
    CHECK(table[0].entry->key == uptime_key);
    CHECK(table[2].entry->key == state_key);
    CHECK_FALSE(table[0].value);
    CHECK_FALSE(table[0].error);
  }

  f.node->watch.set_period(1s);
  REQUIRE(f.run_until([&f] { return f.readings.size() == 3; }));
  clock_type::time_point read_at;
  {
    auto const table = snap.read();
    REQUIRE(table.size() == 3);
    CHECK(table[1].value == od_value{540.0f});
    CHECK(table[2].value == od_value{std::uint16_t{3}});
    CHECK_FALSE(table[1].error);
    read_at = table[0].time;
    CHECK(read_at > clock_type::time_point{});
  }

  // The device falls silent: the errors come, the values read stay.
  f.device.lose_answer = [](std::size_t) { return true; };
  f.node->watch.set_period(10ms); // the next pass is due at once
  REQUIRE(f.run_until([&snap] { return snap.read()[0].error.has_value(); }));
  auto const table = snap.read();
  CHECK(table[0].value == od_value{12.5f});
  CHECK(table[0].error->reason == sdo_error::kind::timeout);
  CHECK(table[0].time > read_at);
}

TEST_CASE("a GUI thread reads the watch snapshot while the client runs",
          "[watch]")
{
  fixture f;
  watch_snapshot snap{f.node->watch};
  f.node->watch.set_period(5ms);
  auto work = boost::asio::make_work_guard(f.io);
  f.io.restart();
  std::jthread client_thread{[&f] { f.io.run(); }};

  // A frame every 2 ms until every object has a value.
  bool complete = false;
  for (int frame = 0; frame < 1000 && !complete; ++frame) {
    auto const table = snap.read();
    complete = std::ranges::all_of(table, [](watched_value const& v) {
      return v.value.has_value();
    });
    std::this_thread::sleep_for(2ms);
  }

  // The snapshot takes events on the client's thread: stop it first.
  work.reset();
  f.io.stop();
  client_thread.join();
  CHECK(complete);
}
