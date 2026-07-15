// canup — CLI diagnostic tool over cannet::canup.
//
// up/down require CAP_NET_ADMIN (run via sudo, or setcap the binary);
// status needs no privileges.

#include <canup/canup.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <print>
#include <string_view>

namespace {

int usage() {
  std::print(
      stderr,
      "usage:\n"
      "  canup up     <iface> <bitrate> [restart_ms]\n"
      "  canup down   <iface>\n"
      "  canup status <iface>\n"
  );
  return 2;
}

std::optional<std::uint32_t> parse_u32(std::string_view s) {
  std::uint32_t value = 0;
  char const* const end = s.data() + s.size();
  auto const [ptr, ec] = std::from_chars(s.data(), end, value);
  if (ec != std::errc{} || ptr != end) { // not a number / overflow / trailing junk
    return std::nullopt;
  }
  return value;
}

} // namespace

int main(int argc, char** argv) {
  using namespace cannet;

  if (argc < 3) {
    return usage();
  }

  std::string_view const cmd = argv[1];
  std::string_view const iface = argv[2];

  if (cmd == "up") {
    if (argc < 4) {
      return usage();
    }
    auto const bitrate = parse_u32(argv[3]);
    if (!bitrate) {
      std::print(stderr, "canup: invalid bitrate: {}\n", argv[3]);
      return 2;
    }
    std::uint32_t restart_ms = 100;
    if (argc >= 5) {
      auto const parsed = parse_u32(argv[4]);
      if (!parsed) {
        std::print(stderr, "canup: invalid restart_ms: {}\n", argv[4]);
        return 2;
      }
      restart_ms = *parsed;
    }

    auto const result = canup::up(iface, *bitrate, restart_ms);
    if (!result) {
      std::print(
          stderr,
          "canup: up {} {} failed: {}\n",
          iface,
          *bitrate,
          canup::to_string(result.error())
      );
      return 1;
    }
    std::print(
        "{} up (bitrate={}, restart_ms={})\n",
        iface,
        *bitrate,
        restart_ms
    );
    return 0;
  }

  if (cmd == "down") {
    auto const result = canup::down(iface);
    if (!result) {
      std::print(
          stderr,
          "canup: down {} failed: {}\n",
          iface,
          canup::to_string(result.error())
      );
      return 1;
    }
    std::print("{} down\n", iface);
    return 0;
  }

  if (cmd == "status") {
    auto const status = canup::status(iface);
    if (!status) {
      std::print(
          stderr,
          "canup: status {} failed: {}\n",
          iface,
          canup::to_string(status.error())
      );
      return 1;
    }
    std::print(
        "{}: {} (bitrate={})\n",
        iface,
        status->up ? "UP" : "DOWN",
        status->bitrate
    );
    return 0;
  }

  return usage();
}
