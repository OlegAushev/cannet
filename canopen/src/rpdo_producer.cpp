#include <canopen/detail/rpdo_producer.hpp>

#include "pdo.hpp"
#include "timers.hpp"

#include <array>
#include <utility>

namespace cannet::canopen::detail {

namespace {

struct rpdo_slot {
  // `id` is the producer's, followed through every rebind.
  rpdo_slot(transport& bus, unsigned n, rpdo_config c, node_id const& id)
      : config(std::move(c)), number(n), node(id), sender(bus, [this] {
          return make_frame(*pdo_cob_id(cob_type::rpdo, number, node),
                            config.len,
                            config.provider());
        })
  {
  }

  rpdo_config config;
  unsigned number;
  node_id const& node;
  periodic_sender sender;
};

} // namespace

struct rpdo_producer::state {
  state(transport& b, node_id i) : bus(b), id(i) {}

  transport& bus;
  node_id id;
  bool started = false;
  bool all_enabled = true;
  std::array<bool, pdo_count> enabled{true, true, true, true};
  std::array<std::unique_ptr<rpdo_slot>, pdo_count> slots;

  // Runs the RPDO's sender exactly when the RPDO is due to go out.
  void update(std::size_t i)
  {
    auto const& slot = slots[i];
    if (!slot) {
      return;
    }
    bool const due = started
                  && all_enabled
                  && enabled[i]
                  && slot->config.provider
                  && slot->config.period > std::chrono::milliseconds::zero();
    if (due && !slot->sender.running()) {
      slot->sender.start(slot->config.period);
    }
    else if (!due && slot->sender.running()) {
      slot->sender.stop();
    }
  }

  void update_all()
  {
    for (std::size_t i = 0; i < pdo_count; ++i) {
      update(i);
    }
  }

  std::expected<void, setup_error> set_enabled(unsigned number, bool on)
  {
    if (!valid_pdo(number, 0)) {
      return std::unexpected(setup_error::invalid_pdo);
    }
    enabled[number - 1] = on;
    update(number - 1);
    return {};
  }
};

rpdo_producer::rpdo_producer(transport& bus, node_id id)
    : state_(std::make_unique<state>(bus, id))
{
}

rpdo_producer::~rpdo_producer() = default;

std::expected<void, setup_error> rpdo_producer::setup(unsigned number,
                                                      rpdo_config config)
{
  if (!valid_pdo(number, config.len)) {
    return std::unexpected(setup_error::invalid_pdo);
  }
  auto& s = *state_;
  auto& slot = s.slots[number - 1];
  if (slot) {
    slot->sender.stop();
    retire(s.bus.get_executor(), std::move(slot));
  }
  slot = std::make_unique<rpdo_slot>(s.bus, number, std::move(config), s.id);
  s.update(number - 1);
  return {};
}

std::expected<void, setup_error> rpdo_producer::enable(unsigned number)
{
  return state_->set_enabled(number, true);
}

std::expected<void, setup_error> rpdo_producer::disable(unsigned number)
{
  return state_->set_enabled(number, false);
}

bool rpdo_producer::enabled(unsigned number) const
{
  return valid_pdo(number, 0) && state_->enabled[number - 1];
}

void rpdo_producer::enable()
{
  state_->all_enabled = true;
  state_->update_all();
}

void rpdo_producer::disable()
{
  state_->all_enabled = false;
  state_->update_all();
}

bool rpdo_producer::enabled() const
{
  return state_->all_enabled;
}

void rpdo_producer::rebind(node_id id)
{
  state_->id = id; // the next frames carry it
}

void rpdo_producer::start()
{
  state_->started = true;
  state_->update_all();
}

void rpdo_producer::stop()
{
  state_->started = false;
  state_->update_all();
}

} // namespace cannet::canopen::detail
