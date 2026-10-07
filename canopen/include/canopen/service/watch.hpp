#pragma once

// cannet::canopen::service::watch — polls a remote node's watch objects
// over SDO, the readable scalar objects of its dictionary's watch category,
// and reports every reading as an event. ucanopen's watch fired its
// requests round-robin, with no timeout and no back-pressure; this one
// sends the next request after an answer or a timeout, so the node's SDO
// queue never holds more than one of its requests. A member of remote_node
// (remote_node.hpp), not a type to use on its own.
//
// A device that stops answering costs one request per SDO timeout, each
// reported as a timeout; another request for the node waits behind at most
// one of the watch's. A request the transport cannot send ends the pass:
// the other objects would fail as well. An object the device answers
// object_not_found for is polled no more: the device lacks it, as one board
// has fewer sensors than another, and will not grow it while it runs.
//
// Protocol plane, unprivileged.
//
// Thread model: the client's (client.hpp). Every call on the client's
// executor; events are emitted there.

#include <canopen/detail/sdo_client.hpp>
#include <canopen/od.hpp>
#include <canopen/service/object_reading.hpp>
#include <canopen/setup_error.hpp>
#include <canopen/subscription.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace cannet::canopen {

class remote_node;

namespace service {

class watch {
public:
  using value_handler = std::move_only_function<void(object_reading const&)>;

  watch(detail::sdo_client& sdo, dictionary_view dictionary);
  ~watch();
  watch(watch const&) = delete;
  watch& operator=(watch const&) = delete;

  // What the watch polls: the readable scalar objects of the dictionary's
  // watch category, ordered by key. Strings and commands are not polled.
  std::span<od_entry const* const> objects() const;

  // The interval between the starts of two passes, each of which reads
  // every enabled object once, one request at a time. A pass that runs
  // longer is followed by the next one at once; missed passes are not made
  // up. Zero, the default: no polling.
  void set_period(std::chrono::milliseconds period);
  std::chrono::milliseconds period() const;

  // Whether the watch polls at all; on by default. Polling stops at once
  // when disabled: the read in flight is dropped, unreported. Enabled
  // again, it starts a pass at once.
  void enable();
  void disable();
  bool enabled() const;

  // Whether one object is polled; every object is by default. Fail with
  // no_such_object for a key objects() does not hold. Enabling an object
  // the device lacked polls it again, as after a firmware update.
  std::expected<void, setup_error> enable(od_key key);
  std::expected<void, setup_error> disable(od_key key);
  bool enabled(od_key key) const;

  // Whether the device answered that it has no such object, which the
  // watch then polls no more, enabled or not. A change of the node's id
  // forgets it.
  bool missing(od_key key) const;

  // Calls `handler` with every reading: a value, or why there is none — a
  // timeout, an abort, a request the transport could not send. A read cut
  // short by disable() or by a change of the node's id is not reported; an
  // object the device lacks is reported once, with its object_not_found.
  [[nodiscard]] subscription on_value(value_handler handler);

private:
  friend class cannet::canopen::remote_node;

  // The node is another device now: forget what the last one lacked.
  void rebind();
  // The client is gone: polling stops for good.
  void close();

  struct state;
  std::shared_ptr<state> state_;
};

} // namespace service
} // namespace cannet::canopen
