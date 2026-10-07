#pragma once

// What the tests of the canopen CLI's bus commands share: a device behind
// the loopback bus that holds drive.od's objects, and commands run on it
// to their end.

#include "bus_commands.hpp"
#include "drive_dictionary.hpp"

#include <canopen/loopback.hpp>
#include <canopen/testing/emulated_device.hpp>
#include <canopen/testing/run.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace cannet::canopen::test {

inline constexpr auto cli_device_id = node_id::literal(1);

inline constexpr od_key device_name_key{0x1008, 0x00}; // "drive-01"
inline constexpr od_key clear_errors_key{0x2000, 0x02};
inline constexpr od_key speed_key{0x3000, 0x01};  // 1500
inline constexpr od_key uptime_key{0x5000, 0x01}; // 12.5
inline constexpr od_key vdc_key{0x5000, 0x02};    // 540

struct cli_fixture {
  cli_fixture()
  {
    using enum od_value_type;
    device.add_object(
        device_name_key,
        {.type = string, .access = od_access::const_, .text = "drive-01"});
    device.add_object(clear_errors_key,
                      {.type = exec, .access = od_access::wo});
    device.add_object(speed_key,
                      {.type = uint16,
                       .access = od_access::rw,
                       .value = to_raw(std::uint16_t{1500})});
    device.add_object(
        uptime_key,
        {.type = float32, .access = od_access::ro, .value = to_raw(12.5f)});
    device.add_object(
        vdc_key,
        {.type = float32, .access = od_access::ro, .value = to_raw(540.0f)});
  }

  // The device, with drive.od, or with `dictionary`.
  static tool::node_options
  options(dictionary_view dictionary = cannet_test::drive_dictionary)
  {
    using namespace std::chrono_literals;
    return {.node = cli_device_id,
            .dictionary = dictionary,
            .sdo_timeout = 50ms};
  }

  tool::console console()
  {
    return {.out = out, .err = err};
  }

  // Starts `command`; its exit status lands in what this returns, -1 for
  // an exception. `signal` cancels it, as Ctrl+C does: `stop` unless
  // given. A signal cancels one command at a time.
  std::shared_ptr<std::optional<int>>
  start(boost::asio::awaitable<int> command,
        boost::asio::cancellation_signal& signal)
  {
    auto status = std::make_shared<std::optional<int>>();
    boost::asio::co_spawn(io,
                          std::move(command),
                          boost::asio::bind_cancellation_slot(
                              signal.slot(),
                              [status](std::exception_ptr error, int result) {
                                *status = error ? -1 : result;
                              }));
    return status;
  }

  std::shared_ptr<std::optional<int>> start(boost::asio::awaitable<int> command)
  {
    return start(std::move(command), stop);
  }

  static constexpr std::chrono::milliseconds run_limit{2000};

  // Runs `command` to its end and returns its exit status; nullopt when it
  // has not ended within `limit`.
  std::optional<int> run(boost::asio::awaitable<int> command,
                         std::chrono::milliseconds limit = run_limit)
  {
    auto const status = start(std::move(command));
    testing::run_until(io, [&] { return status->has_value(); }, limit);
    return *status;
  }

  boost::asio::io_context io;
  loopback_bus bus{io.get_executor()};
  loopback_transport host_bus{bus};
  loopback_transport device_bus{bus};
  testing::emulated_device device{device_bus,
                                  cli_device_id,
                                  std::chrono::milliseconds::zero()};
  boost::asio::cancellation_signal stop;
  std::ostringstream out;
  std::ostringstream err;
};

} // namespace cannet::canopen::test
