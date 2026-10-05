#pragma once

// cannet::canopen::transport — the protocol plane's view of a CAN bus: send a
// frame, subscribe to the frames that match a filter. CANopen services talk
// to this interface only, never to sockets.
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

#include <boost/asio/any_io_executor.hpp>

#include <linux/can.h>

#include <expected>
#include <functional>
#include <string_view>

namespace cannet::canopen {

enum class transport_error {
  closed, // the transport is not open, or was closed with the send queued
  send_failed,
  tx_queue_full, // the interface TX queue is full and the frame was dropped —
                 // transient, routine on short-queue (SPI) controllers
};

// Returns human-readable name for a `transport_error` (for logging / CLI).
std::string_view to_string(transport_error e);

// Delivery of frames to one handler, in force until the subscription is
// destroyed or reset(). Move-only; a default-constructed subscription is
// empty. Outliving its transport is harmless.
class subscription {
public:
  subscription() = default;
  explicit subscription(std::move_only_function<void()> cancel);
  ~subscription();
  subscription(subscription&& other) noexcept;
  subscription& operator=(subscription&& other) noexcept;
  subscription(subscription const&) = delete;
  subscription& operator=(subscription const&) = delete;

  // Ends delivery now; safe from inside the subscription's own handler.
  void reset();

private:
  std::move_only_function<void()> cancel_;
};

class transport {
public:
  using executor_type = boost::asio::any_io_executor;
  using frame_handler = std::move_only_function<void(can_frame const&)>;
  using send_handler =
      std::move_only_function<void(std::expected<void, transport_error>)>;

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

protected:
  transport() = default;
  transport(transport const&) = default;
  transport& operator=(transport const&) = default;
};

} // namespace cannet::canopen
