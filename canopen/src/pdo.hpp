#pragma once

// What the TPDO consumer and the RPDO producer share. Private to canopen.

#include <canopen/types.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>

#include <cstddef>
#include <memory>

namespace cannet::canopen::detail {

// PDOs 1..4: the predefined connection set.
inline constexpr std::size_t pdo_count = 4;

inline bool valid_pdo(unsigned number, std::uint8_t len)
{
  return number >= 1 && number <= pdo_count && len <= CAN_MAX_DLEN;
}

// Releases a PDO slot that a new setup replaced, once the executor gets to
// it: the slot's own handler or provider may be the one that replaced it,
// and must be able to return into a live object. The caller has stopped
// the slot already.
template<typename Slot>
void retire(boost::asio::any_io_executor const& executor,
            std::unique_ptr<Slot> slot)
{
  boost::asio::post(executor, [old = std::move(slot)] {});
}

} // namespace cannet::canopen::detail
