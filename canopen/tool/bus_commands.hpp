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
#include <vector>

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

// An NMT command as the CLI names it, nmt takes it and dump shows it:
// "start", "stop", "pre-operational", "reset-node", "reset-communication".
std::string_view cli_name(nmt_command command);
std::optional<nmt_command> parse_nmt_command(std::string_view text);

struct dump_options {
  // Only this node's frames, and those for every node (NMT, SYNC, TIME);
  // nullopt: every frame.
  std::optional<node_id> node{};
  // The objects SDO frames name, with their names and their values' types.
  dictionary_view dictionary{};
};

// What dump prints of a frame after its time: the COB-ID, the node, and
// what the frame says, "601  node 1    SDO read 3000:01
// config::drive::speed"; nullopt for a frame `options` leave out.
std::optional<std::string> describe(can_frame const& frame,
                                    dump_options const& options);

// dump: prints every frame on the bus, decoded, with the time since the
// start, until stopped. Sends nothing.
boost::asio::awaitable<int> dump(transport& bus,
                                 dump_options options,
                                 console io);

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

struct watch_options {
  // The objects to poll, by key or by name; empty: every object the watch
  // service polls, the readable scalars of the watch category.
  std::vector<std::string> objects{};
  std::chrono::milliseconds period{500};
  // How many passes to print before ending; 0: until stopped.
  unsigned count = 0;
  // Draw each pass over the screen, for a terminal; else table after table.
  bool redraw = false;
};

// watch: polls a node's watch objects through its watch service and prints
// their readings, a table per pass, until stopped or `count` passes.
boost::asio::awaitable<int>
watch(transport& bus, node_options options, watch_options watching, console io);

// nmt: sends an NMT command to `target`, or to every node. Prints nothing.
boost::asio::awaitable<int> nmt(transport& bus,
                                nmt_command command,
                                std::optional<node_id> target,
                                console io);

// od-verify: reads every object of the dictionary and prints those the
// device does not have as the dictionary does: an object it lacks, one it
// reads that the dictionary has write-only or the other way round, a
// value of another size. Writes nothing, and so cannot tell whether an
// object takes a write. Gives up after three timeouts in a row.
boost::asio::awaitable<int> od_verify(transport& bus,
                                      node_options options,
                                      console io);

} // namespace cannet::canopen::tool
