#include <canopen/types.hpp>

#include <algorithm>

namespace cannet::canopen {

namespace {

// The data bytes a frame really holds: len, capped at a classic frame's 8.
std::size_t data_size(can_frame const& frame)
{
  return std::min<std::size_t>(frame.len, CAN_MAX_DLEN);
}

} // namespace

can_frame make_nmt_frame(nmt_message const& message)
{
  payload data{};
  data[0] = std::to_underlying(message.command);
  data[1] = message.target ? message.target->get() : std::uint8_t{0};
  return make_frame(*cob_id(cob_type::nmt), 2, data);
}

std::optional<nmt_message> decode_nmt(can_frame const& frame)
{
  if (data_size(frame) < 2) {
    return std::nullopt;
  }
  std::optional<nmt_command> command;
  for (auto const c : {nmt_command::start,
                       nmt_command::stop,
                       nmt_command::enter_pre_operational,
                       nmt_command::reset_node,
                       nmt_command::reset_communication}) {
    if (std::to_underlying(c) == frame.data[0]) {
      command = c;
    }
  }
  if (!command) {
    return std::nullopt;
  }
  if (frame.data[1] == 0) {
    return nmt_message{.command = *command, .target = std::nullopt};
  }
  auto const target = node_id::make(frame.data[1]);
  if (!target) {
    return std::nullopt;
  }
  return nmt_message{.command = *command, .target = *target};
}

can_frame make_sync_frame()
{
  return make_frame(*cob_id(cob_type::sync), 0, {});
}

can_frame make_heartbeat_frame(node_id id, nmt_state state)
{
  payload data{};
  data[0] = std::to_underlying(state);
  return make_frame(cob_id(cob_type::heartbeat, id), 1, data);
}

std::optional<nmt_state> decode_heartbeat(can_frame const& frame)
{
  if (data_size(frame) < 1) {
    return std::nullopt;
  }
  auto const reported = static_cast<std::uint8_t>(frame.data[0] & 0x7F);
  for (auto const s : {nmt_state::initializing,
                       nmt_state::stopped,
                       nmt_state::operational,
                       nmt_state::pre_operational}) {
    if (std::to_underlying(s) == reported) {
      return s;
    }
  }
  return std::nullopt;
}

can_frame make_emcy_frame(node_id id, emcy_message const& message)
{
  payload data{};
  data[0] = static_cast<std::uint8_t>(message.error_code & 0xFF);
  data[1] = static_cast<std::uint8_t>(message.error_code >> 8);
  data[2] = message.error_register;
  std::ranges::copy(message.manufacturer, data.begin() + 3);
  return make_frame(cob_id(cob_type::emcy, id), CAN_MAX_DLEN, data);
}

std::optional<emcy_message> decode_emcy(can_frame const& frame)
{
  auto const size = data_size(frame);
  if (size < 3) {
    return std::nullopt;
  }
  emcy_message message;
  message.error_code = static_cast<std::uint16_t>(frame.data[0]
                                                  | (frame.data[1] << 8));
  message.error_register = frame.data[2];
  std::copy_n(frame.data + 3, size - 3, message.manufacturer.begin());
  return message;
}

std::string_view to_string(nmt_state state)
{
  switch (state) {
  case nmt_state::initializing: return "initializing";
  case nmt_state::stopped: return "stopped";
  case nmt_state::operational: return "operational";
  case nmt_state::pre_operational: return "pre-operational";
  }
  return "unknown";
}

std::string_view to_string(nmt_command command)
{
  switch (command) {
  case nmt_command::start: return "start";
  case nmt_command::stop: return "stop";
  case nmt_command::enter_pre_operational: return "enter pre-operational";
  case nmt_command::reset_node: return "reset node";
  case nmt_command::reset_communication: return "reset communication";
  }
  return "unknown";
}

std::string_view name(nmt_state state)
{
  switch (state) {
  case nmt_state::initializing: return "initializing";
  case nmt_state::stopped: return "stopped";
  case nmt_state::operational: return "operational";
  case nmt_state::pre_operational: return "pre_operational";
  }
  return "unknown";
}

std::string_view name(nmt_command command)
{
  switch (command) {
  case nmt_command::start: return "start";
  case nmt_command::stop: return "stop";
  case nmt_command::enter_pre_operational: return "enter_pre_operational";
  case nmt_command::reset_node: return "reset_node";
  case nmt_command::reset_communication: return "reset_communication";
  }
  return "unknown";
}

} // namespace cannet::canopen
