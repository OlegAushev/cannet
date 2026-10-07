// canopen watch, against an emulated device.

#include "cli_fixture.hpp"

#include <canopen/od_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using namespace cannet::canopen;
using namespace cannet::canopen::test;
using namespace cannet::canopen::testing;
using namespace std::chrono_literals;

namespace tool = cannet::canopen::tool;

namespace {

constexpr std::string_view clear_screen = "\x1b[H\x1b[2J";

std::vector<std::string> lines_of(std::string_view text)
{
  std::vector<std::string> lines;
  while (!text.empty()) {
    auto const end = text.find('\n');
    lines.emplace_back(text.substr(0, end));
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  }
  return lines;
}

// Printed table after table, each ended by a blank line.
std::vector<std::vector<std::string>> tables_of(std::string_view text)
{
  std::vector<std::vector<std::string>> tables(1);
  for (auto& line : lines_of(text)) {
    if (line.empty()) {
      tables.emplace_back();
    }
    else {
      tables.back().push_back(std::move(line));
    }
  }
  if (tables.back().empty()) {
    tables.pop_back();
  }
  return tables;
}

std::size_t count_of(std::string_view text, std::string_view what)
{
  std::size_t count = 0;
  for (auto at = text.find(what); at != std::string_view::npos;
       at = text.find(what, at + what.size())) {
    ++count;
  }
  return count;
}

} // namespace

TEST_CASE("watch prints a table per pass, and ends after --count passes",
          "[cli]")
{
  cli_fixture f;
  CHECK(f.run(tool::watch(f.host_bus,
                          f.options(),
                          {.period = 20ms, .count = 2},
                          f.console()))
        == 0);
  auto const tables = tables_of(f.out.str());
  REQUIRE(tables.size() == 2);
  for (std::size_t i = 0; i < tables.size(); ++i) {
    CAPTURE(i);
    REQUIRE(tables[i].size() == 3);
    CHECK(tables[i][0].starts_with(std::format("node 1, pass {}, ", i + 1)));
    CHECK(tables[i][0].ends_with(" s"));
    CHECK(tables[i][1] == "sys::uptime  = 12.500000 s");
    CHECK(tables[i][2] == "elec::Vdc    = 540.000000 V");
  }
  CHECK(f.err.str().empty());
  CHECK(f.out.str().find(clear_screen) == std::string::npos);
}

TEST_CASE("watch shows why an object has no value", "[cli]")
{
  cli_fixture f;
  // Requests alternate between uptime and Vdc: Vdc's go unanswered.
  f.device.lose_answer = [](std::size_t request) { return request % 2 == 1; };
  CHECK(f.run(tool::watch(f.host_bus,
                          f.options(),
                          {.period = 20ms, .count = 1},
                          f.console()))
        == 0);
  auto const tables = tables_of(f.out.str());
  REQUIRE(tables.size() == 1);
  REQUIRE(tables[0].size() == 3);
  CHECK(tables[0][1] == "sys::uptime  = 12.500000 s");
  CHECK(tables[0][2] == "elec::Vdc    ? no SDO answer in time");
}

TEST_CASE("a pass the transport cannot send ends at once", "[cli]")
{
  cli_fixture f;
  f.host_bus.fail_next_send(transport_error::send_failed);
  CHECK(f.run(tool::watch(f.host_bus,
                          f.options(),
                          {.period = 20ms, .count = 1},
                          f.console()))
        == 0);
  auto const tables = tables_of(f.out.str());
  REQUIRE(tables.size() == 1);
  REQUIRE(tables[0].size() == 3);
  CHECK(tables[0][1] == "sys::uptime  ? SDO request not sent: send failed");
  CHECK(tables[0][2] == "elec::Vdc    ? not read");
}

TEST_CASE("watch polls the objects it is given, and only watch objects",
          "[cli]")
{
  cli_fixture f;

  SECTION("one of them")
  {
    CHECK(f.run(tool::watch(
              f.host_bus,
              f.options(),
              {.objects = {"watch::elec::Vdc"}, .period = 20ms, .count = 2},
              f.console()))
          == 0);
    auto const tables = tables_of(f.out.str());
    REQUIRE(tables.size() == 2);
    CHECK(tables[1].size() == 2);
    CHECK(tables[1][1] == "elec::Vdc  = 540.000000 V");
    REQUIRE_FALSE(f.device.sdo_requests().empty());
    for (auto const key : f.device.sdo_requests()) {
      CHECK(key == vdc_key);
    }
  }
  SECTION("a parameter")
  {
    CHECK(f.run(tool::watch(f.host_bus,
                            f.options(),
                            {.objects = {"config::drive::speed"}},
                            f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: 3000:01 config::drive::speed: not watched: the watch "
             "polls the readable scalars of the watch category\n");
    CHECK(f.device.sdo_requests().empty());
  }
  SECTION("no such object")
  {
    CHECK(f.run(tool::watch(f.host_bus,
                            f.options(),
                            {.objects = {"watch::elec::Idc"}},
                            f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: watch::elec::Idc: no such object in the dictionary\n");
    CHECK(f.device.sdo_requests().empty());
  }
}

TEST_CASE("watch needs a dictionary with something to watch", "[cli]")
{
  cli_fixture f;

  SECTION("no dictionary")
  {
    CHECK(f.run(tool::watch(f.host_bus, f.options({}), {}, f.console())) == 2);
    CHECK(f.err.str() == "canopen: watch needs the node's dictionary (-d)\n");
  }
  SECTION("no watch category")
  {
    auto const loaded = parse_od_file(
        "cannet-od 1\n"
        "1008:00 info sys device_name - const string\n");
    REQUIRE(loaded);
    CHECK(f.run(tool::watch(f.host_bus, f.options(*loaded), {}, f.console()))
          == 2);
    CHECK(f.err.str()
          == "canopen: nothing to watch: the dictionary's watch category has "
             "no readable scalar\n");
  }
  SECTION("no period")
  {
    CHECK(
        f.run(
            tool::watch(f.host_bus, f.options(), {.period = 0ms}, f.console()))
        == 2);
    CHECK(f.err.str() == "canopen: the period must be above 0 ms\n");
  }
  CHECK(f.device.sdo_requests().empty());
}

TEST_CASE("on a terminal, watch draws each pass over the screen", "[cli]")
{
  cli_fixture f;
  CHECK(f.run(tool::watch(f.host_bus,
                          f.options(),
                          {.period = 100ms, .count = 2, .redraw = true},
                          f.console()))
        == 0);
  auto const out = f.out.str();
  REQUIRE(out.starts_with(clear_screen));
  // Once a pass.
  CHECK(count_of(out, clear_screen) == 2);
  auto const last = lines_of(
      out.substr(out.rfind(clear_screen) + clear_screen.size()));
  REQUIRE(last.size() == 3);
  CHECK(last[0].starts_with("node 1, pass 2, "));
  CHECK(last[1] == "sys::uptime  = 12.500000 s");
  CHECK(last[2] == "elec::Vdc    = 540.000000 V");
}

TEST_CASE("on a terminal, a pass that runs over is drawn as it goes", "[cli]")
{
  cli_fixture f;
  f.device.lose_answer = [](std::size_t) { return true; };
  CHECK(f.run(tool::watch(f.host_bus,
                          f.options(),
                          {.period = 20ms, .count = 1, .redraw = true},
                          f.console()))
        == 0);
  auto const out = f.out.str();
  REQUIRE(count_of(out, clear_screen) == 2);
  // At the first timeout, two periods on, and at the end of the pass.
  auto const first = lines_of(
      out.substr(clear_screen.size(),
                 out.rfind(clear_screen) - clear_screen.size()));
  REQUIRE(first.size() == 3);
  CHECK(first[1] == "sys::uptime  ? no SDO answer in time");
  CHECK(first[2] == "elec::Vdc    ? not read");
  auto const last = lines_of(
      out.substr(out.rfind(clear_screen) + clear_screen.size()));
  REQUIRE(last.size() == 3);
  CHECK(last[2] == "elec::Vdc    ? no SDO answer in time");
}

TEST_CASE("a stop ends watch", "[cli]")
{
  cli_fixture f;
  auto const status = f.start(
      tool::watch(f.host_bus, f.options(), {.period = 20ms}, f.console()));
  REQUIRE(run_until(f.io, [&] { return tables_of(f.out.str()).size() >= 3; }));
  CHECK_FALSE(status->has_value());
  f.stop.emit(boost::asio::cancellation_type::terminal);
  REQUIRE(run_until(f.io, [&] { return status->has_value(); }));
  CHECK(*status == 0);
  auto const requests = f.device.sdo_requests().size();
  run_for(f.io, 60ms);
  CHECK(f.device.sdo_requests().size() == requests);
}
