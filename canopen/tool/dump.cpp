// dump: every frame on the bus, decoded as CANopen's predefined connection
// set lays the bus out.

#include "command_support.hpp"

#include <canopen/od_format.hpp>
#include <canopen/sdo.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <ostream>
#include <print>
#include <span>

namespace cannet::canopen::tool {

namespace {

namespace asio = boost::asio;

// Where a COB-ID sits in the predefined connection set.
struct cob {
  enum class kind : std::uint8_t {
    nmt,
    sync,
    time,
    emcy,
    tpdo,
    rpdo,
    tsdo,
    rsdo,
    heartbeat,
    other, // no CANopen frame: another id, an extended or a remote frame
  };

  kind what;
  unsigned node = 0; // of a node's own frames, else 0
  unsigned pdo = 0;  // of a PDO, 1 to 4
};

cob classify(can_frame const& frame)
{
  using enum cob::kind;
  if ((frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0) {
    return {.what = other};
  }
  auto const id = frame.can_id & CAN_SFF_MASK;
  auto const node = id & 0x7F;
  if (node == 0) {
    switch (id) {
    case 0x000: return {.what = nmt};
    case 0x080: return {.what = sync};
    case 0x100: return {.what = time};
    default: return {.what = other};
    }
  }
  switch (id & 0x780) {
  case 0x080: return {.what = emcy, .node = node};
  case 0x180: return {.what = tpdo, .node = node, .pdo = 1};
  case 0x200: return {.what = rpdo, .node = node, .pdo = 1};
  case 0x280: return {.what = tpdo, .node = node, .pdo = 2};
  case 0x300: return {.what = rpdo, .node = node, .pdo = 2};
  case 0x380: return {.what = tpdo, .node = node, .pdo = 3};
  case 0x400: return {.what = rpdo, .node = node, .pdo = 3};
  case 0x480: return {.what = tpdo, .node = node, .pdo = 4};
  case 0x500: return {.what = rpdo, .node = node, .pdo = 4};
  case 0x580: return {.what = tsdo, .node = node};
  case 0x600: return {.what = rsdo, .node = node};
  case 0x700: return {.what = heartbeat, .node = node};
  default: return {.what = other};
  }
}

// Whether a frame concerns `node`: one of its own, or one for every node.
bool concerns(cob const& c, can_frame const& frame, node_id node)
{
  switch (c.what) {
  case cob::kind::nmt: {
    auto const message = decode_nmt(frame);
    return !message || !message->target || *message->target == node;
  }
  case cob::kind::sync:
  case cob::kind::time: return true;
  case cob::kind::other: return false;
  default: return c.node == node.get();
  }
}

std::span<std::uint8_t const> data_of(can_frame const& frame)
{
  return {frame.data, std::min<std::size_t>(frame.len, CAN_MAX_DLEN)};
}

// "DC 05".
std::string hex(std::span<std::uint8_t const> data)
{
  std::string text;
  for (auto const byte : data) {
    text += std::format("{}{:02X}", text.empty() ? "" : " ", byte);
  }
  return text;
}

// "[2] DC 05".
std::string bytes(std::span<std::uint8_t const> data)
{
  if (data.empty()) {
    return "[0]";
  }
  return std::format("[{}] {}", data.size(), hex(data));
}

// Four characters of a string as C writes them: "ADPT", "r\0\0\0".
std::string quoted(std::span<std::uint8_t const> data)
{
  std::string text = "\"";
  for (auto const byte : data) {
    if (byte == 0) {
      text += "\\0";
    }
    else if (byte == '"' || byte == '\\') {
      text += '\\';
      text += static_cast<char>(byte);
    }
    else if (byte < 0x20 || byte >= 0x7F) {
      text += std::format("\\x{:02X}", byte);
    }
    else {
      text += static_cast<char>(byte);
    }
  }
  return text + '"';
}

// The object an SDO frame names.
object_ref named(std::uint32_t index,
                 std::uint32_t subindex,
                 dictionary_view dictionary)
{
  od_key const key{static_cast<std::uint16_t>(index),
                   static_cast<std::uint8_t>(subindex)};
  return {.key = key, .entry = dictionary.find(key)};
}

// The value an expedited SDO frame carries: as the object's type when the
// dictionary has the object and the sizes agree, else its bytes.
std::string value_of(expedited_sdo const& sdo, od_entry const* entry)
{
  std::size_t const size = sdo.data_size_indicated != 0 ? sdo.data_size()
                                                        : sdo.data.size();
  auto const data = std::span{sdo.data}.first(size);
  if (entry == nullptr) {
    return bytes(data);
  }
  auto const type = entry->object.type;
  if (type == od_value_type::exec || size != od_value_size(type)) {
    return std::format("{} (the dictionary has {})", bytes(data), name(type));
  }
  if (type == od_value_type::string) {
    return quoted(data);
  }
  auto text = to_string(make_od_value(sdo.data, type));
  if (!entry->object.unit.empty()) {
    text += ' ';
    text += entry->object.unit;
  }
  return text;
}

std::string sdo_abort(payload const& data, dictionary_view dictionary)
{
  auto const abort = from_payload<abort_sdo>(data);
  return std::format("SDO abort {}: {} (0x{:08X})",
                     label(named(abort.index, abort.subindex, dictionary)),
                     to_string(static_cast<sdo_abort_code>(abort.error_code)),
                     abort.error_code);
}

// An SDO request, host to device.
std::string sdo_request(can_frame const& frame, dictionary_view dictionary)
{
  if (frame.len != CAN_MAX_DLEN) {
    return std::format("SDO {}", bytes(data_of(frame)));
  }
  payload data{};
  std::ranges::copy(data_of(frame), data.begin());
  auto const cs = sdo_cs_code(data);
  if (cs == sdo_cs_codes::abort) {
    return sdo_abort(data, dictionary);
  }
  auto const sdo = from_payload<expedited_sdo>(data);
  auto const object = named(sdo.index, sdo.subindex, dictionary);
  if (cs == sdo_cs_codes::client_init_read) {
    return std::format("SDO read {}", label(object));
  }
  if (cs != sdo_cs_codes::client_init_write) {
    return std::format("SDO {}", bytes(data_of(frame)));
  }
  if (sdo.expedited_transfer == 0) {
    return std::format("SDO write {}, segmented", label(object));
  }
  if (object.key == detail::sdo_client::restore_default_key) {
    // The data name the parameter: its index, little-endian, and subindex.
    auto const target = named(std::uint32_t{sdo.data[0]}
                                  | (std::uint32_t{sdo.data[1]} << 8),
                              sdo.data[2],
                              dictionary);
    return std::format("SDO restore default {}", label(target));
  }
  if (object.entry != nullptr
      && object.entry->object.type == od_value_type::exec) {
    return std::format("SDO exec {}", label(object));
  }
  return std::format("SDO write {} = {}",
                     label(object),
                     value_of(sdo, object.entry));
}

// An SDO answer, device to host.
std::string sdo_answer(can_frame const& frame, dictionary_view dictionary)
{
  if (frame.len != CAN_MAX_DLEN) {
    return std::format("SDO {}", bytes(data_of(frame)));
  }
  payload data{};
  std::ranges::copy(data_of(frame), data.begin());
  auto const cs = sdo_cs_code(data);
  if (cs == sdo_cs_codes::abort) {
    return sdo_abort(data, dictionary);
  }
  auto const sdo = from_payload<expedited_sdo>(data);
  auto const object = named(sdo.index, sdo.subindex, dictionary);
  if (cs == sdo_cs_codes::server_init_read) {
    if (sdo.expedited_transfer == 0) {
      return std::format("SDO {}, segmented", label(object));
    }
    return std::format("SDO {} = {}",
                       label(object),
                       value_of(sdo, object.entry));
  }
  if (cs == sdo_cs_codes::server_init_write) {
    return std::format("SDO written {}", label(object));
  }
  return std::format("SDO {}", bytes(data_of(frame)));
}

std::string emergency(can_frame const& frame)
{
  auto const message = decode_emcy(frame);
  if (!message) {
    return std::format("EMCY {}", bytes(data_of(frame)));
  }
  if (message->error_code == 0) {
    return std::format("EMCY error reset, register {:02X}",
                       message->error_register);
  }
  return std::format("EMCY {:04X}, register {:02X}, manufacturer {}",
                     message->error_code,
                     message->error_register,
                     hex(message->manufacturer));
}

// What a frame says.
std::string decoded(cob const& c, can_frame const& frame, dictionary_view dict)
{
  switch (c.what) {
  case cob::kind::nmt: {
    auto const message = decode_nmt(frame);
    if (!message) {
      return std::format("NMT {}", bytes(data_of(frame)));
    }
    if (!message->target) {
      return std::format("NMT {} all", cli_name(message->command));
    }
    return std::format("NMT {} node {}",
                       cli_name(message->command),
                       message->target->get());
  }
  case cob::kind::sync:
    if (frame.len == 0) {
      return "SYNC";
    }
    if (frame.len == 1) {
      return std::format("SYNC counter {}", frame.data[0]);
    }
    return std::format("SYNC {}", bytes(data_of(frame)));
  case cob::kind::time: return std::format("TIME {}", bytes(data_of(frame)));
  case cob::kind::emcy: return emergency(frame);
  case cob::kind::tpdo:
    return std::format("TPDO{} {}", c.pdo, bytes(data_of(frame)));
  case cob::kind::rpdo:
    return std::format("RPDO{} {}", c.pdo, bytes(data_of(frame)));
  case cob::kind::rsdo: return sdo_request(frame, dict);
  case cob::kind::tsdo: return sdo_answer(frame, dict);
  case cob::kind::heartbeat: {
    auto const state = decode_heartbeat(frame);
    if (!state) {
      return std::format("heartbeat {}", bytes(data_of(frame)));
    }
    if (*state == nmt_state::initializing) {
      return "boot-up";
    }
    return std::format("heartbeat {}", to_string(*state));
  }
  case cob::kind::other:
    if ((frame.can_id & CAN_RTR_FLAG) != 0) {
      // A remote frame carries no data: len is the length it requests.
      return std::format("remote request [{}]", frame.len);
    }
    return bytes(data_of(frame));
  }
  return bytes(data_of(frame));
}

} // namespace

std::optional<std::string> describe(can_frame const& frame,
                                    dump_options const& options)
{
  auto const c = classify(frame);
  if (options.node && !concerns(c, frame, *options.node)) {
    return std::nullopt;
  }
  auto const id = (frame.can_id & CAN_EFF_FLAG) != 0
                    ? std::format("{:08X}", frame.can_id & CAN_EFF_MASK)
                    : std::format("{:03X}", frame.can_id & CAN_SFF_MASK);
  auto const node = c.node != 0 ? std::format("node {}", c.node)
                                : std::string{};
  return std::format("{}  {:8}  {}",
                     id,
                     node,
                     decoded(c, frame, options.dictionary));
}

asio::awaitable<int> dump(transport& bus, dump_options options, console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  auto const start = std::chrono::steady_clock::now();
  auto const frames = bus.subscribe(
      {.can_id = 0, .can_mask = 0},
      [&](can_frame const& frame) {
        auto const line = describe(frame, options);
        if (!line) {
          return;
        }
        std::chrono::duration<double> const time =
            std::chrono::steady_clock::now() - start;
        std::print(io.out, "{:10.6f}  {}\n", time.count(), *line);
        io.out.flush();
      });
  // A stop cancels this wait: the coroutine suspends nowhere before it.
  asio::steady_timer until_stopped{bus.get_executor(),
                                   asio::steady_timer::time_point::max()};
  co_await until_stopped.async_wait(asio::as_tuple(asio::use_awaitable));
  co_return 0;
}

} // namespace cannet::canopen::tool
