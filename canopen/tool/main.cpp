// canopen — CLI diagnostic tool over cannet::canopen.
//
// Dictionaries, as OD files (canopen/od_file.hpp); these read and write
// files only, and nothing goes on a bus:
// od check: reads an OD file and counts its objects.
// od from-emblib: makes an OD file from an emblib firmware's od.cpp.
// od header: makes a C++ header that compiles an OD file's dictionary in;
//            CMake's cannet_canopen_dictionary() runs it.

#include "od_tools.hpp"

#include <canopen/od_file.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace cannet::canopen;
namespace tool = cannet::canopen::tool;

int usage()
{
  std::print(stderr,
             "usage:\n"
             "  canopen od check <file.od>\n"
             "  canopen od from-emblib <od.cpp> [--watch-category <category>]\n"
             "                         [--config-category <category>]\n"
             "                         [--output <file.od>]\n"
             "  canopen od header <file.od> --name <identifier>\n"
             "                    [--namespace <namespace>] [--output "
             "<file.hpp>]\n");
  return 2;
}

// A command's arguments: the positional ones, and "--option value" pairs
// among the options it takes.
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
    if (!args[i].starts_with("--")) {
      parsed.positional.push_back(args[i]);
      continue;
    }
    if (std::ranges::find(known, args[i]) == known.end()
        || i + 1 == args.size()) {
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
  if (args.size() < 2 || args[0] != "od") {
    return usage();
  }
  auto const rest = std::span{args}.subspan(2);
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
