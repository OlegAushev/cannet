#pragma once

// cannet::canopen::detail::client_op — the composed operation behind the
// initiating functions of a node's services over SDO: it hops onto the
// client's executor, hands its work to the service there, and reaches its
// handler through the handler's own executor, so the completion token
// decides how the result comes back. Private to the services, not a type to
// use on its own.
//
// Protocol plane, unprivileged.
//
// Thread model: an operation may be initiated on any thread; its work
// starts on the client's executor.

#include <canopen/sdo_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/append.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include <expected>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace cannet::canopen::detail {

// Narrows a service's verdict, a variant of the results of all its
// operations, to the result of one.
template<typename T, typename... Ts>
sdo_result<T> narrow(sdo_result<std::variant<Ts...>>&& outcome)
{
  if (!outcome) {
    return std::unexpected(outcome.error());
  }
  if constexpr (std::is_void_v<T>) {
    return {};
  }
  else {
    return std::get<T>(std::move(*outcome));
  }
}

// One operation of a service, completing with sdo_result<T>. On the
// client's executor it calls submit(slot, complete), which starts the work:
// `complete` must then run exactly once, with the service's verdict, an
// `Outcome`, or with `cancelled` once `slot` fires.
template<typename T, typename Outcome, typename Submit>
struct client_op {
  Submit submit;
  boost::asio::any_io_executor executor;
  bool on_executor = false;

  template<typename Self>
  void operator()(Self& self)
  {
    // Moving `self` moves this object along: take what the steps need first.
    auto const ex = executor;
    if (!on_executor) {
      // Posted, never dispatched: an operation must not complete inside
      // its initiating function.
      on_executor = true;
      boost::asio::post(ex, boost::asio::bind_executor(ex, std::move(self)));
      return;
    }
    auto start = std::move(submit);
    auto const slot = self.get_cancellation_state().slot();
    bool const cancelled = self.get_cancellation_state().cancelled()
                        != boost::asio::cancellation_type::none;
    auto complete = [self = std::move(self)](Outcome outcome) mutable {
      // Boxed: moved through Asio's handler wrappers, an expected loses
      // GCC's track of the member it holds, and -O3 warns that its error
      // may be used uninitialized.
      auto boxed = std::make_unique<Outcome>(std::move(outcome));
      auto const handler_ex = boost::asio::get_associated_executor(self);
      boost::asio::dispatch(
          handler_ex,
          boost::asio::append(std::move(self), std::move(boxed)));
    };
    if (cancelled) {
      // Cancelled on its way here, before the service could hear of it.
      complete(
          std::unexpected(sdo_error{.reason = sdo_error::kind::cancelled}));
      return;
    }
    start(slot, std::move(complete));
  }

  template<typename Self>
  void operator()(Self& self, std::unique_ptr<Outcome> outcome)
  {
    self.complete(narrow<T>(std::move(*outcome)));
  }
};

} // namespace cannet::canopen::detail
