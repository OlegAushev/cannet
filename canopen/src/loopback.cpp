#include <canopen/loopback.hpp>

#include "subscribers.hpp"

#include <canopen/event.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace cannet::canopen {

namespace detail {

struct loopback_endpoint {
  detail::subscribers subscribers;
  std::optional<transport_error> fail_next;
  bus_status status;
  event<bus_status> changed;
};

struct loopback_bus_state {
  loopback_bus::executor_type executor;
  std::vector<std::weak_ptr<loopback_endpoint>> endpoints;
};

} // namespace detail

loopback_bus::loopback_bus(executor_type const& executor)
    : state_(std::make_shared<detail::loopback_bus_state>(
          detail::loopback_bus_state{.executor = executor, .endpoints = {}}))
{
}

loopback_bus::~loopback_bus() = default;

loopback_bus::executor_type loopback_bus::get_executor() const
{
  return state_->executor;
}

loopback_transport::loopback_transport(loopback_bus& bus)
    : bus_(bus.state_), endpoint_(std::make_shared<detail::loopback_endpoint>())
{
  std::erase_if(bus_->endpoints, [](auto const& e) { return e.expired(); });
  bus_->endpoints.push_back(endpoint_);
}

loopback_transport::~loopback_transport()
{
  auto const self = endpoint_.get();
  std::erase_if(bus_->endpoints, [self](auto const& e) {
    auto const endpoint = e.lock();
    return !endpoint || endpoint.get() == self;
  });
}

loopback_transport::executor_type loopback_transport::get_executor()
{
  return bus_->executor;
}

void loopback_transport::send(can_frame const& frame, send_handler done)
{
  if (auto const error = std::exchange(endpoint_->fail_next, std::nullopt)) {
    boost::asio::post(bus_->executor,
                      [done = std::move(done), e = *error]() mutable {
                        done(std::unexpected(e));
                      });
    return;
  }

  boost::asio::post(bus_->executor,
                    [bus = bus_,
                     sender = std::weak_ptr{endpoint_},
                     frame,
                     done = std::move(done)]() mutable {
                      auto const from = sender.lock();
                      if (!from) {
                        done(std::unexpected(transport_error::closed));
                        return;
                      }
                      // A handler may create or destroy endpoints: walk a copy.
                      auto const endpoints = bus->endpoints;
                      for (auto const& e : endpoints) {
                        auto const to = e.lock();
                        if (to && to != from) {
                          to->subscribers.dispatch(frame);
                        }
                      }
                      done({});
                    });
}

subscription loopback_transport::subscribe(can_filter filter,
                                           frame_handler on_frame)
{
  auto const id = endpoint_->subscribers.add(filter, std::move(on_frame));
  return subscription{[weak = std::weak_ptr{endpoint_}, id] {
    if (auto const endpoint = weak.lock()) {
      endpoint->subscribers.remove(id);
    }
  }};
}

void loopback_transport::fail_next_send(transport_error error)
{
  endpoint_->fail_next = error;
}

bus_status loopback_transport::status() const
{
  return endpoint_->status;
}

subscription loopback_transport::on_status(status_handler on_status)
{
  return endpoint_->changed.subscribe(std::move(on_status));
}

void loopback_transport::set_status(bus_status status)
{
  if (status == endpoint_->status) {
    return;
  }
  // Kept alive through a handler that destroys this transport.
  auto const endpoint = endpoint_;
  endpoint->status = status;
  endpoint->changed.emit(endpoint->status);
}

} // namespace cannet::canopen
