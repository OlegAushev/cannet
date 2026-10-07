#include <canopen/client.hpp>
#include <canopen/loopback.hpp>

#include <canopen/testing/bus_log.hpp>
#include <canopen/testing/emulated_device.hpp>
#include <canopen/testing/run.hpp>

#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::testing::bus_log;
using cannet::canopen::testing::emulated_device;
using cannet::canopen::testing::run_for;

namespace {

constexpr auto host_id = node_id::literal(127);
constexpr auto device_id = node_id::literal(1);

// A client with one node, "drive" at node id 1, and a device-side endpoint
// that sends whatever a test hands it.
struct fixture {
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  bus_log log{bus};
  client host{host_bus, {.id = host_id, .heartbeat_period = 0ms}};
  std::shared_ptr<remote_node> node = *host.add_node(
      {.name = "drive", .id = device_id, .heartbeat_timeout = 100ms});

  void from_device(can_frame const& frame)
  {
    device_bus.send(frame, [](auto const&) {});
  }

  void run(std::chrono::milliseconds duration = 10ms)
  {
    run_for(io, duration);
  }
};

can_frame heartbeat(nmt_state state, node_id id = device_id)
{
  return make_heartbeat_frame(id, state);
}

can_frame pdo_frame(canid_t cob, std::uint8_t len, payload const& data = {})
{
  return make_frame(cob, len, data);
}

} // namespace

TEST_CASE("a node is alive while its heartbeats come", "[remote_node]")
{
  fixture f;
  std::vector<heartbeat_status> changes;
  auto const sub = f.node->heartbeat.on_change(
      [&](heartbeat_status const& s) { changes.push_back(s); });
  CHECK(f.node->heartbeat.status() == heartbeat_status{});

  f.from_device(heartbeat(nmt_state::operational));
  f.run();
  CHECK(f.node->heartbeat.status()
        == heartbeat_status{.alive = true, .state = nmt_state::operational});

  for (int i = 0; i < 3; ++i) { // the same state again: no event
    f.from_device(heartbeat(nmt_state::operational));
    f.run(20ms);
  }
  CHECK(changes.size() == 1);

  f.run(150ms); // silence beyond the timeout
  REQUIRE(changes.size() == 2);
  CHECK(changes[1]
        == heartbeat_status{.alive = false, .state = nmt_state::operational});

  f.from_device(heartbeat(nmt_state::operational));
  f.run();
  REQUIRE(changes.size() == 3);
  CHECK(changes[2].alive);
}

TEST_CASE("every state change is an event, and so is every boot-up",
          "[remote_node]")
{
  fixture f;
  std::vector<heartbeat_status> changes;
  auto const sub = f.node->heartbeat.on_change(
      [&](heartbeat_status const& s) { changes.push_back(s); });

  f.from_device(heartbeat(nmt_state::pre_operational));
  f.from_device(heartbeat(nmt_state::pre_operational));
  f.from_device(heartbeat(nmt_state::operational));
  f.from_device(heartbeat(nmt_state::initializing)); // boot-up
  f.from_device(heartbeat(nmt_state::initializing)); // and again
  f.run();

  using enum nmt_state;
  CHECK(changes
        == std::vector<heartbeat_status>{
            {.alive = true, .state = pre_operational},
            {.alive = true, .state = operational},
            {.alive = true, .state = initializing},
            {.alive = true, .state = initializing}});
}

TEST_CASE("malformed heartbeats are ignored", "[remote_node]")
{
  fixture f;
  auto remote = heartbeat(nmt_state::operational);
  remote.can_id |= CAN_RTR_FLAG;
  auto extended = heartbeat(nmt_state::operational);
  extended.can_id |= CAN_EFF_FLAG;
  auto empty = heartbeat(nmt_state::operational);
  empty.len = 0;
  auto unknown = heartbeat(nmt_state::operational);
  unknown.data[0] = 0x42;

  for (auto const& frame : {remote, extended, empty, unknown}) {
    f.from_device(frame);
  }
  f.run();

  CHECK(f.node->heartbeat.status() == heartbeat_status{});
}

TEST_CASE("a node moved to a new id forgets the old one", "[remote_node]")
{
  fixture f;
  std::vector<heartbeat_status> changes;
  auto const sub = f.node->heartbeat.on_change(
      [&](heartbeat_status const& s) { changes.push_back(s); });
  f.from_device(heartbeat(nmt_state::operational));
  f.run();
  REQUIRE(f.node->heartbeat.status().alive);

  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  REQUIRE(changes.size() == 2); // at once, inside the call
  CHECK(changes[1] == heartbeat_status{});

  f.from_device(heartbeat(nmt_state::operational)); // the old id
  f.run();
  CHECK_FALSE(f.node->heartbeat.status().alive);

  f.from_device(heartbeat(nmt_state::pre_operational, node_id::literal(2)));
  f.run();
  CHECK(
      f.node->heartbeat.status()
      == heartbeat_status{.alive = true, .state = nmt_state::pre_operational});
}

TEST_CASE("every emergency reaches every subscriber", "[remote_node]")
{
  fixture f;
  std::vector<emcy_message> a;
  std::vector<emcy_message> b;
  auto const sub_a = f.node->emcy.on_emcy(
      [&](emcy_message const& m) { a.push_back(m); });
  auto const sub_b = f.node->emcy.on_emcy(
      [&](emcy_message const& m) { b.push_back(m); });

  emcy_message const rpdo_timeout{.error_code = 0x8250,
                                  .error_register = 0x10,
                                  .manufacturer = {}};
  auto too_short = make_emcy_frame(device_id, rpdo_timeout);
  too_short.len = 2;
  f.from_device(make_emcy_frame(device_id, rpdo_timeout));
  f.from_device(too_short);
  f.from_device(make_emcy_frame(node_id::literal(2), rpdo_timeout));
  f.run();

  CHECK(a == std::vector{rpdo_timeout});
  CHECK(b == std::vector{rpdo_timeout});
}

TEST_CASE("a TPDO reaches its handler, unless shorter than its mapping",
          "[remote_node]")
{
  fixture f;
  std::vector<payload> tpdo1;
  std::vector<payload> tpdo2;
  REQUIRE(f.node->tpdo.setup(1, {.handler = [&](payload const& p) {
                               tpdo1.push_back(p);
                             }}));
  REQUIRE(f.node->tpdo.setup(
      2,
      {.handler = [&](payload const& p) { tpdo2.push_back(p); }, .len = 2}));

  f.from_device(pdo_frame(0x181, 8, {1, 2, 3, 4, 5, 6, 7, 8}));
  f.from_device(pdo_frame(0x181, 4, {9, 9, 9, 9})); // dropped
  f.from_device(pdo_frame(0x281, 2, {7, 7, 7, 7})); // bytes past 2 unsent
  f.from_device(pdo_frame(0x381, 8));               // TPDO3: not set up
  f.run();

  CHECK(tpdo1 == std::vector{payload{1, 2, 3, 4, 5, 6, 7, 8}});
  CHECK(tpdo2 == std::vector{payload{7, 7, 0, 0, 0, 0, 0, 0}});
}

TEST_CASE("a TPDO times out once per loss and recovers silently",
          "[remote_node]")
{
  fixture f;
  int frames = 0;
  int timeouts = 0;
  REQUIRE(f.node->tpdo.setup(1,
                             {.handler = [&](payload const&) { ++frames; },
                              .timeout = 50ms,
                              .on_timeout = [&] { ++timeouts; }}));

  f.run(100ms);
  CHECK(timeouts == 1); // one that never came times out too, once

  f.from_device(pdo_frame(0x181, 8));
  f.run();
  CHECK(frames == 1);
  CHECK(timeouts == 1);

  f.run(100ms);
  CHECK(timeouts == 2);
}

TEST_CASE("a node moved to a new id times out the old one's TPDOs",
          "[remote_node]")
{
  fixture f;
  int frames = 0;
  int timeouts = 0;
  REQUIRE(f.node->tpdo.setup(1,
                             {.handler = [&](payload const&) { ++frames; },
                              .timeout = 1s,
                              .on_timeout = [&] { ++timeouts; }}));
  f.from_device(pdo_frame(0x181, 8));
  f.run();
  REQUIRE(frames == 1);

  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  CHECK(timeouts == 1); // at once, inside the call

  f.from_device(pdo_frame(0x181, 8)); // the old id
  f.from_device(pdo_frame(0x182, 8));
  f.run();
  CHECK(frames == 2);
  CHECK(timeouts == 1);
}

TEST_CASE("a TPDO may be set up anew from its own handler", "[remote_node]")
{
  fixture f;
  int first = 0;
  int second = 0;
  REQUIRE(
      f.node->tpdo.setup(1, {.handler = [&](payload const&) {
                           ++first;
                           auto const again = f.node->tpdo.setup(
                               1,
                               {.handler = [&](payload const&) { ++second; }});
                           CHECK(again);
                         }}));

  f.from_device(pdo_frame(0x181, 8));
  f.from_device(pdo_frame(0x181, 8));
  f.run();

  CHECK(first == 1);
  CHECK(second == 1);
}

TEST_CASE("PDO numbers outside 1..4 and lengths above 8 are refused",
          "[remote_node]")
{
  fixture f;
  CHECK(f.node->tpdo.setup(0, {}).error() == setup_error::invalid_pdo);
  CHECK(f.node->tpdo.setup(5, {}).error() == setup_error::invalid_pdo);
  CHECK(f.node->tpdo.setup(1, {.len = 9}).error() == setup_error::invalid_pdo);
  CHECK(f.node->rpdo.setup(5, {}).error() == setup_error::invalid_pdo);
  CHECK(f.node->rpdo.setup(1, {.len = 9}).error() == setup_error::invalid_pdo);
  CHECK(f.node->rpdo.enable(0).error() == setup_error::invalid_pdo);
  CHECK(f.node->rpdo.disable(5).error() == setup_error::invalid_pdo);
  CHECK_FALSE(f.node->rpdo.enabled(5));
}

TEST_CASE("RPDOs go out at their period while the client runs", "[remote_node]")
{
  fixture f;
  std::uint8_t counter = 0;
  REQUIRE(f.node->rpdo.setup(1,
                             {.provider =
                                  [&] {
                                    payload p{};
                                    p[7] = static_cast<std::uint8_t>(counter++
                                                                     & 0x3);
                                    return p;
                                  },
                              .period = 10ms}));

  f.run(30ms);
  CHECK(f.log.with_id(0x201).empty()); // the client has not started

  f.host.start();
  f.run(55ms); // the first at once: 6
  auto const sent = f.log.with_id(0x201);
  CHECK(sent.size() >= 3);
  CHECK(sent.size() <= 7);
  for (std::size_t i = 0; i < sent.size(); ++i) {
    CHECK(sent[i].len == 8);
    // The provider's counter, in turn.
    CHECK(sent[i].data[7] == static_cast<std::uint8_t>(i & 0x3));
  }

  f.host.stop();
  f.run(5ms);
  auto const total = f.log.with_id(0x201).size();
  f.run(30ms);
  CHECK(f.log.with_id(0x201).size() == total);
}

TEST_CASE("a disabled RPDO rests, provider included", "[remote_node]")
{
  fixture f;
  int calls1 = 0;
  int calls2 = 0;
  REQUIRE(f.node->rpdo.setup(1,
                             {.provider =
                                  [&] {
                                    ++calls1;
                                    return payload{};
                                  },
                              .period = 10ms}));
  REQUIRE(f.node->rpdo.setup(2,
                             {.provider =
                                  [&] {
                                    ++calls2;
                                    return payload{};
                                  },
                              .period = 10ms}));
  f.host.start();
  f.run(25ms);
  REQUIRE(calls1 > 0);
  REQUIRE(calls2 > 0);

  REQUIRE(f.node->rpdo.disable(1));
  CHECK_FALSE(f.node->rpdo.enabled(1));
  auto const rested1 = calls1;
  auto const before2 = calls2;
  f.run(30ms);
  CHECK(calls1 == rested1);
  CHECK(calls2 > before2);

  f.node->rpdo.disable(); // all of them
  CHECK_FALSE(f.node->rpdo.enabled());
  f.run(5ms);
  auto const rested2 = calls2;
  f.run(30ms);
  CHECK(calls2 == rested2);

  f.node->rpdo.enable();
  f.run(25ms);
  CHECK(calls2 > rested2);
  CHECK(calls1 == rested1); // still disabled on its own

  REQUIRE(f.node->rpdo.enable(1));
  f.run(25ms);
  CHECK(calls1 > rested1);
}

TEST_CASE("an RPDO follows its node to a new id", "[remote_node]")
{
  fixture f;
  REQUIRE(f.node->rpdo.setup(
      1,
      {.provider = [] { return payload{}; }, .period = 10ms}));
  f.host.start();
  f.run(25ms);
  REQUIRE_FALSE(f.log.with_id(0x201).empty());

  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  f.run(5ms);
  auto const old_id = f.log.with_id(0x201).size();
  f.run(25ms);
  CHECK(f.log.with_id(0x201).size() == old_id);
  CHECK_FALSE(f.log.with_id(0x202).empty());
}

TEST_CASE("the client drives an emulated device", "[remote_node]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  emulated_device device{device_bus, device_id, 20ms};
  std::uint8_t device_counter = 0;
  device.produce_tpdo(1, 10ms, [&] {
    payload p{};
    p[7] = device_counter++;
    return p;
  });

  client host{host_bus, {.id = host_id, .heartbeat_period = 0ms}};
  auto const node = *host.add_node(
      {.name = "drive", .id = device_id, .heartbeat_timeout = 100ms});
  int tpdo1 = 0;
  REQUIRE(node->tpdo.setup(1, {.handler = [&](payload const&) { ++tpdo1; }}));
  REQUIRE(
      node->rpdo.setup(1,
                       {.provider = [] { return payload{}; }, .period = 10ms}));
  host.start();

  run_for(io, 60ms);
  CHECK(node->heartbeat.status()
        == heartbeat_status{.alive = true, .state = nmt_state::operational});
  CHECK(tpdo1 > 0);
  CHECK_FALSE(device.rpdos(1).empty());

  // Stopped, the device keeps its heartbeat, drops its TPDOs and ignores
  // RPDOs.
  bool stop_sent = false;
  host.async_nmt(device_id, nmt_command::stop, [&](auto const& result) {
    stop_sent = result.has_value();
  });
  run_for(io, 60ms);
  CHECK(stop_sent);
  CHECK(device.state() == nmt_state::stopped);
  CHECK(node->heartbeat.status()
        == heartbeat_status{.alive = true, .state = nmt_state::stopped});
  auto const tpdo1_stopped = tpdo1;
  auto const rpdo1_stopped = device.rpdos(1).size();
  run_for(io, 40ms);
  CHECK(tpdo1 == tpdo1_stopped);
  CHECK(device.rpdos(1).size() == rpdo1_stopped);

  // Started again, for every node.
  host.async_nmt(nmt_command::start, [](auto const&) {});
  run_for(io, 60ms);
  CHECK(node->heartbeat.status()
        == heartbeat_status{.alive = true, .state = nmt_state::operational});
  CHECK(tpdo1 > tpdo1_stopped);
  CHECK(device.rpdos(1).size() > rpdo1_stopped);
}
