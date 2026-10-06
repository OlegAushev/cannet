#pragma once

// The canopen CLI's tools over OD files (od_file.hpp): making one from an
// emblib firmware's table, and a C++ header from one, which a device
// application compiles its dictionary in from (cannet_canopen_dictionary()
// in CMake). Private to the CLI and its tests.

#include <canopen/od.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace cannet::canopen::tool {

// What went wrong, and where: a line of the input, from 1, or 0.
struct tool_error {
  std::size_t line = 0;
  std::string message;
};

struct emblib_options {
  // The categories of dictionary_config. Unset: "watch" and "config", when
  // an object belongs to them. Empty: none.
  std::optional<std::string> watch_category{};
  std::optional<std::string> config_category{};
};

// Makes an OD file from the rows of an emblib dictionary in `source`, the
// firmware's od.cpp: every {{index, subindex}, "category", "subcategory",
// "name", "unit", type, binding} there. The access follows from the
// binding (od_ro, param::rw, od_exec...). `origin` names the source in the
// file's first comment.
std::expected<std::string, tool_error>
od_file_from_emblib(std::string_view source,
                    std::string_view origin,
                    emblib_options const& options = {});

// Makes a header that declares `name`, in the namespace `ns` if not empty,
// as `inline constexpr auto name = cannet::canopen::dictionary{...}`.
// `origin` names the OD file in the header's first comment.
std::expected<std::string, tool_error> cpp_header(dictionary_view dictionary,
                                                  std::string_view name,
                                                  std::string_view ns,
                                                  std::string_view origin);

} // namespace cannet::canopen::tool
