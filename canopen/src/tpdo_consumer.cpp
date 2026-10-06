#include <canopen/detail/tpdo_consumer.hpp>

#include "pdo.hpp"
#include "timers.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace cannet::canopen::detail {

namespace {

struct tpdo_slot {
  tpdo_slot(transport& bus, tpdo_config c)
      : config(std::move(c)),
        watch(bus.get_executor(), config.timeout, [this] { on_expired(); })
  {
  }

  tpdo_config config;
  watchdog watch;
  subscription frames;
  bool rebound = false; // a timeout for notify_rebound() to report

  void bind(transport& bus, unsigned number, node_id id)
  {
    frames = bus.subscribe(cob_filter(*pdo_cob_id(cob_type::tpdo, number, id)),
                           [this](can_frame const& frame) { on_frame(frame); });
  }

  void on_frame(can_frame const& frame)
  {
    if (frame.len < config.len) {
      return; // shorter than the PDO's mapping
    }
    watch.kick();
    payload data{};
    std::copy_n(frame.data,
                std::min<std::size_t>(frame.len, CAN_MAX_DLEN),
                data.begin());
    if (config.handler) {
      config.handler(data); // last: it may replace this slot
    }
  }

  void on_expired()
  {
    if (config.on_timeout) {
      config.on_timeout();
    }
  }
};

} // namespace

struct tpdo_consumer::state {
  state(transport& b, node_id i) : bus(b), id(i) {}

  transport& bus;
  node_id id;
  std::array<std::unique_ptr<tpdo_slot>, pdo_count> slots;
};

tpdo_consumer::tpdo_consumer(transport& bus, node_id id)
    : state_(std::make_unique<state>(bus, id))
{
}

tpdo_consumer::~tpdo_consumer() = default;

std::expected<void, setup_error> tpdo_consumer::setup(unsigned number,
                                                      tpdo_config config)
{
  if (!valid_pdo(number, config.len)) {
    return std::unexpected(setup_error::invalid_pdo);
  }
  auto& s = *state_;
  auto& slot = s.slots[number - 1];
  if (slot) {
    slot->frames.reset();
    slot->watch.stop();
    retire(s.bus.get_executor(), std::move(slot));
  }
  slot = std::make_unique<tpdo_slot>(s.bus, std::move(config));
  slot->bind(s.bus, number, s.id);
  slot->watch.kick();
  return {};
}

void tpdo_consumer::rebind(node_id id)
{
  auto& s = *state_;
  s.id = id;
  for (unsigned number = 1; number <= pdo_count; ++number) {
    if (auto const& slot = s.slots[number - 1]) {
      slot->bind(s.bus, number, id);
      if (slot->watch.running()) {
        slot->watch.stop();
        slot->rebound = true;
      }
    }
  }
}

void tpdo_consumer::notify_rebound()
{
  // By index: a timeout handler may set up a TPDO anew.
  for (std::size_t i = 0; i < pdo_count; ++i) {
    if (auto* slot = state_->slots[i].get();
        slot && std::exchange(slot->rebound, false)) {
      slot->on_expired();
    }
  }
}

} // namespace cannet::canopen::detail
