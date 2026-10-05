#include <canopen/raw_transport.hpp>

#include "subscribers.hpp"

#include <cansocket/raw/async_socket.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>

namespace cannet::canopen {

namespace {

using namespace std::chrono_literals;

// The pause before a failed receive is retried.
constexpr auto receive_retry_delay = 100ms;

std::expected<void, transport_error>
to_send_result(std::expected<void, raw::socket_error> result)
{
  if (result) {
    return {};
  }
  switch (result.error()) {
  case raw::socket_error::tx_queue_full:
    return std::unexpected(transport_error::tx_queue_full);
  case raw::socket_error::not_open:
  case raw::socket_error::cancelled:
    return std::unexpected(transport_error::closed);
  default: return std::unexpected(transport_error::send_failed);
  }
}

} // namespace

struct raw_transport::state : std::enable_shared_from_this<state> {
  struct queued_send {
    can_frame frame;
    send_handler done;
  };

  explicit state(executor_type const& executor)
      : socket(executor), retry_timer(executor)
  {
  }

  raw::async_socket socket;
  boost::asio::steady_timer retry_timer;
  detail::subscribers subscribers;
  std::deque<queued_send> queue; // front() is in flight while `sending`
  bool sending = false;
  // Bumped by every open() and close(): completions of an earlier socket
  // see a stale generation and stop.
  std::uint64_t generation = 0;

  void apply_filters()
  {
    if (!socket.is_open()) {
      return;
    }
    if (socket.set_filters(subscribers.filters())) {
      return;
    }
    // The kernel caps the filter count. Past the cap, receive everything
    // and let the subscriptions' filters sort it out.
    static constexpr can_filter everything{.can_id = 0, .can_mask = 0};
    static_cast<void>(socket.set_filters(std::span{&everything, 1}));
  }

  void receive(std::uint64_t gen)
  {
    socket.async_receive(
        [weak = weak_from_this(),
         gen](std::expected<can_frame, raw::socket_error> result) {
          if (auto const self = weak.lock()) {
            self->on_receive(gen, result);
          }
        });
  }

  void on_receive(std::uint64_t gen,
                  std::expected<can_frame, raw::socket_error> const& result)
  {
    if (gen != generation) {
      return;
    }
    if (result) {
      subscribers.dispatch(*result);
      if (gen == generation) { // a handler may have closed the transport
        receive(gen);
      }
      return;
    }
    if (result.error() == raw::socket_error::cancelled
        || result.error() == raw::socket_error::not_open) {
      return;
    }
    retry_timer.expires_after(receive_retry_delay);
    retry_timer.async_wait(
        [weak = weak_from_this(), gen](boost::system::error_code ec) {
          auto const self = weak.lock();
          if (!ec && self && gen == self->generation) {
            self->receive(gen);
          }
        });
  }

  void send_next()
  {
    if (queue.empty()) {
      sending = false;
      return;
    }
    sending = true;
    socket.async_send(queue.front().frame,
                      [weak = weak_from_this(), gen = generation](
                          std::expected<void, raw::socket_error> result) {
                        auto const self = weak.lock();
                        if (!self || gen != self->generation) {
                          return; // close() has completed the queue already
                        }
                        auto done = std::move(self->queue.front().done);
                        self->queue.pop_front();
                        done(to_send_result(result));
                        if (gen == self->generation) {
                          self->send_next();
                        }
                      });
  }

  void complete_queue_closed()
  {
    auto queued = std::exchange(queue, {});
    sending = false;
    for (auto& q : queued) {
      boost::asio::post(socket.get_executor(),
                        [done = std::move(q.done)]() mutable {
                          done(std::unexpected(transport_error::closed));
                        });
    }
  }
};

raw_transport::raw_transport(executor_type const& executor)
    : state_(std::make_shared<state>(executor))
{
}

raw_transport::~raw_transport()
{
  close();
}

std::expected<void, raw::socket_error>
raw_transport::open(std::string_view iface)
{
  close();
  if (auto const opened = state_->socket.open(iface); !opened) {
    return opened;
  }
  state_->apply_filters();
  state_->receive(state_->generation);
  return {};
}

void raw_transport::close()
{
  auto& s = *state_;
  ++s.generation;
  s.retry_timer.cancel();
  if (s.socket.is_open()) {
    static_cast<void>(s.socket.close());
  }
  s.complete_queue_closed();
}

bool raw_transport::is_open() const
{
  return state_->socket.is_open();
}

raw_transport::executor_type raw_transport::get_executor()
{
  return state_->socket.get_executor();
}

void raw_transport::send(can_frame const& frame, send_handler done)
{
  auto& s = *state_;
  if (!s.socket.is_open()) {
    boost::asio::post(s.socket.get_executor(),
                      [done = std::move(done)]() mutable {
                        done(std::unexpected(transport_error::closed));
                      });
    return;
  }
  s.queue.push_back({.frame = frame, .done = std::move(done)});
  if (!s.sending) {
    s.send_next();
  }
}

subscription raw_transport::subscribe(can_filter filter, frame_handler on_frame)
{
  auto const id = state_->subscribers.add(filter, std::move(on_frame));
  state_->apply_filters();
  return subscription{[weak = std::weak_ptr{state_}, id] {
    if (auto const s = weak.lock()) {
      s->subscribers.remove(id);
      s->apply_filters();
    }
  }};
}

} // namespace cannet::canopen
