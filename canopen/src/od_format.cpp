#include <canopen/od_format.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <system_error>
#include <type_traits>

namespace cannet::canopen {

namespace {

std::string_view trim(std::string_view text)
{
  auto const is_space = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

bool equal_ignore_case(std::string_view lhs, std::string_view rhs)
{
  return std::ranges::equal(lhs, rhs, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a))
        == std::tolower(static_cast<unsigned char>(b));
  });
}

// Parses the whole of `text` as T; trailing garbage is a syntax error.
// An "0x"/"0X" prefix selects base 16 (convenient for index-like values).
template<typename T>
std::expected<od_value, parse_error> parse_integer(std::string_view text)
{
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }

  if constexpr (std::is_unsigned_v<T>) {
    // from_chars refuses a sign outright; for an unsigned object a negative
    // number is a value the type cannot hold, which is worth saying plainly.
    if (text.starts_with('-')) {
      auto const magnitude = text.substr(1);
      std::uint64_t discarded{};
      auto const [ptr,
                  ec] = std::from_chars(magnitude.data(),
                                        magnitude.data() + magnitude.size(),
                                        discarded,
                                        base);
      bool const is_number = (ec == std::errc{}
                              && ptr == magnitude.data() + magnitude.size())
                          || ec == std::errc::result_out_of_range;
      return std::unexpected(is_number ? parse_error::out_of_range
                                       : parse_error::syntax);
    }
  }

  T value{};
  auto const* const first = text.data();
  auto const* const last = text.data() + text.size();
  auto const [ptr, ec] = std::from_chars(first, last, value, base);

  if (ec == std::errc::result_out_of_range) {
    return std::unexpected(parse_error::out_of_range);
  }
  if (ec != std::errc{} || ptr != last) {
    return std::unexpected(parse_error::syntax);
  }
  return od_value{value};
}

std::expected<od_value, parse_error> parse_boolean(std::string_view text)
{
  if (equal_ignore_case(text, "true") || text == "1") {
    return od_value{true};
  }
  if (equal_ignore_case(text, "false") || text == "0") {
    return od_value{false};
  }
  return std::unexpected(parse_error::syntax);
}

std::expected<od_value, parse_error> parse_float(std::string_view text)
{
  float value{};
  auto const* const first = text.data();
  auto const* const last = text.data() + text.size();
  auto const [ptr, ec] = std::from_chars(first, last, value);

  if (ec == std::errc::result_out_of_range) {
    return std::unexpected(parse_error::out_of_range);
  }
  if (ec != std::errc{} || ptr != last) {
    return std::unexpected(parse_error::syntax);
  }
  return od_value{value};
}

std::optional<std::uint16_t> parse_hex(std::string_view text,
                                       std::size_t max_digits)
{
  if (text.empty() || text.size() > max_digits) {
    return std::nullopt;
  }
  std::uint16_t value = 0;
  auto const end = text.data() + text.size();
  auto const [ptr, ec] = std::from_chars(text.data(), end, value, 16);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::string format_float(float value, int precision)
{
  // Below 0.01 fixed notation of a few digits shows nothing useful.
  auto const format = (std::fabs(value) >= 0.01f) ? std::chars_format::fixed
                                                  : std::chars_format::general;
  std::array<char, 32> buffer{};
  auto const [ptr, ec] = std::to_chars(buffer.data(),
                                       buffer.data() + buffer.size(),
                                       value,
                                       format,
                                       precision);
  if (ec != std::errc{}) {
    return "?";
  }
  return std::string(buffer.data(), ptr);
}

} // namespace

std::string_view to_string(parse_error e)
{
  switch (e) {
  case parse_error::syntax: return "not a valid value";
  case parse_error::out_of_range: return "value out of range for this type";
  case parse_error::unsupported_type: return "type cannot be written as text";
  }
  return "unknown error";
}

std::string_view name(parse_error e)
{
  switch (e) {
  case parse_error::syntax: return "syntax";
  case parse_error::out_of_range: return "out_of_range";
  case parse_error::unsupported_type: return "unsupported_type";
  }
  return "unknown";
}

std::string to_string(od_value value, int precision)
{
  return std::visit(
      [precision](auto const& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        }
        else if constexpr (std::is_same_v<T, float>) {
          return format_float(v, precision);
        }
        else {
          // int8_t/uint8_t must not print as characters.
          return std::to_string(static_cast<std::int64_t>(v));
        }
      },
      value);
}

std::expected<od_value, parse_error> parse(std::string_view text,
                                           od_value_type type)
{
  text = trim(text);
  if (text.empty()) {
    return std::unexpected(parse_error::syntax);
  }

  switch (type) {
  case od_value_type::boolean: return parse_boolean(text);
  case od_value_type::int8: return parse_integer<std::int8_t>(text);
  case od_value_type::int16: return parse_integer<std::int16_t>(text);
  case od_value_type::int32: return parse_integer<std::int32_t>(text);
  case od_value_type::uint8: return parse_integer<std::uint8_t>(text);
  case od_value_type::uint16: return parse_integer<std::uint16_t>(text);
  case od_value_type::uint32:
  case od_value_type::exec: return parse_integer<std::uint32_t>(text);
  case od_value_type::float32: return parse_float(text);
  case od_value_type::string:
    return std::unexpected(parse_error::unsupported_type);
  }
  return std::unexpected(parse_error::unsupported_type);
}

std::string to_string(od_key key)
{
  return std::format("{:04X}:{:02X}", key.index, key.subindex);
}

std::optional<od_key> parse_key(std::string_view text)
{
  auto const colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  auto const index = parse_hex(text.substr(0, colon), 4);
  auto const subindex = parse_hex(text.substr(colon + 1), 2);
  if (!index || !subindex) {
    return std::nullopt;
  }
  return od_key{*index, static_cast<std::uint8_t>(*subindex)};
}

std::string_view to_string(od_value_type type)
{
  switch (type) {
  case od_value_type::boolean: return "bool";
  case od_value_type::int8: return "int8";
  case od_value_type::int16: return "int16";
  case od_value_type::int32: return "int32";
  case od_value_type::uint8: return "uint8";
  case od_value_type::uint16: return "uint16";
  case od_value_type::uint32: return "uint32";
  case od_value_type::float32: return "float32";
  case od_value_type::exec: return "exec";
  case od_value_type::string: return "string";
  }
  return "unknown";
}

std::string_view to_string(od_access access)
{
  switch (access) {
  case od_access::rw: return "rw";
  case od_access::ro: return "ro";
  case od_access::wo: return "wo";
  case od_access::const_: return "const";
  }
  return "unknown";
}

std::string_view name(od_value_type type)
{
  switch (type) {
  case od_value_type::boolean: return "boolean";
  case od_value_type::int8: return "int8";
  case od_value_type::int16: return "int16";
  case od_value_type::int32: return "int32";
  case od_value_type::uint8: return "uint8";
  case od_value_type::uint16: return "uint16";
  case od_value_type::uint32: return "uint32";
  case od_value_type::float32: return "float32";
  case od_value_type::exec: return "exec";
  case od_value_type::string: return "string";
  }
  return "unknown";
}

std::string_view name(od_access access)
{
  switch (access) {
  case od_access::rw: return "rw";
  case od_access::ro: return "ro";
  case od_access::wo: return "wo";
  case od_access::const_: return "const";
  }
  return "unknown";
}

} // namespace cannet::canopen
