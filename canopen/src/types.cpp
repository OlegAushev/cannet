#include <canopen/types.hpp>

namespace cannet::canopen {

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

} // namespace cannet::canopen
