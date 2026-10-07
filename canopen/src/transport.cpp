#include <canopen/transport.hpp>

namespace cannet::canopen {

std::string_view to_string(transport_error e)
{
  switch (e) {
  case transport_error::closed: return "transport closed";
  case transport_error::send_failed: return "send failed";
  case transport_error::tx_queue_full: return "TX queue full (frame dropped)";
  case transport_error::interface_down: return "interface down";
  }
  return "unknown error";
}

std::string_view name(transport_error e)
{
  switch (e) {
  case transport_error::closed: return "closed";
  case transport_error::send_failed: return "send_failed";
  case transport_error::tx_queue_full: return "tx_queue_full";
  case transport_error::interface_down: return "interface_down";
  }
  return "unknown";
}

std::string_view to_string(bus_state s)
{
  switch (s) {
  case bus_state::error_active: return "error active";
  case bus_state::error_warning: return "error warning";
  case bus_state::error_passive: return "error passive";
  case bus_state::bus_off: return "bus off";
  case bus_state::down: return "interface down";
  case bus_state::no_interface: return "no interface";
  }
  return "unknown state";
}

std::string_view name(bus_state s)
{
  switch (s) {
  case bus_state::error_active: return "error_active";
  case bus_state::error_warning: return "error_warning";
  case bus_state::error_passive: return "error_passive";
  case bus_state::bus_off: return "bus_off";
  case bus_state::down: return "down";
  case bus_state::no_interface: return "no_interface";
  }
  return "unknown";
}

} // namespace cannet::canopen
