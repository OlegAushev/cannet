#include <canopen/client.hpp>
#include <canopen/loopback.hpp>

#include <canopen/testing/emulated_device.hpp>
#include <canopen/testing/run.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::testing::device_object;
using cannet::canopen::testing::emulated_device;
using cannet::canopen::testing::run_for;
using cannet::canopen::testing::run_until;

namespace {

constexpr auto host_id = node_id::literal(127);
constexpr auto device_id = node_id::literal(1);

constexpr od_key speed_key{0x3000, 0x01};   // uint16 rw, restorable
constexpr od_key gain_key{0x3000, 0x02};    // float32 rw
constexpr od_key serial_key{0x3000, 0x03};  // uint32 ro
constexpr od_key label_key{0x3000, 0x04};   // a string: not read
constexpr od_key secret_key{0x3000, 0x05};  // write-only: not read
constexpr od_key missing_key{0x3000, 0x06}; // int32 rw, not on the device
constexpr od_key uptime_key{0x5000, 0x01};  // a watch object
constexpr od_key save_key = service::config::save_all_key;
constexpr od_key load_key = service::config::restore_all_key;

constexpr auto drive_dictionary = dictionary{
    {.watch_category = "watch", .config_category = "config"},
    {{missing_key,
      {"config", "drive", "missing", "", od_access::rw, od_value_type::int32}},
     {speed_key,
      {"config",
       "drive",
       "speed",
       "rpm",
       od_access::rw,
       od_value_type::uint16}},
     {gain_key,
      {"config", "drive", "gain", "", od_access::rw, od_value_type::float32}},
     {serial_key,
      {"config", "info", "serial", "", od_access::ro, od_value_type::uint32}},
     {label_key,
      {"config", "info", "label", "", od_access::ro, od_value_type::string}},
     {secret_key,
      {"config", "info", "secret", "", od_access::wo, od_value_type::uint8}},
     {uptime_key,
      {"watch", "sys", "uptime", "s", od_access::ro, od_value_type::float32}}}};

od_entry const& entry(od_key key)
{
  auto const* found = drive_dictionary.view().find(key);
  REQUIRE(found != nullptr);
  return *found;
}

void add_objects(emulated_device& device)
{
  using enum od_value_type;
  device.add_object(speed_key,
                    {.type = uint16,
                     .access = od_access::rw,
                     .value = to_raw(std::uint16_t{1234}),
                     .fallback = to_raw(std::uint16_t{1000})});
  device.add_object(
      gain_key,
      {.type = float32, .access = od_access::rw, .value = to_raw(1.5f)});
  device.add_object(serial_key,
                    {.type = uint32,
                     .access = od_access::ro,
                     .value = to_raw(std::uint32_t{458809})});
  device.add_object(label_key,
                    {.type = string, .access = od_access::ro, .text = "x"});
  device.add_object(secret_key, {.type = uint8, .access = od_access::wo});
  device.add_object(
      uptime_key,
      {.type = float32, .access = od_access::ro, .value = to_raw(12.5f)});
  device.add_object(save_key, {.type = exec, .access = od_access::wo});
  device.add_object(load_key, {.type = exec, .access = od_access::wo});
}

// A client with one node, "drive" at node id 1 with the dictionary above,
// the emulated device behind it on the loopback bus, and every reading of
// the node's config service.
struct fixture {
  fixture()
  {
    add_objects(device);
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
  subscription values = node->config.on_value(
      [this](object_reading const& r) { readings.push_back(r); });
};

// Where a test's handler leaves its result.
template<typename T>
struct outcome {
  std::optional<sdo_result<T>> result;

  auto handler()
  {
    return [this](sdo_result<T> r) { result = std::move(r); };
  }

  bool done() const
  {
    return result.has_value();
  }

  sdo_error::kind reason() const
  {
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    return result->error().reason;
  }
};

std::vector<od_key> keys_of(std::vector<object_reading> const& readings)
{
  std::vector<od_key> keys;
  for (auto const& r : readings) {
    keys.push_back(r.entry->key);
  }
  return keys;
}

sdo_error aborted(sdo_abort_code code)
{
  return {.reason = sdo_error::kind::aborted, .abort = code};
}

od_value value_of(device_object const& object)
{
  return make_od_value(object.value, object.type);
}

} // namespace

TEST_CASE("the parameters are the config category's objects, by key",
          "[config]")
{
  fixture f;
  std::vector<od_key> keys;
  for (auto const* p : f.node->config.parameters()) {
    keys.push_back(p->key);
  }
  CHECK(keys
        == std::vector<od_key>{speed_key,
                               gain_key,
                               serial_key,
                               label_key,
                               secret_key,
                               missing_key});
}

TEST_CASE("a parameter is read as the dictionary types it", "[config]")
{
  fixture f;
  outcome<od_value> speed;
  outcome<od_value> label;
  outcome<od_value> secret;
  f.node->config.async_read(entry(speed_key), speed.handler());
  f.node->config.async_read(entry(label_key), label.handler());
  f.node->config.async_read(entry(secret_key), secret.handler());
  REQUIRE(f.run_until([&] { return speed.done() && secret.done(); }));
  REQUIRE(label.done());

  CHECK(*speed.result == od_value{std::uint16_t{1234}});
  // A string has no od_value: refused without a request.
  CHECK(label.reason() == sdo_error::kind::type_mismatch);
  // A write-only object: the device refuses.
  CHECK(secret.result->error()
        == aborted(sdo_abort_code::read_from_write_only));
  CHECK(f.device.sdo_requests() == std::vector<od_key>{speed_key, secret_key});

  // Both answers are readings, reported.
  REQUIRE(keys_of(f.readings) == std::vector<od_key>{speed_key, secret_key});
  CHECK(f.readings[0].value == od_value{std::uint16_t{1234}});
  CHECK(f.readings[0].time > std::chrono::steady_clock::time_point{});
  CHECK(f.readings[1].value.error()
        == aborted(sdo_abort_code::read_from_write_only));
}

TEST_CASE("a write is checked against the dictionary", "[config]")
{
  fixture f;
  outcome<void> gain;
  outcome<void> wrong;
  outcome<void> serial;
  f.node->config.async_write(entry(gain_key), od_value{2.5f}, gain.handler());
  // A uint32 for a float32 parameter would land as garbage.
  f.node->config.async_write(entry(gain_key),
                             od_value{std::uint32_t{5}},
                             wrong.handler());
  f.node->config.async_write(entry(serial_key),
                             od_value{std::uint32_t{1}},
                             serial.handler());
  REQUIRE(f.run_until(
      [&] { return gain.done() && wrong.done() && serial.done(); }));

  CHECK(*gain.result);
  CHECK(value_of(f.device.object(gain_key)) == od_value{2.5f});
  CHECK(wrong.reason() == sdo_error::kind::type_mismatch);
  CHECK(serial.result->error() == aborted(sdo_abort_code::write_to_read_only));
  CHECK(f.device.sdo_requests() == std::vector<od_key>{gain_key, serial_key});
  CHECK(f.readings.empty()); // writes are no readings
}

TEST_CASE("reading all parameters reads every readable scalar, by key",
          "[config]")
{
  fixture f;
  outcome<std::vector<object_reading>> all;
  f.node->config.async_read_all(all.handler());
  REQUIRE(f.run_until([&] { return all.done(); }));

  REQUIRE(*all.result);
  auto const& readings = **all.result;
  // No string, no write-only object; a missing one is a reading too.
  CHECK(keys_of(readings)
        == std::vector<od_key>{speed_key, gain_key, serial_key, missing_key});
  CHECK(readings[0].value == od_value{std::uint16_t{1234}});
  CHECK(readings[1].value == od_value{1.5f});
  CHECK(readings[2].value == od_value{std::uint32_t{458809}});
  CHECK(readings[3].value.error() == aborted(sdo_abort_code::object_not_found));
  CHECK(f.device.sdo_requests() == keys_of(readings));
  // Every reading was an event as it came.
  CHECK(keys_of(f.readings) == keys_of(readings));
}

TEST_CASE("reading all parameters goes past a timeout, and gives up after "
          "three in a row",
          "[config]")
{
  SECTION("one lost answer")
  {
    fixture f;
    f.device.lose_answer = [](std::size_t i) { return i == 1; };
    outcome<std::vector<object_reading>> all;
    f.node->config.async_read_all(all.handler());
    REQUIRE(f.run_until([&] { return all.done(); }));
    REQUIRE(*all.result);
    auto const& readings = **all.result;
    REQUIRE(readings.size() == 4);
    CHECK(readings[1].value.error().reason == sdo_error::kind::timeout);
    CHECK(readings[2].value == od_value{std::uint32_t{458809}});
  }

  SECTION("a silent device")
  {
    fixture f;
    f.device.lose_answer = [](std::size_t) { return true; };
    outcome<std::vector<object_reading>> all;
    f.node->config.async_read_all(all.handler());
    REQUIRE(f.run_until([&] { return all.done(); }));
    CHECK(all.reason() == sdo_error::kind::timeout);
    CHECK(f.device.sdo_requests()
          == std::vector<od_key>{speed_key, gain_key, serial_key});
    CHECK(f.readings.size() == 3); // each timeout reported
  }
}

TEST_CASE("reading all parameters stops when cancelled, when a request "
          "cannot be sent, and when the node changes its id",
          "[config]")
{
  SECTION("cancelled")
  {
    fixture f;
    f.device.answer_delay = 20ms;
    boost::asio::cancellation_signal cancel;
    outcome<std::vector<object_reading>> all;
    f.node->config.async_read_all(
        boost::asio::bind_cancellation_slot(cancel.slot(), all.handler()));
    REQUIRE(f.run_until([&f] { return f.device.sdo_requests().size() == 2; }));
    cancel.emit(boost::asio::cancellation_type::terminal);
    REQUIRE(f.run_until([&] { return all.done(); }));
    CHECK(all.reason() == sdo_error::kind::cancelled);
    f.run(60ms);
    CHECK(f.device.sdo_requests().size() == 2); // nothing after
    CHECK(keys_of(f.readings) == std::vector<od_key>{speed_key});
  }

  SECTION("a request not sent")
  {
    fixture f;
    f.host_bus.fail_next_send(transport_error::send_failed);
    outcome<std::vector<object_reading>> all;
    f.node->config.async_read_all(all.handler());
    REQUIRE(f.run_until([&] { return all.done(); }));
    CHECK(all.result->error()
          == sdo_error{.reason = sdo_error::kind::transport,
                       .send_error = transport_error::send_failed});
    CHECK(f.device.sdo_requests().empty());
  }

  SECTION("a node id change")
  {
    fixture f;
    f.device.answer_delay = 20ms;
    outcome<std::vector<object_reading>> all;
    f.node->config.async_read_all(all.handler());
    REQUIRE(f.run_until([&f] { return f.device.sdo_requests().size() == 1; }));
    REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
    REQUIRE(f.run_until([&] { return all.done(); }));
    CHECK(all.reason() == sdo_error::kind::cancelled);
    CHECK(f.readings.empty());
  }
}

TEST_CASE("storing and restoring all write CiA 301's signatures", "[config]")
{
  fixture f;
  outcome<void> saved;
  outcome<void> restored;
  f.node->config.async_save_all(saved.handler());
  f.node->config.async_restore_all_defaults(restored.handler());
  REQUIRE(f.run_until([&] { return saved.done() && restored.done(); }));

  CHECK(*saved.result);
  CHECK(*restored.result);
  CHECK(f.device.object(save_key).executed == 1);
  CHECK(f.device.object(save_key).value
        == expedited_sdo_data{'s', 'a', 'v', 'e'});
  CHECK(f.device.object(load_key).executed == 1);
  CHECK(f.device.object(load_key).value
        == expedited_sdo_data{'l', 'o', 'a', 'd'});
}

TEST_CASE("a parameter is restored to its default", "[config]")
{
  fixture f;
  outcome<void> written;
  outcome<void> restored;
  f.node->config.async_write(entry(speed_key),
                             od_value{std::uint16_t{7}},
                             written.handler());
  f.node->config.async_restore_default(entry(speed_key), restored.handler());
  REQUIRE(f.run_until([&] { return written.done() && restored.done(); }));
  CHECK(*written.result);
  CHECK(*restored.result);
  CHECK(value_of(f.device.object(speed_key)) == od_value{std::uint16_t{1000}});
}

TEST_CASE("operations reach the device in the order they were made", "[config]")
{
  fixture f;
  outcome<void> written;
  outcome<od_value> direct;
  outcome<void> restored;
  outcome<od_value> read;
  f.node->config.async_write(entry(speed_key),
                             od_value{std::uint16_t{7}},
                             written.handler());
  f.node->sdo.async_read(speed_key, od_value_type::uint16, direct.handler());
  f.node->config.async_restore_default(entry(speed_key), restored.handler());
  f.node->config.async_read(entry(speed_key), read.handler());
  REQUIRE(f.run_until([&] { return read.done(); }));

  CHECK(f.device.sdo_requests()
        == std::vector<od_key>{speed_key,
                               speed_key,
                               detail::sdo_client::restore_default_key,
                               speed_key});
  CHECK(*written.result);
  CHECK(*direct.result == od_value{std::uint16_t{7}});
  CHECK(*restored.result);
  CHECK(*read.result == od_value{std::uint16_t{1000}});
}

TEST_CASE("config operations run from another thread and in a coroutine",
          "[config]")
{
  fixture f;
  auto work = boost::asio::make_work_guard(f.io);
  f.io.restart();
  std::jthread client_thread{[&f] { f.io.run(); }};

  auto all = f.node->config.async_read_all(boost::asio::use_future);
  auto written = f.node->config.async_write(entry(gain_key),
                                            od_value{3.0f},
                                            boost::asio::use_future);
  auto read_back = boost::asio::co_spawn(
      f.io,
      [&f]() -> boost::asio::awaitable<sdo_result<od_value>> {
        co_return co_await f.node->config.async_read(
            entry(gain_key),
            boost::asio::use_awaitable);
      },
      boost::asio::use_future);

  REQUIRE(all.wait_for(2s) == std::future_status::ready);
  REQUIRE(written.wait_for(2s) == std::future_status::ready);
  REQUIRE(read_back.wait_for(2s) == std::future_status::ready);
  auto const readings = all.get();
  auto const write = written.get();
  auto const value = read_back.get();

  work.reset();
  f.io.stop();
  client_thread.join();
  REQUIRE(readings);
  CHECK(readings->size() == 4);
  CHECK(write);
  CHECK(value == od_value{3.0f}); // queued behind the write
}

TEST_CASE("with the client gone, config operations fail with closed",
          "[config]")
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
  int readings = 0;
  auto const sub = node->config.on_value(
      [&readings](object_reading const&) { ++readings; });
  host.reset();

  outcome<od_value> read;
  outcome<std::vector<object_reading>> all;
  outcome<void> written;
  outcome<void> saved;
  node->config.async_read(entry(speed_key), read.handler());
  node->config.async_read_all(all.handler());
  node->config.async_write(entry(speed_key),
                           od_value{std::uint16_t{1}},
                           written.handler());
  node->config.async_save_all(saved.handler());
  REQUIRE(run_until(io, [&] {
    return read.done() && all.done() && written.done() && saved.done();
  }));
  sdo_error const closed{.reason = sdo_error::kind::transport,
                         .send_error = transport_error::closed};
  CHECK(read.result->error() == closed);
  CHECK(all.result->error() == closed);
  CHECK(written.result->error() == closed);
  CHECK(saved.result->error() == closed);
  CHECK(readings == 0);
  CHECK(device.sdo_requests().empty());
}

TEST_CASE("a config handler may drop the node while all parameters are read",
          "[config]")
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
  auto const sub = node->config.on_value([&](object_reading const&) {
    ++readings;
    host.reset();
    node.reset();
  });
  outcome<std::vector<object_reading>> all;
  node->config.async_read_all(all.handler());
  REQUIRE(run_until(io, [&] { return all.done(); }));
  CHECK(readings == 1);
  CHECK(all.result->error()
        == sdo_error{.reason = sdo_error::kind::transport,
                     .send_error = transport_error::closed});
  CHECK(device.sdo_requests().size() == 1);
}
