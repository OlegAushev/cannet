#pragma once

// cannet::canopen::testing::emulated_device — a CANopen device in software,
// for tests: one node behind a transport of its own (an endpoint of the
// loopback bus, or a raw_transport on vcan) that behaves, where tests look,
// as a device on emblib's CANopen server does. It starts operational by
// itself, sends its heartbeat in every state, obeys NMT commands for every
// node or for it, sends TPDOs and takes RPDOs only while operational, and
// serves SDO as emblib's sdo_server does: expedited only, one string
// cursor, restore default at 1011h:04, an answer to every request in the
// order they came. A test gives it its objects; a few knobs make it
// misbehave, for a client's error paths.
//
// Protocol plane, unprivileged, no I/O of its own: everything goes through
// its transport.
//
// Thread model: the transport's. Every call on the transport's executor,
// where the device's handlers run too.

#include <canopen/od.hpp>
#include <canopen/sdo.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cannet::canopen::testing {

// An object of the device's dictionary, as the device holds it.
struct device_object {
  od_value_type type;
  od_access access;
  expedited_sdo_data value{};                   // a scalar's bytes, or the
                                                // last a command was sent
  std::string text{};                           // a string's
  std::optional<expedited_sdo_data> fallback{}; // restorable when set
  int executed = 0;                             // an exec object's runs
};

class emulated_device {
public:
  emulated_device(transport& endpoint,
                  node_id id,
                  std::chrono::milliseconds heartbeat_period)
      : endpoint_(endpoint),
        id_(id),
        heartbeat_period_(heartbeat_period),
        heartbeat_(endpoint.get_executor())
  {
    subscriptions_.push_back(
        endpoint_.subscribe(cob_filter(*cob_id(cob_type::nmt)),
                            [this](can_frame const& frame) { on_nmt(frame); }));
    subscriptions_.push_back(
        endpoint_.subscribe(cob_filter(cob_id(cob_type::rsdo, id)),
                            [this](can_frame const& frame) { on_sdo(frame); }));
    for (unsigned number = 1; number <= 4; ++number) {
      subscriptions_.push_back(endpoint_.subscribe(
          cob_filter(*pdo_cob_id(cob_type::rpdo, number, id)),
          [this, number](can_frame const& frame) {
            if (state_ == nmt_state::operational) {
              rpdos_[number - 1].push_back(frame);
            }
          }));
    }
    if (heartbeat_period_ > std::chrono::milliseconds::zero()) {
      beat();
    }
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

  void add_object(od_key key, device_object object)
  {
    objects_.insert_or_assign(key, std::move(object));
  }

  device_object const& object(od_key key) const
  {
    return objects_.at(key);
  }

  // Every SDO request the device took, in order.
  std::vector<od_key> const& sdo_requests() const
  {
    return sdo_requests_;
  }

  // Requests that came while an earlier answer was still on its way: a
  // client that keeps one request in flight never causes one.
  int overlapping_requests() const
  {
    return overlapping_;
  }

  // Every answer waits this long.
  std::chrono::milliseconds answer_delay{0};
  // The next answer alone waits this long instead.
  std::optional<std::chrono::milliseconds> next_answer_delay;
  // Requests, counted from 0 in the order they came, that are served but
  // whose answers are lost.
  std::function<bool(std::size_t)> lose_answer;
  // The next read is answered as a segmented transfer would begin.
  bool answer_segmented = false;

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

  // emblib's sdo_server::try_handle.
  void on_sdo(can_frame const& frame)
  {
    payload data{};
    std::copy_n(frame.data, CAN_MAX_DLEN, data.begin());
    auto const rsdo = from_payload<expedited_sdo>(data);
    if (rsdo.cs == sdo_cs_codes::abort) {
      return;
    }
    od_key const key{static_cast<std::uint16_t>(rsdo.index),
                     static_cast<std::uint8_t>(rsdo.subindex)};
    auto const request_index = sdo_requests_.size();
    sdo_requests_.push_back(key);
    if (pending_answers_ > 0) {
      ++overlapping_;
    }

    auto const result = [&]() -> std::expected<expedited_sdo, sdo_abort_code> {
      if (rsdo.cs == sdo_cs_codes::client_init_write
          && key == od_key{0x1011, 0x04}) {
        return restore_default(rsdo);
      }
      auto const it = objects_.find(key);
      if (it == objects_.end()) {
        return std::unexpected(sdo_abort_code::object_not_found);
      }
      if (rsdo.cs == sdo_cs_codes::client_init_read) {
        return read(key, it->second, rsdo);
      }
      if (rsdo.cs == sdo_cs_codes::client_init_write) {
        return write(it->second, rsdo);
      }
      return std::unexpected(sdo_abort_code::invalid_cs);
    }();

    if (lose_answer && lose_answer(request_index)) {
      return;
    }
    payload answer;
    if (result) {
      answer = to_payload(*result);
    }
    else {
      abort_sdo abort;
      abort.index = rsdo.index;
      abort.subindex = rsdo.subindex;
      abort.error_code = std::to_underlying(result.error());
      answer = to_payload(abort);
    }
    send_answer(make_frame(cob_id(cob_type::tsdo, id_), CAN_MAX_DLEN, answer));
  }

  std::expected<expedited_sdo, sdo_abort_code> read(od_key key,
                                                    device_object const& object,
                                                    expedited_sdo const& rsdo)
  {
    if (object.access == od_access::wo) {
      return std::unexpected(sdo_abort_code::read_from_write_only);
    }
    expedited_sdo tsdo;
    if (object.type == od_value_type::string) {
      if (text_key_ != key) {
        text_key_ = key;
        text_word_ = 0;
      }
      for (std::size_t i = 0; i < 4; ++i) {
        auto const at = (4 * std::size_t{text_word_}) + i;
        tsdo.data[i] = at < object.text.size()
                         ? static_cast<std::uint8_t>(object.text[at])
                         : std::uint8_t{0};
      }
      bool const last = std::ranges::find(tsdo.data, 0) != tsdo.data.end();
      text_word_ = last ? std::uint16_t{0}
                        : static_cast<std::uint16_t>(text_word_ + 1);
    }
    else {
      tsdo.data = object.value;
    }
    tsdo.index = rsdo.index;
    tsdo.subindex = rsdo.subindex;
    tsdo.cs = sdo_cs_codes::server_init_read;
    tsdo.expedited_transfer = std::exchange(answer_segmented, false) ? 0 : 1;
    tsdo.data_size_indicated = 1;
    tsdo.data_empty_bytes = (4 - od_value_size(object.type)) & 0x3;
    return tsdo;
  }

  static std::expected<expedited_sdo, sdo_abort_code>
  write(device_object& object, expedited_sdo const& rsdo)
  {
    if (object.access != od_access::rw && object.access != od_access::wo) {
      return std::unexpected(sdo_abort_code::write_to_read_only);
    }
    if (object.type == od_value_type::exec) {
      ++object.executed; // a command: the bytes are not a value, though
    } // kept for a test to look at
    object.value = rsdo.data; // taken as the object's own type
    return written(rsdo);
  }

  std::expected<expedited_sdo, sdo_abort_code>
  restore_default(expedited_sdo const& rsdo)
  {
    od_key const target{
        static_cast<std::uint16_t>(rsdo.data[0] | (rsdo.data[1] << 8)),
        rsdo.data[2]};
    auto const it = objects_.find(target);
    if (it == objects_.end()) {
      return std::unexpected(sdo_abort_code::object_not_found);
    }
    if (it->second.access != od_access::rw
        && it->second.access != od_access::wo) {
      return std::unexpected(sdo_abort_code::write_to_read_only);
    }
    if (!it->second.fallback) {
      return std::unexpected(sdo_abort_code::no_data_available);
    }
    it->second.value = *it->second.fallback;
    return written(rsdo);
  }

  static expedited_sdo written(expedited_sdo const& rsdo)
  {
    expedited_sdo tsdo;
    tsdo.index = rsdo.index;
    tsdo.subindex = rsdo.subindex;
    tsdo.cs = sdo_cs_codes::server_init_write;
    return tsdo;
  }

  void send_answer(can_frame const& frame)
  {
    auto const delay =
        std::exchange(next_answer_delay, std::nullopt).value_or(answer_delay);
    if (delay <= std::chrono::milliseconds::zero()) {
      send(frame);
      return;
    }
    ++pending_answers_;
    auto& timer = answer_timers_.emplace_back(endpoint_.get_executor());
    timer.expires_after(delay);
    timer.async_wait([this, frame](boost::system::error_code ec) {
      if (ec) {
        return;
      }
      --pending_answers_;
      send(frame);
    });
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

  transport& endpoint_;
  node_id id_;
  nmt_state state_ = nmt_state::operational; // the firmware starts itself
  std::chrono::milliseconds heartbeat_period_;
  boost::asio::steady_timer heartbeat_;
  std::vector<std::unique_ptr<boost::asio::steady_timer>> tpdo_timers_;
  std::array<std::vector<can_frame>, 4> rpdos_;
  std::map<od_key, device_object> objects_;
  std::vector<od_key> sdo_requests_;
  std::optional<od_key> text_key_; // emblib's text_cursor
  std::uint16_t text_word_ = 0;
  int pending_answers_ = 0;
  int overlapping_ = 0;
  std::list<boost::asio::steady_timer> answer_timers_;
  std::vector<subscription> subscriptions_;
};

} // namespace cannet::canopen::testing
