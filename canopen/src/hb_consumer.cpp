#include <canopen/detail/hb_consumer.hpp>

#include "timers.hpp"

#include <utility>

namespace cannet::canopen::detail {

struct hb_consumer::state {
  state(transport& b, std::chrono::milliseconds timeout)
      : bus(b), lost(b.get_executor(), timeout, [this] { on_lost(); })
  {
  }

  transport& bus;
  watchdog lost;
  subscription frames;
  heartbeat_status status;
  bool rebound = false; // a change for notify_rebound() to report
  event<heartbeat_status> changed;

  void bind(node_id id)
  {
    frames = bus.subscribe(cob_filter(cob_id(cob_type::heartbeat, id)),
                           [this](can_frame const& frame) { on_frame(frame); });
  }

  void on_frame(can_frame const& frame)
  {
    auto const reported = decode_heartbeat(frame);
    if (!reported) {
      return;
    }
    lost.kick();
    heartbeat_status const next{.alive = true, .state = *reported};
    if (next == status && *reported != nmt_state::initializing) {
      return;
    }
    status = next;
    changed.emit(next); // last: a handler may reconfigure or drop the node
  }

  void on_lost()
  {
    status.alive = false; // the state it reported last stays
    auto const current = status;
    changed.emit(current);
  }
};

hb_consumer::hb_consumer(transport& bus,
                         node_id id,
                         std::chrono::milliseconds timeout)
    : state_(std::make_unique<state>(bus, timeout))
{
  state_->bind(id);
}

hb_consumer::~hb_consumer() = default;

heartbeat_status hb_consumer::status() const
{
  return state_->status;
}

subscription hb_consumer::on_change(change_handler handler)
{
  return state_->changed.subscribe(std::move(handler));
}

void hb_consumer::rebind(node_id id)
{
  auto& s = *state_;
  s.bind(id);
  s.lost.stop();
  s.rebound = s.rebound || s.status != heartbeat_status{};
  s.status = {};
}

void hb_consumer::notify_rebound()
{
  auto& s = *state_;
  if (std::exchange(s.rebound, false)) {
    auto const current = s.status;
    s.changed.emit(current);
  }
}

} // namespace cannet::canopen::detail
