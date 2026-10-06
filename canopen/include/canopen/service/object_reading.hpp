#pragma once

// cannet::canopen::object_reading — what a service learned by reading one
// of a remote node's objects: its value, or why there is none. The watch
// and config services report their reads as events carrying it.
//
// Protocol plane, unprivileged, no I/O, no state.
//
// Thread model: a plain value. The entry it points at belongs to a
// dictionary, constant data that any thread may read.

#include <canopen/detail/sdo_client.hpp>
#include <canopen/od.hpp>

#include <chrono>

namespace cannet::canopen {

struct object_reading {
  // The object read: an entry of the node's dictionary.
  od_entry const* entry = nullptr;
  // Its value, or why the read failed.
  sdo_result<od_value> value{};
  // When the read completed.
  std::chrono::steady_clock::time_point time{};
};

} // namespace cannet::canopen
