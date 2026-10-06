// The dictionary cannet_canopen_dictionary() compiles in from drive.od.
#include "drive_dictionary.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cannet::canopen;

TEST_CASE("a dictionary is compiled in from an OD file", "[od_header]")
{
  constexpr dictionary_view view = cannet_test::drive_dictionary;
  STATIC_CHECK(view.size() == 5);
  STATIC_CHECK(view.config().watch_category == "watch");
  STATIC_CHECK(view.config().config_category == "config");
  STATIC_CHECK(view.find({0x3000, 0x01})->object.type == od_value_type::uint16);
  STATIC_CHECK(view.find({0x3000, 0x01})->object.unit == "rpm");
  STATIC_CHECK(view.find("watch", "elec", "Vdc")->key == od_key{0x5000, 0x02});
  STATIC_CHECK(view.find({0x1008, 0x00})->object.access == od_access::const_);
  STATIC_CHECK(view.find({0x2000, 0x02})->object.unit.empty());
}
