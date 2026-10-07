// canopen sdo read|write|exec, against an emulated device.

#include "cli_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>

using namespace cannet::canopen;
using namespace cannet::canopen::test;
using namespace cannet::canopen::testing;
using namespace std::chrono_literals;

namespace tool = cannet::canopen::tool;

TEST_CASE("an object is given by key or by name", "[cli]")
{
  dictionary_view const drive = cannet_test::drive_dictionary;

  auto const by_key = tool::find_object("5000:01", drive);
  REQUIRE(by_key);
  CHECK(by_key->key == uptime_key);
  REQUIRE(by_key->entry != nullptr);
  CHECK(tool::label(*by_key) == "5000:01 watch::sys::uptime");

  auto const outside = tool::find_object("6000:1", drive);
  REQUIRE(outside);
  CHECK(outside->key == od_key{0x6000, 0x01});
  CHECK(outside->entry == nullptr);
  CHECK(tool::label(*outside) == "6000:01");

  auto const by_name = tool::find_object("config::drive::speed", drive);
  REQUIRE(by_name);
  CHECK(by_name->key == speed_key);

  CHECK(tool::find_object("config::drive::torque", drive).error()
        == "config::drive::torque: no such object in the dictionary");
  CHECK(tool::find_object("config::drive::speed", {}).error()
        == "config::drive::speed: an object by name needs a dictionary (-d)");
  for (auto const* text : {"5000",
                           "5000:100",
                           "10000:01",
                           "x:01",
                           "drive::speed",
                           "a::b::c::d",
                           "config::::speed"}) {
    CAPTURE(text);
    CHECK(
        tool::find_object(text, drive).error().starts_with("not an object: "));
  }
}

TEST_CASE("a type is given by its name in an OD file", "[cli]")
{
  CHECK(tool::parse_type("uint16") == od_value_type::uint16);
  CHECK(tool::parse_type("boolean") == od_value_type::boolean);
  CHECK(tool::parse_type("string") == od_value_type::string);
  CHECK_FALSE(tool::parse_type("bool"));
  CHECK_FALSE(tool::parse_type("float"));
}

TEST_CASE("sdo read prints a value as the dictionary types it", "[cli]")
{
  cli_fixture f;

  SECTION("a scalar")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "config::drive::speed",
                               std::nullopt,
                               f.console()))
          == 0);
    CHECK(f.out.str() == "1500\n");
  }
  SECTION("a float, by key")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "5000:01",
                               std::nullopt,
                               f.console()))
          == 0);
    CHECK(f.out.str() == "12.500000\n");
  }
  SECTION("a string")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "info::sys::device_name",
                               std::nullopt,
                               f.console()))
          == 0);
    CHECK(f.out.str() == "drive-01\n");
  }
  CHECK(f.err.str().empty());
}

TEST_CASE("sdo read takes the type from --type, before the dictionary's",
          "[cli]")
{
  cli_fixture f;

  SECTION("without a dictionary")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options({}),
                               "3000:01",
                               od_value_type::uint16,
                               f.console()))
          == 0);
    CHECK(f.out.str() == "1500\n");
  }
  SECTION("a type the device disagrees with")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "config::drive::speed",
                               od_value_type::uint32,
                               f.console()))
          == 1);
    CHECK(f.err.str()
          == "canopen: 3000:01 config::drive::speed: the device's object "
             "has another size\n");
  }
  SECTION("no type at all")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options({}),
                               "3000:01",
                               std::nullopt,
                               f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: 3000:01: the type is unknown: give --type, or a "
             "dictionary that has the object\n");
    CHECK(f.device.sdo_requests().empty());
  }
}

TEST_CASE("sdo read reports why it failed", "[cli]")
{
  cli_fixture f;

  SECTION("the device refuses")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "ctl::sys::clear_errors",
                               std::nullopt,
                               f.console()))
          == 1);
    CHECK(f.err.str().starts_with(
        "canopen: 2000:02 ctl::sys::clear_errors: SDO aborted by the "
        "device: "));
    CHECK(f.err.str().ends_with("(0x06010001)\n"));
  }
  SECTION("the device does not answer")
  {
    f.device.lose_answer = [](std::size_t) { return true; };
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "config::drive::speed",
                               std::nullopt,
                               f.console()))
          == 1);
    CHECK(f.err.str()
          == "canopen: 3000:01 config::drive::speed: no SDO answer in "
             "time\n");
  }
  SECTION("no such object in the dictionary")
  {
    CHECK(f.run(tool::sdo_read(f.host_bus,
                               f.options(),
                               "config::drive::torque",
                               std::nullopt,
                               f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: config::drive::torque: no such object in the "
             "dictionary\n");
    CHECK(f.device.sdo_requests().empty());
  }
  CHECK(f.out.str().empty());
}

TEST_CASE("sdo write sends a value parsed as the object's type", "[cli]")
{
  cli_fixture f;
  auto const speed = [&f] {
    return make_od_value(f.device.object(speed_key).value,
                         od_value_type::uint16);
  };

  SECTION("a value the type holds")
  {
    CHECK(f.run(tool::sdo_write(f.host_bus,
                                f.options(),
                                "config::drive::speed",
                                "1200",
                                std::nullopt,
                                f.console()))
          == 0);
    CHECK(speed() == od_value{std::uint16_t{1200}});
    CHECK(f.err.str().empty());
  }
  SECTION("a value it cannot hold is never sent")
  {
    CHECK(f.run(tool::sdo_write(f.host_bus,
                                f.options(),
                                "config::drive::speed",
                                "-5",
                                std::nullopt,
                                f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: 3000:01 config::drive::speed: -5 as uint16: value "
             "out of range for this type\n");
    CHECK(f.device.sdo_requests().empty());
    CHECK(speed() == od_value{std::uint16_t{1500}});
  }
  SECTION("a string cannot be written")
  {
    CHECK(f.run(tool::sdo_write(f.host_bus,
                                f.options(),
                                "info::sys::device_name",
                                "drive-02",
                                std::nullopt,
                                f.console()))
          == 2);
    CHECK(f.device.sdo_requests().empty());
  }
  SECTION("the device refuses a read-only object")
  {
    CHECK(f.run(tool::sdo_write(f.host_bus,
                                f.options(),
                                "watch::sys::uptime",
                                "1",
                                std::nullopt,
                                f.console()))
          == 1);
    CHECK(f.err.str().ends_with("(0x06010002)\n"));
  }
  CHECK(f.out.str().empty());
}

TEST_CASE("sdo exec runs a command and refuses any other object", "[cli]")
{
  cli_fixture f;

  SECTION("by name")
  {
    CHECK(f.run(tool::sdo_exec(f.host_bus,
                               f.options(),
                               "ctl::sys::clear_errors",
                               f.console()))
          == 0);
    CHECK(f.device.object(clear_errors_key).executed == 1);
  }
  SECTION("by key, without a dictionary")
  {
    CHECK(
        f.run(tool::sdo_exec(f.host_bus, f.options({}), "2000:02", f.console()))
        == 0);
    CHECK(f.device.object(clear_errors_key).executed == 1);
  }
  SECTION("a parameter, which the write would change")
  {
    CHECK(f.run(tool::sdo_exec(f.host_bus, f.options(), "3000:01", f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: 3000:01 config::drive::speed: not a command but a "
             "uint16 object; sdo write writes it\n");
    CHECK(f.device.sdo_requests().empty());
  }
}

TEST_CASE("a device at the CLI's own id is refused", "[cli]")
{
  cli_fixture f;
  auto options = f.options();
  options.host = cli_device_id;
  CHECK(f.run(tool::sdo_read(f.host_bus,
                             options,
                             "config::drive::speed",
                             std::nullopt,
                             f.console()))
        == 2);
  CHECK(f.err.str()
        == "canopen: node 1 is the CLI's own id; give it another with "
           "--host-id\n");
  CHECK(f.device.sdo_requests().empty());
}

TEST_CASE("a stop lets the string read in flight run to its NUL", "[cli]")
{
  cli_fixture f;
  f.device.answer_delay = 5ms;
  auto const status = f.start(tool::sdo_read(f.host_bus,
                                             f.options(),
                                             "info::sys::device_name",
                                             std::nullopt,
                                             f.console()));
  REQUIRE(run_until(f.io, [&] { return !f.device.sdo_requests().empty(); }));
  f.stop.emit(boost::asio::cancellation_type::terminal);
  REQUIRE(run_until(f.io, [&] { return status->has_value(); }));
  CHECK(*status == 0);
  CHECK(f.out.str() == "drive-01\n");
  // "driv", "e-01", and the word of the NUL.
  CHECK(f.device.sdo_requests().size() == 3);
}
