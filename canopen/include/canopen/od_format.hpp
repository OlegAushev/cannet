#pragma once

// Text conversion for OD values: the boundary between a typed od_value and
// the strings a UI or CLI deals in.
//
// Protocol plane, unprivileged, no I/O, no state. Kept apart from od.hpp so
// the protocol path — which never formats anything — does not pull in
// formatting code.

#include <canopen/od.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace cannet::canopen {

enum class parse_error {
  syntax,          // not a number (or not a boolean literal) at all
  out_of_range,    // parsed, but does not fit the object's type
  unsupported_type // type carries no user-writable scalar (string)
};

std::string_view to_string(parse_error e);

// Formats a value for display. `precision` applies to floats only: they are
// printed fixed, or in general format when the magnitude is small enough that
// fixed notation would show nothing.
std::string to_string(od_value value, int precision = 6);

// Parses user input into a value of the object's declared type. Booleans
// accept "true"/"false" (any case) as well as "1"/"0"; exec objects take any
// uint32 (the write is the command, the value is ignored by most devices).
std::expected<od_value, parse_error> parse(std::string_view text,
                                           od_value_type type);

std::string_view to_string(od_value_type type);
std::string_view to_string(od_access access);

} // namespace cannet::canopen
