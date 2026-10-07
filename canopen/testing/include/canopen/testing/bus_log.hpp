#pragma once

// cannet::canopen::testing::bus_log — every frame on a loopback bus
// (loopback.hpp), as candump shows it: an endpoint of its own that hears
// what the others send.
//
// Test support over the protocol plane, unprivileged, no I/O.
//
// Thread model: the loopback bus's. Frames are logged on its executor;
// read them there.

#include <canopen/loopback.hpp>

#include <algorithm>
#include <iterator>
#include <vector>

namespace cannet::canopen::testing {

class bus_log {
public:
  explicit bus_log(loopback_bus& bus)
      : endpoint_(bus),
        all_(endpoint_.subscribe(
            {.can_id = 0, .can_mask = 0},
            [this](can_frame const& frame) { frames_.push_back(frame); }))
  {
  }

  bus_log(bus_log const&) = delete;
  bus_log& operator=(bus_log const&) = delete;

  std::vector<can_frame> const& frames() const
  {
    return frames_;
  }

  std::vector<can_frame> with_id(canid_t id) const
  {
    std::vector<can_frame> result;
    std::ranges::copy_if(frames_,
                         std::back_inserter(result),
                         [id](can_frame const& f) { return f.can_id == id; });
    return result;
  }

private:
  loopback_transport endpoint_;
  std::vector<can_frame> frames_;
  subscription all_;
};

} // namespace cannet::canopen::testing
