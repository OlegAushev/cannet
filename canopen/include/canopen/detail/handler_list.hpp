#pragma once

// cannet::canopen::detail::handler_list — handlers that may be added and
// removed while they are being called: the registry behind transport
// subscriptions and events. An implementation detail of canopen.
//
// Protocol plane, unprivileged, no I/O.
//
// Thread model: NOT thread-safe; the owner's executor serializes every call.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cannet::canopen::detail {

template<typename Entry>
class handler_list {
public:
  // Adds an entry; the returned id removes it.
  std::uint64_t add(Entry entry)
  {
    auto const id = next_id_++;
    slots_.push_back(std::make_unique<slot>(id, std::move(entry)));
    return id;
  }

  // Safe inside for_each(), including from the removed entry's own handler:
  // the entry is skipped from then on and erased once the outermost
  // for_each() returns.
  void remove(std::uint64_t id)
  {
    auto const it = std::ranges::find(slots_, id, [](auto const& s) {
      return s->id;
    });
    if (it == slots_.end()) {
      return;
    }
    if (depth_ > 0) {
      // The entry's handler may be running right now: keep it alive.
      (*it)->live = false;
      has_dead_ = true;
      return;
    }
    slots_.erase(it);
  }

  // Calls `f` with every live entry. `f` may add and remove entries and call
  // for_each() again; an entry added meanwhile waits for the next call.
  template<typename F>
  void for_each(F&& f)
  {
    scope const guard{*this};
    auto const count = slots_.size();
    for (std::size_t i = 0; i < count; ++i) {
      if (slot& s = *slots_[i]; s.live) {
        f(s.entry);
      }
    }
  }

  // Calls `f` with every live entry, read-only.
  template<typename F>
  void visit(F&& f) const
  {
    for (auto const& s : slots_) {
      if (s->live) {
        f(std::as_const(s->entry));
      }
    }
  }

private:
  struct slot {
    slot(std::uint64_t slot_id, Entry slot_entry)
        : id(slot_id), entry(std::move(slot_entry))
    {
    }

    std::uint64_t id;
    Entry entry;
    bool live = true;
  };

  struct scope {
    explicit scope(handler_list& owner) : list(owner)
    {
      ++list.depth_;
    }

    ~scope()
    {
      if (--list.depth_ == 0 && list.has_dead_) {
        std::erase_if(list.slots_, [](auto const& s) { return !s->live; });
        list.has_dead_ = false;
      }
    }

    scope(scope const&) = delete;
    scope& operator=(scope const&) = delete;

    handler_list& list;
  };

  // Slots live on the heap so that an entry keeps its address while the
  // vector grows under a running handler.
  std::vector<std::unique_ptr<slot>> slots_;
  std::uint64_t next_id_ = 1;
  unsigned depth_ = 0; // nested for_each() calls in progress
  bool has_dead_ = false;
};

} // namespace cannet::canopen::detail
