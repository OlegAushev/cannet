#include <canopen/client.hpp>
#include <canopen/loopback.hpp>
#include <canopen/raw_transport.hpp>

#include <canopen/testing/emulated_device.hpp>
#include <canopen/testing/run.hpp>

#include <cannet_test/vcan.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_after.hpp>
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
#include <string>
#include <thread>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::testing::device_object;
using cannet::canopen::testing::emulated_device;
using cannet::canopen::testing::run_for;

namespace {

constexpr auto host_id = node_id::literal(127);
constexpr auto device_id = node_id::literal(1);

constexpr od_key reset_key{0x2000, 0x01};   // exec
constexpr od_key speed_key{0x3000, 0x01};   // uint16 rw, restorable
constexpr od_key offset_key{0x3000, 0x02};  // int32 ro
constexpr od_key gain_key{0x3000, 0x03};    // float32 rw
constexpr od_key limit_key{0x3000, 0x04};   // uint32 rw, not restorable
constexpr od_key name_key{0x1008, 0x00};    // "drive-01": 2 words and a NUL
constexpr od_key version_key{0x1009, 0x00}; // "abc"
constexpr od_key empty_key{0x100A, 0x00};   // ""
constexpr od_key missing_key{0x5000, 0x01};

void add_objects(emulated_device& device, std::int32_t offset = -5)
{
  using enum od_value_type;
  device.add_object(reset_key, {.type = exec, .access = od_access::wo});
  device.add_object(speed_key,
                    {.type = uint16,
                     .access = od_access::rw,
                     .value = to_raw(std::uint16_t{1234}),
                     .fallback = to_raw(std::uint16_t{1000})});
  device.add_object(
      offset_key,
      {.type = int32, .access = od_access::ro, .value = to_raw(offset)});
  device.add_object(
      gain_key,
      {.type = float32, .access = od_access::rw, .value = to_raw(1.5f)});
  device.add_object(limit_key,
                    {.type = uint32,
                     .access = od_access::rw,
                     .value = to_raw(std::uint32_t{7})});
  device.add_object(
      name_key,
      {.type = string, .access = od_access::const_, .text = "drive-01"});
  device.add_object(
      version_key,
      {.type = string, .access = od_access::const_, .text = "abc"});
  device.add_object(empty_key,
                    {.type = string, .access = od_access::const_, .text = ""});
}

// A client with one node, "drive" at node id 1, and the emulated device
// behind it, on the loopback bus.
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
      {.name = "drive", .id = device_id, .sdo_timeout = 50ms});
};

// Where a test's handler leaves its result.
template<typename T>
struct outcome {
  std::optional<sdo_result<T>> result;

  auto handler()
  {
    return [this](sdo_result<T> r) { result = std::move(r); };
  }

  sdo_error error() const
  {
    REQUIRE(result);
    REQUIRE_FALSE(*result);
    return result->error();
  }

  sdo_error::kind reason() const
  {
    return error().reason;
  }
};

sdo_error aborted(sdo_abort_code code)
{
  return {.reason = sdo_error::kind::aborted, .abort = code};
}

od_value value_of(device_object const& object)
{
  return make_od_value(object.value, object.type);
}

} // namespace

TEST_CASE("every sdo_error kind has a description and a name", "[sdo_client]")
{
  using enum sdo_error::kind;
  for (auto const k :
       {aborted, timeout, cancelled, type_mismatch, malformed, transport}) {
    CHECK(to_string(k) != "unknown error");
    CHECK(name(k) != "unknown");
  }
  CHECK(name(type_mismatch) == "type_mismatch");
  CHECK(to_string(::aborted(sdo_abort_code::object_not_found))
            .contains("object does not exist"));
  CHECK(to_string(sdo_error{.reason = transport,
                            .send_error = transport_error::tx_queue_full})
            .contains("TX queue full"));
}

TEST_CASE("an SDO read returns the object's value as its type", "[sdo_client]")
{
  fixture f;
  outcome<od_value> speed;
  outcome<od_value> offset;
  outcome<od_value> gain;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, speed.handler());
  f.node->sdo.async_read(offset_key, od_value_type::int32, offset.handler());
  f.node->sdo.async_read(gain_key, od_value_type::float32, gain.handler());
  CHECK_FALSE(speed.result); // never inside the call
  f.run();

  REQUIRE(speed.result);
  CHECK(*speed.result == od_value{std::uint16_t{1234}});
  REQUIRE(offset.result);
  CHECK(*offset.result == od_value{std::int32_t{-5}});
  REQUIRE(gain.result);
  CHECK(*gain.result == od_value{1.5f});
}

TEST_CASE("an SDO write reaches the device, unless the object is read-only",
          "[sdo_client]")
{
  fixture f;
  outcome<void> written;
  outcome<void> refused;
  f.node->sdo.async_write(speed_key,
                          od_value{std::uint16_t{4321}},
                          written.handler());
  f.node->sdo.async_write(offset_key,
                          od_value{std::int32_t{1}},
                          refused.handler());
  f.run();

  REQUIRE(written.result);
  CHECK(written.result->has_value());
  CHECK(value_of(f.device.object(speed_key)) == od_value{std::uint16_t{4321}});
  REQUIRE(refused.result);
  REQUIRE_FALSE(*refused.result);
  CHECK(refused.error() == aborted(sdo_abort_code::write_to_read_only));
}

TEST_CASE("a command runs on the device and answers with no value",
          "[sdo_client]")
{
  fixture f;
  outcome<void> executed;
  outcome<od_value> read;
  f.node->sdo.async_exec(reset_key, executed.handler());
  f.node->sdo.async_read(reset_key, od_value_type::exec, read.handler());
  f.run();

  REQUIRE(executed.result);
  CHECK(executed.result->has_value());
  CHECK(f.device.object(reset_key).executed == 1);
  REQUIRE(read.result);
  REQUIRE_FALSE(*read.result);
  CHECK(read.error() == aborted(sdo_abort_code::read_from_write_only));
}

TEST_CASE("a string is read 4 bytes at a time up to its NUL", "[sdo_client]")
{
  fixture f;
  outcome<std::string> name;
  outcome<std::string> version;
  outcome<std::string> empty;
  f.node->sdo.async_read_string(name_key, name.handler());
  f.node->sdo.async_read_string(version_key, version.handler());
  f.node->sdo.async_read_string(empty_key, empty.handler());
  f.run();

  REQUIRE(name.result);
  CHECK(*name.result == std::string{"drive-01"});
  REQUIRE(version.result);
  CHECK(*version.result == std::string{"abc"});
  REQUIRE(empty.result);
  CHECK(*empty.result == std::string{});
  CHECK(f.device.sdo_requests()
        == std::vector{name_key, name_key, name_key, version_key, empty_key});
}

TEST_CASE("a parameter is restored to its default through 1011h:04",
          "[sdo_client]")
{
  fixture f;
  outcome<void> written;
  outcome<void> restored;
  outcome<void> no_default;
  outcome<void> read_only;
  outcome<void> missing;
  f.node->sdo.async_write(speed_key,
                          od_value{std::uint16_t{4321}},
                          written.handler());
  f.node->sdo.async_restore_default(speed_key, restored.handler());
  f.node->sdo.async_restore_default(limit_key, no_default.handler());
  f.node->sdo.async_restore_default(offset_key, read_only.handler());
  f.node->sdo.async_restore_default(missing_key, missing.handler());
  f.run();

  REQUIRE(restored.result);
  CHECK(restored.result->has_value());
  CHECK(value_of(f.device.object(speed_key)) == od_value{std::uint16_t{1000}});
  CHECK(no_default.error() == aborted(sdo_abort_code::no_data_available));
  CHECK(read_only.error() == aborted(sdo_abort_code::write_to_read_only));
  CHECK(missing.error() == aborted(sdo_abort_code::object_not_found));
  CHECK(f.device.sdo_requests().at(1) == od_key{0x1011, 0x04});
}

TEST_CASE("a size that disagrees with the type read is a type mismatch",
          "[sdo_client]")
{
  fixture f;
  outcome<od_value> wider;
  outcome<od_value> narrower;
  outcome<od_value> missing;
  f.node->sdo.async_read(speed_key, od_value_type::int32, wider.handler());
  f.node->sdo.async_read(speed_key, od_value_type::uint8, narrower.handler());
  f.node->sdo.async_read(missing_key, od_value_type::uint8, missing.handler());
  f.run();

  CHECK(wider.reason() == sdo_error::kind::type_mismatch);
  CHECK(narrower.reason() == sdo_error::kind::type_mismatch);
  CHECK(missing.error() == aborted(sdo_abort_code::object_not_found));
}

TEST_CASE("a segmented answer is malformed", "[sdo_client]")
{
  fixture f;
  f.device.answer_segmented = true;
  outcome<od_value> segmented;
  outcome<od_value> next;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, segmented.handler());
  f.node->sdo.async_read(speed_key, od_value_type::uint16, next.handler());
  f.run();

  CHECK(segmented.reason() == sdo_error::kind::malformed);
  REQUIRE(next.result);
  CHECK(next.result->has_value());
}

TEST_CASE("a request without an answer times out, and the next one goes on",
          "[sdo_client]")
{
  fixture f;
  f.device.lose_answer = [](std::size_t request) { return request == 0; };
  outcome<od_value> lost;
  outcome<od_value> next;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, lost.handler());
  f.node->sdo.async_read(offset_key, od_value_type::int32, next.handler());

  f.run(30ms);
  CHECK_FALSE(lost.result); // the timeout is 50 ms
  f.run(60ms);
  CHECK(lost.reason() == sdo_error::kind::timeout);
  REQUIRE(next.result);
  CHECK(*next.result == od_value{std::int32_t{-5}});
}

TEST_CASE("requests queue behind the one in flight", "[sdo_client]")
{
  fixture f;
  f.device.answer_delay = 5ms;
  std::vector<int> completed;
  for (int i = 0; i < 3; ++i) {
    f.node->sdo.async_read(speed_key,
                           od_value_type::uint16,
                           [&completed, i](sdo_result<od_value> r) {
                             CHECK(r.has_value());
                             completed.push_back(i);
                           });
  }
  f.node->sdo.async_exec(reset_key, [&completed](sdo_result<void> r) {
    CHECK(r.has_value());
    completed.push_back(3);
  });
  f.run(60ms);

  CHECK(completed == std::vector{0, 1, 2, 3});
  CHECK(f.device.sdo_requests().size() == 4);
  CHECK(f.device.overlapping_requests() == 0);
}

TEST_CASE("a late answer does not pass for the next request's", "[sdo_client]")
{
  fixture f;
  // The first answer comes after the timeout, while the client waits for
  // the second request's.
  f.device.next_answer_delay = 80ms;
  f.device.answer_delay = 40ms;
  outcome<od_value> late;
  outcome<od_value> next;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, late.handler());
  f.node->sdo.async_read(offset_key, od_value_type::int32, next.handler());
  f.run(150ms);

  CHECK(late.reason() == sdo_error::kind::timeout);
  REQUIRE(next.result);
  CHECK(*next.result == od_value{std::int32_t{-5}});
}

TEST_CASE("a queued request cancelled is never sent", "[sdo_client]")
{
  fixture f;
  f.device.answer_delay = 20ms;
  boost::asio::cancellation_signal signal;
  outcome<od_value> first;
  outcome<od_value> cancelled;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, first.handler());
  f.node->sdo.async_read(
      offset_key,
      od_value_type::int32,
      boost::asio::bind_cancellation_slot(signal.slot(), cancelled.handler()));
  f.run(5ms);

  signal.emit(boost::asio::cancellation_type::terminal);
  f.run(5ms);
  CHECK(cancelled.reason() == sdo_error::kind::cancelled);
  CHECK_FALSE(first.result);

  f.run(40ms);
  REQUIRE(first.result);
  CHECK(first.result->has_value());
  CHECK(f.device.sdo_requests() == std::vector{speed_key});
}

TEST_CASE("a request cancelled in flight answers at once but keeps the "
          "channel until its answer",
          "[sdo_client]")
{
  fixture f;
  f.device.answer_delay = 30ms;
  boost::asio::cancellation_signal signal;
  outcome<od_value> cancelled;
  outcome<od_value> next;
  f.node->sdo.async_read(
      speed_key,
      od_value_type::uint16,
      boost::asio::bind_cancellation_slot(signal.slot(), cancelled.handler()));
  f.node->sdo.async_read(offset_key, od_value_type::int32, next.handler());
  f.run(5ms);

  signal.emit(boost::asio::cancellation_type::terminal);
  f.run(5ms);
  CHECK(cancelled.reason() == sdo_error::kind::cancelled);
  CHECK(f.device.sdo_requests() == std::vector{speed_key}); // the next waits

  f.run(80ms);
  REQUIRE(next.result);
  CHECK(*next.result == od_value{std::int32_t{-5}});
  CHECK(f.device.overlapping_requests() == 0);
}

TEST_CASE("cancel_after cancels a slow request", "[sdo_client]")
{
  fixture f;
  f.device.answer_delay = 30ms;
  outcome<od_value> slow;
  f.node->sdo.async_read(speed_key,
                         od_value_type::uint16,
                         boost::asio::cancel_after(5ms, slow.handler()));
  f.run(15ms);
  CHECK(slow.reason() == sdo_error::kind::cancelled);
}

TEST_CASE("a string read cancelled midway still runs to its NUL",
          "[sdo_client]")
{
  fixture f;
  f.device.answer_delay = 10ms;
  boost::asio::cancellation_signal signal;
  outcome<std::string> cancelled;
  f.node->sdo.async_read_string(
      name_key,
      boost::asio::bind_cancellation_slot(signal.slot(), cancelled.handler()));
  f.run(15ms); // the first word is in
  signal.emit(boost::asio::cancellation_type::terminal);
  f.run(5ms);
  CHECK(cancelled.reason() == sdo_error::kind::cancelled);

  f.run(40ms);
  CHECK(f.device.sdo_requests().size() == 3); // to the NUL all the same

  outcome<std::string> again;
  f.node->sdo.async_read_string(name_key, again.handler());
  f.run(60ms);
  REQUIRE(again.result);
  CHECK(*again.result == std::string{"drive-01"});
}

TEST_CASE("a string read cut short by a timeout is run to its end before "
          "the next read",
          "[sdo_client]")
{
  fixture f;
  // The second word is served, so the device's cursor moves on, but its
  // answer is lost.
  f.device.lose_answer = [](std::size_t request) { return request == 1; };
  outcome<std::string> cut;
  f.node->sdo.async_read_string(name_key, cut.handler());
  f.run(80ms);
  CHECK(cut.reason() == sdo_error::kind::timeout);

  // Read as it stands, the device would return the tail: an empty string.
  outcome<std::string> whole;
  f.node->sdo.async_read_string(name_key, whole.handler());
  f.run(30ms);
  REQUIRE(whole.result);
  CHECK(*whole.result == std::string{"drive-01"});
  CHECK(f.device.sdo_requests().size() == 2 + 1 + 3); // cut, drain, whole
}

TEST_CASE("SDO operations may come from another thread, or a coroutine",
          "[sdo_client]")
{
  fixture f;
  auto work = boost::asio::make_work_guard(f.io);
  std::thread runner{[&f] { f.io.run(); }};

  // From this thread, as a GUI would.
  auto const read = f.node->sdo
                        .async_read(speed_key,
                                    od_value_type::uint16,
                                    boost::asio::use_future)
                        .get();

  // Awaited in a coroutine on the client's executor, with the default token.
  auto const text = boost::asio::co_spawn(
                        f.io,
                        [&f]()
                            -> boost::asio::awaitable<sdo_result<std::string>> {
                          co_return co_await f.node->sdo.async_read_string(
                              version_key);
                        },
                        boost::asio::use_future)
                        .get();

  work.reset();
  runner.join();

  CHECK(read == od_value{std::uint16_t{1234}});
  CHECK(text == std::string{"abc"});
}

TEST_CASE("a request in flight when the node changes id is cancelled; "
          "queued ones go to the new id",
          "[sdo_client]")
{
  fixture f;
  emulated_device moved{f.device_bus, node_id::literal(2), 0ms};
  add_objects(moved, -7);
  f.device.answer_delay = 30ms;
  outcome<od_value> in_flight;
  outcome<od_value> queued;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, in_flight.handler());
  f.node->sdo.async_read(offset_key, od_value_type::int32, queued.handler());
  f.run(5ms);

  REQUIRE(f.host.set_remote_node_id("drive", node_id::literal(2)));
  f.run(20ms);
  CHECK(in_flight.reason() == sdo_error::kind::cancelled);
  REQUIRE(queued.result);
  CHECK(*queued.result == od_value{std::int32_t{-7}});
  CHECK(moved.sdo_requests() == std::vector{offset_key});
}

TEST_CASE("SDO requests fail with closed once the client is gone",
          "[sdo_client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  emulated_device device{device_bus, device_id, 0ms};
  add_objects(device);
  device.answer_delay = 30ms;
  auto host = std::make_unique<client>(
      host_bus,
      client_options{.id = host_id, .heartbeat_period = 0ms});
  auto const node = *host->add_node({.name = "drive", .id = device_id});

  outcome<od_value> in_flight;
  outcome<od_value> queued;
  node->sdo.async_read(speed_key, od_value_type::uint16, in_flight.handler());
  node->sdo.async_read(offset_key, od_value_type::int32, queued.handler());
  run_for(io, 5ms);
  host.reset();
  outcome<od_value> later;
  node->sdo.async_read(speed_key, od_value_type::uint16, later.handler());
  run_for(io, 20ms);

  sdo_error const closed{.reason = sdo_error::kind::transport,
                         .send_error = transport_error::closed};
  for (auto const* o : {&in_flight, &queued, &later}) {
    REQUIRE(o->result);
    REQUIRE_FALSE(*o->result);
    CHECK(o->error() == closed);
  }
}

TEST_CASE("a failed send fails its request, and the next one goes on",
          "[sdo_client]")
{
  fixture f;
  f.host_bus.fail_next_send(transport_error::tx_queue_full);
  outcome<od_value> failed;
  outcome<od_value> next;
  f.node->sdo.async_read(speed_key, od_value_type::uint16, failed.handler());
  f.node->sdo.async_read(speed_key, od_value_type::uint16, next.handler());
  f.run();

  REQUIRE(failed.result);
  REQUIRE_FALSE(*failed.result);
  CHECK(failed.error()
        == sdo_error{.reason = sdo_error::kind::transport,
                     .send_error = transport_error::tx_queue_full});
  REQUIRE(next.result);
  CHECK(next.result->has_value());
}

TEST_CASE("SDO goes through the kernel to an emulated device on vcan",
          "[sdo_client][vcan]")
{
  if (!cannet::test::vcan_up()) {
    SKIP("vcan0 is not up");
  }

  boost::asio::io_context io;
  raw_transport device_bus{io.get_executor()};
  raw_transport host_bus{io.get_executor()};
  REQUIRE(device_bus.open(cannet::test::vcan_iface));
  REQUIRE(host_bus.open(cannet::test::vcan_iface));
  emulated_device device{device_bus, device_id, 0ms};
  add_objects(device);
  client host{host_bus, {.id = host_id, .heartbeat_period = 0ms}};
  auto const node = *host.add_node({.name = "drive", .id = device_id});

  outcome<od_value> read;
  outcome<void> written;
  outcome<void> executed;
  outcome<std::string> name;
  node->sdo.async_read(offset_key, od_value_type::int32, read.handler());
  node->sdo.async_write(gain_key, od_value{2.5f}, written.handler());
  node->sdo.async_exec(reset_key, executed.handler());
  node->sdo.async_read_string(name_key, name.handler());
  run_for(io, 100ms);

  REQUIRE(read.result);
  CHECK(*read.result == od_value{std::int32_t{-5}});
  REQUIRE(written.result);
  CHECK(written.result->has_value());
  CHECK(value_of(device.object(gain_key)) == od_value{2.5f});
  REQUIRE(executed.result);
  CHECK(executed.result->has_value());
  CHECK(device.object(reset_key).executed == 1);
  REQUIRE(name.result);
  CHECK(*name.result == std::string{"drive-01"});
}
