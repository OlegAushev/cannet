#include "od_tools.hpp"

#include <canopen/od_file.hpp>
#include <canopen/od_format.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

namespace cannet::canopen::tool {

namespace {

std::unexpected<tool_error> fail(std::size_t line, std::string message)
{
  return std::unexpected(
      tool_error{.line = line, .message = std::move(message)});
}

bool identifier_start(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool identifier_char(char c)
{
  return identifier_start(c) || (c >= '0' && c <= '9');
}

bool identifier(std::string_view text)
{
  return !text.empty()
      && identifier_start(text.front())
      && std::ranges::all_of(text, identifier_char);
}

// What a field may hold, in an OD file and in a C++ string literal.
bool holdable(std::string_view field)
{
  return std::ranges::none_of(field, [](char c) {
    return c == ' '
        || c == '\t'
        || c == '"'
        || c == '\\'
        || c == '\n'
        || c == '\r';
  });
}

// --- C++ tokens, as far as an emblib table needs them ---------------------

struct token {
  enum class kind : std::uint8_t { punct, identifier, number, string };

  kind what;
  std::string text; // a string literal's content, unescaped
  std::size_t line;
};

std::expected<std::vector<token>, tool_error> tokenize(std::string_view src)
{
  std::vector<token> tokens;
  std::size_t line = 1;
  std::size_t i = 0;
  auto const at = [src](std::size_t k) {
    return k < src.size() ? src[k] : '\0';
  };
  while (i < src.size()) {
    char const c = src[i];
    if (c == '\n') {
      ++line;
      ++i;
    }
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      ++i;
    }
    else if (c == '/' && at(i + 1) == '/') {
      while (i < src.size() && src[i] != '\n') {
        ++i;
      }
    }
    else if (c == '/' && at(i + 1) == '*') {
      auto const end = src.find("*/", i + 2);
      if (end == std::string_view::npos) {
        return fail(line, "a comment that never ends");
      }
      line += static_cast<std::size_t>(
          std::ranges::count(src.substr(i, end - i), '\n'));
      i = end + 2;
    }
    else if (c == '"' || c == '\'') {
      auto const start = line;
      std::string text;
      for (++i; i < src.size() && src[i] != c; ++i) {
        if (src[i] == '\n') {
          return fail(start, "a literal that never ends");
        }
        if (src[i] == '\\' && i + 1 < src.size()) {
          ++i;
        }
        text.push_back(src[i]);
      }
      if (i == src.size()) {
        return fail(start, "a literal that never ends");
      }
      ++i;
      if (c == '"') {
        tokens.push_back({token::kind::string, std::move(text), start});
      }
    }
    else if (identifier_start(c)) {
      auto const start = i;
      while (identifier_char(at(i))) {
        ++i;
      }
      tokens.push_back({token::kind::identifier,
                        std::string{src.substr(start, i - start)},
                        line});
    }
    else if (c >= '0' && c <= '9') {
      std::string text;
      for (; identifier_char(at(i)) || at(i) == '\''; ++i) {
        if (src[i] != '\'') { // a digit separator
          text.push_back(src[i]);
        }
      }
      tokens.push_back({token::kind::number, std::move(text), line});
    }
    else if (c == ':' && at(i + 1) == ':') {
      tokens.push_back({token::kind::punct, "::", line});
      i += 2;
    }
    else {
      tokens.push_back({token::kind::punct, std::string(1, c), line});
      ++i;
    }
  }
  return tokens;
}

std::optional<std::uint32_t> number_value(std::string_view text)
{
  while (!text.empty() && std::string_view{"uUlL"}.contains(text.back())) {
    text.remove_suffix(1);
  }
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    base = 16;
    text.remove_prefix(2);
  }
  else if (text.size() > 1 && text.starts_with('0')) {
    base = 8;
  }
  std::uint32_t value = 0;
  auto const end = text.data() + text.size();
  auto const [ptr, ec] = std::from_chars(text.data(), end, value, base);
  if (text.empty() || ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<od_value_type> type_named(std::string_view text)
{
  for (std::size_t i = 0; i < od_value_type_count; ++i) {
    auto const type = static_cast<od_value_type>(i);
    if (name(type) == text) {
      return type;
    }
  }
  return std::nullopt;
}

// The access an emblib binding gives its object: od_handlers.hpp's, and
// the rw and ro of od_settings.hpp's bridges.
std::optional<od_access> access_of(std::string_view binding)
{
  if (binding == "od_ro" || binding == "ro") {
    return od_access::ro;
  }
  if (binding == "od_rw" || binding == "rw") {
    return od_access::rw;
  }
  if (binding == "od_wo"
      || binding == "wo"
      || binding == "od_exec"
      || binding == "od_restore_default") {
    return od_access::wo;
  }
  if (binding == "od_const" || binding == "od_text") {
    return od_access::const_;
  }
  return std::nullopt;
}

struct row {
  std::size_t line;
  od_key key;
  std::array<std::string, 4> names; // category, subcategory, name, unit
  od_access access;
  od_value_type type;
};

class row_scanner {
public:
  explicit row_scanner(std::vector<token> const& tokens) : tokens_(tokens) {}

  // Every {{index, subindex}, "category", "subcategory", "name", "unit",
  // type, binding} among the tokens.
  std::expected<std::vector<row>, tool_error> scan()
  {
    std::vector<row> rows;
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      auto const end = match(i);
      if (!end) {
        continue;
      }
      auto r = take(i);
      if (!r) {
        return std::unexpected(r.error());
      }
      rows.push_back(std::move(*r));
      i = *end;
    }
    return rows;
  }

private:
  bool is(std::size_t k, std::string_view punct) const
  {
    return k < tokens_.size()
        && tokens_[k].what == token::kind::punct
        && tokens_[k].text == punct;
  }

  bool is(std::size_t k, token::kind what) const
  {
    return k < tokens_.size() && tokens_[k].what == what;
  }

  // The last name of a qualified name at `k`, and the token after it.
  std::optional<std::pair<std::string_view, std::size_t>>
  qualified(std::size_t k) const
  {
    if (is(k, "::")) {
      ++k;
    }
    if (!is(k, token::kind::identifier)) {
      return std::nullopt;
    }
    std::string_view last = tokens_[k].text;
    for (++k; is(k, "::") && is(k + 1, token::kind::identifier); k += 2) {
      last = tokens_[k + 1].text;
    }
    return std::pair{last, k};
  }

  // Where a row that begins at `i` ends: the token of its closing brace.
  std::optional<std::size_t> match(std::size_t i) const
  {
    using enum token::kind;
    if (!(is(i, "{")
          && is(i + 1, "{")
          && is(i + 2, number)
          && is(i + 3, ",")
          && is(i + 4, number)
          && is(i + 5, "}")
          && is(i + 6, ","))) {
      return std::nullopt;
    }
    auto k = i + 7;
    for (int field = 0; field < 4; ++field, k += 2) {
      if (!(is(k, string) && is(k + 1, ","))) {
        return std::nullopt;
      }
    }
    auto const type = qualified(k);
    if (!type || !is(type->second, ",") || !qualified(type->second + 1)) {
      return std::nullopt;
    }
    // The binding runs to the brace that closes the row.
    int depth = 0;
    for (k = type->second + 1; k < tokens_.size(); ++k) {
      if (is(k, "{")) {
        ++depth;
      }
      else if (is(k, "}")) {
        if (depth == 0) {
          return k;
        }
        --depth;
      }
    }
    return std::nullopt;
  }

  std::expected<row, tool_error> take(std::size_t i) const
  {
    auto const line = tokens_[i].line;
    auto const index = number_value(tokens_[i + 2].text);
    auto const subindex = number_value(tokens_[i + 4].text);
    if (!index || *index > 0xFFFF || !subindex || *subindex > 0xFF) {
      return fail(line, "a key out of range");
    }
    row r{.line = line,
          .key = {static_cast<std::uint16_t>(*index),
                  static_cast<std::uint8_t>(*subindex)},
          .names = {tokens_[i + 7].text,
                    tokens_[i + 9].text,
                    tokens_[i + 11].text,
                    tokens_[i + 13].text},
          .access = od_access::ro,
          .type = od_value_type::uint32};
    auto const type = qualified(i + 15);
    auto const binding = qualified(type->second + 1);
    auto const named_type = type_named(type->first);
    if (!named_type) {
      return fail(line, std::format("an unknown type, {}", type->first));
    }
    auto const access = access_of(binding->first);
    if (!access) {
      return fail(
          line,
          std::format("a binding of no known access, {}", binding->first));
    }
    r.type = *named_type;
    r.access = *access;
    for (std::size_t n = 0; n < r.names.size(); ++n) {
      if (!holdable(r.names[n])) {
        return fail(
            line,
            std::format("\"{}\" holds a blank, '\"' or '\\'", r.names[n]));
      }
      if (n < 3 && r.names[n].empty()) {
        return fail(line, "an empty category, subcategory or name");
      }
    }
    return r;
  }

  std::vector<token> const& tokens_;
};

} // namespace

std::expected<std::string, tool_error>
od_file_from_emblib(std::string_view source,
                    std::string_view origin,
                    emblib_options const& options)
{
  auto const tokens = tokenize(source);
  if (!tokens) {
    return std::unexpected(tokens.error());
  }
  auto const rows = row_scanner{*tokens}.scan();
  if (!rows) {
    return std::unexpected(rows.error());
  }
  if (rows->empty()) {
    return fail(0, "no dictionary rows");
  }

  auto const category = [&rows](std::optional<std::string> const& chosen,
                                std::string_view usual) {
    if (chosen) {
      return *chosen;
    }
    bool const used = std::ranges::any_of(*rows, [usual](row const& r) {
      return r.names[0] == usual;
    });
    return used ? std::string{usual} : std::string{};
  };

  // The rows as an OD file, and the line of the source each line came
  // from, to blame the source for what parse_od_file() finds.
  std::string text = "cannet-od 1\n";
  std::vector<std::size_t> source_lines{0, 0};
  for (auto const& [directive, value] :
       {std::pair{"watch_category", category(options.watch_category, "watch")},
        std::pair{"config_category",
                  category(options.config_category, "config")}}) {
    if (!value.empty()) {
      text += std::format("{} {}\n", directive, value);
      source_lines.push_back(0);
    }
  }
  for (auto const& r : *rows) {
    auto const& [category_name, subcategory, object, unit] = r.names;
    text += std::format("{} {} {} {} {} {} {}\n",
                        to_string(r.key),
                        category_name,
                        subcategory,
                        object,
                        unit.empty() ? std::string_view{"-"} : unit,
                        name(r.access),
                        name(r.type));
    source_lines.push_back(r.line);
  }

  auto const loaded = parse_od_file(std::move(text));
  if (!loaded) {
    auto const& e = loaded.error();
    return fail(e.line < source_lines.size() ? source_lines[e.line] : 0,
                std::string{to_string(e.reason)});
  }
  auto const written = to_od_file(*loaded);
  if (!written) {
    return fail(0, to_string(written.error()));
  }
  return std::format("# Made by canopen od from-emblib from {}.\n", origin)
       + *written;
}

std::expected<std::string, tool_error> cpp_header(dictionary_view dictionary,
                                                  std::string_view name,
                                                  std::string_view ns,
                                                  std::string_view origin)
{
  if (dictionary.empty()) {
    return fail(0, "the dictionary is empty");
  }
  if (!identifier(name)) {
    return fail(0, std::format("not a C++ name: {}", name));
  }
  if (!ns.empty()) {
    for (auto const part : std::views::split(ns, std::string_view{"::"})) {
      if (!identifier(std::string_view{part.begin(), part.end()})) {
        return fail(0, std::format("not a C++ namespace: {}", ns));
      }
    }
  }

  std::string out = std::format(
      "// Made by `canopen od header` from {}: edit that file, not this one.\n"
      "#pragma once\n"
      "\n"
      "#include <canopen/od.hpp>\n"
      "\n",
      origin);
  if (!ns.empty()) {
    out += std::format("namespace {} {{\n\n", ns);
  }
  auto const config = dictionary.config();
  out += std::format(
      "inline constexpr auto {} = cannet::canopen::dictionary{{\n"
      "    {{.watch_category = \"{}\", .config_category = "
      "\"{}\"}},\n"
      "    {{\n",
      name,
      config.watch_category,
      config.config_category);
  for (auto const& e : dictionary.entries()) {
    auto const& o = e.object;
    if (!holdable(o.category)
        || !holdable(o.subcategory)
        || !holdable(o.name)
        || !holdable(o.unit)) {
      return fail(0,
                  std::format("{}:{:02X} holds a blank, '\"' or '\\'",
                              e.key.index,
                              e.key.subindex));
    }
    out += std::format(
        "        {{{{0x{:04X}, 0x{:02X}}}, {{\"{}\", \"{}\", \"{}\", \"{}\", "
        "cannet::canopen::od_access::{}, "
        "cannet::canopen::od_value_type::{}}}}},\n",
        e.key.index,
        e.key.subindex,
        o.category,
        o.subcategory,
        o.name,
        o.unit,
        o.access == od_access::const_ ? "const_" : canopen::name(o.access),
        canopen::name(o.type));
  }
  out += "    }};\n";
  if (!ns.empty()) {
    out += std::format("\n}} // namespace {}\n", ns);
  }
  return out;
}

} // namespace cannet::canopen::tool
