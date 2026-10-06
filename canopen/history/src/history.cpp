#include <canopen/history.hpp>

#include <canopen/remote_node.hpp>
#include <canopen/subscription.hpp>

#include <boost/circular_buffer.hpp>

#include <functional>
#include <limits>
#include <map>
#include <string>
#include <variant>

namespace cannet::canopen {

namespace {

// Allocates as samples come, not the whole capacity at once: a signal
// that is never plotted for long costs little.
using ring = boost::circular_buffer_space_optimized<sample>;

double to_double(od_value const& value)
{
  return std::visit([](auto v) { return static_cast<double>(v); }, value);
}

} // namespace

struct history::state {
  explicit state(history_options options) : capacity(options.capacity) {}

  void record(std::string_view node,
              od_key key,
              double value,
              clock::time_point time)
  {
    double const t = std::chrono::duration<double>(time - origin).count();
    std::scoped_lock const lock{mutex};
    auto node_signals = signals.find(node);
    if (node_signals == signals.end()) {
      node_signals = signals.try_emplace(std::string{node}).first;
    }
    auto& samples =
        node_signals->second.try_emplace(key, capacity).first->second;
    samples.push_back({.t = t, .value = value});
  }

  clock::time_point const origin = clock::now();

  mutable std::mutex mutex; // guards what follows, up to `attached`
  std::size_t capacity;
  std::map<std::string, std::map<od_key, ring>, std::less<>> signals;

  // The client's executor's.
  std::map<std::string, subscription, std::less<>> attached;
};

history::history(history_options options)
    : state_(std::make_unique<state>(options))
{
}

history::~history() = default;

history::clock::time_point history::origin() const
{
  return state_->origin;
}

void history::attach(remote_node& node)
{
  auto& s = *state_;
  if (s.attached.contains(node.name())) {
    return;
  }
  s.attached.try_emplace(
      std::string{node.name()},
      node.watch.on_value([&s, name = std::string{node.name()}](
                              object_reading const& r) {
        double const value = r.value ? to_double(*r.value)
                                     : std::numeric_limits<double>::quiet_NaN();
        s.record(name, r.entry->key, value, r.time);
      }));
}

void history::detach(remote_node const& node)
{
  auto& attached = state_->attached;
  if (auto const it = attached.find(node.name()); it != attached.end()) {
    attached.erase(it);
  }
}

void history::push(std::string_view node,
                   od_key key,
                   double value,
                   clock::time_point time)
{
  state_->record(node, key, value, time);
}

void history::set_capacity(std::size_t capacity)
{
  auto& s = *state_;
  std::scoped_lock const lock{s.mutex};
  s.capacity = capacity;
  for (auto& [node, node_signals] : s.signals) {
    for (auto& [key, samples] : node_signals) {
      samples.rset_capacity(capacity); // drops the oldest
    }
  }
}

std::size_t history::capacity() const
{
  std::scoped_lock const lock{state_->mutex};
  return state_->capacity;
}

void history::clear()
{
  std::scoped_lock const lock{state_->mutex};
  state_->signals.clear();
}

history::reader history::read() const
{
  return reader{*this};
}

history::reader::reader(history const& h) : history_(h), lock_(h.state_->mutex)
{
}

sample_view history::reader::find(std::string_view node, od_key key) const
{
  auto const& signals = history_.state_->signals;
  auto const node_signals = signals.find(node);
  if (node_signals == signals.end()) {
    return {};
  }
  auto const samples = node_signals->second.find(key);
  if (samples == node_signals->second.end()) {
    return {};
  }
  auto const one = samples->second.array_one();
  auto const two = samples->second.array_two();
  return {{one.first, one.second}, {two.first, two.second}};
}

} // namespace cannet::canopen
