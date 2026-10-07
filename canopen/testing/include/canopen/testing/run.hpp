#pragma once

// cannet::canopen::testing::run_for, run_until — running an io_context in
// a test: for a while, or until what the test expects has happened.
//
// Test support over the protocol plane, unprivileged, no I/O of its own.
//
// Thread model: call from the thread that runs the io_context, the test's
// own; the handlers it runs run there.

#include <boost/asio/io_context.hpp>

#include <chrono>

namespace cannet::canopen::testing {

// Runs handlers for `duration`. An io_context that ran out of work stops,
// and runs nothing more until restarted: restart it every time.
inline void run_for(boost::asio::io_context& io,
                    std::chrono::milliseconds duration)
{
  io.restart();
  io.run_for(duration);
}

// Runs handlers until `done()` holds, for at most `limit`, and returns
// done(). A test that waits for what it expects, rather than for a time it
// guessed, holds on a loaded machine too.
template<typename Done>
bool run_until(boost::asio::io_context& io,
               Done done,
               std::chrono::milliseconds limit = std::chrono::seconds{2})
{
  auto const deadline = std::chrono::steady_clock::now() + limit;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    io.restart();
    io.run_one_for(std::chrono::milliseconds{1});
  }
  return done();
}

} // namespace cannet::canopen::testing
