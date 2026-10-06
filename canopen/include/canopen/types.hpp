#pragma once

// CANopen wire primitives: node ids, the predefined connection set (COB-ID
// arithmetic), NMT states and commands, payload (de)serialization, and the
// frames of NMT, SYNC, heartbeat and EMCY.
//
// Protocol plane, unprivileged, no I/O. Everything here is pure: constexpr
// data and functions without state, thread-safe by construction.
//
// Names and semantics mirror the device-side stack (emb::can::canopen) so the
// two ends of the wire can be read side by side. Roles are inverted: this is
// the CANopen *client* (master), so TPDO is device -> host and RPDO is
// host -> device, exactly as the device names them.

#include <linux/can.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cannet::canopen {

// The on-wire structs in this plane are laid out for the machine that runs
// SocketCAN; CANopen itself is little-endian.
static_assert(std::endian::native == std::endian::little,
              "cannet::canopen assumes a little-endian host");

using payload = std::array<std::uint8_t, CAN_MAX_DLEN>;

template<typename T>
  requires std::is_trivially_copyable_v<T> && (sizeof(T) == sizeof(payload))
constexpr payload to_payload(T const& message)
{
  return std::bit_cast<payload>(message);
}

template<typename T>
  requires std::is_trivially_copyable_v<T> && (sizeof(T) == sizeof(payload))
constexpr T from_payload(payload const& data)
{
  return std::bit_cast<T>(data);
}

// A CANopen node id: [1, 127]. Constructible only through make() (runtime
// input, e.g. from a GUI) or literal() (compile-time constant), so an
// out-of-range id cannot reach COB-ID arithmetic.
class node_id {
public:
  static constexpr unsigned min = 1;
  static constexpr unsigned max = 127;

  static constexpr bool is_valid(unsigned value)
  {
    return value >= min && value <= max;
  }

  static constexpr std::optional<node_id> make(unsigned value)
  {
    if (!is_valid(value)) {
      return std::nullopt;
    }
    return node_id(static_cast<std::uint8_t>(value));
  }

  // Compile-time construction; an out-of-range value is a compile error.
  static consteval node_id literal(unsigned value)
  {
    if (!is_valid(value)) {
      throw "node id must be in [1, 127]";
    }
    return node_id(static_cast<std::uint8_t>(value));
  }

  constexpr std::uint8_t get() const
  {
    return value_;
  }

  friend constexpr bool operator==(node_id, node_id) = default;
  friend constexpr auto operator<=>(node_id, node_id) = default;

private:
  explicit constexpr node_id(std::uint8_t value) : value_(value) {}

  std::uint8_t value_;
};

enum class nmt_state : std::uint8_t {
  initializing = 0x00,
  stopped = 0x04,
  operational = 0x05,
  pre_operational = 0x7F
};

// Unlike the device-side enum, these carry their on-wire command specifiers:
// the client encodes them into NMT frames.
enum class nmt_command : std::uint8_t {
  start = 0x01,
  stop = 0x02,
  enter_pre_operational = 0x80,
  reset_node = 0x81,
  reset_communication = 0x82
};

enum class cob_type : std::uint8_t {
  nmt,
  sync,
  emcy,
  time,
  tpdo, // device -> host
  rpdo, // host -> device
  tsdo, // device -> host (SDO response)
  rsdo, // host -> device (SDO request)
  heartbeat
};

inline constexpr std::size_t cob_type_count = 9;

inline constexpr std::array<canid_t, cob_type_count> cob_function_codes = {
    0x000, // nmt
    0x080, // sync
    0x080, // emcy
    0x100, // time
    0x180, // tpdo (base)
    0x200, // rpdo (base)
    0x580, // tsdo
    0x600, // rsdo
    0x700  // heartbeat
};

// Broadcast services carry no node id in their COB-ID.
constexpr bool is_broadcast(cob_type service)
{
  return service == cob_type::nmt
      || service == cob_type::sync
      || service == cob_type::time;
}

constexpr bool is_pdo(cob_type service)
{
  return service == cob_type::tpdo || service == cob_type::rpdo;
}

// COB-ID of a non-PDO service in the predefined connection set. For broadcast
// services the node id is not part of the COB-ID and is ignored.
constexpr canid_t cob_id(cob_type service, node_id id)
{
  canid_t const code = cob_function_codes[std::to_underlying(service)];
  return is_broadcast(service) ? code : code + id.get();
}

// COB-ID of a broadcast service; nullopt for services that need a node id.
constexpr std::optional<canid_t> cob_id(cob_type service)
{
  if (!is_broadcast(service)) {
    return std::nullopt;
  }
  return cob_function_codes[std::to_underlying(service)];
}

// COB-ID of PDO number `number` (1-based) in the predefined connection set.
// Only PDOs 1..4 are predefined; a higher PDO needs an explicitly configured
// COB-ID and yields nullopt here.
constexpr std::optional<canid_t> pdo_cob_id(cob_type pdo,
                                            unsigned number,
                                            node_id id)
{
  if (!is_pdo(pdo) || number < 1 || number > 4) {
    return std::nullopt;
  }
  return cob_function_codes[std::to_underlying(pdo)]
       + 0x100 * (number - 1)
       + id.get();
}

// The filter for one COB-ID: it passes exactly the standard data frames with
// that id. A mask of CAN_SFF_MASK alone would also pass remote frames with
// the id, and extended frames whose low 11 bits equal it.
constexpr can_filter cob_filter(canid_t id)
{
  return {.can_id = id, .can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG};
}

// Builds a classic (11-bit id, up to 8 byte) CAN frame. Bytes of `data`
// beyond `len` are not transmitted.
inline can_frame make_frame(canid_t id, std::uint8_t len, payload const& data)
{
  can_frame frame = {};
  frame.can_id = id;
  frame.len = len;
  std::copy_n(data.begin(), len, frame.data);
  return frame;
}

// An NMT node control request: a command for one node, or for every node.
struct nmt_message {
  nmt_command command;
  std::optional<node_id> target{}; // nullopt: every node (0 on the wire)

  friend bool operator==(nmt_message const&, nmt_message const&) = default;
};

// NMT node control from the master: two bytes, the command specifier and the
// target node id.
can_frame make_nmt_frame(nmt_message const& message);

// The request an NMT frame carries; nullopt for fewer than two bytes, an
// unknown command specifier or a target above 127.
std::optional<nmt_message> decode_nmt(can_frame const& frame);

// SYNC carries no data: no counter, as emblib's sync_producer sends it.
can_frame make_sync_frame();

// A heartbeat of node `id`: one byte, its NMT state. A boot-up message is a
// heartbeat reporting `initializing`.
can_frame make_heartbeat_frame(node_id id, nmt_state state);

// The state a heartbeat reports; nullopt for a frame without data or with an
// unknown state. Bit 7, the toggle bit of node guarding, is ignored.
std::optional<nmt_state> decode_heartbeat(can_frame const& frame);

// An emergency message, as emblib's emcy_producer::emit sends it.
struct emcy_message {
  std::uint16_t error_code = 0;    // 0x0000: error reset, or no error
  std::uint8_t error_register = 0; // object 1001h
  std::array<std::uint8_t, 5> manufacturer{};

  friend bool operator==(emcy_message const&, emcy_message const&) = default;
};

// EMCY of node `id`: eight bytes, the error code (little-endian), the error
// register and the manufacturer-specific bytes.
can_frame make_emcy_frame(node_id id, emcy_message const& message);

// The emergency an EMCY frame carries; nullopt for a frame too short to hold
// the error code and register. Manufacturer bytes it lacks read as zero.
std::optional<emcy_message> decode_emcy(can_frame const& frame);

// Human-readable descriptions, for people; the wording may change.
std::string_view to_string(nmt_state state);
std::string_view to_string(nmt_command command);

// Stable identifiers: the enumerators' names, such as "pre_operational", for
// logs and formats a program reads.
std::string_view name(nmt_state state);
std::string_view name(nmt_command command);

} // namespace cannet::canopen
