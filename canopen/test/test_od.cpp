#include <canopen/od.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace cannet::canopen;

namespace {

// Deliberately declared out of key order: the dictionary sorts at compile
// time, so the device's own table order can be mirrored verbatim.
inline constexpr auto test_dictionary = dictionary{
    {.watch_category = "watch", .config_category = "config"},
    {{{0x2001, 0x01},
      {"config",
       "drive",
       "pwm_freq",
       "Hz",
       od_access::rw,
       od_value_type::float32}},
     {{0x1008, 0x00},
      {"info",
       "sys",
       "device_name",
       "",
       od_access::const_,
       od_value_type::string}},
     {{0x5000, 0x02},
      {"watch", "drive", "speed", "rpm", od_access::ro, od_value_type::int16}},
     {{0x5000, 0x01},
      {"watch",
       "drive",
       "current",
       "A",
       od_access::ro,
       od_value_type::float32}},
     {{0x2000, 0x01},
      {"ctl", "sys", "reset_device", "", od_access::wo, od_value_type::exec}},
     {{0x5000, 0x03},
      {"watch", "sys", "uptime", "s", od_access::ro, od_value_type::uint32}}}};

constexpr dictionary_view od = test_dictionary.view();

} // namespace

TEST_CASE("dictionary is sorted by key at compile time", "[od]")
{
  STATIC_CHECK(od.size() == 6);
  STATIC_CHECK(
      std::ranges::is_sorted(od.entries(), std::less{}, &od_entry::key));
  STATIC_CHECK(od.entries().front().key == od_key{0x1008, 0x00});
  STATIC_CHECK(od.entries().back().key == od_key{0x5000, 0x03});
}

TEST_CASE("lookup by key", "[od]")
{
  auto const* entry = od.find({0x2001, 0x01});
  REQUIRE(entry != nullptr);
  CHECK(entry->object.name == "pwm_freq");
  CHECK(entry->object.unit == "Hz");
  CHECK(entry->object.type == od_value_type::float32);

  CHECK(od.find({0x2001, 0x02}) == nullptr); // subindex absent
  CHECK(od.find({0x3000, 0x01}) == nullptr); // index absent
  CHECK(od.find({0x0000, 0x00}) == nullptr); // below the first key
  CHECK(od.find({0xFFFF, 0xFF}) == nullptr); // above the last key
}

TEST_CASE("lookup by category/subcategory/name", "[od]")
{
  auto const* entry = od.find("watch", "drive", "speed");
  REQUIRE(entry != nullptr);
  CHECK(entry->key == od_key{0x5000, 0x02});

  CHECK(od.find("watch", "drive", "torque") == nullptr); // no such name
  CHECK(od.find("watch", "sys", "speed") == nullptr); // right name, wrong sub
  CHECK(od.find("nope", "drive", "speed") == nullptr);
}

TEST_CASE("category iteration is ordered and complete", "[od]")
{
  std::vector<std::string_view> names;
  for (od_entry const& entry : od.in_category(od.config().watch_category)) {
    names.push_back(entry.object.name);
  }
  CHECK(names == std::vector<std::string_view>{"current", "speed", "uptime"});

  CHECK(std::ranges::distance(od.in_category("info")) == 1);
  CHECK(std::ranges::distance(od.in_category("absent")) == 0);
}

TEST_CASE("entries_by_name spans the whole dictionary in name order", "[od]")
{
  std::vector<std::string_view> categories;
  for (od_entry const& entry : od.entries_by_name()) {
    categories.push_back(entry.object.category);
  }
  CHECK(categories
        == std::vector<std::string_view>{"config",
                                         "ctl",
                                         "info",
                                         "watch",
                                         "watch",
                                         "watch"});
}

TEST_CASE("access permissions", "[od]")
{
  auto const* readonly = od.find({0x5000, 0x01});
  auto const* writeonly = od.find({0x2000, 0x01});
  auto const* readwrite = od.find({0x2001, 0x01});
  REQUIRE(readonly != nullptr);
  REQUIRE(writeonly != nullptr);
  REQUIRE(readwrite != nullptr);

  CHECK(readonly->object.readable());
  CHECK_FALSE(readonly->object.writable());
  CHECK_FALSE(writeonly->object.readable());
  CHECK(writeonly->object.writable());
  CHECK(readwrite->object.readable());
  CHECK(readwrite->object.writable());
}

TEST_CASE("an empty dictionary_view is usable", "[od]")
{
  constexpr dictionary_view empty;
  STATIC_CHECK(empty.empty());
  STATIC_CHECK(empty.size() == 0);
  CHECK(empty.find({0x1000, 0x00}) == nullptr);
  CHECK(empty.find("a", "b", "c") == nullptr);
  CHECK(std::ranges::distance(empty.in_category("a")) == 0);
}

TEST_CASE("od value sizes match the wire", "[od]")
{
  STATIC_CHECK(od_value_size(od_value_type::boolean) == 1);
  STATIC_CHECK(od_value_size(od_value_type::int16) == 2);
  STATIC_CHECK(od_value_size(od_value_type::uint32) == 4);
  STATIC_CHECK(od_value_size(od_value_type::float32) == 4);
  STATIC_CHECK(od_value_size(od_value_type::exec) == 4);
  STATIC_CHECK(od_value_size(od_value_type::string) == 4);
}

TEST_CASE("od values round-trip through the raw SDO data field", "[od]")
{
  struct {
    od_value value;
    od_value_type type;
  } const cases[] = {
      {true, od_value_type::boolean},
      {std::int8_t{-42}, od_value_type::int8},
      {std::int16_t{-12345}, od_value_type::int16},
      {std::int32_t{-1'234'567}, od_value_type::int32},
      {std::uint8_t{200}, od_value_type::uint8},
      {std::uint16_t{60000}, od_value_type::uint16},
      {std::uint32_t{4'000'000'000}, od_value_type::uint32},
      {3.5f, od_value_type::float32},
  };

  for (auto const& c : cases) {
    CAPTURE(std::to_underlying(c.type));
    CHECK(make_od_value(to_raw(c.value), c.type) == c.value);
  }
}

TEST_CASE("raw decoding ignores bytes beyond the type's size", "[od]")
{
  expedited_sdo_data const raw = {0x2A, 0xFF, 0xFF, 0xFF};

  CHECK(make_od_value(raw, od_value_type::uint8) == od_value{std::uint8_t{42}});
  CHECK(make_od_value(raw, od_value_type::int8) == od_value{std::int8_t{42}});
  CHECK(make_od_value(raw, od_value_type::boolean) == od_value{true});
  CHECK(make_od_value(expedited_sdo_data{0, 0, 0, 0}, od_value_type::boolean)
        == od_value{false});
}

TEST_CASE("exec and string are carried as uint32", "[od]")
{
  expedited_sdo_data const raw = {'t', 'e', 's', 't'};
  auto const as_string = make_od_value(raw, od_value_type::string);
  auto const as_exec = make_od_value(raw, od_value_type::exec);

  REQUIRE(std::holds_alternative<std::uint32_t>(as_string));
  REQUIRE(std::holds_alternative<std::uint32_t>(as_exec));
  CHECK(to_raw(as_string) == raw);
}

TEST_CASE("a value's type is that of its alternative", "[od]")
{
  STATIC_CHECK(type_of(od_value{true}) == od_value_type::boolean);
  STATIC_CHECK(type_of(od_value{std::int8_t{-1}}) == od_value_type::int8);
  STATIC_CHECK(type_of(od_value{std::int16_t{-1}}) == od_value_type::int16);
  STATIC_CHECK(type_of(od_value{std::int32_t{-1}}) == od_value_type::int32);
  STATIC_CHECK(type_of(od_value{std::uint8_t{1}}) == od_value_type::uint8);
  STATIC_CHECK(type_of(od_value{std::uint16_t{1}}) == od_value_type::uint16);
  STATIC_CHECK(type_of(od_value{std::uint32_t{1}}) == od_value_type::uint32);
  STATIC_CHECK(type_of(od_value{1.5f}) == od_value_type::float32);
}
