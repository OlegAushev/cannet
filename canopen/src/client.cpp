#include <canopen/client.hpp>

#include "timers.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>

namespace cannet::canopen {

std::string_view to_string(setup_error e)
{
  switch (e) {
  case setup_error::node_id_taken: return "node id already taken";
  case setup_error::name_taken: return "node name already taken";
  case setup_error::no_such_node: return "no node by that name";
  case setup_error::invalid_pdo: return "invalid PDO number or length";
  }
  return "unknown error";
}

std::string_view name(setup_error e)
{
  switch (e) {
  case setup_error::node_id_taken: return "node_id_taken";
  case setup_error::name_taken: return "name_taken";
  case setup_error::no_such_node: return "no_such_node";
  case setup_error::invalid_pdo: return "invalid_pdo";
  }
  return "unknown";
}

struct client::state {
  state(transport& b, client_options const& options)
      : bus(b),
        id(options.id),
        heartbeat_period(options.heartbeat_period),
        sync_period(options.sync_period),
        heartbeat(b,
                  [this] {
                    // The NMT master reports itself operational.
                    return make_heartbeat_frame(id, nmt_state::operational);
                  }),
        sync(b, [] { return make_sync_frame(); })
  {
  }

  transport& bus;
  node_id id;
  std::chrono::milliseconds heartbeat_period;
  std::chrono::milliseconds sync_period;
  bool started = false;
  detail::periodic_sender heartbeat;
  detail::periodic_sender sync;
  std::vector<std::shared_ptr<remote_node>> nodes;

  std::shared_ptr<remote_node> find(std::string_view node_name) const
  {
    auto const it = std::ranges::find(nodes, node_name, &remote_node::name);
    return it == nodes.end() ? nullptr : *it;
  }

  // Whether a node other than `except` has `node`.
  bool node_has(node_id node, remote_node const* except = nullptr) const
  {
    return std::ranges::any_of(nodes, [&](auto const& n) {
      return n.get() != except && n->id() == node;
    });
  }
};

client::client(transport& bus, client_options options)
    : executor_(bus.get_executor()),
      state_(std::make_shared<state>(bus, options))
{
}

client::~client()
{
  stop();
  // Nodes that outlive the client fall silent: the transport may go next.
  for (auto const& node : state_->nodes) {
    node->detach();
  }
}

client::executor_type client::get_executor() const noexcept
{
  return executor_;
}

node_id client::id() const
{
  return state_->id;
}

void client::start()
{
  auto& s = *state_;
  if (s.started) {
    return;
  }
  s.started = true;
  s.heartbeat.start(s.heartbeat_period);
  s.sync.start(s.sync_period);
  for (auto const& node : s.nodes) {
    node->start();
  }
}

void client::stop()
{
  auto& s = *state_;
  s.started = false;
  s.heartbeat.stop();
  s.sync.stop();
  for (auto const& node : s.nodes) {
    node->stop();
  }
}

bool client::started() const
{
  return state_->started;
}

std::expected<std::shared_ptr<remote_node>, setup_error>
client::add_node(remote_node_options options)
{
  auto& s = *state_;
  if (s.find(options.name)) {
    return std::unexpected(setup_error::name_taken);
  }
  if (options.id == s.id || s.node_has(options.id)) {
    return std::unexpected(setup_error::node_id_taken);
  }
  // remote_node's constructor is private to its client: no make_shared.
  auto const node = std::shared_ptr<remote_node>(
      new remote_node(s.bus, std::move(options)));
  if (s.started) {
    node->start();
  }
  s.nodes.push_back(node);
  return node;
}

std::shared_ptr<remote_node> client::find_node(std::string_view name) const
{
  return state_->find(name);
}

std::vector<std::shared_ptr<remote_node>> client::nodes() const
{
  return state_->nodes;
}

std::expected<void, setup_error> client::set_node_id(node_id id)
{
  auto& s = *state_;
  if (s.node_has(id)) {
    return std::unexpected(setup_error::node_id_taken);
  }
  s.id = id;
  return {};
}

std::expected<void, setup_error>
client::set_remote_node_id(std::string_view name, node_id id)
{
  auto& s = *state_;
  auto const node = s.find(name);
  if (!node) {
    return std::unexpected(setup_error::no_such_node);
  }
  if (node->id() == id) {
    return {};
  }
  if (id == s.id || s.node_has(id, node.get())) {
    return std::unexpected(setup_error::node_id_taken);
  }
  node->set_id(id);
  return {};
}

void client::set_heartbeat_period(std::chrono::milliseconds period)
{
  auto& s = *state_;
  s.heartbeat_period = period;
  if (s.started) {
    s.heartbeat.start(period);
  }
}

void client::set_sync_period(std::chrono::milliseconds period)
{
  auto& s = *state_;
  s.sync_period = period;
  if (s.started) {
    s.sync.start(period);
  }
}

void client::send_from_any_thread(std::weak_ptr<state> const& weak,
                                  executor_type const& executor,
                                  can_frame const& frame,
                                  transport::send_handler done)
{
  boost::asio::dispatch(
      executor,
      [weak, executor, frame, done = std::move(done)]() mutable {
        if (auto const s = weak.lock()) {
          s->bus.send(frame, std::move(done));
          return;
        }
        // Completes as a transport would: never inside the call.
        boost::asio::post(executor, [done = std::move(done)]() mutable {
          done(std::unexpected(transport_error::closed));
        });
      });
}

} // namespace cannet::canopen
