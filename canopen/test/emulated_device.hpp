#pragma once

// An emulated device for the client's tests: one node on a loopback bus that
// behaves, where the tests look, as emblib's server does in the
// adpt-etk-inverter firmware. It starts operational by itself, sends its
// heartbeat in every state, obeys NMT commands for every node or for it,
// and sends TPDOs and takes RPDOs only while operational.

#include <canopen/loopback.hpp>
#include <canopen/types.hpp>

#include <boost/asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace cannet::canopen::test {

class emulated_device {
public:
  emulated_device(loopback_bus& bus,
                  node_id id,
                  std::chrono::milliseconds heartbeat_period)
      : endpoint_(bus),
        id_(id),
        heartbeat_period_(heartbeat_period),
        heartbeat_(bus.get_executor())
  {
    subscriptions_.push_back(
        endpoint_.subscribe(cob_filter(*cob_id(cob_type::nmt)),
                            [this](can_frame const& frame) { on_nmt(frame); }));
    for (unsigned number = 1; number <= 4; ++number) {
      subscriptions_.push_back(endpoint_.subscribe(
          cob_filter(*pdo_cob_id(cob_type::rpdo, number, id)),
          [this, number](can_frame const& frame) {
            if (state_ == nmt_state::operational) {
              rpdos_[number - 1].push_back(frame);
            }
          }));
    }
    beat();
  }

  emulated_device(emulated_device const&) = delete;
  emulated_device& operator=(emulated_device const&) = delete;

  nmt_state state() const
  {
    return state_;
  }

  // RPDO `number` as the device took it, while operational.
  std::vector<can_frame> const& rpdos(unsigned number) const
  {
    return rpdos_[number - 1];
  }

  // Sends TPDO `number` every `period` while operational.
  void produce_tpdo(unsigned number,
                    std::chrono::milliseconds period,
                    std::function<payload()> provider)
  {
    auto& timer = tpdo_timers_.emplace_back(
        std::make_unique<boost::asio::steady_timer>(endpoint_.get_executor()));
    tpdo_loop(*timer,
              *pdo_cob_id(cob_type::tpdo, number, id_),
              period,
              std::move(provider));
  }

private:
  void on_nmt(can_frame const& frame)
  {
    auto const request = decode_nmt(frame);
    if (!request || (request->target && *request->target != id_)) {
      return;
    }
    switch (request->command) {
    case nmt_command::start: state_ = nmt_state::operational; break;
    case nmt_command::stop: state_ = nmt_state::stopped; break;
    case nmt_command::enter_pre_operational:
    case nmt_command::reset_node:
    case nmt_command::reset_communication:
      state_ = nmt_state::pre_operational;
      break;
    }
  }

  // As emblib's hb_producer: the first heartbeat one period after start.
  void beat()
  {
    heartbeat_.expires_after(heartbeat_period_);
    heartbeat_.async_wait([this](boost::system::error_code ec) {
      if (ec) {
        return;
      }
      send(make_heartbeat_frame(id_, state_));
      beat();
    });
  }

  void tpdo_loop(boost::asio::steady_timer& timer,
                 canid_t cob,
                 std::chrono::milliseconds period,
                 std::function<payload()> provider)
  {
    timer.expires_after(period);
    timer.async_wait(
        [this, &timer, cob, period, provider = std::move(provider)](
            boost::system::error_code ec) mutable {
          if (ec) {
            return;
          }
          if (state_ == nmt_state::operational) {
            send(make_frame(cob, CAN_MAX_DLEN, provider()));
          }
          tpdo_loop(timer, cob, period, std::move(provider));
        });
  }

  void send(can_frame const& frame)
  {
    endpoint_.send(frame, [](auto const&) {});
  }

  loopback_transport endpoint_;
  node_id id_;
  nmt_state state_ = nmt_state::operational; // the firmware starts itself
  std::chrono::milliseconds heartbeat_period_;
  boost::asio::steady_timer heartbeat_;
  std::vector<std::unique_ptr<boost::asio::steady_timer>> tpdo_timers_;
  std::vector<subscription> subscriptions_;
  std::array<std::vector<can_frame>, 4> rpdos_;
};

} // namespace cannet::canopen::test
