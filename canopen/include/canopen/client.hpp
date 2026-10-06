#pragma once

// cannet::canopen::client — the host's own CANopen node on one bus: NMT
// master, SYNC producer, heartbeat producer, and the registry of the remote
// nodes (remote_node.hpp) it talks to. One client per bus.
//
// Protocol plane, unprivileged: it reaches an already-up interface through
// a transport (transport.hpp).
//
// Thread model: NOT thread-safe, like the transport it runs on. Make every
// call, remote nodes and their services included, on the client's
// executor, which is the transport's; handlers and events run there. Two
// exceptions may be called from any thread: get_executor(), and the
// initiating functions async_*, which start their work on the client's
// executor themselves and complete on the handler's. A GUI on a thread of
// its own sends commands through async_*, changes settings through a post()
// to the executor, and follows state through events.
//
// The transport must outlive the client. Remote nodes may outlive it, and
// stop transmitting when it goes.

#include <canopen/remote_node.hpp>
#include <canopen/setup_error.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <boost/asio/append.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/default_completion_token.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/dispatch.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace cannet::canopen {

struct client_options {
  // The host's own node id; ucanopen used 127.
  node_id id;
  // Zero: not sent.
  std::chrono::milliseconds heartbeat_period{1000};
  // Zero: not sent. emblib's devices do not consume SYNC.
  std::chrono::milliseconds sync_period{0};
};

namespace detail {

// An asynchronous operation over one frame sent from any thread: `send`
// hands the frame to the transport on its executor, and the transport's
// verdict reaches the final handler through that handler's own executor.
template<typename Send>
struct send_op {
  Send send;
  can_frame frame;

  template<typename Self>
  void operator()(Self& self)
  {
    // Moving `self` moves this object along: take what the call needs first.
    auto start = std::move(send);
    auto const f = frame;
    start(f,
          [self = std::move(self)](
              std::expected<void, transport_error> result) mutable {
            auto const executor = boost::asio::get_associated_executor(self);
            boost::asio::dispatch(executor,
                                  boost::asio::append(std::move(self), result));
          });
  }

  template<typename Self>
  void operator()(Self& self, std::expected<void, transport_error> result)
  {
    self.complete(result);
  }
};

} // namespace detail

class client {
public:
  using executor_type = transport::executor_type;
  // Completion signature of async_nmt(): the transport's verdict on the
  // frame.
  using nmt_signature = void(std::expected<void, transport_error>);

  client(transport& bus, client_options options);
  ~client();
  client(client const&) = delete;
  client& operator=(client const&) = delete;

  // Any thread.
  executor_type get_executor() const noexcept;

  node_id id() const;

  // Start and stop what the host transmits: SYNC, its heartbeat, the RPDOs
  // of every node. Reception and timeouts run regardless, from a node's
  // registration on.
  void start();
  void stop();
  bool started() const;

  // Registers a node; fails with name_taken or node_id_taken, the client's
  // own id included. A node added to a started client transmits at once.
  std::expected<std::shared_ptr<remote_node>, setup_error>
  add_node(remote_node_options options);
  // nullptr when the client has no node by that name.
  std::shared_ptr<remote_node> find_node(std::string_view name) const;
  std::vector<std::shared_ptr<remote_node>> nodes() const;

  // Changes the host's own node id; its next heartbeat carries it.
  std::expected<void, setup_error> set_node_id(node_id id);
  // Moves a node to a new id. What the node at the old id reported is
  // forgotten: its heartbeat status resets and its TPDOs time out.
  std::expected<void, setup_error> set_remote_node_id(std::string_view name,
                                                      node_id id);

  // Zero stops the producer.
  void set_heartbeat_period(std::chrono::milliseconds period);
  void set_sync_period(std::chrono::milliseconds period);

  // NMT node control for every node (node id 0 on the wire). Any thread;
  // any completion token, asio::deferred by default.
  template<boost::asio::completion_token_for<nmt_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_nmt(
      nmt_command command,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return async_send(
        make_nmt_frame({.command = command, .target = std::nullopt}),
        std::forward<Token>(token));
  }

  // NMT node control for one node. ucanopen had no such call.
  template<boost::asio::completion_token_for<nmt_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_nmt(
      node_id target,
      nmt_command command,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return async_send(make_nmt_frame({.command = command, .target = target}),
                      std::forward<Token>(token));
  }

private:
  struct state;

  // Starts on the client's executor; a client gone by then completes the
  // send with transport_error::closed.
  static void send_from_any_thread(std::weak_ptr<state> const& weak,
                                   executor_type const& executor,
                                   can_frame const& frame,
                                   transport::send_handler done);

  // Captures no `this`: a deferred operation may start after the client is
  // gone.
  template<typename Token>
  auto async_send(can_frame const& frame, Token&& token)
  {
    auto send = [weak = std::weak_ptr{state_},
                 executor = executor_](can_frame const& f,
                                       transport::send_handler done) {
      send_from_any_thread(weak, executor, f, std::move(done));
    };
    return boost::asio::async_compose<Token, nmt_signature>(
        detail::send_op<decltype(send)>{std::move(send), frame},
        token,
        executor_);
  }

  executor_type executor_;
  std::shared_ptr<state> state_;
};

} // namespace cannet::canopen
