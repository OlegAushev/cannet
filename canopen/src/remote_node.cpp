#include <canopen/remote_node.hpp>

#include <utility>

namespace cannet::canopen {

remote_node::remote_node(transport& bus, remote_node_options options)
    : sdo(bus, options.id, options.sdo_timeout),
      heartbeat(bus, options.id, options.heartbeat_timeout),
      emcy(bus, options.id),
      tpdo(bus, options.id),
      rpdo(bus, options.id),
      watch(sdo, options.dictionary),
      config(sdo, options.dictionary),
      name_(std::move(options.name)),
      id_(options.id),
      dictionary_(options.dictionary)
{
}

remote_node::~remote_node() = default;

std::string_view remote_node::name() const
{
  return name_;
}

node_id remote_node::id() const
{
  return id_;
}

dictionary_view remote_node::dictionary() const
{
  return dictionary_;
}

void remote_node::set_id(node_id id)
{
  // A handler notified below may drop the last other reference to the node.
  auto const keep = shared_from_this();
  id_ = id;
  sdo.rebind(id);
  heartbeat.rebind(id);
  emcy.rebind(id);
  tpdo.rebind(id);
  rpdo.rebind(id);
  // Every service has moved before any handler runs.
  heartbeat.notify_rebound();
  tpdo.notify_rebound();
}

void remote_node::start()
{
  rpdo.start();
}

void remote_node::stop()
{
  rpdo.stop();
}

void remote_node::detach()
{
  rpdo.stop();
  watch.close();
  config.close();
  sdo.close();
}

} // namespace cannet::canopen
