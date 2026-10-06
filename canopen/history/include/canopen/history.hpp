#pragma once

// cannet::canopen::history — signal history for plots: what nodes' watches
// read, and what the application decodes from TPDOs, kept per signal in a
// ring of fixed capacity, so that each application does not rewrite it. A
// signal is a node's object: the node's name and the object's key. Kept in
// cannet but a target of its own, cannet::canopen-history: only a GUI needs
// it.
//
// Protocol plane, unprivileged.
//
// Thread model: attach() and detach() on the client's executor, where the
// watches' readings come in, and so the history's destruction while it is
// attached; everything else from any thread. A reader holds the history's
// lock while it lives, and recording waits for it: keep one to a frame's
// drawing.

#include <canopen/od.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace cannet::canopen {

class remote_node;

// One point of a signal. `t` is in seconds since the history's origin, and
// a double: a process may run for weeks, and a float would advance in 2 ms
// steps after 4.5 hours. `value` holds every od_value exactly.
struct sample {
  double t;
  double value;
};

// A signal's samples, oldest first, in at most two runs of contiguous
// storage, as a ring holds them. Valid while the reader it came from lives.
class sample_view {
public:
  sample_view() = default;
  sample_view(std::span<sample const> first, std::span<sample const> second)
      : first_(first), second_(second)
  {
  }

  std::size_t size() const
  {
    return first_.size() + second_.size();
  }

  bool empty() const
  {
    return size() == 0;
  }

  // The `i`-th oldest; a plot's getter reads through it.
  sample const& operator[](std::size_t i) const
  {
    return i < first_.size() ? first_[i] : second_[i - first_.size()];
  }

  std::span<sample const> first() const
  {
    return first_;
  }

  std::span<sample const> second() const
  {
    return second_;
  }

private:
  std::span<sample const> first_;
  std::span<sample const> second_;
};

struct history_options {
  // Samples per signal; the oldest go first. Memory grows with the samples,
  // not with the capacity.
  std::size_t capacity{100'000};
};

class history {
public:
  using clock = std::chrono::steady_clock;

  explicit history(history_options options = {});
  ~history();
  history(history const&) = delete;
  history& operator=(history const&) = delete;

  // When t is zero: the history's construction.
  clock::time_point origin() const;

  // Records every reading of `node`'s watch until detach(): a value, or
  // NaN for a read that failed, which a plot draws as a gap. Attaching a
  // node again changes nothing.
  void attach(remote_node& node);
  // Stops recording `node`'s watch; its samples stay.
  void detach(remote_node const& node);

  // Records `value` of the signal (`node`, `key`) at `time`: a value the
  // application decoded from a TPDO, say. Samples are kept in the order
  // they come.
  void push(std::string_view node,
            od_key key,
            double value,
            clock::time_point time = clock::now());

  // Shrinking keeps the newest samples of every signal.
  void set_capacity(std::size_t capacity);
  std::size_t capacity() const;
  // Forgets every sample; attachments stay.
  void clear();

  // The history under its lock.
  class reader {
  public:
    // The samples of the signal (`node`, `key`); empty when it has none.
    sample_view find(std::string_view node, od_key key) const;

  private:
    friend class history;
    explicit reader(history const& h);

    history const& history_;
    std::unique_lock<std::mutex> lock_;
  };

  reader read() const;

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace cannet::canopen
