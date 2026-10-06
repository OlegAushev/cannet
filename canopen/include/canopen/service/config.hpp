#pragma once

// cannet::canopen::service::config — a remote node's parameters, the
// objects of its dictionary's config category: reading them all, reading
// and writing one, typed by the dictionary, storing them all in the
// device's non-volatile memory, restoring defaults, one or all. A member of
// remote_node (remote_node.hpp), not a type to use on its own.
//
// A write checks the value's type against the dictionary: the device takes
// the bytes as its own object's type, so a float written to an int32
// object would land as garbage. Storing all and restoring all are
// CiA 301's 1010h:01 and 1011h:01, written with the signatures CiA 301 asks
// for, "save" and "load"; emblib's devices ignore a command's bytes, a
// CiA 301 device refuses a command without them.
//
// Protocol plane, unprivileged.
//
// Thread model: the operations are initiating functions on completion
// tokens, as the SDO client's (detail/sdo_client.hpp), and may be called
// from any thread: each starts on the client's executor and completes on
// its handler's. Emit a cancellation signal on the client's executor.
// Everything else belongs to the client's executor, where events are
// emitted too.

#include <canopen/detail/client_op.hpp>
#include <canopen/detail/sdo_client.hpp>
#include <canopen/od.hpp>
#include <canopen/sdo_error.hpp>
#include <canopen/service/object_reading.hpp>
#include <canopen/subscription.hpp>
#include <canopen/transport.hpp>

#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/default_completion_token.hpp>
#include <boost/asio/deferred.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace cannet::canopen {

class remote_node;

namespace detail {

// What a config operation asks, built by the initiating functions and
// served on the client's executor.
struct config_request {
  enum class kind : std::uint8_t { read, write, read_all };

  kind what;
  od_entry const* parameter = nullptr; // read, write
  od_value value{};                    // write
};

// The service's verdict, before an operation narrows it to its own result.
using config_outcome = sdo_result<
    std::variant<std::monostate, od_value, std::vector<object_reading>>>;
using config_completion = std::move_only_function<void(config_outcome)>;

struct config_state;

// Starts `request` on the client's executor. `complete` runs exactly once:
// with the verdict, or with `cancelled` once `slot` fires.
void config_submit(std::weak_ptr<config_state> const& state,
                   config_request const& request,
                   boost::asio::cancellation_slot slot,
                   config_completion complete);

} // namespace detail

namespace service {

class config {
public:
  using executor_type = transport::executor_type;
  using value_handler = std::move_only_function<void(object_reading const&)>;
  using read_signature = void(sdo_result<od_value>);
  using write_signature = void(sdo_result<void>);
  using read_all_signature = void(sdo_result<std::vector<object_reading>>);

  // CiA 301: "save" written to 1010h:01 stores every parameter, "load"
  // written to 1011h:01 restores every default.
  static constexpr od_key save_all_key{0x1010, 0x01};
  static constexpr od_key restore_all_key{0x1011, 0x01};
  static constexpr std::uint32_t save_signature = 0x65766173; // "save"
  static constexpr std::uint32_t load_signature = 0x64616F6C; // "load"

  config(detail::sdo_client& sdo, dictionary_view dictionary);
  ~config();
  config(config const&) = delete;
  config& operator=(config const&) = delete;

  // The objects of the dictionary's config category, ordered by key.
  std::span<od_entry const* const> parameters() const;

  // Reads `parameter`, an object of the node's dictionary, as the
  // dictionary types it. A string or a command has no od_value: it
  // completes with type_mismatch, and no request goes out.
  template<boost::asio::completion_token_for<read_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_read(
      od_entry const& parameter,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return start<od_value>(
        {.what = detail::config_request::kind::read, .parameter = &parameter},
        std::forward<Token>(token));
  }

  // Writes `value` to `parameter`. A value of another type than the
  // dictionary gives the parameter completes with type_mismatch, and no
  // request goes out. A write cancelled in flight may have been applied.
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_write(
      od_entry const& parameter,
      od_value value,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return start<void>({.what = detail::config_request::kind::write,
                        .parameter = &parameter,
                        .value = value},
                       std::forward<Token>(token));
  }

  // Reads every readable parameter but strings, one request at a time, by
  // key, and completes with the readings; each is an event too, as it
  // comes. An abort or a timeout is the reading of its parameter. The read
  // stops early with cancelled, with transport when a request cannot be
  // sent, and with timeout after three timeouts in a row: the device is
  // not there, and the rest would time out one by one.
  template<boost::asio::completion_token_for<read_all_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_read_all(
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return start<std::vector<object_reading>>(
        {.what = detail::config_request::kind::read_all},
        std::forward<Token>(token));
  }

  // Restores `parameter` to its default (1011h:04, emblib's sdo_server).
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_restore_default(
      od_entry const& parameter,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return sdo_.async_restore_default(parameter.key,
                                      std::forward<Token>(token));
  }

  // Stores every parameter in the device's non-volatile memory.
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_save_all(
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return sdo_.async_write(save_all_key,
                            od_value{save_signature},
                            std::forward<Token>(token));
  }

  // Restores every parameter to its default.
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_restore_all_defaults(
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return sdo_.async_write(restore_all_key,
                            od_value{load_signature},
                            std::forward<Token>(token));
  }

  // Calls `handler` with every reading of async_read() and
  // async_read_all(): a value, or why there is none. A read cut short by a
  // cancellation or by a change of the node's id is not reported.
  [[nodiscard]] subscription on_value(value_handler handler);

private:
  friend class cannet::canopen::remote_node;

  // The client is gone: no more events, and every later operation fails
  // with transport_error::closed.
  void close();

  // Captures no `this`: a deferred operation may start after the node is
  // gone.
  template<typename T, typename Token>
  auto start(detail::config_request const& request, Token&& token)
  {
    using signature = void(sdo_result<T>);
    auto submit = [state = std::weak_ptr{state_},
                   request](boost::asio::cancellation_slot slot,
                            detail::config_completion complete) {
      detail::config_submit(state, request, slot, std::move(complete));
    };
    return boost::asio::async_compose<Token, signature>(
        detail::client_op<T, detail::config_outcome, decltype(submit)>{
            .submit = std::move(submit),
            .executor = sdo_.get_executor()},
        token,
        sdo_.get_executor());
  }

  detail::sdo_client& sdo_;
  std::shared_ptr<detail::config_state> state_;
};

} // namespace service
} // namespace cannet::canopen
