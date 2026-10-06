#pragma once

// cannet::canopen::detail::emcy_consumer — the host's EMCY consumer for one
// remote node: every emergency the node sends, decoded. The host half of
// emblib's emcy_producer, and a member of remote_node (remote_node.hpp), not
// a type to use on its own.
//
// Protocol plane, unprivileged.
//
// Thread model: the client's (client.hpp). Every call on the client's
// executor; events are emitted there.

#include <canopen/event.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <functional>
#include <memory>

namespace cannet::canopen {

class remote_node;

namespace detail {

class emcy_consumer {
public:
  using emcy_handler = std::move_only_function<void(emcy_message const&)>;

  emcy_consumer(transport& bus, node_id id);
  ~emcy_consumer();
  emcy_consumer(emcy_consumer const&) = delete;
  emcy_consumer& operator=(emcy_consumer const&) = delete;

  // Calls `handler` with every emergency the node sends, error resets
  // (code 0x0000) included.
  [[nodiscard]] subscription on_emcy(emcy_handler handler);

private:
  friend class cannet::canopen::remote_node;

  void rebind(node_id id);

  struct state;
  std::unique_ptr<state> state_;
};

} // namespace detail
} // namespace cannet::canopen
