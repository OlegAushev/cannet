#include <canopen/sdo.hpp>

namespace cannet::canopen {

std::string_view to_string(sdo_abort_code code)
{
  switch (code) {
  case sdo_abort_code::invalid_cs:
    return "command specifier not valid or unknown";
  case sdo_abort_code::unsupported_access:
    return "unsupported access to an object";
  case sdo_abort_code::read_from_write_only:
    return "attempt to read a write-only object";
  case sdo_abort_code::write_to_read_only:
    return "attempt to write a read-only object";
  case sdo_abort_code::object_not_found:
    return "object does not exist in the object dictionary";
  case sdo_abort_code::hardware_error:
    return "access failed due to a hardware error";
  case sdo_abort_code::data_type_mismatch: return "data type does not match";
  case sdo_abort_code::value_range_exceeded:
    return "value range of parameter exceeded";
  case sdo_abort_code::value_too_high: return "value of parameter too high";
  case sdo_abort_code::value_too_low: return "value of parameter too low";
  case sdo_abort_code::general_error: return "general error";
  case sdo_abort_code::data_store_error:
    return "data cannot be transferred or stored to the application";
  case sdo_abort_code::local_control_error:
    return "data cannot be transferred or stored to the application because "
           "of local control";
  case sdo_abort_code::state_error:
    return "data cannot be transferred or stored to the application because "
           "of the present device state";
  case sdo_abort_code::no_data_available: return "no data available";
  }
  return "unknown abort code";
}

std::string_view name(sdo_abort_code code)
{
  switch (code) {
  case sdo_abort_code::invalid_cs: return "invalid_cs";
  case sdo_abort_code::unsupported_access: return "unsupported_access";
  case sdo_abort_code::read_from_write_only: return "read_from_write_only";
  case sdo_abort_code::write_to_read_only: return "write_to_read_only";
  case sdo_abort_code::object_not_found: return "object_not_found";
  case sdo_abort_code::hardware_error: return "hardware_error";
  case sdo_abort_code::data_type_mismatch: return "data_type_mismatch";
  case sdo_abort_code::value_range_exceeded: return "value_range_exceeded";
  case sdo_abort_code::value_too_high: return "value_too_high";
  case sdo_abort_code::value_too_low: return "value_too_low";
  case sdo_abort_code::general_error: return "general_error";
  case sdo_abort_code::data_store_error: return "data_store_error";
  case sdo_abort_code::local_control_error: return "local_control_error";
  case sdo_abort_code::state_error: return "state_error";
  case sdo_abort_code::no_data_available: return "no_data_available";
  }
  return "unknown";
}

} // namespace cannet::canopen
