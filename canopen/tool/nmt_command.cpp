// nmt: one NMT command, from the CLI's own node.

#include "command_support.hpp"

#include <ostream>
#include <print>

namespace cannet::canopen::tool {

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

} // namespace

asio::awaitable<int> nmt(transport& bus,
                         nmt_command command,
                         std::optional<node_id> target,
                         console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  // NMT frames carry no sender: the CLI's own node id does not matter.
  client host{bus, {.id = node_id::literal(127), .heartbeat_period = 0ms}};
  auto const sent = target ? co_await host.async_nmt(*target,
                                                     command,
                                                     uninterrupted())
                           : co_await host.async_nmt(command, uninterrupted());
  if (!sent) {
    std::print(io.err,
               "canopen: NMT {} not sent: {}\n",
               cli_name(command),
               to_string(sent.error()));
    co_return 1;
  }
  co_return 0;
}

} // namespace cannet::canopen::tool
