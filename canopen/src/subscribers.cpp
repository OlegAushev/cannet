#include "subscribers.hpp"

#include <algorithm>
#include <utility>

namespace cannet::canopen::detail {

namespace {

auto filter_key(can_filter const& filter)
{
  return std::pair{filter.can_id, filter.can_mask};
}

} // namespace

std::uint64_t subscribers::add(can_filter filter,
                               transport::frame_handler handler)
{
  auto const id = next_id_++;
  entries_.push_back(std::make_unique<entry>(
      entry{.id = id, .filter = filter, .handler = std::move(handler)}));
  return id;
}

void subscribers::remove(std::uint64_t id)
{
  auto const it = std::ranges::find(entries_, id, [](auto const& e) {
    return e->id;
  });
  if (it == entries_.end()) {
    return;
  }
  if (dispatching_) {
    // The handler may be running right now: keep it alive until dispatch
    // is over.
    (*it)->active = false;
    has_removed_ = true;
    return;
  }
  entries_.erase(it);
}

void subscribers::dispatch(can_frame const& frame)
{
  struct dispatch_scope {
    subscribers& self;

    explicit dispatch_scope(subscribers& s) : self(s)
    {
      self.dispatching_ = true;
    }

    ~dispatch_scope()
    {
      self.dispatching_ = false;
      if (self.has_removed_) {
        std::erase_if(self.entries_, [](auto const& e) { return !e->active; });
        self.has_removed_ = false;
      }
    }
  } const scope{*this};

  // Entries appended by a handler sit past `count` and wait for the next
  // frame.
  auto const count = entries_.size();
  for (std::size_t i = 0; i < count; ++i) {
    entry& e = *entries_[i];
    if (e.active && matches(e.filter, frame)) {
      e.handler(frame);
    }
  }
}

std::vector<can_filter> subscribers::filters() const
{
  std::vector<can_filter> result;
  for (auto const& e : entries_) {
    if (e->active) {
      result.push_back(e->filter);
    }
  }
  std::ranges::sort(result, {}, filter_key);
  auto const duplicates = std::ranges::unique(result, {}, filter_key);
  result.erase(duplicates.begin(), duplicates.end());
  return result;
}

} // namespace cannet::canopen::detail
