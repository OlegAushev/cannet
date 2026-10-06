#pragma once

// cannet::canopen::setup_error — why the client refused a configuration
// change: adding a node, changing a node id, setting up a PDO, picking the
// objects a service polls.
//
// Protocol plane, unprivileged, no I/O, no state.

#include <string_view>

namespace cannet::canopen {

enum class setup_error {
  node_id_taken,  // another node of the client, or the client itself, has it
  name_taken,     // another node of the client has it
  no_such_node,   // the client has no node by that name
  invalid_pdo,    // a PDO number outside 1..4, the predefined connection set
  no_such_object, // the service has no object by that key
};

// A human-readable description of a `setup_error`, for people; the wording
// may change.
std::string_view to_string(setup_error e);

// The stable identifier of a `setup_error`: its enumerator's name, such as
// "node_id_taken", for logs and formats a program reads.
std::string_view name(setup_error e);

} // namespace cannet::canopen
