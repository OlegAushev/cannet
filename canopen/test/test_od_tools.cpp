#include "od_tools.hpp"

#include <canopen/od_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace cannet::canopen;
using namespace cannet::canopen::tool;

namespace {

// As an emblib firmware writes its table: every kind of binding, a row
// over three lines, rows a comment hides.
constexpr std::string_view firmware = R"cpp(
#include "od.hpp"
// clang-format off
constexpr od_row<context> rows[] = {
{{0x1008, 0x00}, "info", "sys", "device_name", "", string,  od_text<get_device_name>},
{{0x1011, 0x04}, "ctl", "sys", "restore_default_parameter", "", exec, od_restore_default},
{{0x2000, 0x02}, "ctl", "sys", "clear_errors", "", exec,    od_exec<clear_errors>},
/* {{0x9999, 0x01}, "x", "y", "z", "", uint8, od_ro<z>}, */
// {{0x9999, 0x02}, "x", "y", "z", "", uint8, od_ro<z>},
{{0x2001, 0x03}, "ctl", "drive", "set_angle_correction", "°", float32, od_wo<set_angle_correction>},
{{0x3000, 0x09}, "config", "nvm", "valid", "", boolean, od_ro<nvm::valid>},
{{0x3002, 0x02},
 "config", "drive", "torque_slope", "pu/s",
 float32, param::rw<"model.torque_slope">},
{{0x3008, 0x01}, "config", "phase_current_sensor", "gain_a", "A/V", float32, od_const<phase_a_gain>},
{{0x5000, 0x01}, "watch", "sys", "uptime", "s", float32, od_ro<uptime>},
{{0x5000, 0xF1}, "watch", "logger", "ch0", "", float32, od_ro<logger<probe_channel::ch0>>},
{{0x3004, 0x01}, "config", "motor", "pole_pairs", "", od_value_type::int32, param::rw<"motor.p">},
};
)cpp";

struct expected_object {
  od_key key;
  std::string_view unit;
  od_access access;
  od_value_type type;
};

tool_error error_of(std::string_view source, emblib_options const& options = {})
{
  auto const made = od_file_from_emblib(source, "od.cpp", options);
  REQUIRE_FALSE(made);
  return made.error();
}

} // namespace

TEST_CASE("an OD file is made from an emblib firmware's table", "[od_tools]")
{
  auto const made = od_file_from_emblib(firmware, "od.cpp");
  REQUIRE(made);
  CHECK(made->starts_with("# Made by canopen od from-emblib from od.cpp.\n"
                          "cannet-od 1\n"
                          "watch_category  watch\n"
                          "config_category config\n"));
  auto const loaded = parse_od_file(*made);
  REQUIRE(loaded);
  dictionary_view const view = *loaded;

  using enum od_access;
  using enum od_value_type;
  std::vector<expected_object> const objects = {
      {{0x1008, 0x00}, "", const_, string},
      {{0x1011, 0x04}, "", wo, exec},
      {{0x2000, 0x02}, "", wo, exec},
      {{0x2001, 0x03}, "°", wo, float32},
      {{0x3000, 0x09}, "", ro, boolean},
      {{0x3002, 0x02}, "pu/s", rw, float32},
      {{0x3004, 0x01}, "", rw, int32},
      {{0x3008, 0x01}, "A/V", const_, float32},
      {{0x5000, 0x01}, "s", ro, float32},
      {{0x5000, 0xF1}, "", ro, float32},
  };
  REQUIRE(view.size() == objects.size());
  for (std::size_t i = 0; i < objects.size(); ++i) {
    CAPTURE(i);
    auto const& e = view.entries()[i];
    CHECK(e.key == objects[i].key);
    CHECK(e.object.unit == objects[i].unit);
    CHECK(e.object.access == objects[i].access);
    CHECK(e.object.type == objects[i].type);
  }
  CHECK(view.find("config", "drive", "torque_slope") != nullptr);
}

TEST_CASE("the categories of an imported dictionary can be chosen",
          "[od_tools]")
{
  auto const made = od_file_from_emblib(
      firmware,
      "od.cpp",
      {.watch_category = "", .config_category = "ctl"});
  REQUIRE(made);
  CHECK_FALSE(made->contains("watch_category"));
  CHECK(made->contains("config_category ctl\n"));

  // A category no object belongs to.
  CHECK(error_of(firmware, {.watch_category = "telemetry"}).message
        == "a category no object belongs to");
}

TEST_CASE("a firmware's table that cannot become an OD file is refused at "
          "its line",
          "[od_tools]")
{
  auto const binding = error_of("\n\n"
                                "{{0x5000, 0x02}, \"watch\", \"sys\", \"x\", "
                                "\"\", float32, my_binding<x>},\n");
  CHECK(binding.line == 3);
  CHECK(binding.message.contains("my_binding"));

  auto const type = error_of("{{0x5000, 0x02}, \"watch\", \"sys\", \"x\", "
                             "\"\", float64, od_ro<x>},\n");
  CHECK(type.line == 1);
  CHECK(type.message.contains("float64"));

  auto const blank = error_of("{{0x5000, 0x02}, \"watch\", \"sys\", \"x\", "
                              "\"m s\", float32, od_ro<x>},\n");
  CHECK(blank.line == 1);
  CHECK(blank.message.contains("m s"));

  auto const duplicate = error_of(
      "{{0x5000, 0x01}, \"watch\", \"sys\", \"x\", \"\", float32, od_ro<x>},\n"
      "\n"
      "{{0x5000, 0x01}, \"watch\", \"sys\", \"y\", \"\", float32, od_ro<y>},\n");
  CHECK(duplicate.line == 3);
  CHECK(duplicate.message == "two objects at one key");

  auto const none = error_of("int main() {}\n");
  CHECK(none.line == 0);
  CHECK(none.message == "no dictionary rows");

  CHECK(error_of("/* never closed\n").message == "a comment that never ends");
}

TEST_CASE("a header compiles an OD file's dictionary in", "[od_tools]")
{
  auto const loaded = parse_od_file("cannet-od 1\n"
                                    "watch_category watch\n"
                                    "1008:00 info sys device_name - const "
                                    "string\n"
                                    "5000:01 watch sys uptime s ro float32\n");
  REQUIRE(loaded);
  auto const header = cpp_header(*loaded,
                                 "drive_dictionary",
                                 "app::od",
                                 "drive.od");
  REQUIRE(header);
  CHECK(header->starts_with("// Made by `canopen od header` from drive.od"));
  CHECK(header->contains("namespace app::od {\n"));
  CHECK(header->contains("inline constexpr auto drive_dictionary = "
                         "cannet::canopen::dictionary{\n"));
  CHECK(header->contains("{.watch_category = \"watch\", .config_category = "
                         "\"\"},\n"));
  CHECK(header->contains("{{0x1008, 0x00}, {\"info\", \"sys\", "
                         "\"device_name\", \"\", "
                         "cannet::canopen::od_access::const_, "
                         "cannet::canopen::od_value_type::string}},\n"));
  CHECK(header->contains("} // namespace app::od\n"));

  CHECK_FALSE(cpp_header(*loaded, "1st", "", "drive.od"));
  CHECK_FALSE(cpp_header(*loaded, "drive", "app::1od", "drive.od"));
  auto const empty = parse_od_file("cannet-od 1\n");
  REQUIRE(empty);
  auto const nothing = cpp_header(*empty, "drive", "", "drive.od");
  REQUIRE_FALSE(nothing);
  CHECK(nothing.error().message == "the dictionary is empty");
}
