// watch: a node's watch objects, polled by its watch service and printed a
// table per pass.

#include "command_support.hpp"

#include <canopen/od_format.hpp>
#include <canopen/service/object_reading.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <ostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace cannet::canopen::tool {

namespace {

namespace asio = boost::asio;
using clock_type = std::chrono::steady_clock;

// The cursor home, and the screen cleared.
constexpr std::string_view clear_screen = "\x1b[H\x1b[2J";

// A watched object and its latest reading.
struct cell {
  od_entry const* entry;
  std::optional<sdo_result<od_value>> reading{};
  unsigned pass = 0; // the pass the reading came in, from 1
};

// Every watched object is of the watch category: its subcategory and name
// tell it.
std::string short_name(od_entry const& entry)
{
  return std::format("{}::{}", entry.object.subcategory, entry.object.name);
}

std::string reading_text(cell const& c, bool stale)
{
  if (!c.reading || stale) {
    return "? not read";
  }
  if (!*c.reading) {
    return std::format("? {}", to_string(c.reading->error()));
  }
  auto text = std::format("= {}", to_string(**c.reading));
  if (!c.entry->object.unit.empty()) {
    text += std::format(" {}", c.entry->object.unit);
  }
  return text;
}

} // namespace

asio::awaitable<int>
watch(transport& bus, node_options options, watch_options watching, console io)
{
  co_await asio::this_coro::throw_if_cancelled(false);
  if (options.dictionary.empty()) {
    std::print(io.err, "canopen: watch needs the node's dictionary (-d)\n");
    co_return 2;
  }
  if (watching.period <= std::chrono::milliseconds::zero()) {
    std::print(io.err, "canopen: the period must be above 0 ms\n");
    co_return 2;
  }
  client host{bus, host_options(options)};
  auto const node = add_device(host, options, io);
  if (!node) {
    co_return 2;
  }
  auto& service = node->watch;
  auto const objects = service.objects();
  if (objects.empty()) {
    std::print(io.err,
               "canopen: nothing to watch: the dictionary's watch category "
               "has no readable scalar\n");
    co_return 2;
  }
  if (!watching.objects.empty()) {
    for (auto const* entry : objects) {
      static_cast<void>(service.disable(entry->key));
    }
    for (auto const& text : watching.objects) {
      auto const target = find_object(text, options.dictionary);
      if (!target) {
        std::print(io.err, "canopen: {}\n", target.error());
        co_return 2;
      }
      if (!service.enable(target->key)) {
        std::print(io.err,
                   "canopen: {}: not watched: the watch polls the readable "
                   "scalars of the watch category\n",
                   label(*target));
        co_return 2;
      }
    }
  }

  // The watch reads the objects of a pass in the order of objects(): the
  // reading of the last one ends the pass, and so does one the transport
  // could not send.
  std::vector<cell> cells;
  std::size_t width = 0;
  for (auto const* entry : objects) {
    if (service.enabled(entry->key)) {
      cells.push_back({.entry = entry});
      width = std::max(width, short_name(*entry).size());
    }
  }
  auto const start = clock_type::now();
  auto last_drawn = start;
  unsigned pass = 1;
  bool finished = false;
  asio::steady_timer done{bus.get_executor(),
                          asio::steady_timer::time_point::max()};

  // `complete`: the pass has ended, and an object it did not read is shown
  // as not read rather than with an earlier pass's reading.
  auto const draw = [&](bool complete) {
    std::chrono::duration<double> const time = clock_type::now() - start;
    std::string text{watching.redraw ? clear_screen : ""};
    text += std::format("node {}, pass {}, {:.3f} s\n",
                        options.node.get(),
                        pass,
                        time.count());
    for (auto const& c : cells) {
      text += std::format("{:{}}  {}\n",
                          short_name(*c.entry),
                          width,
                          reading_text(c, complete && c.pass != pass));
    }
    if (!watching.redraw) {
      text += '\n';
    }
    io.out << text << std::flush;
    last_drawn = clock_type::now();
  };

  auto const readings = service.on_value([&](object_reading const& r) {
    auto const it = std::ranges::find(cells, r.entry, &cell::entry);
    if (finished || it == cells.end()) {
      return;
    }
    it->reading = r.value;
    it->pass = pass;
    bool const unsent = !r.value
                     && r.value.error().reason == sdo_error::kind::transport;
    if (it + 1 == cells.end() || unsent) {
      draw(true);
      ++pass;
      if (watching.count != 0 && pass > watching.count) {
        finished = true;
        done.cancel();
      }
    }
    else if (watching.redraw
             && clock_type::now() - last_drawn >= 2 * watching.period) {
      // A pass that runs over, a silent device's: a terminal shows how it
      // goes. A pass on time ends within a period of the last drawing.
      draw(false);
    }
  });

  service.set_period(watching.period);
  // Ends at the last pass to print, or at a stop.
  co_await done.async_wait(asio::as_tuple(asio::use_awaitable));
  service.disable();
  co_return 0;
}

} // namespace cannet::canopen::tool
