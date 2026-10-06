#include <canopen/od_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace cannet::canopen;

namespace {

using enum od_access;
using enum od_value_type;

constexpr auto drive_dictionary = dictionary{
    {.watch_category = "watch", .config_category = "config"},
    {{{0x5000, 0x01}, {"watch", "sys", "uptime", "s", ro, float32}},
     {{0x5000, 0x21}, {"watch", "temp", "Tmcu", "°C", ro, float32}},
     {{0x3003, 0x01}, {"config", "protection", "uvp_dc", "V", rw, float32}},
     {{0x3004, 0x01}, {"config", "motor", "pole_pairs", "", rw, int32}},
     {{0x1008, 0x00}, {"info", "sys", "device_name", "", const_, string}},
     {{0x2000, 0x02}, {"ctl", "sys", "clear_errors", "", wo, exec}}}};

bool same(dictionary_view a, dictionary_view b)
{
  if (a.size() != b.size()
      || a.config().watch_category != b.config().watch_category
      || a.config().config_category != b.config().config_category) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto const& x = a.entries()[i];
    auto const& y = b.entries()[i];
    if (x.key != y.key
        || x.object.category != y.object.category
        || x.object.subcategory != y.object.subcategory
        || x.object.name != y.object.name
        || x.object.unit != y.object.unit
        || x.object.access != y.object.access
        || x.object.type != y.object.type) {
      return false;
    }
  }
  return true;
}

od_file_error error_of(std::string text)
{
  auto const loaded = parse_od_file(std::move(text));
  REQUIRE_FALSE(loaded);
  return loaded.error();
}

} // namespace

TEST_CASE("every od_file_error kind has a description and a name", "[od_file]")
{
  using enum od_file_error::kind;
  for (auto const k : {version,
                       syntax,
                       bad_key,
                       bad_field,
                       unknown_access,
                       unknown_type,
                       duplicate_category,
                       duplicate_key,
                       duplicate_name,
                       empty_name,
                       unknown_category,
                       too_many_objects}) {
    CHECK(to_string(k) != "unknown error");
    CHECK(name(k) != "unknown");
  }
  CHECK(name(duplicate_key) == "duplicate_key");
  CHECK(to_string(od_file_error{.reason = duplicate_key, .line = 12})
        == "line 12: two objects at one key");
}

TEST_CASE("a dictionary goes through an OD file and back unchanged",
          "[od_file]")
{
  auto const text = to_od_file(drive_dictionary);
  REQUIRE(text);
  CHECK(text->starts_with("cannet-od 1\n"
                          "watch_category  watch\n"
                          "config_category config\n"
                          "\n"
                          "1008:00  info    sys         device_name   -   "
                          "const  string\n"));
  // Columns are aligned by code points: "°C" takes two.
  CHECK(text->contains("5000:21  watch   temp        Tmcu          °C  "
                       "ro     float32\n"));

  auto const loaded = parse_od_file(*text);
  REQUIRE(loaded);
  CHECK(same(*loaded, drive_dictionary));
  CHECK(to_od_file(*loaded) == text);
}

TEST_CASE("an OD file takes comments, blank lines, tabs, CRLF and any order",
          "[od_file]")
{
  auto const loaded = parse_od_file(
      "# the drive, by hand\r\n"
      "\n"
      "   cannet-od 1\r\n"
      "3004:01 config motor pole_pairs - rw int32\n"
      "\t# a comment after a blank\n"
      "5000:01\twatch\tsys\tuptime\ts\tro\tfloat32\n"
      "config_category config\n"
      "watch_category watch");
  REQUIRE(loaded);
  dictionary_view const view = *loaded;
  CHECK(view.size() == 2);
  CHECK(view.config().watch_category == "watch");
  auto const* pole_pairs = view.find({0x3004, 0x01});
  REQUIRE(pole_pairs != nullptr);
  CHECK(pole_pairs->object.unit.empty()); // "-"
  CHECK(pole_pairs->object.type == int32);
  CHECK(view.find("watch", "sys", "uptime") == view.find({0x5000, 0x01}));
  CHECK(std::ranges::distance(view.in_category("config")) == 1);
}

TEST_CASE("a loaded dictionary's view survives a move", "[od_file]")
{
  auto loaded = parse_od_file(*to_od_file(drive_dictionary));
  REQUIRE(loaded);
  auto moved = std::move(*loaded);
  auto other = std::move(moved);
  CHECK(same(other, drive_dictionary));
  CHECK(other.view().find("config", "motor", "pole_pairs") != nullptr);
}

TEST_CASE("a malformed OD file is refused with the line to blame", "[od_file]")
{
  using enum od_file_error::kind;
  auto const header = std::string{"cannet-od 1\n"};
  auto const at = [](od_file_error::kind reason, std::size_t line) {
    return od_file_error{.reason = reason, .line = line};
  };

  CHECK(error_of("") == at(version, 0));
  CHECK(error_of("# only a comment\n") == at(version, 0));
  CHECK(error_of("cannet-od 2\n") == at(version, 1));
  CHECK(error_of("\n5000:01 watch sys uptime s ro float32\n")
        == at(version, 2));

  CHECK(error_of(header + "5000:01 watch sys uptime s ro\n") == at(syntax, 2));
  CHECK(error_of(header + "watch watch\n") == at(syntax, 2));
  CHECK(error_of(header + "watch_category watch extra\n") == at(syntax, 2));

  for (auto const* key : {"5000-01",
                          "50000:01",
                          "0x5000:01",
                          "5000:100",
                          "5000:",
                          ":01",
                          "50g0:01"}) {
    CAPTURE(key);
    CHECK(error_of(header
                   + std::format("{} watch sys uptime s ro float32\n", key))
          == at(bad_key, 2));
  }
  CHECK(error_of(header + "5000:01 watch sys up\"time s ro float32\n")
        == at(bad_field, 2));
  CHECK(error_of(header + "5000:01 watch sys uptime s rx float32\n")
        == at(unknown_access, 2));
  CHECK(error_of(header + "5000:01 watch sys uptime s ro float64\n")
        == at(unknown_type, 2));
  CHECK(error_of(header + "watch_category watch\nwatch_category other\n")
        == at(duplicate_category, 3));

  CHECK(error_of(header
                 + "5000:01 watch sys uptime s ro float32\n"
                   "# between them\n"
                   "5000:01 watch sys other s ro float32\n")
        == at(duplicate_key, 4));
  CHECK(error_of(header
                 + "5000:01 watch sys uptime s ro float32\n"
                   "5000:02 watch sys uptime s ro float32\n")
            .reason
        == duplicate_name);
  CHECK(error_of(header
                 + "watch_category watch\n"
                   "config_category config\n"
                   "5000:01 watch sys uptime s ro float32\n")
        == at(unknown_category, 3));
}

TEST_CASE("an OD file holds 65535 objects at most", "[od_file]")
{
  std::string text = "cannet-od 1\n";
  for (std::size_t i = 0; i <= 65535; ++i) {
    text += std::format("{:04X}:{:02X} c s n{} - ro uint8\n",
                        0x2000 + i / 256,
                        i % 256,
                        i);
  }
  CHECK(error_of(text)
        == od_file_error{.reason = od_file_error::kind::too_many_objects,
                         .line = 65537});
}

TEST_CASE("a field the format cannot hold is not written", "[od_file]")
{
  static constexpr auto spaced = dictionary{
      {.watch_category = "watch", .config_category = ""},
      {{{0x5000, 0x01}, {"watch", "sys", "uptime", "s", ro, float32}},
       {{0x5000, 0x02}, {"watch", "sys", "speed", "m s", ro, float32}}}};
  auto const text = to_od_file(spaced);
  REQUIRE_FALSE(text);
  CHECK(text.error()
        == od_file_error{.reason = od_file_error::kind::bad_field, .line = 5});
}
