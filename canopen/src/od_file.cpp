#include <canopen/od_file.hpp>

#include <canopen/od_format.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace cannet::canopen {

std::string_view to_string(od_file_error::kind k)
{
  using enum od_file_error::kind;
  switch (k) {
  case version: return "not an OD file: \"cannet-od 1\" does not come first";
  case syntax: return "a line with a wrong number of fields, or no sense";
  case bad_key: return "a key that is not index:subindex in hex";
  case bad_field: return "a field that holds a blank, '\"' or '\\'";
  case unknown_access: return "an unknown access";
  case unknown_type: return "an unknown type";
  case duplicate_category: return "a category line given twice";
  case duplicate_key: return "two objects at one key";
  case duplicate_name: return "two objects with one name";
  case empty_name: return "an empty category, subcategory or name";
  case unknown_category: return "a category no object belongs to";
  case too_many_objects: return "more than 65535 objects";
  }
  return "unknown error";
}

std::string_view name(od_file_error::kind k)
{
  using enum od_file_error::kind;
  switch (k) {
  case version: return "version";
  case syntax: return "syntax";
  case bad_key: return "bad_key";
  case bad_field: return "bad_field";
  case unknown_access: return "unknown_access";
  case unknown_type: return "unknown_type";
  case duplicate_category: return "duplicate_category";
  case duplicate_key: return "duplicate_key";
  case duplicate_name: return "duplicate_name";
  case empty_name: return "empty_name";
  case unknown_category: return "unknown_category";
  case too_many_objects: return "too_many_objects";
  }
  return "unknown";
}

std::string to_string(od_file_error const& e)
{
  if (e.line == 0) {
    return std::string{to_string(e.reason)};
  }
  return std::format("line {}: {}", e.line, to_string(e.reason));
}

namespace {

using kind = od_file_error::kind;

// dictionary_view indexes names with 16 bits.
constexpr std::size_t max_objects = 65535;

std::unexpected<od_file_error> fail(kind reason, std::size_t line)
{
  return std::unexpected(od_file_error{.reason = reason, .line = line});
}

bool blank(char c)
{
  return c == ' ' || c == '\t';
}

std::vector<std::string_view> fields_of(std::string_view line)
{
  std::vector<std::string_view> fields;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && blank(line[i])) {
      ++i;
    }
    auto const start = i;
    while (i < line.size() && !blank(line[i])) {
      ++i;
    }
    if (i > start) {
      fields.push_back(line.substr(start, i - start));
    }
  }
  return fields;
}

// What a field may hold, for the format and for a C++ string literal.
bool holdable(std::string_view field)
{
  return std::ranges::none_of(field, [](char c) {
    return blank(c) || c == '"' || c == '\\' || c == '\n' || c == '\r';
  });
}

std::optional<std::uint16_t> parse_hex(std::string_view text,
                                       std::size_t max_digits)
{
  if (text.empty() || text.size() > max_digits) {
    return std::nullopt;
  }
  std::uint16_t value = 0;
  auto const end = text.data() + text.size();
  auto const [ptr, ec] = std::from_chars(text.data(), end, value, 16);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<od_key> parse_key(std::string_view text)
{
  auto const colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  auto const index = parse_hex(text.substr(0, colon), 4);
  auto const subindex = parse_hex(text.substr(colon + 1), 2);
  if (!index || !subindex) {
    return std::nullopt;
  }
  return od_key{*index, static_cast<std::uint8_t>(*subindex)};
}

std::optional<od_access> parse_access(std::string_view text)
{
  for (auto const access :
       {od_access::rw, od_access::ro, od_access::wo, od_access::const_}) {
    if (name(access) == text) {
      return access;
    }
  }
  return std::nullopt;
}

std::optional<od_value_type> parse_type(std::string_view text)
{
  for (std::size_t i = 0; i < od_value_type_count; ++i) {
    auto const type = static_cast<od_value_type>(i);
    if (name(type) == text) {
      return type;
    }
  }
  return std::nullopt;
}

// Columns a field takes: one per UTF-8 code point, as in "°C".
std::size_t columns(std::string_view field)
{
  return static_cast<std::size_t>(std::ranges::count_if(field, [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}

void append_column(std::string& out, std::string_view field, std::size_t width)
{
  out += field;
  out.append(width - columns(field) + 2, ' ');
}

} // namespace

dictionary_view loaded_dictionary::view() const
{
  return {config_, entries_, name_order_};
}

std::expected<loaded_dictionary, od_file_error> parse_od_file(std::string text)
{
  loaded_dictionary d;
  d.text_ = std::make_unique<std::string const>(std::move(text));

  struct object {
    od_entry entry;
    std::size_t line;
  };
  std::vector<object> objects;
  std::size_t watch_line = 0;
  std::size_t config_line = 0;
  bool versioned = false;
  std::size_t number = 0;

  for (auto const range : std::views::split(std::string_view{*d.text_}, '\n')) {
    ++number;
    std::string_view line{range.begin(), range.end()};
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    auto const fields = fields_of(line);
    if (fields.empty() || fields.front().starts_with('#')) {
      continue;
    }
    if (!versioned) {
      if (fields.size() != 2 || fields[0] != "cannet-od" || fields[1] != "1") {
        return fail(kind::version, number);
      }
      versioned = true;
      continue;
    }
    if (fields[0] == "watch_category" || fields[0] == "config_category") {
      if (fields.size() != 2) {
        return fail(kind::syntax, number);
      }
      if (!holdable(fields[1])) {
        return fail(kind::bad_field, number);
      }
      bool const watch = fields[0] == "watch_category";
      auto& seen = watch ? watch_line : config_line;
      if (seen != 0) {
        return fail(kind::duplicate_category, number);
      }
      seen = number;
      (watch ? d.config_.watch_category
             : d.config_.config_category) = fields[1];
      continue;
    }
    if (fields.size() != 7) {
      return fail(kind::syntax, number);
    }
    auto const key = parse_key(fields[0]);
    if (!key) {
      return fail(kind::bad_key, number);
    }
    if (!std::ranges::all_of(fields | std::views::drop(1), holdable)) {
      return fail(kind::bad_field, number);
    }
    auto const access = parse_access(fields[5]);
    if (!access) {
      return fail(kind::unknown_access, number);
    }
    auto const type = parse_type(fields[6]);
    if (!type) {
      return fail(kind::unknown_type, number);
    }
    if (objects.size() == max_objects) {
      return fail(kind::too_many_objects, number);
    }
    objects.push_back(
        {.entry = {.key = *key,
                   .object = {.category = fields[1],
                              .subcategory = fields[2],
                              .name = fields[3],
                              .unit = fields[4] == "-" ? std::string_view{}
                                                       : fields[4],
                              .access = *access,
                              .type = *type}},
         .line = number});
  }
  if (!versioned) {
    return fail(kind::version, 0);
  }

  // Stable: of two objects at one key, the second line is the one to blame.
  std::ranges::stable_sort(objects, std::less{}, [](object const& o) {
    return o.entry.key;
  });
  std::vector<std::size_t> lines;
  d.entries_.reserve(objects.size());
  lines.reserve(objects.size());
  for (auto const& o : objects) {
    d.entries_.push_back(o.entry);
    lines.push_back(o.line);
  }
  d.name_order_.resize(d.entries_.size());
  detail::order_by_name(d.entries_, d.name_order_);

  auto const finding = detail::check_dictionary(d.config_,
                                                d.entries_,
                                                d.name_order_);
  if (!finding) {
    return d;
  }
  switch (finding->defect) {
  case detail::od_defect::empty_name:
    return fail(kind::empty_name, lines[finding->entry]);
  case detail::od_defect::duplicate_key:
    return fail(kind::duplicate_key, lines[finding->entry]);
  case detail::od_defect::duplicate_name:
    return fail(kind::duplicate_name, lines[finding->entry]);
  case detail::od_defect::unknown_watch_category:
    return fail(kind::unknown_category, watch_line);
  case detail::od_defect::unknown_config_category:
    return fail(kind::unknown_category, config_line);
  }
  return d;
}

std::expected<std::string, od_file_error> to_od_file(dictionary_view dictionary)
{
  std::string out = "cannet-od 1\n";
  std::size_t line = 1;

  auto const config = dictionary.config();
  for (auto const& [directive, category] :
       {std::pair{std::string_view{"watch_category"}, config.watch_category},
        std::pair{std::string_view{"config_category"},
                  config.config_category}}) {
    if (category.empty()) {
      continue;
    }
    ++line;
    if (!holdable(category)) {
      return fail(kind::bad_field, line);
    }
    out += std::format("{:<15} {}\n", directive, category);
  }

  auto const entries = dictionary.entries();
  if (!entries.empty()) {
    out += '\n';
    ++line;
  }
  auto const unit_of = [](od_object const& o) {
    return o.unit.empty() ? std::string_view{"-"} : o.unit;
  };
  std::size_t category_width = 0;
  std::size_t subcategory_width = 0;
  std::size_t name_width = 0;
  std::size_t unit_width = 0;
  std::size_t access_width = 0;
  for (auto const& e : entries) {
    category_width = std::max(category_width, columns(e.object.category));
    subcategory_width = std::max(subcategory_width,
                                 columns(e.object.subcategory));
    name_width = std::max(name_width, columns(e.object.name));
    unit_width = std::max(unit_width, columns(unit_of(e.object)));
    access_width = std::max(access_width, name(e.object.access).size());
  }

  for (auto const& e : entries) {
    ++line;
    auto const& o = e.object;
    auto const unit = unit_of(o);
    if (!holdable(o.category)
        || !holdable(o.subcategory)
        || !holdable(o.name)
        || !holdable(unit)) {
      return fail(kind::bad_field, line);
    }
    out += std::format("{:04X}:{:02X}  ", e.key.index, e.key.subindex);
    append_column(out, o.category, category_width);
    append_column(out, o.subcategory, subcategory_width);
    append_column(out, o.name, name_width);
    append_column(out, unit, unit_width);
    append_column(out, name(o.access), access_width);
    out += name(o.type);
    out += '\n';
  }
  return out;
}

} // namespace cannet::canopen
