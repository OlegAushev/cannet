// canopen od-verify, against an emulated device.

#include "cli_fixture.hpp"

#include <canopen/od_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

using namespace cannet::canopen;
using namespace cannet::canopen::test;
using namespace std::chrono_literals;

namespace tool = cannet::canopen::tool;

TEST_CASE("od-verify finds a device as its dictionary has it", "[cli]")
{
  cli_fixture f;
  CHECK(f.run(tool::od_verify(f.host_bus, f.options(), f.console())) == 0);
  CHECK(f.out.str() == "5 objects: 5 match, 0 differ, 0 not checked\n");
  CHECK(f.err.str().empty());
  // A read each, and the string's three: "driv", "e-01", the NUL.
  CHECK(f.device.sdo_requests().size() == 7);
}

TEST_CASE("od-verify reports each object the device has otherwise", "[cli]")
{
  cli_fixture f;
  using enum od_value_type;
  f.device.add_object(device_name_key,
                      {.type = uint8,
                       .access = od_access::const_,
                       .value = to_raw(std::uint8_t{7})});
  f.device.add_object(clear_errors_key,
                      {.type = exec, .access = od_access::rw});
  f.device.add_object(speed_key,
                      {.type = uint32,
                       .access = od_access::rw,
                       .value = to_raw(std::uint32_t{1500})});
  f.device.add_object(uptime_key, {.type = float32, .access = od_access::wo});
  // drive.od, and an object the device lacks.
  auto const loaded = parse_od_file(
      "cannet-od 1\n"
      "1008:00 info   sys   device_name  -    const string\n"
      "2000:02 ctl    sys   clear_errors -    wo    exec\n"
      "3000:01 config drive speed        rpm  rw    uint16\n"
      "5000:01 watch  sys   uptime       s    ro    float32\n"
      "5000:02 watch  elec  Vdc          V    ro    float32\n"
      "5000:03 watch  elec  Idc          A    ro    float32\n");
  REQUIRE(loaded);

  CHECK(f.run(tool::od_verify(f.host_bus, f.options(*loaded), f.console()))
        == 1);
  CHECK(f.out.str()
        == "1008:00 info::sys::device_name: differs: not a string on the "
           "device: an answer of another size than 4 bytes\n"
           "2000:02 ctl::sys::clear_errors: differs: readable on the "
           "device, wo in the dictionary\n"
           "3000:01 config::drive::speed: differs: the device's object has "
           "another size than uint16\n"
           "5000:01 watch::sys::uptime: differs: write-only on the device, "
           "ro in the dictionary\n"
           "5000:03 watch::elec::Idc: differs: the device has no such "
           "object\n"
           "6 objects: 1 match, 5 differ, 0 not checked\n");
  CHECK(f.err.str().empty());
}

TEST_CASE("od-verify reports an object it could not check", "[cli]")
{
  cli_fixture f;
  // The fourth request, for 3000:01 after the string's three, is lost.
  f.device.lose_answer = [](std::size_t request) { return request == 4; };
  CHECK(f.run(tool::od_verify(f.host_bus, f.options(), f.console())) == 1);
  CHECK(f.out.str()
        == "3000:01 config::drive::speed: not checked: no SDO answer in "
           "time\n"
           "5 objects: 4 match, 0 differ, 1 not checked\n");
}

TEST_CASE("od-verify gives up on a device that does not answer", "[cli]")
{
  cli_fixture f;
  f.device.lose_answer = [](std::size_t) { return true; };
  CHECK(f.run(tool::od_verify(f.host_bus, f.options(), f.console())) == 1);
  CHECK(f.out.str()
        == "1008:00 info::sys::device_name: not checked: no SDO answer in "
           "time\n"
           "2000:02 ctl::sys::clear_errors: not checked: no SDO answer in "
           "time\n"
           "3000:01 config::drive::speed: not checked: no SDO answer in "
           "time\n"
           "5 objects: 0 match, 0 differ, 5 not checked\n");
  CHECK(f.err.str()
        == "canopen: the device does not answer; the 2 objects left are "
           "not checked\n");
  CHECK(f.device.sdo_requests().size() == 3);
}

TEST_CASE("od-verify gives up when the transport cannot send", "[cli]")
{
  cli_fixture f;
  f.host_bus.fail_next_send(transport_error::send_failed);
  CHECK(f.run(tool::od_verify(f.host_bus, f.options(), f.console())) == 1);
  CHECK(f.out.str()
        == "1008:00 info::sys::device_name: not checked: SDO request not "
           "sent: send failed\n"
           "5 objects: 0 match, 0 differ, 5 not checked\n");
  CHECK(f.err.str()
        == "canopen: SDO request not sent: send failed; the 4 objects left "
           "are not checked\n");
  CHECK(f.device.sdo_requests().empty());
}

TEST_CASE("a stop ends od-verify after the object it reads", "[cli]")
{
  cli_fixture f;
  f.device.answer_delay = 5ms;
  auto const status = f.start(
      tool::od_verify(f.host_bus, f.options(), f.console()));
  REQUIRE(run_until(f.io, [&] { return !f.device.sdo_requests().empty(); }));
  f.stop.emit(boost::asio::cancellation_type::terminal);
  REQUIRE(run_until(f.io, [&] { return status->has_value(); }));
  CHECK(*status == 1);
  // The string, to its NUL, and nothing after it.
  CHECK(f.device.sdo_requests().size() == 3);
  CHECK(f.out.str() == "5 objects: 1 match, 0 differ, 4 not checked\n");
  CHECK(f.err.str()
        == "canopen: stopped; the 4 objects left are not checked\n");
}

TEST_CASE("od-verify needs the node's dictionary", "[cli]")
{
  cli_fixture f;
  CHECK(f.run(tool::od_verify(f.host_bus, f.options({}), f.console())) == 2);
  CHECK(f.err.str() == "canopen: od-verify needs the node's dictionary (-d)\n");
  CHECK(f.device.sdo_requests().empty());
}
