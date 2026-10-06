#pragma once

// cannet::canopen::detail::tpdo_consumer — the host's side of one remote
// node's TPDOs (device -> host): a handler and a timeout per TPDO. Decoding
// the payload is the device application's job. The host half of emblib's
// tpdo_producer, and a member of remote_node (remote_node.hpp), not a type
// to use on its own.
//
// Protocol plane, unprivileged.
//
// Thread model: the client's (client.hpp). Every call on the client's
// executor, where handlers run too.

#include <canopen/setup_error.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

namespace cannet::canopen {

class remote_node;

// How the host takes one TPDO. The device's tpdo_config in emblib holds a
// provider and a period; this is the consumer half, laid out as emblib's
// consumer config, rpdo_config.
struct tpdo_config {
  // Called with every frame of the TPDO; bytes past the frame's length read
  // as zero.
  std::move_only_function<void(payload const&)> handler{};
  // With no frame for this long, on_timeout runs, once per loss; the next
  // frame recovers silently. Zero: not monitored.
  std::chrono::milliseconds timeout{0};
  std::move_only_function<void()> on_timeout{};
  // The PDO's mapped length: shorter frames are dropped, as CiA 301 has it.
  std::uint8_t len = CAN_MAX_DLEN;
};

namespace detail {

class tpdo_consumer {
public:
  tpdo_consumer(transport& bus, node_id id);
  ~tpdo_consumer();
  tpdo_consumer(tpdo_consumer const&) = delete;
  tpdo_consumer& operator=(tpdo_consumer const&) = delete;

  // Takes TPDO `number` (1..4) on its predefined COB-ID, replacing an
  // earlier setup of the same number; this may happen inside that TPDO's
  // own handler. Monitoring starts at once, so a TPDO that never arrives
  // times out too. Fails with invalid_pdo for a number outside 1..4 or a
  // length above 8.
  std::expected<void, setup_error> setup(unsigned number, tpdo_config config);

private:
  friend class cannet::canopen::remote_node;

  // Follows the node to a new id. The old node's TPDOs have stopped coming,
  // so notify_rebound() times out every TPDO that had not timed out yet.
  void rebind(node_id id);
  void notify_rebound();

  struct state;
  std::unique_ptr<state> state_;
};

} // namespace detail
} // namespace cannet::canopen
