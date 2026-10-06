#pragma once

// Object dictionary: the description of one device's objects, and the typed
// value that travels through an expedited SDO.
//
// Protocol plane, unprivileged, no I/O, no state — a dictionary is constexpr
// data. cannet ships no dictionary of its own: a per-device application
// declares one and hands it to the client as `dictionary_view`.
//
//   inline constexpr auto object_dictionary = cannet::canopen::dictionary{
//       {.watch_category = "watch", .config_category = "config"},
//       {{{0x1008, 0x00}, {"info", "sys", "device_name", "",
//                          od_access::const_, od_value_type::string}},
//        {{0x2000, 0x01}, {"ctl", "sys", "reset_device", "",
//                          od_access::wo, od_value_type::exec}}}};
//
// The constructor is consteval and validates the table (ordering, duplicate
// keys, duplicate names, empty names, categories named in the config): a
// malformed dictionary is a compile error, never a runtime surprise. Entries
// are sorted by key at compile time and indexed by name, so both lookups are
// binary searches over static storage with no allocation.
//
// A `dictionary_view` is a non-owning view: declare dictionaries at namespace
// scope (`inline constexpr`), never as locals.
//
// Object metadata mirrors emb::can::canopen::od_object minus the accessors —
// reading and writing objects is the device's job, not the client's.

#include <canopen/sdo.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

namespace cannet::canopen {

// Typed OD value: every scalar representable in a 4-byte expedited SDO.
using od_value = std::variant<bool,
                              std::int8_t,
                              std::int16_t,
                              std::int32_t,
                              std::uint8_t,
                              std::uint16_t,
                              std::uint32_t,
                              float>;

namespace detail {

template<typename T, typename Variant>
struct is_alternative_of;

template<typename T, typename... Ts>
struct is_alternative_of<T, std::variant<Ts...>>
    : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};

} // namespace detail

// T is one of od_value's alternatives; derived from od_value itself so the
// two cannot drift apart.
template<typename T>
concept od_scalar = detail::is_alternative_of<T, od_value>::value;

enum class od_value_type : std::uint8_t {
  boolean,
  int8,
  int16,
  int32,
  uint8,
  uint16,
  uint32,
  float32,
  exec,  // write-only trigger; the written bytes are a command, not a value
  string // read as a sequence of expedited responses, NUL-terminated
};

inline constexpr std::size_t od_value_type_count = 10;

enum class od_access : std::uint8_t { rw, ro, wo, const_ };

// Number of significant bytes an object of this type puts on the wire.
constexpr std::size_t od_value_size(od_value_type type)
{
  constexpr std::array<std::size_t, od_value_type_count> sizes = {
      sizeof(bool),
      sizeof(std::int8_t),
      sizeof(std::int16_t),
      sizeof(std::int32_t),
      sizeof(std::uint8_t),
      sizeof(std::uint16_t),
      sizeof(std::uint32_t),
      sizeof(float),
      4,
      4};
  return sizes[std::to_underlying(type)];
}

// The type a value travels as: that of its alternative. Never exec or
// string, which no od_value holds.
constexpr od_value_type type_of(od_value const& value)
{
  return std::visit(
      []<typename T>(T const&) {
        if constexpr (std::is_same_v<T, bool>) {
          return od_value_type::boolean;
        }
        else if constexpr (std::is_same_v<T, std::int8_t>) {
          return od_value_type::int8;
        }
        else if constexpr (std::is_same_v<T, std::int16_t>) {
          return od_value_type::int16;
        }
        else if constexpr (std::is_same_v<T, std::int32_t>) {
          return od_value_type::int32;
        }
        else if constexpr (std::is_same_v<T, std::uint8_t>) {
          return od_value_type::uint8;
        }
        else if constexpr (std::is_same_v<T, std::uint16_t>) {
          return od_value_type::uint16;
        }
        else if constexpr (std::is_same_v<T, std::uint32_t>) {
          return od_value_type::uint32;
        }
        else if constexpr (std::is_same_v<T, float>) {
          return od_value_type::float32;
        }
        else {
          static_assert(false, "od: an od_value alternative has no type");
        }
      },
      value);
}

// Decodes the raw 4-byte SDO data field per the object's declared type.
// `exec` and `string` are carried as uint32: neither has a scalar value —
// exec bytes are a command, string bytes are four characters of a longer
// value the SDO client reassembles.
od_value make_od_value(expedited_sdo_data raw, od_value_type type);

// Encodes a typed value back into the raw 4-byte data field.
expedited_sdo_data to_raw(od_value value);

struct od_key {
  std::uint16_t index;
  std::uint8_t subindex;

  friend constexpr bool operator==(od_key, od_key) = default;
  friend constexpr auto operator<=>(od_key, od_key) = default;
};

struct od_object {
  std::string_view category;
  std::string_view subcategory;
  std::string_view name;
  std::string_view unit;
  od_access access;
  od_value_type type;

  constexpr bool readable() const
  {
    return access != od_access::wo;
  }

  constexpr bool writable() const
  {
    return access == od_access::rw || access == od_access::wo;
  }
};

struct od_entry {
  od_key key;
  od_object object;
};

namespace detail {

using od_name_tuple =
    std::tuple<std::string_view, std::string_view, std::string_view>;

constexpr od_name_tuple name_tuple(od_object const& object)
{
  return {object.category, object.subcategory, object.name};
}

// Projections from an index in the name order to what a lookup compares. Each
// captures the entry span by value, so a view built from one stays valid as
// long as the dictionary does, independently of the dictionary_view object it
// came from.
constexpr auto entry_at(std::span<od_entry const> entries)
{
  return [entries](std::uint16_t i) -> od_entry const& { return entries[i]; };
}

constexpr auto category_at(std::span<od_entry const> entries)
{
  return [entries](std::uint16_t i) -> std::string_view {
    return entries[i].object.category;
  };
}

constexpr auto name_tuple_at(std::span<od_entry const> entries)
{
  return [entries](std::uint16_t i) { return name_tuple(entries[i].object); };
}

} // namespace detail

// Categories a client's higher-level services key on. Empty disables the
// corresponding service for this device.
struct dictionary_config {
  std::string_view watch_category;
  std::string_view config_category;
};

// Non-owning view of a validated dictionary. Cheap to copy; must not outlive
// the `dictionary` it came from.
class dictionary_view {
public:
  constexpr dictionary_view() = default;

  constexpr dictionary_view(dictionary_config config,
                            std::span<od_entry const> entries,
                            std::span<std::uint16_t const> name_order)
      : config_(config), entries_(entries), name_order_(name_order)
  {
  }

  constexpr dictionary_config const& config() const
  {
    return config_;
  }

  constexpr bool empty() const
  {
    return entries_.empty();
  }

  constexpr std::size_t size() const
  {
    return entries_.size();
  }

  // All entries, ordered by {index, subindex}.
  constexpr std::span<od_entry const> entries() const
  {
    return entries_;
  }

  // All entries, ordered by {category, subcategory, name}.
  constexpr auto entries_by_name() const
  {
    return name_order_ | std::views::transform(detail::entry_at(entries_));
  }

  // Entries of one category, ordered by {subcategory, name}.
  constexpr auto in_category(std::string_view category) const
  {
    auto const range = std::ranges::equal_range(name_order_,
                                                category,
                                                std::less{},
                                                detail::category_at(entries_));
    return range | std::views::transform(detail::entry_at(entries_));
  }

  // Lookup by {index, subindex}; nullptr when absent.
  constexpr od_entry const* find(od_key key) const
  {
    auto it = std::ranges::lower_bound(entries_,
                                       key,
                                       std::less{},
                                       &od_entry::key);
    if (it == entries_.end() || it->key != key) {
      return nullptr;
    }
    return &*it;
  }

  // Lookup by the name triple the UI and CLI use; nullptr when absent.
  constexpr od_entry const* find(std::string_view category,
                                 std::string_view subcategory,
                                 std::string_view name) const
  {
    auto const target = std::tuple{category, subcategory, name};
    auto it = std::ranges::lower_bound(name_order_,
                                       target,
                                       std::less{},
                                       detail::name_tuple_at(entries_));
    if (it == name_order_.end()
        || detail::name_tuple(entries_[*it].object) != target) {
      return nullptr;
    }
    return &entries_[*it];
  }

private:
  dictionary_config config_;
  std::span<od_entry const> entries_;
  std::span<std::uint16_t const> name_order_;
};

// A device's object dictionary, validated and indexed at compile time.
template<std::size_t N>
class dictionary {
  static_assert(N > 0, "od: dictionary is empty");
  static_assert(N <= 65535, "od: dictionary is too large");

public:
  consteval dictionary(dictionary_config config, od_entry const (&entries)[N])
      : config_(config)
  {
    std::ranges::copy(entries, entries_.begin());
    std::ranges::sort(entries_, std::less{}, &od_entry::key);

    for (std::size_t i = 0; i < N; ++i) {
      auto const& object = entries_[i].object;
      if (object.category.empty()
          || object.subcategory.empty()
          || object.name.empty()) {
        throw "od: entry has an empty category, subcategory or name";
      }
      if (i + 1 < N && entries_[i].key == entries_[i + 1].key) {
        throw "od: duplicate {index, subindex}";
      }
      name_order_[i] = static_cast<std::uint16_t>(i);
    }

    std::ranges::sort(name_order_, std::less{}, [this](std::uint16_t i) {
      auto const& object = entries_[i].object;
      return std::tuple{object.category, object.subcategory, object.name};
    });

    for (std::size_t i = 0; i + 1 < N; ++i) {
      auto const& lhs = entries_[name_order_[i]].object;
      auto const& rhs = entries_[name_order_[i + 1]].object;
      if (lhs.category == rhs.category
          && lhs.subcategory == rhs.subcategory
          && lhs.name == rhs.name) {
        throw "od: duplicate {category, subcategory, name}";
      }
    }

    if (!config_.watch_category.empty()
        && !has_category(config_.watch_category)) {
      throw "od: watch_category names a category no entry belongs to";
    }
    if (!config_.config_category.empty()
        && !has_category(config_.config_category)) {
      throw "od: config_category names a category no entry belongs to";
    }
  }

  constexpr dictionary_view view() const
  {
    return {config_, entries_, name_order_};
  }

  constexpr operator dictionary_view() const
  {
    return view();
  }

private:
  consteval bool has_category(std::string_view category) const
  {
    return std::ranges::any_of(entries_, [category](od_entry const& e) {
      return e.object.category == category;
    });
  }

  dictionary_config config_;
  std::array<od_entry, N> entries_{};
  std::array<std::uint16_t, N> name_order_{};
};

template<std::size_t N>
dictionary(dictionary_config, od_entry const (&)[N]) -> dictionary<N>;

} // namespace cannet::canopen
