// od-verify: whether a device's objects are as its dictionary has them.
//
// Every object is read, nothing written: a read tells whether the device
// has the object, whether it reads it, and the size of its value, never
// whether it takes a write. A dictionary's ro and const look alike, and so
// do two types of one size, a float32 and a uint32.

#include "command_support.hpp"

#include <canopen/od_format.hpp>
#include <canopen/sdo.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <print>
#include <string>

namespace cannet::canopen::tool {

namespace {

namespace asio = boost::asio;

// After this many timeouts in a row, the device is taken for silent.
constexpr int max_unanswered = 3;

enum class verdict : std::uint8_t { match, differs, unchecked };

struct finding {
  verdict what;
  std::string why{}; // for all but a match
};

// Reads the object as its dictionary has it: a string to its NUL, anything
// else as one expedited value of its type, a command's four bytes
// included.
asio::awaitable<sdo_result<void>> read_as_dictionary(remote_node& node,
                                                     od_entry const& entry)
{
  auto const& object = entry.object;
  if (object.readable() && object.type == od_value_type::string) {
    auto const text = co_await node.sdo.async_read_string(entry.key,
                                                          uninterrupted());
    if (!text) {
      co_return std::unexpected(text.error());
    }
    co_return sdo_result<void>{};
  }
  auto const value = co_await node.sdo.async_read(entry.key,
                                                  object.type,
                                                  uninterrupted());
  if (!value) {
    co_return std::unexpected(value.error());
  }
  co_return sdo_result<void>{};
}

// What the read of an object tells of it.
finding judge(od_object const& object, sdo_result<void> const& read)
{
  auto const readable_on_device = [&object] {
    return finding{verdict::differs,
                   std::format("readable on the device, {} in the dictionary",
                               name(object.access))};
  };
  if (read) {
    return object.readable() ? finding{verdict::match} : readable_on_device();
  }
  auto const& error = read.error();
  switch (error.reason) {
  case sdo_error::kind::aborted:
    if (error.abort == sdo_abort_code::object_not_found) {
      return {verdict::differs, "the device has no such object"};
    }
    if (error.abort == sdo_abort_code::read_from_write_only) {
      if (!object.readable()) {
        return {verdict::match};
      }
      return {verdict::differs,
              std::format("write-only on the device, {} in the dictionary",
                          name(object.access))};
    }
    return {verdict::unchecked, to_string(error)};
  case sdo_error::kind::type_mismatch:
  case sdo_error::kind::malformed: {
    // The device answered with a value, which the dictionary's type does
    // not fit.
    if (!object.readable()) {
      return readable_on_device();
    }
    bool const mismatch = error.reason == sdo_error::kind::type_mismatch;
    if (object.type == od_value_type::string) {
      return {verdict::differs,
              std::format("not a string on the device: {}",
                          mismatch ? "an answer of another size than 4 bytes"
                                   : to_string(error))};
    }
    if (mismatch) {
      return {verdict::differs,
              std::format("the device's object has another size than {}",
                          name(object.type))};
    }
    return {verdict::differs,
            std::format("the device's answer is no expedited {}: {}",
                        name(object.type),
                        to_string(error))};
  }
  case sdo_error::kind::timeout:
  case sdo_error::kind::cancelled:
  case sdo_error::kind::transport: break;
  }
  return {verdict::unchecked, to_string(error)};
}

} // namespace

asio::awaitable<int> od_verify(transport& bus, node_options options, console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  if (options.dictionary.empty()) {
    std::print(io.err, "canopen: od-verify needs the node's dictionary (-d)\n");
    co_return 2;
  }
  client host{bus, host_options(options)};
  auto const node = add_device(host, options, io);
  if (!node) {
    co_return 2;
  }

  auto const entries = options.dictionary.entries();
  std::size_t matched = 0;
  std::size_t differing = 0;
  std::size_t unchecked = 0;
  int unanswered = 0;
  std::optional<std::string> halt; // why the objects left go unchecked
  std::size_t next = 0;
  while (next < entries.size() && !halt) {
    if (co_await stop_requested()) {
      halt = "stopped";
      break;
    }
    auto const& entry = entries[next++];
    auto const read = co_await read_as_dictionary(*node, entry);
    auto const found = judge(entry.object, read);
    switch (found.what) {
    case verdict::match: ++matched; break;
    case verdict::differs:
      ++differing;
      std::print(io.out,
                 "{}: differs: {}\n",
                 label({.key = entry.key, .entry = &entry}),
                 found.why);
      break;
    case verdict::unchecked:
      ++unchecked;
      std::print(io.out,
                 "{}: not checked: {}\n",
                 label({.key = entry.key, .entry = &entry}),
                 found.why);
      break;
    }

    auto const failure = read ? std::nullopt
                              : std::optional{read.error().reason};
    unanswered = failure == sdo_error::kind::timeout ? unanswered + 1 : 0;
    if (failure == sdo_error::kind::transport) {
      halt = to_string(read.error()); // the rest would fail as well
    }
    else if (unanswered == max_unanswered) {
      halt = "the device does not answer";
    }
  }

  auto const left = entries.size() - next;
  unchecked += left;
  if (halt && left > 0) {
    std::print(io.err,
               "canopen: {}; the {} objects left are not checked\n",
               *halt,
               left);
  }
  std::print(io.out,
             "{} objects: {} match, {} differ, {} not checked\n",
             entries.size(),
             matched,
             differing,
             unchecked);
  co_return differing == 0 && unchecked == 0 ? 0 : 1;
}

} // namespace cannet::canopen::tool
