# CANopen stack plan

Status: in progress — stages 1 (`cd2712d`) and 0 done; stage 2 (the client
and its services) and stage 3 (the SDO client) next.
Last updated: 2026-10-06.

A limited host-side CANopen stack for `cannet::canopen`: the host's half of
the protocol whose device half is emblib's `emb::can::canopen`. It replaces
ucanopen, the CANopen stack inside ucan-monitor, and is written in emblib's
style, adjusted for a host: heap allocation, `std::function`,
`std::vector`/`std::string` and Boost are fine here.

## Scope

"Limited" means what the device side implements, mirrored, and nothing more:

- Expedited SDO only. Every object the device side serves fits in 4 bytes;
  strings are read as a sequence of expedited responses ending in a NUL byte.
  No segmented or block transfer.
- The predefined connection set: every COB-ID follows from the node id.
- NMT master, SYNC producer, heartbeat producer and consumer, EMCY consumer.
- TPDOs and RPDOs whose layouts belong to the per-device application and reach
  cannet as data.

## Principles

- **Mirror of emblib.** Names and semantics follow the device side, so both
  ends of the wire can be read side by side. Roles invert, names do not:

  | emblib (device) | cannet (host)   | direction        |
  |-----------------|-----------------|------------------|
  | `nmt_slave`     | `nmt_master`    | host → device    |
  | `hb_producer`   | `hb_consumer`   | device → host    |
  | `hb_consumer`   | `hb_producer`   | host → device    |
  | `sync_producer` | `sync_producer` | host → bus       |
  | `sdo_server`    | `sdo_client`    | request/response |
  | `tpdo_producer` | `tpdo_consumer` | device → host    |
  | `rpdo_consumer` | `rpdo_producer` | host → device    |
  | `emcy_producer` | `emcy_consumer` | device → host    |

  PDO names stay the device's: a TPDO always travels device → host.
- **Constexpr for data, runtime for objects.** Dictionaries, COB-ID tables and
  payload casts are constexpr; the client and its services are runtime
  objects. emblib's NTTP `server<Opt>` is not mirrored: on the host the node
  id changes at runtime, from a GUI.
- **Nothing device-specific.** Dictionaries, PDO layouts and device state
  machines belong to the per-device application.
- **The application owns the executor.** cannet never owns an `io_context`.
- **Events first.** A service reports what happens as events, to any number
  of subscribers. Whoever needs current state builds it from them — a GUI
  snapshot, history, the sessions of a web daemon; a snapshot read on its own
  loses whatever happened between two reads.
- Failures go through `std::expected` with a per-module error enum, as
  everywhere in cannet. Beside `to_string()`, a phrase for people, every enum
  a program may branch on — the error enums first — has `name()`: a stable
  identifier such as `tx_queue_full`, so rewording a message breaks no client.

## Layout

```
canopen/
  include/canopen/
    types.hpp sdo.hpp od.hpp od_format.hpp   # stage 1, done
    transport.hpp raw_transport.hpp
    loopback.hpp                             # stage 0, done
    client.hpp remote_node.hpp               # stage 2
    detail/  nmt_master sync_producer hb_producer hb_consumer
             tpdo_consumer rpdo_producer emcy_consumer   # stage 2
             sdo_client                                  # stage 3
    service/ watch config                    # stage 4
  src/ test/
  tool/main.cpp                              # stage 5: CLI `canopen`
```

`cannet::canopen-history` (stage 4) is a separate, optional target.

## Stages

| # | Content | Verified by | Status |
|---|---------|-------------|--------|
| 0 | Boost.Asio; `cannet::raw::async_socket`; `cansock dump` on async; `transport.hpp` with `raw_transport` and `loopback_transport` | `cansock dump` on vcan without manual polling; protocol tests run on the loopback bus | done |
| 1 | Wire data layer: `types.hpp`, `sdo.hpp`, `od.hpp`, `od_format.hpp`; Catch2 | codec and dictionary unit tests | done, `cd2712d` |
| 2 | `client` and `detail/*` except `sdo_client` | exchange with an emulated device on the loopback bus; SYNC and heartbeat visible in `candump` on vcan | |
| 3 | `sdo_client` on completion tokens: queue, timeout, cancellation, strings, restore default | SDO read/write/exec against a live device over vcan or a real bus; cancellation mid-request and mid-string against an emulated device on the loopback bus | |
| 4 | `service::{watch, config}` with value events, a snapshot adapter for a GUI on its own thread, `cannet::canopen-history` | watch polling behaves when the device disappears | |
| 5 | CLI `canopen`: dump, sdo read/write/exec, watch, nmt, od-verify | the acceptance scenario, entirely from a terminal | |
| 6 | A pilot per-device application on top of cannet | one device moved off ucan-monitor | |

Stage 1 went first: the data layer needs no Asio. The transport interface and
the loopback bus, first planned for stage 1, moved to stage 0 because the
interface is asynchronous. Stages 2 and 3 can run in parallel once
`transport.hpp` is fixed.

## Data layer (stage 1, done)

- `types.hpp` — `node_id` (built only by `make()` from runtime input or
  `literal()` for constants, so an invalid id never reaches COB-ID
  arithmetic), the predefined connection set, NMT states and commands,
  `to_payload`/`from_payload` over `std::bit_cast`, `make_frame`.
- `sdo.hpp` — the expedited SDO structs, byte for byte as on the device;
  command specifiers; `sdo_abort_code` with messages; request encoders.
- `od.hpp` — `od_value` (a variant of the device's scalar types),
  `od_value_type`, `od_access` and `dictionary<N>`. Its consteval constructor
  rejects duplicate keys, duplicate names, empty names and a config naming a
  category no entry belongs to, sorts the entries by key and indexes them by
  name, so both lookups are binary searches over static storage, with no
  allocation. Declaration order is free: a table can be copied from the
  firmware line by line. `dictionary_view` erases `N`; the rest of the stack
  takes only the view.
- `od_format.hpp` — `to_string`/`parse` between `od_value` and the text a UI
  or CLI deals in.

In ucanopen the dictionary is two `std::map`s built at runtime, keyed by
`string_view`s into memory the maps do not own.

The dictionary is still written twice: in the firmware and in the device
application on the host. Stage 5's `od-verify` walks a dictionary over SDO and
checks readability, types and aborts against a live device.

## Transport and execution model (stage 0)

**Asio is Boost.Asio** (decided 2026-10-05). Web applications on top of cannet
may use Boost.Beast, which is built on Boost.Asio; with one Asio, cannet and a
Beast server share one `io_context` instead of two event loops bridged by
threads. cannet follows recent Boost releases and uses their features; older
distribution packages are not a target. Boost.Asio is header-only, so the
dependency is the Boost headers:
`cannet::cansocket` (for `async_socket`) and `cannet::canopen` (from
`transport.hpp` on) depend on them; `cannet::canup` does not.

- `cannet::raw::async_socket` takes an executor from outside, opens and binds
  the socket itself and is the sole owner of the fd. Its `async_receive` and
  `async_send` take any Asio completion token (a callback, `deferred` — the
  default — or `use_awaitable` in a coroutine, `use_future`, `cancel_after`)
  and complete with `std::expected<..., socket_error>`; a cancelled
  operation completes with `socket_error::cancelled`. Each operation owns the
  frame it moves, so closing or destroying the socket with operations
  outstanding is safe.
- The protocol does not know sockets, only a narrow transport interface
  (`transport.hpp`), like emblib's `emb::can::transport` but asynchronous. A
  virtual call costs nothing that matters on a host and buys testability, so
  this is an interface, not a concept (`asio` stands for `boost::asio` in all
  sketches):

  ```cpp
  class transport {
  public:
    using executor_type = asio::any_io_executor;
    using frame_handler = std::move_only_function<void(can_frame const&)>;
    using send_handler =
        std::move_only_function<void(std::expected<void, transport_error>)>;

    virtual executor_type get_executor() = 0;
    // Frames go out in call order; `done` never runs inside send().
    virtual void send(can_frame const& frame, send_handler done) = 0;
    // Delivers matching frames until the subscription ends.
    [[nodiscard]] virtual subscription subscribe(can_filter filter,
                                                 frame_handler on_frame) = 0;
  };
  ```

  A subscription carries its filter and ends when destroyed, so a node-id
  change simply replaces subscriptions; handlers may subscribe and
  unsubscribe while frames are dispatched. Send failures come as
  `transport_error` (`closed`, `send_failed`, `tx_queue_full`). A full TX
  queue goes back to the caller and is not retried: SYNC and heartbeat drop
  the frame, the SDO client decides for itself.
- `raw_transport` binds the interface to `cannet::raw::async_socket`. It lives
  in canopen, since a plane never reaches upward. The socket's kernel filters
  follow the live subscriptions; a failed receive is retried after a pause;
  `close()` completes queued sends with `closed` and keeps the subscriptions
  for the next `open()`.
- `loopback_bus` and `loopback_transport` are an in-memory bus: a frame from
  one endpoint reaches the subscribers of the others, as between sockets on
  vcan, and `fail_next_send()` injects failures. They are public, so device
  applications can test their own logic without an interface.
- Everything of one client runs on one strand. Periodic work (SYNC,
  heartbeat, RPDOs, watch polling) runs on a `steady_timer` per producer;
  there is no `_run` loop polling futures.
- Bus error frames (`CAN_RAW_ERR_FILTER`) and interface state are not in the
  transport yet; see [Open questions](#open-questions).

## Client and remote nodes (stage 2)

In CiA 301 terms the host is the SDO client and each device an SDO server. The
host side has two kinds of objects:

- `client` — the host's own node on one bus: NMT master, SYNC producer, its
  own heartbeat. One per bus.
- `remote_node` — one per device on that bus: the host's handle to the device,
  holding its node id, its dictionary view and the services addressed to it.

ucanopen calls the second `Server`; cannet does not. The object is made of
client-side services (`sdo_client`, `tpdo_consumer`, …), emblib's `server` is
the device's own stack, and python-canopen names the same aggregate
`RemoteNode`.

```cpp
struct client_options {
  node_id id;
  std::chrono::milliseconds sync_period{1000};
  bool sync_enabled{true};
  std::chrono::milliseconds heartbeat_period{1000};
};

class client {
public:
  client(transport& bus, client_options opt);
  void start();
  void stop();

  // Fails when the node id or the name is taken.
  std::expected<std::shared_ptr<remote_node>, setup_error>
  add_node(remote_node_options);
  std::expected<void, setup_error> set_node_id(node_id);
  std::expected<void, setup_error> set_remote_node_id(std::string_view name,
                                                      node_id);

  void nmt(nmt_command);          // broadcast
  void nmt(node_id, nmt_command); // addressed; ucanopen has no such call
};

class remote_node {
public:
  detail::sdo_client sdo;
  detail::tpdo_consumer tpdo;      // a handler and a timeout per TPDO
  detail::rpdo_producer rpdo;      // a provider and a period per RPDO
  detail::hb_consumer heartbeat;   // events: liveness, remote NMT state
  detail::emcy_consumer emcy;      // events: every EMCY
  service::watch watch;            // events: every polled value
  service::config config;
};
```

The services are public subobjects, as in ucanopen, and the object aggregates
them the way emblib's `server` aggregates its `detail::*`. Changing a node id
re-registers the filters and COB-IDs of every service through a `post` to the
strand. ucanopen does this with an `update_node_id()` per service and leaves
the cleanup of the client's routing table commented out.

Services report through events (see [Principles](#principles)): the heartbeat
consumer on a change of liveness or of the remote NMT state, the EMCY consumer
on every emergency, watch (stage 4) on every value. An event takes any number
of subscribers, each held by a `subscription` as in `transport.hpp`, with
handlers on the client's strand. A TPDO keeps one handler: decoding belongs to
the device application, which fans its values out itself.

## SDO client (stage 3)

```cpp
// Initiating functions, as on cannet::raw::async_socket: any completion
// token, asio::deferred by default. Each completes with
// std::expected<T, sdo_error>, T on the right.
auto async_read(od_key, od_value_type, Token&&);  // od_value
auto async_write(od_key, od_value, Token&&);      // void
auto async_exec(od_key, Token&&);                 // od_value
auto async_read_string(od_key, Token&&);          // std::string
auto async_restore_default(od_key, Token&&);      // void
```

- One request in flight per node, with a queue behind it. A node has exactly
  one SDO channel, shared by the whole bus rather than per process, so it
  serves one SDO client. A second one — the CLI beside a running application —
  would take the first one's responses, told apart by index and subindex
  alone, and break its strings (below). `canopen dump` only listens.
- Each operation `co_spawn`s its coroutine onto the client's strand, so it
  runs there whatever the caller's executor, and the token decides how the
  result comes back: awaited in a coroutine — the CLI, the tests, a Beast
  session on a strand of its own — or `use_future` on a GUI thread. The
  coroutines stay inside; an `asio::awaitable` in the signature would run on
  the caller's executor and touch the queue off the client's strand.
- The response timeout is the client's own (`cancel_after` on the wait for the
  response) and completes with `timeout`. A caller cancels through the
  operation's cancellation slot — a `cancellation_signal`, its own
  `cancel_after` — and gets `cancelled` at once, but the request keeps the
  channel until its response or its timeout: a late response must not pass
  for the next request's. A cancelled write may already have been applied;
  `cancelled` does not mean "not written".
- `async_read_string` hides the device's string convention (4-byte expedited
  chunks up to a NUL) behind one operation, where ucanopen has a
  `StringReader` and a busy-wait in `get()`. It holds the channel until the
  NUL, even after its caller cancels: the device keeps one string cursor per
  SDO server (`text_cursor` in emblib's `sdo_server.hpp`). Reading another
  string object restarts the cursor, and an interrupted read leaves it
  mid-string, so the next read of that string would silently return only its
  tail.
- `async_restore_default` is the client half of emblib's
  `sdo_server::write_restore_default`: an expedited write to `0x1011:04`
  whose data carries the target key (index little-endian, then subindex).

One error type instead of ucanopen's three statuses:

```cpp
struct sdo_error {
  enum class kind { not_found, access_denied, type_mismatch, malformed,
                    aborted, timeout, cancelled, transport };
  kind reason;
  sdo_abort_code abort{}; // meaningful when reason == aborted
};
```

## Services (stage 4)

- `service::watch` polls the dictionary's watch category over SDO. ucanopen
  fires these requests round-robin, with no timeout and no back-pressure.
  Here a coroutine walks the enabled objects and sends the next request after
  a response or a timeout; the period is the interval between full passes.
  Each value goes out as an event (`od_value` plus formatted text). A snapshot
  behind a double buffer, which a GUI on its own thread reads with no lock per
  frame, is an adapter subscribed like any other consumer, beside history
  (`attach`) and the sessions of a web daemon.
- `service::config` covers the config category: read all parameters, write
  one, save all, restore one to its default.
- `cannet::canopen-history` keeps signal history for plots inside cannet, so
  that each application does not rewrite it. It is a separate, optional
  target: only a GUI needs it, and a headless application or the CLI does not
  link it. It is built on `boost::circular_buffer`; the point type is cannet's
  own trivially copyable `sample{double t; double value;}` rather than
  `boost::geometry`'s (ImPlot reads it through a getter, into `double`
  anyway). `t` is in seconds, and a process may run for weeks: a `float` `t`
  would advance in 2 ms steps after 4.5 hours and in 62 ms steps after a week.
  A `double` `t` makes the struct 16 bytes whatever `value` is, so `value` is
  a `double` too and holds every `od_value` exactly, where a `float` rounds
  integers above 2^24. Changes against ucanopen's log service: the capacity
  belongs to the instance, not to a static; shrinking truncates instead of
  clearing; drawing code takes an RAII `reader` instead of a public mutex.
  Signals are keyed by dictionary entry index. Values come from a node's watch
  service (`attach`, a subscription to its events) and from TPDOs the
  application decodes (`push`).

  ```cpp
  struct history_options {
    std::size_t capacity{100'000}; // points per signal
  };

  class history {
  public:
    explicit history(history_options opt = {});
    void attach(remote_node&);
    void detach(remote_node&);
    void push(signal_id, double value);
    void push(signal_id, double value, std::chrono::steady_clock::time_point);
    void set_capacity(std::size_t); // truncates when shrinking
    void clear();

    class reader; // holds the lock while alive
    reader read() const;
  };
  ```
- CSV logging stays in the application.

## CLI (stage 5)

`canopen` is to the protocol what `cansock` is to the transport: `dump`,
`sdo read|write|exec`, `watch`, `nmt`, `od-verify`. `watch` and `od-verify`
work from a device dictionary, and cannet ships none; how the CLI gets one is
open.

## Pilot application (stage 6)

One device first; ucan-monitor is not migrated big-bang. Decide on Flatpak
before this stage: the Flatpak sandbox gives no CAP_NET_ADMIN, so interface
bring-up has to happen outside the application — systemd-networkd on a
Raspberry Pi, `canup` interactively on a PC.

A GUI written in TypeScript, in a browser, turns the application into a
daemon: Beast and cannet on one `io_context` and one thread — a bus carries at
most about 8000 full frames a second — and the page talking to it over HTTP
and a WebSocket. Flatpak then no longer concerns the GUI, and the daemon takes
on what a desktop GUI never had to:

- Anything open in the browser can reach a port on localhost, and a WebSocket
  is outside the same-origin policy, while what the daemon writes reaches real
  equipment. The daemon checks `Origin` on the upgrade and `Host` against DNS
  rebinding, and requires a token issued per launch; reached from another
  machine, it needs authentication and TLS.
- What may be written, and when, is the daemon's policy, not the page's: any
  client can bypass the page.
- The daemon holds no CAP_NET_ADMIN. Bring-up stays with systemd-networkd, or
  goes to a small privileged helper with a whitelist if the UI must offer it.
- A browser may freeze a background tab, so each session has a bounded send
  queue: telemetry keeps the latest value, RPC replies are never dropped.
  Telemetry goes out in batches, plot data as binary frames.
- The page gets the dictionary from the daemon as JSON and builds parameter
  tables, watch and the SDO console from it: no third copy of the dictionary
  in TypeScript. Every `od_value` fits a JS `number` exactly; NaN and ±Inf
  need a convention. Input travels as text through `parse()`, the same
  validation as the CLI's.

## Not carried over from ucanopen

- `bsclog`, a hard dependency on the GUI's logger; it becomes an optional
  `std::function<void(level, std::string_view)>` in the client options.
- `boost::geometry` points in the log service (replaced by `sample`).
- The `SdoSubscriber`/`TpdoPublisher` hierarchies with manual
  register/unregister.
- `std::promise`/`std::future` chains and busy-waits in `get()`.
- `ObjectDictionaryAux` and the runtime maps.
- TPDO "mapping" by strings inside the log service. Decoding TPDOs is the
  device application's job: it registers its handler and pushes values into
  history itself.
- The `Tester` class.

## Decisions

| Date | Decision |
|------|----------|
| 2026-07-16 | The executor comes from outside; SDO on coroutines; constexpr for data, runtime for objects; no NTTP client on the host |
| 2026-07-16 | Protocol unit tests against an in-memory bus; integration tests on vcan |
| 2026-07-16 | Bus error frames deferred to the CANopen work |
| 2026-09-03 | Signal history inside cannet, as the optional target `cannet::canopen-history` over `boost::circular_buffer` with its own `sample`; CSV logging stays in the application |
| 2026-09-03 | Catch2 v3: the system package, else a pinned FetchContent build |
| 2026-09-03 | The data layer before Asio (stage 1 before stage 0) |
| 2026-10-05 | Boost.Asio, not standalone asio: one Asio with Boost.Beast in web applications |
| 2026-10-05 | Boost: follow recent releases and use their features; older distribution packages are not a target |
| 2026-10-05 | The per-device object is `remote_node`, not `server` |
| 2026-10-05 | cannet calls `find_package(Boost 1.90 CONFIG REQUIRED)` and the top-level project supplies Boost (one Asio per process); a standalone build falls back to a pinned fetch of 1.90 |
| 2026-10-05 | Transport: a subscription carries its filter and ends on destruction; failures come as `transport_error`; a full TX queue is reported, not retried; the loopback bus is public |
| 2026-10-06 | Events first: a service reports to any number of subscribers, each held by a `subscription`; current state (a GUI snapshot, history, a web session) is built from events |
| 2026-10-06 | SDO operations are initiating functions on completion tokens, run on the client's strand through `co_spawn`; no separate future facade for a GUI |
| 2026-10-06 | A cancelled SDO request keeps the channel until its response or timeout; strings are read to the NUL even when cancelled; one SDO client per node on a bus |
| 2026-10-06 | `sample{double t; double value;}`: a `float` time fails a process that runs for weeks |
| 2026-10-06 | Every enum a program may branch on, the error enums first, has `name()`, a stable identifier, beside `to_string()` |

## Open questions

1. **Bus error frames and interface state**: how they surface through
   `transport`. Today a failed receive is retried silently after a pause. No
   longer optional: a GUI in a browser sees the bus only through its daemon,
   so bus-off, error-passive and a downed interface must reach it as events.
   Needed by stage 6 if its GUI is a web page.
2. **A dictionary for the CLI**: `watch` and `od-verify` need one (stage 5).
3. **A single source for the dictionary** (firmware and host): revisit once
   `od-verify` exists, if the duplication hurts.
4. **A web layer in cannet**: the JSON mapping of the dictionary, values and
   errors and the RPC over SDO, NMT, config and watch are not device-specific.
   By the argument that put history in cannet, they are a candidate for an
   optional target over Boost.Beast and Boost.JSON. Revisit after the pilot,
   not before.
