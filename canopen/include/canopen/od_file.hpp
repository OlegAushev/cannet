#pragma once

// cannet::canopen OD files — a device's object dictionary as text, one
// object per line. A program that cannot have the dictionary compiled in,
// such as the canopen CLI, reads one; a device application generates its
// compiled-in dictionary from one (cannet_canopen_dictionary() in CMake);
// and one is made from a firmware's own table (canopen od from-emblib), so
// that the firmware stays the dictionary's only source.
//
// Protocol plane, unprivileged, no I/O: text in, text out.
//
// Thread model: a loaded_dictionary is a value; any number of threads may
// read it and its view.
//
// The format, version 1:
//
//   cannet-od 1
//   # adpt-etk-inverter, firmware 15a8617
//   watch_category  watch
//   config_category config
//   1008:00  info    sys         device_name  -  const  string
//   5000:01  watch   sys         uptime       s  ro     float32
//   3003:01  config  protection  uvp_dc       V  rw     float32
//
// Blank lines and lines whose first non-blank character is '#' are skipped.
// The first other line names the format and its version. A category line
// names a category of dictionary_config, each at most once. An object line
// holds the key, index:subindex in hex, then the category, subcategory,
// name and unit ("-" for none), and the access and type by their name()s
// (od_format.hpp). Fields are separated by blanks, so none holds one, nor
// '"' or '\': each may become a C++ string literal. Lines come in any
// order; the checks are dictionary<N>'s.

#include <canopen/od.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cannet::canopen {

// Why an OD file cannot be read, or a dictionary not written as one.
struct od_file_error {
  enum class kind : std::uint8_t {
    version,            // no "cannet-od 1" before everything else
    syntax,             // a line with a wrong number of fields, or no sense
    bad_key,            // a key that is not index:subindex in hex
    bad_field,          // a field that holds '"' or '\', or a blank
    unknown_access,     // an access no od_access is named
    unknown_type,       // a type no od_value_type is named
    duplicate_category, // a category line given twice
    duplicate_key,      // two objects at one {index, subindex}
    duplicate_name,     // two objects with one {category, subcategory, name}
    empty_name,         // an empty category, subcategory or name
    unknown_category,   // a category line no object belongs to
    too_many_objects,   // more than 65535
  };

  kind reason;
  // The line, from 1; 0 when the file has no line to blame.
  std::size_t line = 0;

  friend bool operator==(od_file_error const&, od_file_error const&) = default;
};

// A human-readable description, "line 12: two objects at one key", for
// people; the wording may change.
std::string to_string(od_file_error const& e);
std::string_view to_string(od_file_error::kind k);

// The stable identifier of a kind: its enumerator's name, such as
// "duplicate_key", for logs and formats a program reads.
std::string_view name(od_file_error::kind k);

// A dictionary read from an OD file. It owns what its view points into, so
// the view is valid while the dictionary lives, moves included.
class loaded_dictionary {
public:
  dictionary_view view() const;

  operator dictionary_view() const
  {
    return view();
  }

private:
  friend std::expected<loaded_dictionary, od_file_error>
  parse_od_file(std::string text);

  loaded_dictionary() = default;

  std::unique_ptr<std::string const> text_;
  dictionary_config config_;
  std::vector<od_entry> entries_;
  std::vector<std::uint16_t> name_order_;
};

// Reads an OD file.
std::expected<loaded_dictionary, od_file_error> parse_od_file(std::string text);

// Writes `dictionary` as an OD file: objects by key, columns aligned. A
// field that the format cannot hold (bad_field) is reported with the line
// its object would have taken.
std::expected<std::string, od_file_error>
to_od_file(dictionary_view dictionary);

} // namespace cannet::canopen
