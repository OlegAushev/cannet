#pragma once

// cannet::canopen::detail::sdo_client — the host's SDO client for one remote
// node: expedited reads and writes of the node's objects, commands,
// strings, and restoring a parameter's default. The host half of emblib's
// sdo_server, and a member of remote_node (remote_node.hpp), not a type to
// use on its own.
//
// One request is in flight at a time, with a queue behind it. A node has one
// SDO channel, shared by the whole bus, so it serves one SDO client: a
// second one, such as a CLI beside a running application, would take this
// one's answers, told apart by index and subindex alone, and break its
// strings.
//
// Protocol plane, unprivileged.
//
// Thread model: the operations are initiating functions on completion
// tokens, as on cannet::raw::async_socket, and may be called from any
// thread: each starts on the client's executor and completes on its
// handler's. Emit a cancellation signal on the client's executor
// (cancel_after does). Everything else belongs to the client's executor.

#include <canopen/detail/client_op.hpp>
#include <canopen/od.hpp>
#include <canopen/sdo.hpp>
#include <canopen/sdo_error.hpp>
#include <canopen/transport.hpp>
#include <canopen/types.hpp>

#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/default_completion_token.hpp>
#include <boost/asio/deferred.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace cannet::canopen {

class remote_node;

namespace detail {

// What an operation asks of the device, built by the initiating functions
// and served by the client's worker.
struct sdo_request {
  enum class kind : std::uint8_t { read, write, read_string };

  kind what;
  od_key key;
  od_value_type type = od_value_type::uint32; // read: how to decode
  expedited_sdo_data data{};                  // write: what to send
  std::uint8_t size = 4;                      // write: significant bytes
};

// The worker's verdict, before an operation narrows it to its own result.
using sdo_outcome =
    sdo_result<std::variant<std::monostate, od_value, std::string>>;
using sdo_completion = std::move_only_function<void(sdo_outcome)>;

struct sdo_state;
struct config_state;

// Queues `request` on the client's executor. `complete` runs exactly once:
// with the worker's verdict, or with `cancelled` when `slot` fires first.
void sdo_submit(std::weak_ptr<sdo_state> const& state,
                sdo_request const& request,
                boost::asio::cancellation_slot slot,
                sdo_completion complete);

class sdo_client {
public:
  using executor_type = transport::executor_type;
  using read_signature = void(sdo_result<od_value>);
  using write_signature = void(sdo_result<void>);
  using string_signature = void(sdo_result<std::string>);

  // The key emblib's sdo_server serves restore-default requests at.
  static constexpr od_key restore_default_key{0x1011, 0x04};

  sdo_client(transport& bus, node_id id, std::chrono::milliseconds timeout);
  ~sdo_client();
  sdo_client(sdo_client const&) = delete;
  sdo_client& operator=(sdo_client const&) = delete;

  // The client's executor. Any thread.
  executor_type get_executor() const noexcept
  {
    return executor_;
  }

  // Reads the object at `key` as `type`. type_mismatch when the device
  // answers with another size than `type` has.
  template<boost::asio::completion_token_for<read_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_read(
      od_key key,
      od_value_type type,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return start<od_value>(
        {.what = sdo_request::kind::read, .key = key, .type = type},
        std::forward<Token>(token));
  }

  // Writes `value`, announcing its size: the device takes the bytes as its
  // object's own type. A write cancelled in flight may have been applied.
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_write(
      od_key key,
      od_value value,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    auto const size = std::visit(
        [](auto const& v) { return static_cast<std::uint8_t>(sizeof(v)); },
        value);
    return start<void>({.what = sdo_request::kind::write,
                        .key = key,
                        .data = to_raw(value),
                        .size = size},
                       std::forward<Token>(token));
  }

  // Runs the command at `key`, an exec object: a write whose bytes the
  // device ignores, as emblib's od_exec does. The answer carries no value.
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_exec(
      od_key key,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return start<void>({.what = sdo_request::kind::write, .key = key},
                       std::forward<Token>(token));
  }

  // Reads the string at `key`: 4 bytes per answer, up to a NUL. The read
  // holds the channel until the NUL even after its caller cancels, for the
  // device keeps one string cursor per SDO server: an interrupted read
  // would leave it mid-string. A read the client could not finish (a
  // timeout, a failure) leaves a mark, and the next read of that string
  // first runs the cursor to the end.
  template<boost::asio::completion_token_for<string_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_read_string(
      od_key key,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    return start<std::string>(
        {.what = sdo_request::kind::read_string, .key = key},
        std::forward<Token>(token));
  }

  // Restores the parameter at `key` to its default: the client half of
  // emblib's sdo_server::write_restore_default, a write to 1011h:04 whose
  // data carry `key` (index little-endian, then subindex).
  template<boost::asio::completion_token_for<write_signature> Token =
               boost::asio::default_completion_token_t<executor_type>>
  auto async_restore_default(
      od_key key,
      Token&& token = boost::asio::default_completion_token_t<executor_type>())
  {
    expedited_sdo_data const target = {
        static_cast<std::uint8_t>(key.index & 0xFF),
        static_cast<std::uint8_t>(key.index >> 8),
        key.subindex,
        0};
    return start<void>({.what = sdo_request::kind::write,
                        .key = restore_default_key,
                        .data = target},
                       std::forward<Token>(token));
  }

private:
  friend class cannet::canopen::remote_node;
  friend struct config_state;

  // A request in flight completes with `cancelled`; queued ones go to the
  // new id.
  void rebind(node_id id);
  // Requests in flight and queued, and every later one, complete with
  // transport_error::closed: the client is gone.
  void close();

  // For a service already on the client's executor: queues `request` at
  // once, as sdo_submit() does. The operations above first hop onto the
  // executor; a service's request skips that hop, and so keeps its
  // caller's order among theirs.
  void queue(sdo_request const& request,
             boost::asio::cancellation_slot slot,
             sdo_completion complete)
  {
    sdo_submit(state_, request, slot, std::move(complete));
  }

  // Captures no `this`: a deferred operation may start after the node is
  // gone.
  template<typename T, typename Token>
  auto start(sdo_request const& request, Token&& token)
  {
    using signature = void(sdo_result<T>);
    auto submit = [state = std::weak_ptr{state_},
                   request](boost::asio::cancellation_slot slot,
                            sdo_completion complete) {
      sdo_submit(state, request, slot, std::move(complete));
    };
    return boost::asio::async_compose<Token, signature>(
        client_op<T, sdo_outcome, decltype(submit)>{.submit = std::move(submit),
                                                    .executor = executor_},
        token,
        executor_);
  }

  executor_type executor_;
  std::shared_ptr<sdo_state> state_;
};

} // namespace detail
} // namespace cannet::canopen
