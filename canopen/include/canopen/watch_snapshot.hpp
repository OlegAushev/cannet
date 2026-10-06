#pragma once

// cannet::canopen::watch_snapshot — the latest reading of every object a
// node's watch polls, for a GUI on a thread of its own. It is one more
// consumer of the watch's events, as history and the sessions of a web
// daemon are: it keeps a table on the client's executor and publishes it
// through a snapshot (snapshot.hpp), which the GUI reads once per frame
// with no lock.
//
// Protocol plane, unprivileged.
//
// Thread model: construct and destroy it on the client's executor, or
// while no thread runs that executor; it takes the watch's events there.
// read() from one thread, the GUI's.

#include <canopen/detail/sdo_client.hpp>
#include <canopen/od.hpp>
#include <canopen/service/watch.hpp>
#include <canopen/snapshot.hpp>
#include <canopen/subscription.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <vector>

namespace cannet::canopen {

// What a watch snapshot holds of one object.
struct watched_value {
  od_entry const* entry = nullptr;
  // The last value read; none before the first.
  std::optional<od_value> value{};
  // Why the last read failed; none when it succeeded. `value` keeps what
  // was read before.
  std::optional<sdo_error> error{};
  // When the last reading came; the clock's epoch before the first.
  std::chrono::steady_clock::time_point time{};
};

class watch_snapshot {
public:
  explicit watch_snapshot(service::watch& watch);
  watch_snapshot(watch_snapshot const&) = delete;
  watch_snapshot& operator=(watch_snapshot const&) = delete;

  // An entry per object of the watch, in its order. Valid and unchanged
  // until the next read().
  std::span<watched_value const> read();

private:
  std::vector<watched_value> table_; // the executor's copy
  snapshot<std::vector<watched_value>> shared_;
  subscription readings_;
};

} // namespace cannet::canopen
