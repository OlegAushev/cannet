#pragma once

// Expedited SDO wire format: the on-wire structs, command specifiers, abort
// codes, and the encoders/decoders the client needs to talk to a device-side
// SDO server.
//
// Protocol plane, unprivileged, no I/O — pure data, no state. Layouts match
// emb::can::canopen (device side) byte for byte.
//
// Only expedited transfers are covered: every object of the dictionaries this
// stack serves fits in 4 bytes, and strings are read as a sequence of
// expedited responses terminated by a NUL byte (see the string handling in
// the SDO client), not as a block transfer.

#include <canopen/types.hpp>

#include <cstdint>
#include <string_view>

namespace cannet::canopen {

// Command specifiers, named from the point of view of the peer that sends
// them. The client sends client_*, the device answers with server_*.
namespace sdo_cs_codes {

inline constexpr std::uint32_t client_init_write = 1;
inline constexpr std::uint32_t server_init_write = 3;
inline constexpr std::uint32_t client_init_read = 2;
inline constexpr std::uint32_t server_init_read = 2;

inline constexpr std::uint32_t abort = 4;

inline constexpr std::uint32_t client_block_write = 6;
inline constexpr std::uint32_t server_block_write = 5;

inline constexpr std::uint32_t client_block_read = 5;
inline constexpr std::uint32_t server_block_read = 6;

} // namespace sdo_cs_codes

// The 4-byte data field of an expedited SDO frame.
using expedited_sdo_data = std::array<std::uint8_t, 4>;

struct expedited_sdo {
  std::uint32_t data_size_indicated : 1;
  std::uint32_t expedited_transfer : 1;
  std::uint32_t data_empty_bytes : 2;
  std::uint32_t _reserved : 1;
  std::uint32_t cs : 3;
  std::uint32_t index : 16;
  std::uint32_t subindex : 8;
  expedited_sdo_data data;

  constexpr expedited_sdo()
      : data_size_indicated(0),
        expedited_transfer(0),
        data_empty_bytes(0),
        _reserved(0),
        cs(0),
        index(0),
        subindex(0),
        data{}
  {
  }

  // Number of significant data bytes, valid only when the size is indicated.
  constexpr std::uint32_t data_size() const
  {
    return 4 - data_empty_bytes;
  }
};

static_assert(sizeof(expedited_sdo) == 8);

enum class sdo_abort_code : std::uint32_t {
  invalid_cs = 0x05040001,
  unsupported_access = 0x06010000,
  read_from_write_only = 0x06010001,
  write_to_read_only = 0x06010002,
  object_not_found = 0x06020000,
  hardware_error = 0x06060000,
  data_type_mismatch = 0x06070010,
  value_range_exceeded = 0x06090030,
  value_too_high = 0x06090031,
  value_too_low = 0x06090032,
  general_error = 0x08000000,
  data_store_error = 0x08000020,
  local_control_error = 0x08000021,
  state_error = 0x08000022,
  no_data_available = 0x08000024
};

struct abort_sdo {
  std::uint32_t _reserved : 5;
  std::uint32_t cs : 3;
  std::uint32_t index : 16;
  std::uint32_t subindex : 8;
  std::uint32_t error_code;

  constexpr abort_sdo()
      : _reserved(0),
        cs(sdo_cs_codes::abort),
        index(0),
        subindex(0),
        error_code(0)
  {
  }

  constexpr bool valid() const
  {
    return cs == sdo_cs_codes::abort;
  }
};

static_assert(sizeof(abort_sdo) == 8);

// Command specifier of a received SDO frame, without decoding the rest.
constexpr std::uint32_t sdo_cs_code(payload const& data)
{
  return (data[0] >> 5) & 0x07;
}

// ---- client-side request encoding ----

// Read request (SDO upload initiate) for the object at index/subindex.
constexpr expedited_sdo make_sdo_read_request(std::uint16_t index,
                                              std::uint8_t subindex)
{
  expedited_sdo sdo;
  sdo.cs = sdo_cs_codes::client_init_read;
  sdo.index = index;
  sdo.subindex = subindex;
  return sdo;
}

// Write request (SDO download initiate). `size` is the number of significant
// bytes in `data` (1..4); it is announced on the wire so the server knows how
// much of the field to take.
constexpr expedited_sdo make_sdo_write_request(std::uint16_t index,
                                               std::uint8_t subindex,
                                               expedited_sdo_data data,
                                               std::uint32_t size)
{
  expedited_sdo sdo;
  sdo.cs = sdo_cs_codes::client_init_write;
  sdo.expedited_transfer = 1;
  sdo.data_size_indicated = 1;
  sdo.data_empty_bytes = (4 - size) & 0x3;
  sdo.index = index;
  sdo.subindex = subindex;
  sdo.data = data;
  return sdo;
}

// Returns a human-readable description of an SDO abort code (CiA 301 §7.2.4).
std::string_view to_string(sdo_abort_code code);

} // namespace cannet::canopen
