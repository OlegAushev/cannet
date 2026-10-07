#include <canopen/od_format.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace cannet::canopen;

TEST_CASE("integers and booleans format for display", "[od_format]")
{
  CHECK(to_string(od_value{true}) == "true");
  CHECK(to_string(od_value{false}) == "false");
  CHECK(to_string(od_value{std::int8_t{-42}}) == "-42");
  CHECK(to_string(od_value{std::uint8_t{200}}) == "200");
  CHECK(to_string(od_value{std::int32_t{-1'234'567}}) == "-1234567");
  CHECK(to_string(od_value{std::uint32_t{4'000'000'000}}) == "4000000000");
}

TEST_CASE("floats honour the requested precision", "[od_format]")
{
  CHECK(to_string(od_value{3.5f}, 2) == "3.50");
  CHECK(to_string(od_value{-0.125f}, 3) == "-0.125");
  CHECK(to_string(od_value{100.0f}, 0) == "100");

  // Below 0.01 fixed notation would round to zero, so general format is used.
  CHECK(to_string(od_value{1e-6f}, 2) != "0.00");
}

TEST_CASE("parsing produces the object's declared type", "[od_format]")
{
  auto const parsed = parse("-12345", od_value_type::int16);
  REQUIRE(parsed.has_value());
  CHECK(std::holds_alternative<std::int16_t>(*parsed));
  CHECK(*parsed == od_value{std::int16_t{-12345}});

  CHECK(parse("42", od_value_type::uint8) == od_value{std::uint8_t{42}});
  CHECK(parse("3.5", od_value_type::float32) == od_value{3.5f});
  CHECK(parse("  7  ", od_value_type::int32) == od_value{std::int32_t{7}});
}

TEST_CASE("booleans accept literals and digits", "[od_format]")
{
  CHECK(parse("true", od_value_type::boolean) == od_value{true});
  CHECK(parse("TRUE", od_value_type::boolean) == od_value{true});
  CHECK(parse("1", od_value_type::boolean) == od_value{true});
  CHECK(parse("False", od_value_type::boolean) == od_value{false});
  CHECK(parse("0", od_value_type::boolean) == od_value{false});
  CHECK(parse("yes", od_value_type::boolean).error() == parse_error::syntax);
}

TEST_CASE("hexadecimal input is accepted for integers", "[od_format]")
{
  CHECK(parse("0x1F", od_value_type::uint16) == od_value{std::uint16_t{31}});
  CHECK(parse("0XFF", od_value_type::uint8) == od_value{std::uint8_t{255}});
  CHECK(parse("0x1G", od_value_type::uint16).error() == parse_error::syntax);
}

TEST_CASE("out-of-range values are rejected, not truncated", "[od_format]")
{
  CHECK(parse("300", od_value_type::uint8).error()
        == parse_error::out_of_range);
  CHECK(parse("-1", od_value_type::uint8).error() == parse_error::out_of_range);
  CHECK(parse("70000", od_value_type::uint16).error()
        == parse_error::out_of_range);
  CHECK(parse("128", od_value_type::int8).error() == parse_error::out_of_range);
  CHECK(parse("1e40", od_value_type::float32).error()
        == parse_error::out_of_range);
}

TEST_CASE("malformed input is a syntax error", "[od_format]")
{
  CHECK(parse("", od_value_type::int32).error() == parse_error::syntax);
  CHECK(parse("   ", od_value_type::int32).error() == parse_error::syntax);
  CHECK(parse("12abc", od_value_type::int32).error() == parse_error::syntax);
  CHECK(parse("3.5", od_value_type::int32).error() == parse_error::syntax);
  CHECK(parse("abc", od_value_type::float32).error() == parse_error::syntax);
}

TEST_CASE("exec takes a uint32, string takes nothing", "[od_format]")
{
  CHECK(parse("1", od_value_type::exec) == od_value{std::uint32_t{1}});
  CHECK(parse("anything", od_value_type::string).error()
        == parse_error::unsupported_type);
}

TEST_CASE("a key goes as index:subindex in hex", "[od_format]")
{
  CHECK(to_string(od_key{0x5000, 0x01}) == "5000:01");
  CHECK(to_string(od_key{0x001A, 0xF2}) == "001A:F2");

  CHECK(parse_key("5000:01") == od_key{0x5000, 0x01});
  CHECK(parse_key("5000:1") == od_key{0x5000, 0x01});
  CHECK(parse_key("1a:f2") == od_key{0x001A, 0xF2});
  CHECK(parse_key(to_string(od_key{0xFFFF, 0xFF})) == od_key{0xFFFF, 0xFF});
  for (auto const* text : {"",
                           "5000",
                           "5000:",
                           ":01",
                           "10000:01",
                           "5000:100",
                           "50x0:01",
                           "5000:01 ",
                           "-1:01",
                           "5000::01"}) {
    CAPTURE(text);
    CHECK_FALSE(parse_key(text));
  }
}

TEST_CASE("type and access names", "[od_format]")
{
  CHECK(to_string(od_value_type::float32) == "float32");
  CHECK(to_string(od_value_type::string) == "string");
  CHECK(to_string(od_access::rw) == "rw");
  CHECK(to_string(od_access::const_) == "const");
  CHECK_FALSE(to_string(parse_error::out_of_range).empty());
}

TEST_CASE("types, accesses and parse errors have stable names", "[od_format]")
{
  CHECK(name(od_value_type::boolean) == "boolean");
  CHECK(name(od_value_type::int8) == "int8");
  CHECK(name(od_value_type::int16) == "int16");
  CHECK(name(od_value_type::int32) == "int32");
  CHECK(name(od_value_type::uint8) == "uint8");
  CHECK(name(od_value_type::uint16) == "uint16");
  CHECK(name(od_value_type::uint32) == "uint32");
  CHECK(name(od_value_type::float32) == "float32");
  CHECK(name(od_value_type::exec) == "exec");
  CHECK(name(od_value_type::string) == "string");

  CHECK(name(od_access::rw) == "rw");
  CHECK(name(od_access::ro) == "ro");
  CHECK(name(od_access::wo) == "wo");
  CHECK(name(od_access::const_) == "const");

  CHECK(name(parse_error::syntax) == "syntax");
  CHECK(name(parse_error::out_of_range) == "out_of_range");
  CHECK(name(parse_error::unsupported_type) == "unsupported_type");
}
