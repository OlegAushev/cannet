#include <canopen/od.hpp>

namespace cannet::canopen {

namespace {

template<typename T>
T read_raw(expedited_sdo_data raw)
{
  T value;
  std::memcpy(&value, raw.data(), sizeof(value));
  return value;
}

} // namespace

od_value make_od_value(expedited_sdo_data raw, od_value_type type)
{
  switch (type) {
  case od_value_type::boolean: return raw[0] != 0;
  case od_value_type::int8: return static_cast<std::int8_t>(raw[0]);
  case od_value_type::int16: return read_raw<std::int16_t>(raw);
  case od_value_type::int32: return read_raw<std::int32_t>(raw);
  case od_value_type::uint8: return raw[0];
  case od_value_type::uint16: return read_raw<std::uint16_t>(raw);
  case od_value_type::float32: return read_raw<float>(raw);
  case od_value_type::uint32:
  case od_value_type::exec:
  case od_value_type::string: return read_raw<std::uint32_t>(raw);
  }
  return std::uint32_t{0};
}

expedited_sdo_data to_raw(od_value value)
{
  expedited_sdo_data raw{};
  std::visit([&raw](auto const& v) { std::memcpy(raw.data(), &v, sizeof(v)); },
             value);
  return raw;
}

} // namespace cannet::canopen
