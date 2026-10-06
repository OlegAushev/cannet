#include <canopen/client.hpp>
#include <canopen/loopback.hpp>

#include "support.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <thread>
#include <vector>

using namespace cannet::canopen;
using namespace std::chrono_literals;
using cannet::canopen::test::bus_log;
using cannet::canopen::test::run_for;

namespace {

using send_result = std::expected<void, transport_error>;

constexpr auto host = node_id::literal(127);

} // namespace

TEST_CASE("every setup_error has a description and a name", "[client]")
{
  for (auto const e : {setup_error::node_id_taken,
                       setup_error::name_taken,
                       setup_error::no_such_node,
                       setup_error::invalid_pdo}) {
    CHECK(to_string(e) != "unknown error");
    CHECK(name(e) != "unknown");
  }
  CHECK(name(setup_error::node_id_taken) == "node_id_taken");
}

TEST_CASE("the client sends its heartbeat and SYNC only while started",
          "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  bus_log log{bus};
  client c{host_bus,
           {.id = host, .heartbeat_period = 20ms, .sync_period = 10ms}};

  run_for(io, 30ms);
  CHECK(log.frames().empty());

  c.start();
  CHECK(c.started());
  run_for(io, 105ms); // first frames at once: 6 heartbeats, 11 SYNCs
  auto const heartbeats = log.with_id(0x77F);
  auto const syncs = log.with_id(0x080);
  CHECK(heartbeats.size() >= 3);
  CHECK(heartbeats.size() <= 7);
  CHECK(syncs.size() >= 6);
  CHECK(syncs.size() <= 12);
  REQUIRE_FALSE(heartbeats.empty());
  CHECK(heartbeats.front().len == 1);
  CHECK(heartbeats.front().data[0] == 0x05); // operational
  REQUIRE_FALSE(syncs.empty());
  CHECK(syncs.front().len == 0);

  c.stop();
  run_for(io, 5ms); // what was in flight lands
  auto const sent = log.frames().size();
  run_for(io, 40ms);
  CHECK(log.frames().size() == sent);
}

TEST_CASE("SYNC is off by default, and a zero period stops a producer",
          "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  bus_log log{bus};
  client c{host_bus, {.id = host, .heartbeat_period = 10ms}};
  c.start();

  run_for(io, 35ms);
  CHECK(log.with_id(0x080).empty());
  CHECK_FALSE(log.with_id(0x77F).empty());

  c.set_heartbeat_period(0ms);
  run_for(io, 5ms);
  auto const heartbeats = log.with_id(0x77F).size();
  run_for(io, 30ms);
  CHECK(log.with_id(0x77F).size() == heartbeats);

  c.set_sync_period(10ms);
  run_for(io, 25ms);
  CHECK_FALSE(log.with_id(0x080).empty());
}

TEST_CASE("a new node id goes out with the next heartbeat", "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  bus_log log{bus};
  client c{host_bus, {.id = host, .heartbeat_period = 10ms}};
  c.start();
  run_for(io, 5ms);

  REQUIRE(c.set_node_id(node_id::literal(100)));
  CHECK(c.id() == node_id::literal(100));
  run_for(io, 25ms);
  CHECK_FALSE(log.with_id(0x764).empty());
}

TEST_CASE("NMT commands go to every node or to one", "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  bus_log log{bus};
  client c{host_bus, {.id = host, .heartbeat_period = 0ms}};

  std::vector<send_result> results;
  c.async_nmt(nmt_command::start, [&](send_result r) { results.push_back(r); });
  c.async_nmt(node_id::literal(5), nmt_command::reset_node, [&](send_result r) {
    results.push_back(r);
  });
  CHECK(results.empty()); // never inside the call
  run_for(io, 20ms);

  REQUIRE(results.size() == 2);
  CHECK(results[0].has_value());
  CHECK(results[1].has_value());
  auto const nmt = log.with_id(0x000);
  REQUIRE(nmt.size() == 2);
  CHECK(decode_nmt(nmt[0])
        == nmt_message{.command = nmt_command::start, .target = std::nullopt});
  CHECK(decode_nmt(nmt[1])
        == nmt_message{.command = nmt_command::reset_node,
                       .target = node_id::literal(5)});
}

TEST_CASE("an NMT command reports a failed send", "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  client c{host_bus, {.id = host, .heartbeat_period = 0ms}};

  host_bus.fail_next_send(transport_error::tx_queue_full);
  std::vector<send_result> results;
  c.async_nmt(nmt_command::stop, [&](send_result r) { results.push_back(r); });
  run_for(io, 20ms);

  REQUIRE(results.size() == 1);
  REQUIRE_FALSE(results[0]);
  CHECK(results[0].error() == transport_error::tx_queue_full);
}

TEST_CASE("an NMT command may come from another thread, or a coroutine",
          "[client]")
{
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  bus_log log{bus};
  client c{host_bus, {.id = host, .heartbeat_period = 0ms}};
  std::thread runner{[&io] { io.run(); }};

  // From this thread, as a GUI would.
  auto const sent =
      c.async_nmt(nmt_command::stop, boost::asio::use_future).get();

  // Awaited in a coroutine on the client's executor, with the default token.
  auto const awaited = boost::asio::co_spawn(
                           io,
                           [&c]() -> boost::asio::awaitable<send_result> {
                             co_return co_await c.async_nmt(node_id::literal(1),
                                                            nmt_command::start);
                           },
                           boost::asio::use_future)
                           .get();

  work.reset();
  runner.join();

  CHECK(sent.has_value());
  CHECK(awaited.has_value());
  CHECK(log.with_id(0x000).size() == 2);
}

TEST_CASE("an NMT command outliving its client completes with closed",
          "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  auto c = std::make_unique<client>(
      host_bus,
      client_options{.id = host, .heartbeat_period = 0ms});

  auto op = c->async_nmt(nmt_command::start); // deferred: not started yet
  c.reset();
  std::vector<send_result> results;
  std::move(op)([&](send_result r) { results.push_back(r); });
  run_for(io, 20ms);

  REQUIRE(results.size() == 1);
  REQUIRE_FALSE(results[0]);
  CHECK(results[0].error() == transport_error::closed);
}

TEST_CASE("nodes need names and ids of their own", "[client]")
{
  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  client c{host_bus, {.id = host}};

  auto const a = c.add_node({.name = "a", .id = node_id::literal(1)});
  REQUIRE(a);
  CHECK(c.add_node({.name = "a", .id = node_id::literal(2)}).error()
        == setup_error::name_taken);
  CHECK(c.add_node({.name = "b", .id = node_id::literal(1)}).error()
        == setup_error::node_id_taken);
  CHECK(c.add_node({.name = "b", .id = host}).error()
        == setup_error::node_id_taken);
  auto const b = c.add_node({.name = "b", .id = node_id::literal(2)});
  REQUIRE(b);

  CHECK(c.find_node("b") == *b);
  CHECK(c.find_node("c") == nullptr);
  CHECK(c.nodes().size() == 2);
  CHECK((*a)->name() == "a");

  CHECK(c.set_node_id(node_id::literal(2)).error()
        == setup_error::node_id_taken);
  CHECK(c.set_node_id(node_id::literal(100)));

  CHECK(c.set_remote_node_id("c", node_id::literal(3)).error()
        == setup_error::no_such_node);
  CHECK(c.set_remote_node_id("a", node_id::literal(2)).error()
        == setup_error::node_id_taken);
  CHECK(c.set_remote_node_id("a", node_id::literal(100)).error()
        == setup_error::node_id_taken);
  CHECK(c.set_remote_node_id("a", node_id::literal(1))); // its own: no-op
  CHECK(c.set_remote_node_id("a", node_id::literal(3)));
  CHECK((*a)->id() == node_id::literal(3));
}
