#pragma once

// The subscription registry behind every transport: matching, dispatch, and
// the filter set a transport can hand to the kernel. Private to canopen.

#include <canopen/detail/handler_list.hpp>
#include <canopen/transport.hpp>

#include <cstdint>
#include <vector>

namespace cannet::canopen::detail {

// The CAN_RAW_FILTER rule.
constexpr bool matches(can_filter const& filter, can_frame const& frame)
{
  return (frame.can_id & filter.can_mask) == (filter.can_id & filter.can_mask);
}

class subscribers {
public:
  std::uint64_t add(can_filter filter, transport::frame_handler handler);

  // Safe while dispatching, including from the removed handler itself: the
  // entry stops receiving at once and is erased once dispatch returns.
  void remove(std::uint64_t id);

  // Calls the matching handlers. Handlers may add and remove subscriptions;
  // one added meanwhile first sees the next frame.
  void dispatch(can_frame const& frame);

  // The live filters, deduplicated.
  std::vector<can_filter> filters() const;

private:
  struct entry {
    can_filter filter;
    transport::frame_handler handler;
  };

  handler_list<entry> entries_;
};

} // namespace cannet::canopen::detail
