#pragma once

// cannet::canopen::detail::rpdo_producer — the host's side of one remote
// node's RPDOs (host -> device): a provider and a period per RPDO, sent
// while the client runs. Building the payload is the device application's
// job. The host half of emblib's rpdo_consumer, and a member of remote_node
// (remote_node.hpp), not a type to use on its own.
//
// RPDOs go out whether or not the node is alive: a device that times out
// its RPDOs, as emblib's rpdo_consumer does, must not see them pause.
//
// Protocol plane, unprivileged.
//
// Thread model: the client's (client.hpp). Every call on the client's
// executor, where providers run too.

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

// What the host sends as one RPDO. The device's rpdo_config in emblib holds
// a handler and a timeout; this is the producer half, laid out as emblib's
// producer config, tpdo_config.
struct rpdo_config {
  // Called right before each send. A counter the device checks belongs
  // here, one per node: a static one would be shared by every node of the
  // application.
  std::move_only_function<payload()> provider{};
  // Zero: not sent.
  std::chrono::milliseconds period{0};
  // The PDO's mapped length: the bytes sent.
  std::uint8_t len = CAN_MAX_DLEN;
};

namespace detail {

class rpdo_producer {
public:
  rpdo_producer(transport& bus, node_id id);
  ~rpdo_producer();
  rpdo_producer(rpdo_producer const&) = delete;
  rpdo_producer& operator=(rpdo_producer const&) = delete;

  // Sends RPDO `number` (1..4) on its predefined COB-ID, replacing an
  // earlier setup of the same number; this may happen inside that RPDO's
  // own provider. Fails with invalid_pdo for a number outside 1..4 or a
  // length above 8.
  std::expected<void, setup_error> setup(unsigned number, rpdo_config config);

  // Whether RPDO `number` is sent; on by default. A disabled RPDO keeps its
  // setup, and its provider is not called. Fail with invalid_pdo for a
  // number outside 1..4.
  std::expected<void, setup_error> enable(unsigned number);
  std::expected<void, setup_error> disable(unsigned number);
  bool enabled(unsigned number) const;

  // Whether the node's RPDOs are sent at all; on by default.
  void enable();
  void disable();
  bool enabled() const;

private:
  friend class cannet::canopen::remote_node;

  void rebind(node_id id);
  // RPDOs go out only while the client runs.
  void start();
  void stop();

  struct state;
  std::unique_ptr<state> state_;
};

} // namespace detail
} // namespace cannet::canopen
