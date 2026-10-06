#include <canopen/watch_snapshot.hpp>

#include <algorithm>
#include <functional>

namespace cannet::canopen {

namespace {

std::vector<watched_value> empty_table(service::watch const& watch)
{
  std::vector<watched_value> table;
  for (auto const* entry : watch.objects()) {
    table.push_back({.entry = entry});
  }
  return table;
}

} // namespace

watch_snapshot::watch_snapshot(service::watch& watch)
    : table_(empty_table(watch)), shared_(table_)
{
  readings_ = watch.on_value([this](object_reading const& r) {
    // The watch's objects, and so the table, are ordered by key.
    auto const it = std::ranges::lower_bound(
        table_,
        r.entry->key,
        std::less{},
        [](watched_value const& v) { return v.entry->key; });
    if (it == table_.end() || it->entry != r.entry) {
      return;
    }
    if (r.value) {
      it->value = *r.value;
      it->error.reset();
    }
    else {
      it->error = r.value.error();
    }
    it->time = r.time;
    shared_.publish(table_);
  });
}

std::span<watched_value const> watch_snapshot::read()
{
  return shared_.read();
}

} // namespace cannet::canopen
