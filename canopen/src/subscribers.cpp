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
  return entries_.add(entry{.filter = filter, .handler = std::move(handler)});
}

void subscribers::remove(std::uint64_t id)
{
  entries_.remove(id);
}

void subscribers::dispatch(can_frame const& frame)
{
  entries_.for_each([&frame](entry& e) {
    if (matches(e.filter, frame)) {
      e.handler(frame);
    }
  });
}

std::vector<can_filter> subscribers::filters() const
{
  std::vector<can_filter> result;
  entries_.visit([&result](entry const& e) { result.push_back(e.filter); });
  std::ranges::sort(result, {}, filter_key);
  auto const duplicates = std::ranges::unique(result, {}, filter_key);
  result.erase(duplicates.begin(), duplicates.end());
  return result;
}

} // namespace cannet::canopen::detail
