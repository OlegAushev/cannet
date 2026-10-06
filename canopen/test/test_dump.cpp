// canopen dump: what it makes of each frame, and the command on a bus.

#include "cli_fixture.hpp"

#include <canopen/sdo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace cannet::canopen;
using namespace cannet::canopen::test;

namespace tool = cannet::canopen::tool;

namespace {

constexpr auto node_1 = node_id::literal(1);
constexpr auto node_2 = node_id::literal(2);

std::optional<std::string> described(can_frame const& frame,
                                     tool::dump_options const& options = {})
{
  return tool::describe(frame, options);
}

std::string with_drive(can_frame const& frame)
{
  auto const line = tool::describe(
      frame,
      {.dictionary = cannet_test::drive_dictionary});
  REQUIRE(line);
  return *line;
}

can_frame sdo_request(expedited_sdo const& sdo)
{
  return make_frame(0x601, CAN_MAX_DLEN, to_payload(sdo));
}

// A device's expedited answer to a read: `size` bytes of `data`.
can_frame read_answer(od_key key, expedited_sdo_data data, std::uint32_t size)
{
  expedited_sdo sdo;
  sdo.cs = sdo_cs_codes::server_init_read;
  sdo.expedited_transfer = 1;
  sdo.data_size_indicated = 1;
  sdo.data_empty_bytes = (4 - size) & 0x3;
  sdo.index = key.index;
  sdo.subindex = key.subindex;
  sdo.data = data;
  return make_frame(0x581, CAN_MAX_DLEN, to_payload(sdo));
}

can_frame write_answer(od_key key)
{
  expedited_sdo sdo;
  sdo.cs = sdo_cs_codes::server_init_write;
  sdo.index = key.index;
  sdo.subindex = key.subindex;
  return make_frame(0x581, CAN_MAX_DLEN, to_payload(sdo));
}

can_frame abort_answer(od_key key, sdo_abort_code code)
{
  abort_sdo abort;
  abort.index = key.index;
  abort.subindex = key.subindex;
  abort.error_code = std::to_underlying(code);
  return make_frame(0x581, CAN_MAX_DLEN, to_payload(abort));
}

} // namespace

TEST_CASE("dump decodes NMT, SYNC, heartbeats and emergencies", "[cli]")
{
  CHECK(described(
            make_nmt_frame({.command = nmt_command::start, .target = node_1}))
        == "000            NMT start node 1");
  CHECK(described(make_nmt_frame({.command = nmt_command::reset_node}))
        == "000            NMT reset-node all");
  CHECK(described(make_sync_frame()) == "080            SYNC");
  CHECK(described(make_heartbeat_frame(node_1, nmt_state::pre_operational))
        == "701  node 1    heartbeat pre-operational");
  CHECK(described(make_heartbeat_frame(node_1, nmt_state::initializing))
        == "701  node 1    boot-up");
  CHECK(described(make_emcy_frame(node_1,
                                  {.error_code = 0x2310,
                                   .error_register = 0x02,
                                   .manufacturer = {1, 2, 3, 4, 5}}))
        == "081  node 1    EMCY 2310, register 02, manufacturer 01 02 03 04 "
           "05");
  CHECK(described(make_emcy_frame(node_1, {}))
        == "081  node 1    EMCY error reset, register 00");
}

TEST_CASE("dump shows PDOs, and other frames as they are", "[cli]")
{
  CHECK(described(make_frame(0x181, 8, {1, 2, 3, 4, 5, 6, 7, 8}))
        == "181  node 1    TPDO1 [8] 01 02 03 04 05 06 07 08");
  CHECK(described(make_frame(0x57F, 2, {0xAA, 0xBB}))
        == "57F  node 127  RPDO4 [2] AA BB");
  // Not in the predefined connection set.
  CHECK(described(make_frame(0x123, 2, {1, 2})) == "123            [2] 01 02");
  CHECK(described(make_frame(0x781, 0, {})) == "781            [0]");

  can_frame extended = make_frame(0x12345678 | CAN_EFF_FLAG, 1, {0xFF});
  CHECK(described(extended) == "12345678            [1] FF");
  can_frame remote = make_frame(0x701 | CAN_RTR_FLAG, 1, {});
  CHECK(described(remote) == "701            remote request [1]");
}

TEST_CASE("dump decodes SDO, by the dictionary when it has the object", "[cli]")
{
  auto const speed_read = sdo_request(make_sdo_read_request(0x3000, 0x01));
  CHECK(with_drive(speed_read)
        == "601  node 1    SDO read 3000:01 config::drive::speed");
  CHECK(described(speed_read) == "601  node 1    SDO read 3000:01");

  auto const speed = read_answer(speed_key, to_raw(std::uint16_t{1500}), 2);
  CHECK(with_drive(speed)
        == "581  node 1    SDO 3000:01 config::drive::speed = 1500 rpm");
  CHECK(described(speed) == "581  node 1    SDO 3000:01 = [2] DC 05");
  CHECK(with_drive(read_answer(speed_key, to_raw(std::uint32_t{1500}), 4))
        == "581  node 1    SDO 3000:01 config::drive::speed = [4] DC 05 00 "
           "00 (the dictionary has uint16)");
  CHECK(with_drive(read_answer(uptime_key, to_raw(12.5f), 4))
        == "581  node 1    SDO 5000:01 watch::sys::uptime = 12.500000 s");

  // A string, four characters at a time.
  CHECK(with_drive(read_answer(device_name_key, {'e', '-', '0', '1'}, 4))
        == "581  node 1    SDO 1008:00 info::sys::device_name = \"e-01\"");
  CHECK(with_drive(read_answer(device_name_key, {'r', 0, 0, 0}, 4))
        == "581  node 1    SDO 1008:00 info::sys::device_name = "
           "\"r\\0\\0\\0\"");

  CHECK(
      with_drive(sdo_request(
          make_sdo_write_request(0x3000, 0x01, to_raw(std::uint16_t{1200}), 2)))
      == "601  node 1    SDO write 3000:01 config::drive::speed = 1200 "
         "rpm");
  CHECK(with_drive(write_answer(speed_key))
        == "581  node 1    SDO written 3000:01 config::drive::speed");
  CHECK(with_drive(sdo_request(make_sdo_write_request(0x2000, 0x02, {}, 4)))
        == "601  node 1    SDO exec 2000:02 ctl::sys::clear_errors");
  CHECK(with_drive(sdo_request(
            make_sdo_write_request(0x1011, 0x04, {0x00, 0x30, 0x01, 0x00}, 4)))
        == "601  node 1    SDO restore default 3000:01 "
           "config::drive::speed");
  CHECK(with_drive(abort_answer(uptime_key, sdo_abort_code::write_to_read_only))
        == "581  node 1    SDO abort 5000:01 watch::sys::uptime: attempt to "
           "write a read-only object (0x06010002)");
}

TEST_CASE("dump of one node shows its frames and those for every node", "[cli]")
{
  tool::dump_options const options{.node = node_1};
  CHECK(
      described(make_heartbeat_frame(node_1, nmt_state::operational), options));
  CHECK_FALSE(
      described(make_heartbeat_frame(node_2, nmt_state::operational), options));
  CHECK(described(
      make_nmt_frame({.command = nmt_command::stop, .target = node_1}),
      options));
  CHECK(described(make_nmt_frame({.command = nmt_command::stop}), options));
  CHECK_FALSE(described(
      make_nmt_frame({.command = nmt_command::stop, .target = node_2}),
      options));
  CHECK(described(make_sync_frame(), options));
  CHECK_FALSE(described(make_frame(0x123, 2, {1, 2}), options));
}

TEST_CASE("dump prints what goes on the bus until it is stopped", "[cli]")
{
  cli_fixture f;
  loopback_transport monitor{f.bus};
  std::ostringstream dumped;
  std::ostringstream errors;
  boost::asio::cancellation_signal stop_dump;
  auto const dumping = f.start(
      tool::dump(monitor,
                 {.dictionary = cannet_test::drive_dictionary},
                 {.out = dumped, .err = errors}),
      stop_dump);

  CHECK(f.run(tool::sdo_read(f.host_bus,
                             f.options(),
                             "config::drive::speed",
                             std::nullopt,
                             f.console()))
        == 0);
  REQUIRE(run_until(f.io, [&] {
    return std::ranges::count(dumped.str(), '\n') == 2;
  }));

  std::vector<std::string> lines;
  std::istringstream in{dumped.str()};
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() == 2);
  for (auto const& line : lines) {
    // The seconds since the start, then the frame.
    CAPTURE(line);
    auto const time = line.substr(0, 10);
    auto const first = time.find_first_not_of(' ');
    REQUIRE(first != std::string::npos);
    double seconds = -1;
    auto const [ptr, ec] = std::from_chars(time.data() + first,
                                           time.data() + time.size(),
                                           seconds);
    CHECK(ec == std::errc{});
    CHECK(ptr == time.data() + time.size());
    CHECK(seconds >= 0);
    CHECK(line.substr(10, 2) == "  ");
  }
  CHECK(lines[0].substr(12)
        == "601  node 1    SDO read 3000:01 config::drive::speed");
  CHECK(lines[1].substr(12)
        == "581  node 1    SDO 3000:01 config::drive::speed = 1500 rpm");

  CHECK_FALSE(dumping->has_value());
  stop_dump.emit(boost::asio::cancellation_type::terminal);
  REQUIRE(run_until(f.io, [&] { return dumping->has_value(); }));
  CHECK(*dumping == 0);
  CHECK(errors.str().empty());
}
