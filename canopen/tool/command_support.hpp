#pragma once

// What the canopen CLI's bus commands (bus_commands.hpp) share: the CLI's
// own node, the device's, and how a request is made and reported.

#include "bus_commands.hpp"

#include <canopen/client.hpp>
#include <canopen/remote_node.hpp>
#include <canopen/sdo_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <memory>

namespace cannet::canopen::tool {

// The completion token of a request that a command's cancellation must not
// cut short: a command looks for a stop between its requests instead, and
// so turns off throw_if_cancelled first.
inline auto uninterrupted()
{
  return boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{},
                                             boost::asio::use_awaitable);
}

// Whether the command has been asked to stop.
inline boost::asio::awaitable<bool> stop_requested()
{
  auto const state = co_await boost::asio::this_coro::cancellation_state;
  co_return state.cancelled() != boost::asio::cancellation_type::none;
}

// The CLI's own node, which is never started.
client_options host_options(node_options const& options);

// The device, registered with the CLI's node; null, with the reason on
// `io.err`, when it has the CLI's own id.
std::shared_ptr<remote_node> add_device(client& host,
                                        node_options const& options,
                                        console io);

// "canopen: 3000:01 config::drive::speed: no SDO answer in time".
void report(console io, object_ref const& object, sdo_error const& error);

} // namespace cannet::canopen::tool
