#pragma once

// The canopen CLI's commands on a bus, each a coroutine over a transport:
// the CLI runs them on a raw_transport, their tests against an emulated
// device on the loopback bus. Private to the CLI and its tests.
//
// A command runs on the transport's executor, writes what it found to
// `out` and what went wrong to `err`, and completes with the CLI's exit
// status: 0; 1 when the bus or the device failed it; 2 when it was called
// wrong, before anything went on the bus. A terminal cancellation of the
// coroutine (Ctrl+C) asks it to stop. A request in flight is let finish,
// so that a string read does not leave the device's string cursor
// mid-string.
//
// The CLI is a node on the bus, with an id of its own, but never starts:
// it sends no heartbeat, no SYNC and no RPDO. A device has one SDO channel,
// so an SDO command and an application talking to the same node take each
// other's answers.

#include <canopen/od.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace cannet::canopen::tool {

struct console {
  std::ostream& out;
  std::ostream& err;
};

// The device a command talks to over SDO.
struct node_options {
  node_id node;
  // The CLI's own node id.
  node_id host = node_id::literal(127);
  // The device's objects; empty: objects are given by key.
  dictionary_view dictionary{};
  std::chrono::milliseconds sdo_timeout{500};
};

// An object as the CLI is given it: by key, "5000:01", or by name in the
// dictionary, "watch::sys::uptime".
struct object_ref {
  od_key key;
  od_entry const* entry = nullptr; // null: not in the dictionary
};

std::expected<object_ref, std::string> find_object(std::string_view text,
                                                   dictionary_view dictionary);

// "5000:01 watch::sys::uptime"; the key alone outside the dictionary.
std::string label(object_ref const& object);

// A type by its name(), "uint16", as an OD file gives it.
std::optional<od_value_type> parse_type(std::string_view text);

// sdo read: prints the object's value, or its string. The type is `type`
// when given, else the dictionary's.
boost::asio::awaitable<int> sdo_read(transport& bus,
                                     node_options options,
                                     std::string object,
                                     std::optional<od_value_type> type,
                                     console io);

// sdo write: writes `value`, parsed as the object's type, which is `type`
// when given, else the dictionary's. Prints nothing.
boost::asio::awaitable<int> sdo_write(transport& bus,
                                      node_options options,
                                      std::string object,
                                      std::string value,
                                      std::optional<od_value_type> type,
                                      console io);

// sdo exec: runs a command, an exec object; refuses an object the
// dictionary has as anything else, which the write would change. Prints
// nothing.
boost::asio::awaitable<int>
sdo_exec(transport& bus, node_options options, std::string object, console io);

} // namespace cannet::canopen::tool
