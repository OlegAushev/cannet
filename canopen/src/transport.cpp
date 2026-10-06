#include <canopen/transport.hpp>

namespace cannet::canopen {

std::string_view to_string(transport_error e)
{
  switch (e) {
  case transport_error::closed: return "transport closed";
  case transport_error::send_failed: return "send failed";
  case transport_error::tx_queue_full: return "TX queue full (frame dropped)";
  }
  return "unknown error";
}

std::string_view name(transport_error e)
{
  switch (e) {
  case transport_error::closed: return "closed";
  case transport_error::send_failed: return "send_failed";
  case transport_error::tx_queue_full: return "tx_queue_full";
  }
  return "unknown";
}

} // namespace cannet::canopen
