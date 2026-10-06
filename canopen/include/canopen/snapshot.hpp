#pragma once

// cannet::canopen::snapshot — the latest value of some state, handed from
// one thread to another with no lock: the client's executor publishes what
// events tell it, and a GUI on a thread of its own reads the newest whole
// value once per frame. A triple buffer: the writer fills a buffer of its
// own and swaps it for the one in the middle, and the reader swaps its
// buffer for the middle one when a newer value waits there. Neither side
// ever waits for the other; with two buffers, the writer would wait for the
// reader to finish its frame.
//
// Protocol plane, unprivileged, no I/O.
//
// Thread model: one writer and one reader, each on a thread of its own or
// both on one. publish() on the writer's thread, read() on the reader's.

#include <array>
#include <atomic>
#include <concepts>
#include <cstdint>

namespace cannet::canopen {

template<std::copyable T>
class snapshot {
public:
  snapshot() = default;

  // What read() returns before the first publish().
  explicit snapshot(T const& initial) : buffers_{initial, initial, initial} {}

  snapshot(snapshot const&) = delete;
  snapshot& operator=(snapshot const&) = delete;

  // Writer: makes `value` the newest. It is copied into the writer's
  // buffer, whose storage the copy reuses.
  void publish(T const& value)
  {
    buffers_[write_] = value;
    write_ = index_of(
        middle_.exchange(static_cast<std::uint8_t>(write_ | fresh),
                         std::memory_order_acq_rel));
  }

  // Reader: the newest value published, or the one read() returned last
  // when nothing newer has come. It stays valid and unchanged until the
  // next read().
  T const& read()
  {
    if ((middle_.load(std::memory_order_relaxed) & fresh) != 0) {
      read_ = index_of(middle_.exchange(read_, std::memory_order_acq_rel));
    }
    return buffers_[read_];
  }

private:
  static constexpr std::uint8_t fresh = 0x4; // newer than the reader's

  static constexpr std::uint8_t index_of(std::uint8_t middle)
  {
    return static_cast<std::uint8_t>(middle & 0x3);
  }

  std::array<T, 3> buffers_{};
  std::uint8_t write_ = 0;              // the writer's buffer
  std::uint8_t read_ = 1;               // the reader's buffer
  std::atomic<std::uint8_t> middle_{2}; // the one between, and `fresh`
};

} // namespace cannet::canopen
