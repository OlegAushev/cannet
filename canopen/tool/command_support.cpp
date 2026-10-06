#include "command_support.hpp"

#include <canopen/od_format.hpp>

#include <array>
#include <charconv>
#include <format>
#include <ostream>
#include <print>
#include <system_error>

namespace cannet::canopen::tool {

namespace {

using namespace std::chrono_literals;

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

// As an OD file gives a key: index:subindex in hex.
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

// category::subcategory::name, each part not empty.
std::optional<std::array<std::string_view, 3>> split_name(std::string_view text)
{
  std::array<std::string_view, 3> parts;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    auto const separator = text.find("::");
    bool const last = i + 1 == parts.size();
    if (last != (separator == std::string_view::npos)) {
      return std::nullopt;
    }
    parts[i] = text.substr(0, separator);
    if (parts[i].empty()) {
      return std::nullopt;
    }
    text = last ? std::string_view{} : text.substr(separator + 2);
  }
  return parts;
}

} // namespace

std::expected<object_ref, std::string> find_object(std::string_view text,
                                                   dictionary_view dictionary)
{
  if (!text.contains("::")) {
    auto const key = parse_key(text);
    if (!key) {
      return std::unexpected(
          std::format("not an object: {}; give index:subindex in hex or "
                      "category::subcategory::name",
                      text));
    }
    return object_ref{.key = *key, .entry = dictionary.find(*key)};
  }
  auto const parts = split_name(text);
  if (!parts) {
    return std::unexpected(
        std::format("not an object: {}; give category::subcategory::name",
                    text));
  }
  if (dictionary.empty()) {
    return std::unexpected(
        std::format("{}: an object by name needs a dictionary (-d)", text));
  }
  auto const* entry = dictionary.find((*parts)[0], (*parts)[1], (*parts)[2]);
  if (entry == nullptr) {
    return std::unexpected(
        std::format("{}: no such object in the dictionary", text));
  }
  return object_ref{.key = entry->key, .entry = entry};
}

std::string label(object_ref const& object)
{
  auto text = std::format("{:04X}:{:02X}",
                          object.key.index,
                          object.key.subindex);
  if (object.entry != nullptr) {
    auto const& o = object.entry->object;
    text += std::format(" {}::{}::{}", o.category, o.subcategory, o.name);
  }
  return text;
}

std::optional<od_value_type> parse_type(std::string_view text)
{
  for (std::size_t i = 0; i < od_value_type_count; ++i) {
    auto const type = static_cast<od_value_type>(i);
    if (name(type) == text) {
      return type;
    }
  }
  return std::nullopt;
}

std::string_view cli_name(nmt_command command)
{
  switch (command) {
  case nmt_command::start: return "start";
  case nmt_command::stop: return "stop";
  case nmt_command::enter_pre_operational: return "pre-operational";
  case nmt_command::reset_node: return "reset-node";
  case nmt_command::reset_communication: return "reset-communication";
  }
  return "unknown";
}

std::optional<nmt_command> parse_nmt_command(std::string_view text)
{
  for (auto const command : {nmt_command::start,
                             nmt_command::stop,
                             nmt_command::enter_pre_operational,
                             nmt_command::reset_node,
                             nmt_command::reset_communication}) {
    if (cli_name(command) == text) {
      return command;
    }
  }
  return std::nullopt;
}

client_options host_options(node_options const& options)
{
  return {.id = options.host, .heartbeat_period = 0ms, .sync_period = 0ms};
}

std::shared_ptr<remote_node> add_device(client& host,
                                        node_options const& options,
                                        console io)
{
  auto node = host.add_node({.name = "device",
                             .id = options.node,
                             .dictionary = options.dictionary,
                             .heartbeat_timeout = 0ms,
                             .sdo_timeout = options.sdo_timeout});
  if (!node) {
    // The only failure for the first node: it has the CLI's own id.
    std::print(io.err,
               "canopen: node {} is the CLI's own id; give it another "
               "with --host-id\n",
               options.node.get());
    return nullptr;
  }
  return *node;
}

void report(console io, object_ref const& object, sdo_error const& error)
{
  std::print(io.err, "canopen: {}: {}\n", label(object), to_string(error));
}

} // namespace cannet::canopen::tool
