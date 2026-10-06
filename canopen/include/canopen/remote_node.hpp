#pragma once

// cannet::canopen::remote_node — the host's handle to one device on the
// bus: its node id, its dictionary and the services addressed to it, the
// way emblib's server aggregates the device's own. The client
// (client.hpp) creates it with add_node() and keeps node ids unique; in
// CiA 301 terms the device is an SDO server and this is its client side.
//
// Protocol plane, unprivileged.
//
// Thread model: the client's. Every call, the services' included, on the
// client's executor; handlers and events run there too.

#include <canopen/detail/emcy_consumer.hpp>
#include <canopen/detail/hb_consumer.hpp>
#include <canopen/detail/rpdo_producer.hpp>
#include <canopen/detail/tpdo_consumer.hpp>
#include <canopen/od.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace cannet::canopen {

class client;

struct remote_node_options {
  // Unique within the client: GUIs, CLIs and daemons address nodes by it.
  std::string name{};
  node_id id;
  // The device's objects, for its SDO-based services.
  dictionary_view dictionary{};
  // With no heartbeat for this long the node counts as lost; zero: never.
  std::chrono::milliseconds heartbeat_timeout{2000};
};

class remote_node : public std::enable_shared_from_this<remote_node> {
public:
  ~remote_node();
  remote_node(remote_node const&) = delete;
  remote_node& operator=(remote_node const&) = delete;

  std::string_view name() const;
  node_id id() const;
  dictionary_view dictionary() const;

  detail::hb_consumer heartbeat;
  detail::emcy_consumer emcy;
  detail::tpdo_consumer tpdo;
  detail::rpdo_producer rpdo;

private:
  friend class client;

  remote_node(transport& bus, remote_node_options options);

  // Moves every service to `id`, which the client has checked is free.
  void set_id(node_id id);
  // What the node transmits runs only while the client does.
  void start();
  void stop();

  std::string name_;
  node_id id_;
  dictionary_view dictionary_;
};

} // namespace cannet::canopen
