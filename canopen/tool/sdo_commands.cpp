// sdo read, sdo write, sdo exec: one request each.

#include "command_support.hpp"

#include <canopen/od_format.hpp>

#include <ostream>
#include <print>

namespace cannet::canopen::tool {

namespace {

namespace asio = boost::asio;

// The type to read or write the object as: `type` when given, else the
// dictionary's; nullopt, with the reason on `io.err`, when neither has one.
std::optional<od_value_type> type_for(object_ref const& object,
                                      std::optional<od_value_type> type,
                                      console io)
{
  if (type) {
    return type;
  }
  if (object.entry != nullptr) {
    return object.entry->object.type;
  }
  std::print(io.err,
             "canopen: {}: the type is unknown: give --type, or a "
             "dictionary that has the object\n",
             label(object));
  return std::nullopt;
}

} // namespace

asio::awaitable<int> sdo_read(transport& bus,
                              node_options options,
                              std::string object,
                              std::optional<od_value_type> type,
                              console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  auto const target = find_object(object, options.dictionary);
  if (!target) {
    std::print(io.err, "canopen: {}\n", target.error());
    co_return 2;
  }
  auto const read_type = type_for(*target, type, io);
  if (!read_type) {
    co_return 2;
  }
  client host{bus, host_options(options)};
  auto const node = add_device(host, options, io);
  if (!node) {
    co_return 2;
  }

  if (*read_type == od_value_type::string) {
    auto const text = co_await node->sdo.async_read_string(target->key,
                                                           uninterrupted());
    if (!text) {
      report(io, *target, text.error());
      co_return 1;
    }
    std::print(io.out, "{}\n", *text);
    co_return 0;
  }
  auto const value = co_await node->sdo.async_read(target->key,
                                                   *read_type,
                                                   uninterrupted());
  if (!value) {
    report(io, *target, value.error());
    co_return 1;
  }
  std::print(io.out, "{}\n", to_string(*value));
  co_return 0;
}

asio::awaitable<int> sdo_write(transport& bus,
                               node_options options,
                               std::string object,
                               std::string value,
                               std::optional<od_value_type> type,
                               console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  auto const target = find_object(object, options.dictionary);
  if (!target) {
    std::print(io.err, "canopen: {}\n", target.error());
    co_return 2;
  }
  auto const write_type = type_for(*target, type, io);
  if (!write_type) {
    co_return 2;
  }
  auto const parsed = parse(value, *write_type);
  if (!parsed) {
    std::print(io.err,
               "canopen: {}: {} as {}: {}\n",
               label(*target),
               value,
               name(*write_type),
               to_string(parsed.error()));
    co_return 2;
  }
  client host{bus, host_options(options)};
  auto const node = add_device(host, options, io);
  if (!node) {
    co_return 2;
  }

  auto const written = co_await node->sdo.async_write(target->key,
                                                      *parsed,
                                                      uninterrupted());
  if (!written) {
    report(io, *target, written.error());
    co_return 1;
  }
  co_return 0;
}

asio::awaitable<int>
sdo_exec(transport& bus, node_options options, std::string object, console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  auto const target = find_object(object, options.dictionary);
  if (!target) {
    std::print(io.err, "canopen: {}\n", target.error());
    co_return 2;
  }
  if (target->entry != nullptr
      && target->entry->object.type != od_value_type::exec) {
    std::print(io.err,
               "canopen: {}: not a command but a {} object; sdo write "
               "writes it\n",
               label(*target),
               name(target->entry->object.type));
    co_return 2;
  }
  client host{bus, host_options(options)};
  auto const node = add_device(host, options, io);
  if (!node) {
    co_return 2;
  }

  auto const done = co_await node->sdo.async_exec(target->key, uninterrupted());
  if (!done) {
    report(io, *target, done.error());
    co_return 1;
  }
  co_return 0;
}

} // namespace cannet::canopen::tool
