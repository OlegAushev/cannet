#pragma once

// cannet::canopen::sdo_error — why an SDO operation failed — and
// sdo_result<T>, what the SDO client's operations and those of the
// services over it complete with.
//
// Protocol plane, unprivileged, no I/O, no state.

#include <canopen/sdo.hpp>
#include <canopen/transport.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace cannet::canopen {

struct sdo_error {
  enum class kind : std::uint8_t {
    aborted,       // the device refused; `abort` says why
    timeout,       // no answer within the node's SDO timeout
    cancelled,     // by the caller, or by a change of the node's id
    type_mismatch, // the device's object has another type than the client's:
                   // an answer of another size, a value of another type
    malformed,     // an answer the client cannot use, such as a segmented one
    transport,     // the request was not sent; `send_error` says why
  };

  kind reason;
  sdo_abort_code abort{};       // with aborted
  transport_error send_error{}; // with transport

  friend bool operator==(sdo_error const&, sdo_error const&) = default;
};

// A human-readable description, for people; the wording may change.
std::string to_string(sdo_error const& e);
std::string_view to_string(sdo_error::kind k);

// The stable identifier of a kind: its enumerator's name, such as
// "type_mismatch", for logs and formats a program reads.
std::string_view name(sdo_error::kind k);

template<typename T>
using sdo_result = std::expected<T, sdo_error>;

} // namespace cannet::canopen
