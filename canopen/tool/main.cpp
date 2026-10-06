// canopen — CLI diagnostic tool over cannet::canopen.
//
// On a bus (bus_commands.hpp), whose interface must already be up (see
// canup); no privileges required:
// dump: prints every frame on the bus, decoded, until interrupted (Ctrl+C);
//       sends nothing.
// sdo read|write|exec: one SDO request to a node.
// watch: polls a node's watch objects and prints a table per pass, until
//        interrupted.
//
// Dictionaries, as OD files (canopen/od_file.hpp); these read and write
// files only, and nothing goes on a bus:
// od check: reads an OD file and counts its objects.
// od from-emblib: makes an OD file from an emblib firmware's od.cpp.
// od header: makes a C++ header that compiles an OD file's dictionary in;
//            CMake's cannet_canopen_dictionary() runs it.

#include "bus_commands.hpp"
#include "od_tools.hpp"

#include <canopen/od_file.hpp>
#include <canopen/raw_transport.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace cannet::canopen;
namespace asio = boost::asio;
namespace tool = cannet::canopen::tool;

int usage()
{
  std::print(stderr,
             "usage:\n"
             "  canopen dump <iface> [-d <file.od>] [--node <node>]\n"
             "  canopen sdo read <iface> <node> <object> [-d <file.od>]\n"
             "                   [--type <type>]\n"
             "  canopen sdo write <iface> <node> <object> <value>\n"
             "                    [-d <file.od>] [--type <type>]\n"
             "  canopen sdo exec <iface> <node> <object> [-d <file.od>]\n"
             "  canopen watch <iface> <node> -d <file.od> [<object>...]\n"
             "                [--period <ms>] [--count <passes>]\n"
             "  canopen od check <file.od>\n"
             "  canopen od from-emblib <od.cpp> [--watch-category <category>]\n"
             "                         [--config-category <category>]\n"
             "                         [--output <file.od>]\n"
             "  canopen od header <file.od> --name <identifier>\n"
             "                    [--namespace <namespace>] [--output "
             "<file.hpp>]\n"
             "\n"
             "An <object> is index:subindex in hex (3000:01), or\n"
             "category::subcategory::name in the dictionary -d gives.\n"
             "A <type> is named as in an OD file: uint16, float32...\n"
             "Commands on a <node> take --host-id <id>, the CLI's own\n"
             "node id (127), and --timeout <ms>, the SDO timeout (500).\n"
             "A node has one SDO channel: leave alone a node that an\n"
             "application talks to.\n");
  return 2;
}

// A command's arguments: the positional ones, and "option value" pairs
// among the options it takes. An argument that starts with "--" is an
// option, and so is a short one the command takes, such as "-d"; "-5" is
// positional.
struct arguments {
  std::vector<std::string_view> positional;
  std::map<std::string_view, std::string_view> options;
};

std::optional<arguments>
parse_arguments(std::span<std::string_view const> args,
                std::initializer_list<std::string_view> known)
{
  arguments parsed;
  for (std::size_t i = 0; i < args.size(); ++i) {
    bool const is_known = std::ranges::find(known, args[i]) != known.end();
    if (!is_known && !args[i].starts_with("--")) {
      parsed.positional.push_back(args[i]);
      continue;
    }
    if (!is_known || i + 1 == args.size()) {
      return std::nullopt;
    }
    parsed.options[args[i]] = args[i + 1];
    ++i;
  }
  return parsed;
}

std::optional<std::string_view> option(arguments const& args,
                                       std::string_view name)
{
  if (auto const it = args.options.find(name); it != args.options.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<std::string> read_file(std::string_view path, bool quiet = false)
{
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) {
    if (!quiet) {
      std::print(stderr,
                 "canopen: cannot read {}: {}\n",
                 path,
                 std::strerror(errno));
    }
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{in}, {}};
}

// Writes `text` to `path`, unless the file holds it already: a header
// written again unchanged would rebuild what includes it.
bool write_file(std::string_view path, std::string_view text)
{
  if (read_file(path, true) == text) {
    return true;
  }
  std::ofstream out{std::string{path}, std::ios::binary | std::ios::trunc};
  out << text;
  out.close();
  if (!out) {
    std::print(stderr,
               "canopen: cannot write {}: {}\n",
               path,
               std::strerror(errno));
    return false;
  }
  return true;
}

// To the --output file, or to the standard output.
int emit(arguments const& args, std::string_view text)
{
  if (auto const output = option(args, "--output")) {
    return write_file(*output, text) ? 0 : 1;
  }
  std::print("{}", text);
  return 0;
}

int report(std::string_view file, tool::tool_error const& e)
{
  if (e.line == 0) {
    std::print(stderr, "{}: {}\n", file, e.message);
  }
  else {
    std::print(stderr, "{}:{}: {}\n", file, e.line, e.message);
  }
  return 1;
}

std::optional<loaded_dictionary> load(std::string_view path)
{
  auto text = read_file(path);
  if (!text) {
    return std::nullopt;
  }
  auto loaded = parse_od_file(std::move(*text));
  if (!loaded) {
    report(path,
           {.line = loaded.error().line,
            .message = std::string{to_string(loaded.error().reason)}});
    return std::nullopt;
  }
  return std::move(*loaded);
}

// Decimal, or hex after "0x".
std::optional<unsigned> parse_number(std::string_view text)
{
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  unsigned value = 0;
  auto const end = text.data() + text.size();
  auto const [ptr, ec] = std::from_chars(text.data(), end, value, base);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<node_id> parse_node_id(std::string_view text)
{
  auto const number = parse_number(text);
  auto const id = number ? node_id::make(*number) : std::nullopt;
  if (!id) {
    std::print(stderr, "canopen: not a node id: {}; give 1 to 127\n", text);
  }
  return id;
}

std::optional<std::chrono::milliseconds>
parse_milliseconds(std::string_view option_name, std::string_view text)
{
  auto const number = parse_number(text);
  if (!number) {
    std::print(stderr,
               "canopen: {} takes milliseconds, not {}\n",
               option_name,
               text);
    return std::nullopt;
  }
  return std::chrono::milliseconds{*number};
}

tool::console terminal()
{
  return {.out = std::cout, .err = std::cerr};
}

// The device a command on a node talks to: the <node> argument, and the
// options -d, --host-id and --timeout. The options' dictionary is a view
// of `dictionary`, which a move keeps valid.
struct device {
  std::optional<loaded_dictionary> dictionary;
  tool::node_options options;
};

// nullopt, with the reason on stderr, for an argument that makes no sense
// or a dictionary that cannot be read.
std::optional<device> device_of(arguments const& args, std::string_view node)
{
  auto const id = parse_node_id(node);
  if (!id) {
    return std::nullopt;
  }
  device result{.dictionary = std::nullopt, .options = {.node = *id}};
  if (auto const path = option(args, "-d")) {
    result.dictionary = load(*path);
    if (!result.dictionary) {
      return std::nullopt;
    }
    result.options.dictionary = *result.dictionary;
  }
  if (auto const host = option(args, "--host-id")) {
    auto const host_id = parse_node_id(*host);
    if (!host_id) {
      return std::nullopt;
    }
    result.options.host = *host_id;
  }
  if (auto const timeout = option(args, "--timeout")) {
    auto const ms = parse_milliseconds("--timeout", *timeout);
    if (!ms) {
      return std::nullopt;
    }
    result.options.sdo_timeout = *ms;
  }
  return result;
}

// Runs a bus command on `iface` to its end. Ctrl+C or SIGTERM asks the
// command to stop; a second Ctrl+C ends the process at once.
template<typename Command>
int run_on_bus(std::string_view iface, Command command)
{
  asio::io_context io;
  raw_transport bus{io.get_executor()};
  if (auto const opened = bus.open(iface); !opened) {
    std::print(stderr,
               "canopen: cannot open {}: {}\n",
               iface,
               cannet::raw::to_string(opened.error()));
    return 1;
  }

  asio::cancellation_signal stop;
  asio::signal_set signals{io, SIGINT, SIGTERM};
  signals.async_wait([&](boost::system::error_code ec, int) {
    if (ec) {
      return;
    }
    boost::system::error_code ignored;
    static_cast<void>(signals.clear(ignored)); // back to the default action
    stop.emit(asio::cancellation_type::terminal);
  });

  int status = 1;
  asio::co_spawn(io,
                 command(bus),
                 asio::bind_cancellation_slot(
                     stop.slot(),
                     [&](std::exception_ptr error, int result) {
                       status = result;
                       if (error) {
                         status = 1;
                         try {
                           std::rethrow_exception(error);
                         }
                         catch (std::exception const& e) {
                           std::print(stderr, "canopen: {}\n", e.what());
                         }
                       }
                       io.stop();
                     }));
  io.run();
  return status;
}

int dump(std::span<std::string_view const> args)
{
  auto const parsed = parse_arguments(args, {"-d", "--node"});
  if (!parsed || parsed->positional.size() != 1) {
    return usage();
  }
  tool::dump_options options;
  std::optional<loaded_dictionary> dictionary;
  if (auto const path = option(*parsed, "-d")) {
    dictionary = load(*path);
    if (!dictionary) {
      return 2;
    }
    options.dictionary = *dictionary;
  }
  if (auto const node = option(*parsed, "--node")) {
    options.node = parse_node_id(*node);
    if (!options.node) {
      return 2;
    }
  }
  return run_on_bus(parsed->positional[0], [&](transport& bus) {
    return tool::dump(bus, options, terminal());
  });
}

int sdo(std::string_view what, std::span<std::string_view const> args)
{
  bool const read = what == "read";
  bool const write = what == "write";
  bool const exec = what == "exec";
  auto const parsed = parse_arguments(
      args,
      {"-d", "--type", "--host-id", "--timeout"});
  if ((!read && !write && !exec)
      || !parsed
      || parsed->positional.size() != (write ? 4u : 3u)
      || (exec && option(*parsed, "--type"))) {
    return usage();
  }
  auto const& positional = parsed->positional;
  auto const target = device_of(*parsed, positional[1]);
  if (!target) {
    return 2;
  }
  std::optional<od_value_type> type;
  if (auto const name = option(*parsed, "--type")) {
    type = tool::parse_type(*name);
    if (!type) {
      std::print(stderr, "canopen: not a type: {}\n", *name);
      return 2;
    }
  }

  std::string const object{positional[2]};
  return run_on_bus(positional[0], [&](transport& bus) {
    if (read) {
      return tool::sdo_read(bus, target->options, object, type, terminal());
    }
    if (write) {
      return tool::sdo_write(bus,
                             target->options,
                             object,
                             std::string{positional[3]},
                             type,
                             terminal());
    }
    return tool::sdo_exec(bus, target->options, object, terminal());
  });
}

int watch(std::span<std::string_view const> args)
{
  auto const parsed = parse_arguments(
      args,
      {"-d", "--period", "--count", "--host-id", "--timeout"});
  if (!parsed || parsed->positional.size() < 2) {
    return usage();
  }
  auto const& positional = parsed->positional;
  auto const target = device_of(*parsed, positional[1]);
  if (!target) {
    return 2;
  }
  tool::watch_options watching;
  for (auto const object : std::span{positional}.subspan(2)) {
    watching.objects.emplace_back(object);
  }
  if (auto const period = option(*parsed, "--period")) {
    auto const ms = parse_milliseconds("--period", *period);
    if (!ms) {
      return 2;
    }
    watching.period = *ms;
  }
  if (auto const count = option(*parsed, "--count")) {
    auto const passes = parse_number(*count);
    if (!passes) {
      std::print(stderr, "canopen: --count takes a number, not {}\n", *count);
      return 2;
    }
    watching.count = *passes;
  }
  watching.redraw = isatty(STDOUT_FILENO) != 0;
  return run_on_bus(positional[0], [&](transport& bus) {
    return tool::watch(bus, target->options, watching, terminal());
  });
}

int od_check(std::span<std::string_view const> args)
{
  auto const parsed = parse_arguments(args, {});
  if (!parsed || parsed->positional.size() != 1) {
    return usage();
  }
  auto const path = parsed->positional[0];
  auto const loaded = load(path);
  if (!loaded) {
    return 1;
  }
  dictionary_view const view = *loaded;
  std::print("{}: {} objects", path, view.size());
  for (auto const& [what, category] :
       {std::pair{"watch", view.config().watch_category},
        std::pair{"config", view.config().config_category}}) {
    if (!category.empty()) {
      std::print(", {} in the {} category \"{}\"",
                 std::ranges::distance(view.in_category(category)),
                 what,
                 category);
    }
  }
  std::print("\n");
  return 0;
}

int od_from_emblib(std::span<std::string_view const> args)
{
  auto const parsed = parse_arguments(
      args,
      {"--watch-category", "--config-category", "--output"});
  if (!parsed || parsed->positional.size() != 1) {
    return usage();
  }
  auto const path = parsed->positional[0];
  auto const source = read_file(path);
  if (!source) {
    return 1;
  }
  tool::emblib_options options;
  if (auto const c = option(*parsed, "--watch-category")) {
    options.watch_category = std::string{*c};
  }
  if (auto const c = option(*parsed, "--config-category")) {
    options.config_category = std::string{*c};
  }
  auto const file_name = path.substr(path.find_last_of('/') + 1);
  auto const made = tool::od_file_from_emblib(*source, file_name, options);
  if (!made) {
    return report(path, made.error());
  }
  return emit(*parsed, *made);
}

int od_header(std::span<std::string_view const> args)
{
  auto const parsed = parse_arguments(args,
                                      {"--name", "--namespace", "--output"});
  if (!parsed || parsed->positional.size() != 1 || !option(*parsed, "--name")) {
    return usage();
  }
  auto const path = parsed->positional[0];
  auto const loaded = load(path);
  if (!loaded) {
    return 1;
  }
  auto const file_name = path.substr(path.find_last_of('/') + 1);
  auto const header = tool::cpp_header(
      *loaded,
      *option(*parsed, "--name"),
      option(*parsed, "--namespace").value_or(""),
      file_name);
  if (!header) {
    return report(path, header.error());
  }
  return emit(*parsed, *header);
}

} // namespace

int main(int argc, char** argv)
{
  std::vector<std::string_view> const args(argv + 1, argv + argc);
  if (!args.empty() && args[0] == "dump") {
    return dump(std::span{args}.subspan(1));
  }
  if (args.size() < 2) {
    return usage();
  }
  auto const rest = std::span{args}.subspan(2);
  if (args[0] == "sdo") {
    return sdo(args[1], rest);
  }
  if (args[0] == "watch") {
    return watch(std::span{args}.subspan(1));
  }
  if (args[0] != "od") {
    return usage();
  }
  if (args[1] == "check") {
    return od_check(rest);
  }
  if (args[1] == "from-emblib") {
    return od_from_emblib(rest);
  }
  if (args[1] == "header") {
    return od_header(rest);
  }
  return usage();
}
