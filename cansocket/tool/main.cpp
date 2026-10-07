// cansock — CLI diagnostic tool over cannet::raw.
//
// The interface must already be up (see canup); no privileges required.
// dump: print incoming frames until interrupted (Ctrl+C); runs on
//       cannet::raw::async_socket. --errors adds the controller's error
//       frames, decoded.
// send: transmit one frame on cannet::raw::socket; id > 0x7FF selects an
//       extended (29-bit) id.

#include <cansocket/raw/async_socket.hpp>
#include <cansocket/raw/error_frame.hpp>
#include <cansocket/raw/socket.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace {

int usage()
{
  std::print(stderr,
             "usage:\n"
             "  cansock dump <iface> [--errors]\n"
             "  cansock send <iface> <id-hex> [byte-hex ...]\n");
  return 2;
}

std::optional<std::uint32_t> parse_hex(std::string_view s)
{
  std::uint32_t value = 0;
  char const* const end = s.data() + s.size();
  auto const [ptr, ec] = std::from_chars(s.data(), end, value, 16);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

// What an error frame reports, as the names of what it reports.
std::string describe(cannet::raw::error_report const& report)
{
  std::string text;
  auto const add = [&text](std::string_view part) {
    if (!text.empty()) {
      text += ", ";
    }
    text += part;
  };
  if (report.state) {
    add(name(*report.state));
  }
  if (report.restarted) {
    add("restarted");
  }
  if (report.rx_overflow) {
    add("rx_overflow");
  }
  if (report.tx_overflow) {
    add("tx_overflow");
  }
  if (report.protocol_violation) {
    add("protocol_violation");
  }
  if (report.no_ack) {
    add("no_ack");
  }
  if (report.bus_error) {
    add("bus_error");
  }
  if (report.transceiver) {
    add("transceiver");
  }
  if (report.lost_arbitration) {
    add("lost_arbitration");
  }
  if (report.tx_timeout) {
    add("tx_timeout");
  }
  if (report.counters) {
    add(std::format("counters tx {} rx {}",
                    report.counters->tx,
                    report.counters->rx));
  }
  return text;
}

void print_frame(can_frame const& frame)
{
  if (auto const report = cannet::raw::decode_error_frame(frame)) {
    std::print("ERR  [{}] ", frame.len);
    for (int i = 0; i < frame.len; ++i) {
      std::print(" {:02X}", frame.data[i]);
    }
    std::print("  {}\n", describe(*report));
    return;
  }
  if ((frame.can_id & CAN_EFF_FLAG) != 0) {
    std::print("{:08X}  [{}] ", frame.can_id & CAN_EFF_MASK, frame.len);
  }
  else {
    std::print("{:03X}  [{}] ", frame.can_id & CAN_SFF_MASK, frame.len);
  }
  if ((frame.can_id & CAN_RTR_FLAG) != 0) {
    // A remote frame carries no data: len is the length it requests.
    std::print(" remote request");
  }
  else {
    for (int i = 0; i < frame.len; ++i) {
      std::print(" {:02X}", frame.data[i]);
    }
  }
  std::print("\n");
}

void report_open_failure(std::string_view iface, cannet::raw::socket_error e)
{
  std::print(stderr,
             "cansock: failed to open {}: {}\n",
             iface,
             cannet::raw::to_string(e));
}

boost::asio::awaitable<int> receive_loop(cannet::raw::async_socket& socket)
{
  while (true) {
    auto const frame = co_await socket.async_receive();
    if (!frame) {
      if (frame.error() == cannet::raw::socket_error::cancelled) {
        co_return 0; // interrupted
      }
      std::print(stderr,
                 "cansock: dump failed: {}\n",
                 cannet::raw::to_string(frame.error()));
      co_return 1;
    }
    print_frame(*frame);
  }
}

int dump(std::string_view iface, bool errors)
{
  boost::asio::io_context io;
  cannet::raw::async_socket socket{io.get_executor()};
  if (auto const result = socket.open(iface); !result) {
    report_open_failure(iface, result.error());
    return 1;
  }
  if (errors) {
    if (auto const result = socket.set_error_filter(CAN_ERR_MASK); !result) {
      std::print(stderr,
                 "cansock: cannot receive error frames: {}\n",
                 cannet::raw::to_string(result.error()));
      return 1;
    }
  }

  // Ctrl+C ends the dump cleanly: cancelling the socket completes the
  // pending receive.
  boost::asio::signal_set signals{io, SIGINT, SIGTERM};
  signals.async_wait([&socket](boost::system::error_code ec, int) {
    if (!ec) {
      socket.cancel();
    }
  });

  int status = 1;
  boost::asio::co_spawn(io,
                        receive_loop(socket),
                        [&](std::exception_ptr error, int result) {
                          boost::system::error_code ignored;
                          static_cast<void>(signals.cancel(ignored));
                          status = error ? 1 : result;
                        });
  io.run();
  return status;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 3) {
    return usage();
  }

  std::string_view const cmd = argv[1];
  std::string_view const iface = argv[2];

  if (cmd == "dump") {
    if (argc > 4 || (argc == 4 && std::string_view{argv[3]} != "--errors")) {
      return usage();
    }
    return dump(iface, argc == 4);
  }

  if (cmd == "send") {
    if (argc < 4) {
      return usage();
    }
    auto const id = parse_hex(argv[3]);
    if (!id || *id > CAN_EFF_MASK) {
      std::print(stderr, "cansock: invalid frame id: {}\n", argv[3]);
      return 2;
    }
    if (argc - 4 > CAN_MAX_DLEN) {
      std::print(stderr, "cansock: more than {} data bytes\n", CAN_MAX_DLEN);
      return 2;
    }

    cannet::raw::socket socket;
    if (auto const result = socket.open(iface); !result) {
      report_open_failure(iface, result.error());
      return 1;
    }

    can_frame frame{};
    frame.can_id = (*id > CAN_SFF_MASK) ? (*id | CAN_EFF_FLAG) : *id;
    frame.len = static_cast<__u8>(argc - 4);
    for (int i = 0; i < frame.len; ++i) {
      auto const byte = parse_hex(argv[4 + i]);
      if (!byte || *byte > 0xFF) {
        std::print(stderr, "cansock: invalid data byte: {}\n", argv[4 + i]);
        return 2;
      }
      frame.data[i] = static_cast<__u8>(*byte);
    }

    if (auto const result = socket.send(frame); !result) {
      std::print(stderr,
                 "cansock: send failed: {}\n",
                 cannet::raw::to_string(result.error()));
      return 1;
    }
    print_frame(frame);
    return 0;
  }

  return usage();
}
