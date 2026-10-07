#include <cansocket/raw/error_frame.hpp>

#include <linux/can/error.h>

namespace cannet::raw {

std::optional<error_report> decode_error_frame(can_frame const& frame)
{
  if (!is_error_frame(frame)) {
    return std::nullopt;
  }
  canid_t const classes = frame.can_id & CAN_ERR_MASK;
  // A driver fills in the bytes its classes name, and leaves the rest zero;
  // a frame shorter than CAN_ERR_DLC lacks them.
  auto const byte = [&frame](int i) -> std::uint8_t {
    return i < frame.len ? frame.data[i] : 0;
  };

  error_report report;
  report.tx_timeout = (classes & CAN_ERR_TX_TIMEOUT) != 0;
  report.lost_arbitration = (classes & CAN_ERR_LOSTARB) != 0;
  report.protocol_violation = (classes & CAN_ERR_PROT) != 0;
  report.transceiver = (classes & CAN_ERR_TRX) != 0;
  report.no_ack = (classes & CAN_ERR_ACK) != 0;
  report.bus_error = (classes & CAN_ERR_BUSERROR) != 0;
  report.restarted = (classes & CAN_ERR_RESTARTED) != 0;

  if ((classes & CAN_ERR_CRTL) != 0) {
    auto const status = byte(1);
    report.rx_overflow = (status & CAN_ERR_CRTL_RX_OVERFLOW) != 0;
    report.tx_overflow = (status & CAN_ERR_CRTL_TX_OVERFLOW) != 0;
    // A change of state names the state entered, for the transmit and the
    // receive counter apart; the worse one is the controller's.
    if ((status & (CAN_ERR_CRTL_RX_PASSIVE | CAN_ERR_CRTL_TX_PASSIVE)) != 0) {
      report.state = controller_state::error_passive;
    }
    else if ((status & (CAN_ERR_CRTL_RX_WARNING | CAN_ERR_CRTL_TX_WARNING))
             != 0) {
      report.state = controller_state::error_warning;
    }
    else if ((status & CAN_ERR_CRTL_ACTIVE) != 0) {
      report.state = controller_state::error_active;
    }
  }
  if ((classes & CAN_ERR_BUSOFF) != 0) {
    report.state = controller_state::bus_off;
  }
  else if (report.restarted && !report.state) {
    report.state = controller_state::error_active;
  }

  if ((classes & CAN_ERR_CNT) != 0) {
    report.counters = error_counters{.tx = byte(6), .rx = byte(7)};
  }
  return report;
}

std::string_view to_string(controller_state s)
{
  switch (s) {
  case controller_state::error_active: return "error active";
  case controller_state::error_warning: return "error warning";
  case controller_state::error_passive: return "error passive";
  case controller_state::bus_off: return "bus off";
  }
  return "unknown state";
}

std::string_view name(controller_state s)
{
  switch (s) {
  case controller_state::error_active: return "error_active";
  case controller_state::error_warning: return "error_warning";
  case controller_state::error_passive: return "error_passive";
  case controller_state::bus_off: return "bus_off";
  }
  return "unknown";
}

} // namespace cannet::raw
