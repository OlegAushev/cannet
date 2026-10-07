// canopen nmt, against an emulated device.

#include "cli_fixture.hpp"

#include <canopen/testing/bus_log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace cannet::canopen;
using namespace cannet::canopen::test;
using namespace cannet::canopen::testing;

namespace tool = cannet::canopen::tool;

TEST_CASE("NMT commands go by the names the CLI gives them", "[cli]")
{
  for (auto const command : {nmt_command::start,
                             nmt_command::stop,
                             nmt_command::enter_pre_operational,
                             nmt_command::reset_node,
                             nmt_command::reset_communication}) {
    CAPTURE(name(command));
    CHECK(tool::parse_nmt_command(tool::cli_name(command)) == command);
  }
  CHECK(tool::cli_name(nmt_command::enter_pre_operational)
        == "pre-operational");
  CHECK_FALSE(tool::parse_nmt_command("enter_pre_operational"));
  CHECK_FALSE(tool::parse_nmt_command("reset"));
}

TEST_CASE("nmt sends a command to one node or to every node", "[cli]")
{
  cli_fixture f;
  bus_log log{f.bus};

  CHECK(
      f.run(
          tool::nmt(f.host_bus, nmt_command::stop, cli_device_id, f.console()))
      == 0);
  CHECK(f.device.state() == nmt_state::stopped);
  CHECK(f.run(tool::nmt(f.host_bus,
                        nmt_command::start,
                        node_id::literal(2),
                        f.console()))
        == 0);
  CHECK(f.device.state() == nmt_state::stopped);
  CHECK(
      f.run(
          tool::nmt(f.host_bus, nmt_command::start, std::nullopt, f.console()))
      == 0);
  CHECK(f.device.state() == nmt_state::operational);

  // The commands, and nothing else: no heartbeat of the CLI's own.
  auto const& frames = log.frames();
  REQUIRE(frames.size() == 3);
  std::vector<std::vector<std::uint8_t>> const sent = {{0x02, 0x01},
                                                       {0x01, 0x02},
                                                       {0x01, 0x00}};
  for (std::size_t i = 0; i < frames.size(); ++i) {
    CAPTURE(i);
    CHECK(frames[i].can_id == 0x000);
    CHECK(std::vector<std::uint8_t>(frames[i].data,
                                    frames[i].data + frames[i].len)
          == sent[i]);
  }
  CHECK(f.out.str().empty());
  CHECK(f.err.str().empty());
}

TEST_CASE("an NMT command the transport cannot send fails", "[cli]")
{
  cli_fixture f;
  f.host_bus.fail_next_send(transport_error::tx_queue_full);
  CHECK(f.run(tool::nmt(f.host_bus,
                        nmt_command::reset_node,
                        cli_device_id,
                        f.console()))
        == 1);
  CHECK(f.err.str()
        == "canopen: NMT reset-node not sent: TX queue full (frame "
           "dropped)\n");
  CHECK(f.device.state() == nmt_state::operational);
}
