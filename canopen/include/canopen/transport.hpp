#pragma once

// cannet::canopen::transport — the protocol plane's view of a CAN bus: send a
// frame, subscribe to the frames that match a filter, and know the state of
// the bus. CANopen services talk to this interface only, never to sockets.
//
// Protocol plane, unprivileged; the interface does no I/O of its own. Two
// implementations: raw_transport (raw_transport.hpp, over
// cannet::raw::async_socket) and loopback_transport (loopback.hpp, an
// in-memory bus for tests).
//
// Thread model: NOT thread-safe. Make every call — including destroying a
// subscription — on the transport's executor; frame handlers and send
// completions run there too. With a multi-threaded io_context, give the
// transport a strand.

#include <canopen/subscription.hpp>
#include <cansocket/raw/error_frame.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <linux/can.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>

namespace cannet::canopen {

enum class transport_error {
  closed, // the transport is not open, or was closed with the send queued
  send_failed,
  tx_queue_full,  // the interface TX queue is full and the frame was dropped —
                  // transient, routine on short-queue (SPI) controllers
  interface_down, // the interface is down or gone: nothing goes out until it
                  // is back
};

// A human-readable description of a `transport_error`, for people; the
// wording may change.
std::string_view to_string(transport_error e);

// The stable identifier of a `transport_error`: its enumerator's name, such
// as "tx_queue_full", for logs and formats a program reads.
std::string_view name(transport_error e);

// The state of a bus as the host sees it: its controller's state of CAN
// fault confinement, and whether the interface it sits behind is there.
enum class bus_state : std::uint8_t {
  error_active,  // the controller takes part in the bus fully
  error_warning, // an error counter reached 96: errors are frequent
  error_passive, // an error counter reached 128: the controller signals
                 // errors without disturbing the bus
  bus_off,       // the controller has left the bus until the interface
                 // restarts it (restart-ms, or a restart by hand)
  down,          // the interface is down
  no_interface,  // the transport is on no interface: not open, or the
                 // interface is gone (an adapter unplugged)
};

// A human-readable description, for people; the wording may change.
std::string_view to_string(bus_state s);

// The stable identifier: the enumerator's name, such as "bus_off".
std::string_view name(bus_state s);

// A bus as its transport knows it: the state, and what the controller has
// reported since the transport opened.
struct bus_status {
  bus_state state = bus_state::error_active;
  // Errors on the wire the controller reported: protocol violations, frames
  // nobody acknowledged, transceiver faults.
  std::uint64_t bus_errors = 0;
  // Reports of frames lost to a full buffer of the controller.
  std::uint64_t overflows = 0;
  // The error counters as the controller last reported them, if it does;
  // none since it restarted, after a bus-off or with its interface.
  std::optional<raw::error_counters> counters{};

  bool operator==(bus_status const&) const = default;
};

class transport {
public:
  using executor_type = boost::asio::any_io_executor;
  using frame_handler = std::move_only_function<void(can_frame const&)>;
  using send_handler =
      std::move_only_function<void(std::expected<void, transport_error>)>;
  using status_handler = std::move_only_function<void(bus_status const&)>;

  virtual ~transport() = default;

  virtual executor_type get_executor() = 0;

  // Queues `frame` for transmission; frames go out in call order. `done`
  // runs once the frame has gone to the bus or failed — never from inside
  // send() itself.
  virtual void send(can_frame const& frame, send_handler done) = 0;

  // Delivers every received frame that matches `filter` to `on_frame` until
  // the returned subscription ends. A frame matches when
  //   (frame.can_id & filter.can_mask) == (filter.can_id & filter.can_mask),
  // as for CAN_RAW_FILTER; inverted filters (CAN_INV_FILTER) are not
  // supported. A frame matching several subscriptions reaches each of them;
  // one subscribed from inside a handler first sees the next frame.
  [[nodiscard]] virtual subscription subscribe(can_filter filter,
                                               frame_handler on_frame) = 0;

  // The bus now.
  virtual bus_status status() const = 0;

  // Calls `on_status` with the bus's status after every change of it — its
  // state, its counts, its counters — until the subscription ends. Error
  // frames reach no subscription of subscribe(): what they report reaches
  // these handlers.
  [[nodiscard]] virtual subscription on_status(status_handler on_status) = 0;

protected:
  transport() = default;
  transport(transport const&) = default;
  transport& operator=(transport const&) = default;
};

} // namespace cannet::canopen
