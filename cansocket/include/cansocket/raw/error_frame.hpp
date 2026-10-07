#pragma once

// cannet::raw::error_report — what a CAN controller reports in an error
// frame, decoded. A socket receives error frames only when it asks for them
// (set_error_filter()); they arrive through recv() and async_receive() like
// any frame, with CAN_ERR_FLAG in can_id, and carry what linux/can/error.h
// describes: the controller's state of fault confinement, frames lost to a
// full buffer, errors on the wire, the error counters. Which of these a
// controller reports is up to its driver.
//
// Transport plane, unprivileged, no I/O: pure functions over a frame.
//
// Thread model: none needed; the functions only read their argument.

#include <linux/can.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace cannet::raw {

// A controller's state of CAN fault confinement, set by its error counters.
enum class controller_state : std::uint8_t {
  error_active,  // both counters below 96: it takes part in the bus fully
  error_warning, // a counter reached 96: errors are frequent
  error_passive, // a counter reached 128: it signals errors without
                 // disturbing the bus, and waits longer before sending
  bus_off,       // the transmit counter passed 255: it has left the bus
                 // until it is restarted
};

// A human-readable description, for people; the wording may change.
std::string_view to_string(controller_state s);

// The stable identifier: the enumerator's name, such as "bus_off".
std::string_view name(controller_state s);

// A controller's transmit and receive error counters (TEC and REC).
struct error_counters {
  std::uint8_t tx = 0;
  std::uint8_t rx = 0;
};

// What one error frame reports.
struct error_report {
  // The state the controller is in now, when the frame tells: a change of
  // state, a bus-off, a restart.
  std::optional<controller_state> state;
  bool restarted = false;          // restarted after a bus-off
  bool rx_overflow = false;        // frames received were lost: a full buffer
  bool tx_overflow = false;        // frames to send were lost: a full buffer
  bool protocol_violation = false; // an error on the wire: a bit, form or
                                   // stuffing error
  bool no_ack = false;             // nobody acknowledged a frame it sent
  bool bus_error = false;          // an error on the wire, unspecified
  bool transceiver = false;        // the transceiver reports a fault
  bool lost_arbitration = false;   // not an error: another node won the bus
  bool tx_timeout = false;         // a frame was not sent in time
  std::optional<error_counters> counters;
};

// Whether `frame` is an error frame, which the controller made, rather than
// a frame from the bus.
constexpr bool is_error_frame(can_frame const& frame)
{
  return (frame.can_id & CAN_ERR_FLAG) != 0;
}

// What an error frame reports; nullopt for a frame that is not one.
std::optional<error_report> decode_error_frame(can_frame const& frame);

} // namespace cannet::raw
