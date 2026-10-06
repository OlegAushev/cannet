#pragma once

// cannet::canopen::detail::hb_consumer — the host's heartbeat consumer for
// one remote node: whether the node is alive, and the NMT state it reports.
// The host half of emblib's hb_producer, and a member of remote_node
// (remote_node.hpp), not a type to use on its own.
//
// Protocol plane, unprivileged.
//
// Thread model: the client's (client.hpp). Every call on the client's
// executor; events are emitted there.

#include <canopen/event.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace cannet::canopen {

class remote_node;

// What a node's heartbeat says about it.
struct heartbeat_status {
  bool alive = false;               // a heartbeat came within the timeout
  std::optional<nmt_state> state{}; // the state it reported last

  friend bool operator==(heartbeat_status const&,
                         heartbeat_status const&) = default;
};

namespace detail {

class hb_consumer {
public:
  using change_handler = std::move_only_function<void(heartbeat_status const&)>;

  // A zero timeout never declares the node lost: it is alive from its first
  // heartbeat on.
  hb_consumer(transport& bus, node_id id, std::chrono::milliseconds timeout);
  ~hb_consumer();
  hb_consumer(hb_consumer const&) = delete;
  hb_consumer& operator=(hb_consumer const&) = delete;

  // Before the first heartbeat: not alive, no state.
  heartbeat_status status() const;

  // Calls `handler` with the new status after every change, and on every
  // boot-up message even when nothing changed: a node that rebooted within
  // the timeout is not missed.
  [[nodiscard]] subscription on_change(change_handler handler);

private:
  friend class cannet::canopen::remote_node;

  // Follows the node to a new id, forgetting what the old one reported;
  // notify_rebound() reports that change once every service has moved.
  void rebind(node_id id);
  void notify_rebound();

  struct state;
  std::unique_ptr<state> state_;
};

} // namespace detail
} // namespace cannet::canopen
