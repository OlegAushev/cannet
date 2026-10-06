# CANopen stack plan

Status: in progress — stages 1 (`cd2712d`), 0, 2, 3 and 4 done; stage 5
(the CLI `canopen`) under way: dictionaries as OD files done, the bus
commands next.
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
    od_file.hpp                              # stage 5, done
    transport.hpp raw_transport.hpp
    loopback.hpp                             # stage 0, done
    subscription.hpp event.hpp
    client.hpp remote_node.hpp setup_error.hpp sdo_error.hpp
    snapshot.hpp watch_snapshot.hpp          # stage 4, done
    detail/  hb_consumer emcy_consumer
             tpdo_consumer rpdo_producer     # stage 2, done
             sdo_client                      # stage 3, done
             client_op                       # stage 4, done
    service/ object_reading watch config     # stage 4, done
  src/ test/
  history/                                   # stage 4, done
  tool/main.cpp od_tools                     # stage 5: CLI `canopen`
  cmake/canopen_dictionary.cmake             # stage 5, done
```

`cannet::canopen-history` (stage 4, `history/`) is a separate, optional
target.

## Stages

| # | Content | Verified by | Status |
|---|---------|-------------|--------|
| 0 | Boost.Asio; `cannet::raw::async_socket`; `cansock dump` on async; `transport.hpp` with `raw_transport` and `loopback_transport` | `cansock dump` on vcan without manual polling; protocol tests run on the loopback bus | done |
| 1 | Wire data layer: `types.hpp`, `sdo.hpp`, `od.hpp`, `od_format.hpp`; Catch2 | codec and dictionary unit tests | done, `cd2712d` |
| 2 | `client`, `remote_node` and their services, events | exchange with an emulated device on the loopback bus; SYNC and heartbeat visible in `candump` on vcan | done |
| 3 | `sdo_client` on completion tokens: queue, timeout, cancellation, strings, restore default | SDO read/write/exec against a live device over vcan or a real bus; cancellation mid-request and mid-string against an emulated device on the loopback bus | done |
| 4 | `service::{watch, config}` with value events, a snapshot adapter for a GUI on its own thread, `cannet::canopen-history` | watch polling behaves when the device disappears | done |
| 5 | CLI `canopen`: dump, sdo read/write/exec, watch, nmt, od-verify | the acceptance scenario, entirely from a terminal | in progress: dictionaries as OD files done |
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

The dictionary used to be written twice, in the firmware and in the device
application on the host. Since stage 5 the firmware's table is its only
source: the application compiles in an OD file made from it (see
[CLI](#cli-stage-5)), and `od-verify` walks a dictionary over SDO and checks
readability, types and aborts against a live device.

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
- Everything of one client runs on the transport's executor, which is a
  strand when the `io_context` has several threads. Periodic work (SYNC,
  heartbeat, RPDOs, watch polling) runs on a `steady_timer` per producer;
  there is no `_run` loop polling futures.
- Bus error frames (`CAN_RAW_ERR_FILTER`) and interface state are not in the
  transport yet; see [Open questions](#open-questions).

## Client and remote nodes (stage 2, done)

In CiA 301 terms the host is the SDO client and each device an SDO server. The
host side has two kinds of objects:

- `client` — the host's own node on one bus: NMT master, SYNC producer, its
  own heartbeat, and the registry of remote nodes. One per bus.
- `remote_node` — one per device on that bus: the host's handle to the device,
  holding its node id, its dictionary view and the services addressed to it.

ucanopen calls the second `Server`; cannet does not. The object is made of
client-side services (`sdo_client`, `tpdo_consumer`, …), emblib's `server` is
the device's own stack, and python-canopen names the same aggregate
`RemoteNode`.

```cpp
struct client_options {
  node_id id;                                       // ucanopen used 127
  std::chrono::milliseconds heartbeat_period{1000}; // zero: not sent
  std::chrono::milliseconds sync_period{0};         // zero: not sent
};

class client {
public:
  client(transport& bus, client_options opt);
  void start(); // what the host transmits: SYNC, its heartbeat, RPDOs
  void stop();

  // Fails when the node id or the name is taken.
  std::expected<std::shared_ptr<remote_node>, setup_error>
  add_node(remote_node_options);
  std::shared_ptr<remote_node> find_node(std::string_view name) const;
  std::expected<void, setup_error> set_node_id(node_id);
  std::expected<void, setup_error> set_remote_node_id(std::string_view name,
                                                      node_id);
  void set_heartbeat_period(std::chrono::milliseconds);
  void set_sync_period(std::chrono::milliseconds);

  // Any thread; complete with std::expected<void, transport_error>.
  auto async_nmt(nmt_command, Token&&);          // every node
  auto async_nmt(node_id, nmt_command, Token&&); // one; ucanopen has no such call
};

class remote_node {
public:
  detail::sdo_client sdo;        // stage 3: one request in flight, a queue
  detail::hb_consumer heartbeat; // {alive, state}; events on every change
  detail::emcy_consumer emcy;    // events: every EMCY
  detail::tpdo_consumer tpdo;    // per TPDO: handler, timeout, on_timeout
  detail::rpdo_producer rpdo;    // per RPDO: provider, period; enable/disable
  service::watch watch;          // stage 4: polls the watch objects over SDO
  service::config config;        // stage 4: the parameters
};
```

The services are public subobjects, as in ucanopen, and the object aggregates
them the way emblib's `server` aggregates its `detail::*`. The client's own
producers are no types of their own: SYNC and the heartbeat go out through a
private periodic sender, NMT through `async_nmt()`.

- **Execution.** The client and its nodes run on the transport's executor and,
  like the transport, are not thread-safe. Only `get_executor()` and the
  initiating functions `async_*` may be called from any thread: they start on
  the client's executor themselves and complete on the handler's. A GUI on a
  thread of its own sends commands through `async_*`, changes settings with a
  `post()` and follows state through events.
- **What runs when.** `start()` and `stop()` govern only what the host
  transmits; reception and timeouts run from a node's registration on.
  Periodic frames go out at a fixed rate, skip the ticks missed while the
  executor was busy instead of sending them in a burst, and keep at most one
  frame per producer in the transport's queue.
- **Events.** `event<Args...>` takes any number of subscribers, each held by a
  `subscription` as in `transport.hpp`, with handlers on the client's
  executor. Emissions keep their order: one made from inside a handler waits
  until the current one has reached every handler. A TPDO keeps one handler:
  decoding belongs to the device application, which fans its values out
  itself.
- **Heartbeat.** The status tells liveness and the reported NMT state apart;
  ucanopen's `good()` meant alive *and* operational, so a node in
  pre-operational looked the same as a missing one. Before the first
  heartbeat the node is not alive and has no state. Every change is an event,
  and so is every boot-up, so a node that rebooted within the timeout is not
  missed. The timeout is the node's, 2000 ms by default as in ucanopen; zero
  never declares the node lost.
- **TPDOs.** `tpdo_config` is laid out as emblib's consumer config
  (`rpdo_config`): a handler, a timeout and `on_timeout`, which runs once per
  loss; the next frame recovers silently. Monitoring starts at setup, so a
  TPDO that never arrives times out too; ucanopen counted it as good for the
  first timeout. A frame shorter than the PDO's mapped length (`len`, 8 by
  default) is dropped, as CiA 301 has it; ucanopen padded it with zeros.
- **RPDOs.** `rpdo_config` is laid out as emblib's producer config
  (`tpdo_config`): a provider, called right before each send, and a period.
  Each RPDO and the node as a whole can be disabled; a disabled RPDO keeps its
  setup and its provider rests. RPDOs go out whether or not the node is alive:
  the adpt-etk-inverter firmware times out RPDO1 after 300 ms and latches a
  critical fault after 1 s without it. An interlock such as h2-hess's, which
  silences an RPDO while another master is heard, is the application's: it
  subscribes to that master's heartbeat and disables the RPDO.
- **Node ids.** A node id change runs on the client's executor, moves every
  service first and only then notifies: the heartbeat status resets, and every
  TPDO that had not timed out does so at once. ucanopen changed ids from the
  GUI thread without synchronization, kept the routing table's cleanup
  commented out and reported the old node as alive for up to 2 s.
- **Filters.** Every subscription to a COB-ID takes standard data frames only
  (`cob_filter()`); a mask of `CAN_SFF_MASK` alone would also pass remote
  frames and extended frames with the same low 11 bits.
- **SYNC** carries no counter, as emblib's `sync_producer` sends it, and is
  off by default: emblib's devices do not consume it. A zero period disables
  any producer, so `sync_enabled` is gone. The host's heartbeat reports
  operational.
- Setting up a PDO anew from inside its own handler or provider is safe.

Verified on the loopback bus against an emulated device that behaves as
emblib does in the firmware (it starts operational, keeps its heartbeat in
every state, sends TPDOs and takes RPDOs only while operational), and on
vcan, where `candump` shows SYNC, the heartbeat, NMT and RPDOs.

## SDO client (stage 3, done)

```cpp
// remote_node::sdo. Initiating functions, as on cannet::raw::async_socket:
// any completion token, asio::deferred by default, callable from any
// thread. Each completes with std::expected<T, sdo_error>, T on the right.
auto async_read(od_key, od_value_type, Token&&);  // od_value
auto async_write(od_key, od_value, Token&&);      // void
auto async_exec(od_key, Token&&);                 // void
auto async_read_string(od_key, Token&&);          // std::string
auto async_restore_default(od_key, Token&&);      // void
```

- One request in flight per node, with a queue behind it. A node has exactly
  one SDO channel, shared by the whole bus rather than per process, so it
  serves one SDO client. A second one — the CLI beside a running application —
  would take the first one's responses, told apart by index and subindex
  alone, and break its strings (below). `canopen dump` only listens.
- A worker coroutine per node serves the queue on the client's executor.
  Each operation is a composed operation: it starts on the client's executor
  whatever the caller's, and the worker or a cancellation completes it
  through the handler's own executor, so the token decides how the result
  comes back — awaited in a coroutine (the CLI, the tests, a Beast session on
  a strand of its own) or `use_future` on a GUI thread. The coroutines stay
  inside; an `asio::awaitable` in the signature would run on the caller's
  executor and touch the queue off the client's.
- The response timeout is the node's (`remote_node_options::sdo_timeout`,
  500 ms by default) and completes with `timeout`. A caller cancels through
  the operation's cancellation slot — a `cancellation_signal` emitted on the
  client's executor, or `cancel_after` — and gets `cancelled` at once; a
  queued request is then never sent, but one in flight keeps the channel
  until its response or its timeout: a late response must not pass for the
  next request's. A cancelled write may already have been applied;
  `cancelled` does not mean "not written". Answers are matched by index and
  subindex, and one for another key, a late answer to an earlier request, is
  ignored.
- A request in flight when the node changes its id completes with
  `cancelled`; queued ones go to the new id. Once the client is gone, every
  request fails with `transport` and `transport_error::closed`.
- `async_exec` runs a command, an exec object: a write whose bytes the device
  ignores, as emblib's `od_exec` and ucanopen both have it. Its answer carries
  no value, so the operation has none either.
- `async_write` announces the value's size, but the device takes the bytes
  as its own object's type: the value's type must be the object's. The
  client addresses objects by key and checks nothing against a dictionary;
  the config service of stage 4 does, and so will the CLI.
- `async_read_string` hides the device's string convention (4-byte expedited
  chunks up to a NUL) behind one operation, where ucanopen has a
  `StringReader` and a busy-wait in `get()`. It holds the channel until the
  NUL, even after its caller cancels: the device keeps one string cursor per
  SDO server (`text_cursor` in emblib's `sdo_server.hpp`). Reading another
  string object restarts the cursor, and an interrupted read leaves it
  mid-string, so the next read of that string would silently return only its
  tail. A read the client could not finish (a timeout, a failure past the
  first word) therefore marks the string, and its next read first runs the
  cursor to the NUL.
- `async_restore_default` is the client half of emblib's
  `sdo_server::write_restore_default`: an expedited write to `0x1011:04`
  whose data carries the target key (index little-endian, then subindex).

One error type instead of ucanopen's three statuses:

```cpp
struct sdo_error {
  enum class kind { aborted, timeout, cancelled, type_mismatch, malformed,
                    transport };
  kind reason;
  sdo_abort_code abort{};       // with aborted
  transport_error send_error{}; // with transport
};
```

`type_mismatch` means the device answered a read with another size than the
type read has: the dictionaries in the firmware and in the device
application have drifted apart, which `od-verify` (stage 5) is to report.
The config service reports it too for a value of another type than the
object's, which it does not send.
The earlier sketch's `not_found` and `access_denied` come from the device as
aborts (`object_not_found`, `read_from_write_only`, `write_to_read_only`)
and are reported as `aborted` with the code; `malformed` covers what the
client cannot use, a segmented answer among them.

Verified against an emulated device that serves SDO as emblib's
`sdo_server` does, on the loopback bus (queue, timeouts, late answers,
cancellation queued, in flight and mid-string, a string cut short, a node id
change, the client gone) and on vcan through the kernel; and on 2026-10-06
against a live ADPT-ETK Inverter (firmware `15a8617`, node 1) on a gs_usb
adapter at 500 kbit/s: strings, scalar reads, a write of a parameter's own
value, a command (`clear_errors`), `type_mismatch` and the abort codes, about
1–2 ms per exchange. Timeouts and cancellation were left to the emulated
device.

## Services (stage 4, done)

The services built on a node's dictionary are members of `remote_node`
beside the protocol's own, and report what they read as events:

```cpp
struct object_reading {
  od_entry const* entry;                      // the object read
  sdo_result<od_value> value;                 // its value, or why there is none
  std::chrono::steady_clock::time_point time; // when the read completed
};
```

An event carries no formatted text: formatting is presentation, its
precision the GUI's to choose, and a web daemon sends numbers;
`to_string(value, precision)` is one call.

- `service::watch` (done) polls the readable scalar objects of the
  dictionary's watch category over SDO, ordered by key. ucanopen fires these
  requests round-robin, with no timeout and no back-pressure. Here a
  coroutine walks the enabled objects and sends the next request after an
  answer or a timeout, so the node's SDO queue never holds more than one of
  its requests. The period is the interval between the starts of two passes;
  a pass that runs longer is followed by the next at once, and missed passes
  are not made up. As an RPDO, the watch sends nothing until it has a
  period, and it does not wait for `client::start()`: SDO works on a stopped
  client too. Objects are enabled one by one, and the watch as a whole can be
  disabled, which stops it at once and drops the read in flight, unreported;
  so is a read cut short by a node id change.

  When the device stops answering, every read times out, is reported as a
  timeout, and the pass goes on: the bus sees one request per SDO timeout,
  and another request for the node waits behind at most one of the watch's.
  The first answer after the device returns is reported as usual. A request
  the transport cannot send ends the pass until the next period: the other
  objects would fail as well, and the watch does not spin.
- A GUI on a thread of its own reads state through `snapshot<T>` (done), a
  triple buffer: the writer, on the client's executor, fills a buffer of its
  own and swaps it for the one in the middle, and the reader swaps its
  buffer for the middle one when a newer value waits there. Neither side
  ever waits for the other; with two buffers, the writer would wait for the
  reader to finish its frame. `watch_snapshot` is the watch's adapter,
  subscribed like any other consumer, beside history (`attach`) and the
  sessions of a web daemon: it keeps, per watched object, the last value,
  the error of the last read if it failed, and the time, and publishes the
  table after every reading. The application keeps its own state, such as
  the values it decodes from TPDOs, in a `snapshot<T>` of its own.
- `service::config` (done) covers the config category, whose objects
  `parameters()` lists by key. `async_read_all()` reads every readable one
  but strings, one request at a time, and completes with the readings, each
  also an event as it comes: a GUI's progress. An abort or a single timeout
  is the reading of its parameter, and the read goes on; three timeouts in a
  row end it with `timeout`, as in ucanopen's config service — the device is
  not there, and the rest would time out one by one — and so do a request
  the transport cannot send and a cancellation. `async_read()` and
  `async_write()` take a dictionary entry and type the request by it: a
  value of another type than the object's completes with `type_mismatch`
  and is not sent. `async_save_all()` and `async_restore_all_defaults()`
  write CiA 301's signatures, "save" to 1010h:01 and "load" to 1011h:01:
  emblib's devices ignore a command's bytes, and a CiA 301 device refuses a
  command without them; every ucan-monitor device that has these commands
  keeps them at these keys. `async_restore_default()` is the SDO client's
  1011h:04.

  Operations keep their callers' order, among themselves and among the SDO
  client's: a service already on the client's executor queues its SDO
  requests without the hop the SDO client's operations make first, so a
  write made before a restore reaches the device first. The services'
  operations and the SDO client's are one composed operation,
  `detail::client_op`.
- `cannet::canopen-history` (done) keeps signal history for plots inside
  cannet, so that each application does not rewrite it. It is a separate,
  optional target, in `canopen/history/`: only a GUI needs it, and a headless
  application or the CLI does not link it. A signal is a node's object, keyed
  by the node's name and the object's key rather than by a dictionary entry
  index, as first planned: the application addresses the values it decodes
  from TPDOs by the keys it knows, and one history serves several nodes on
  one time base. Values come from a node's watch (`attach`, a subscription to
  its events; a failed read is recorded as NaN, which a plot draws as a gap)
  and from TPDOs the application decodes (`push`, from any thread). Each
  signal is a `boost::circular_buffer_space_optimized`, whose memory grows
  with the samples: a plain ring takes its whole capacity at once, 96 MB for
  60 signals of 100 000 samples. The point type is cannet's own trivially
  copyable `sample{double t; double value;}` rather than `boost::geometry`'s
  (ImPlot reads it through a getter, into `double` anyway). `t` is in seconds
  since the history's origin, and a process may run for weeks: a `float` `t`
  would advance in 2 ms steps after 4.5 hours and in 62 ms steps after a
  week. A `double` `t` makes the struct 16 bytes whatever `value` is, so
  `value` is a `double` too and holds every `od_value` exactly, where a
  `float` rounds integers above 2^24. Changes against ucanopen's log service:
  the capacity belongs to the instance, not to a static; shrinking keeps the
  newest samples instead of clearing; drawing code takes an RAII `reader`,
  which holds the lock, instead of a public mutex, and gets a signal's
  samples as the ring holds them, in two contiguous runs.

  ```cpp
  struct history_options {
    std::size_t capacity{100'000}; // samples per signal
  };

  class history {
  public:
    explicit history(history_options options = {});
    clock::time_point origin() const;     // where t is zero
    void attach(remote_node&);            // on the client's executor
    void detach(remote_node const&);
    void push(std::string_view node, od_key, double value,
              clock::time_point = clock::now()); // any thread
    void set_capacity(std::size_t);       // shrinking keeps the newest
    void clear();

    class reader;                         // holds the lock while alive
    reader read() const;                  // find(node, key): sample_view
  };
  ```
- CSV logging stays in the application.

Verified on the loopback bus against the emulated device: the watch's
passes, a device that stops answering and comes back, a request the
transport cannot send, a node id change, the client gone, a handler that
drops the node; the snapshot read from a thread of its own; every config
operation, their order among the SDO client's, a cancellation, three
timeouts in a row; history from a node's watch and from another thread.
And on 2026-10-06 against the live ADPT-ETK Inverter (node 1, 500 kbit/s),
reads only: every parameter, 85 of 85, in 62 ms, 0.7 ms each; the watch
over 60 objects in 12 passes 250 ms apart, each 44–59 ms long, without a
failure; a read of every parameter beside the watch, interleaved with it,
85 of 85 in 121 ms; history's uptime samples 0.25 s apart, as the
device's own uptime. Earlier that day, with the inverter switched off,
the same run showed the services facing a silent device: the read of
every parameter gave up after three timeouts, the watch kept one request
in flight and reported each timeout, and history recorded NaN.

## CLI (stage 5)

`canopen` is to the protocol what `cansock` is to the transport: `dump`,
`sdo read|write|exec`, `watch`, `nmt`, `od-verify`. `watch` and `od-verify`
work from a device dictionary, and cannet ships none.

### Dictionaries as files (done)

The CLI cannot have a dictionary compiled in, so it reads one from an OD
file (`od_file.hpp`): text, one object per line, in the order of emblib's
rows, with the access and the type by their `name()`s, the stable
identifiers made for what a program reads.

```
cannet-od 1
watch_category  watch
config_category config
1008:00  info    sys         device_name  -  const  string
5000:01  watch   sys         uptime       s  ro     float32
3003:01  config  protection  uvp_dc       V  rw     float32
```

`parse_od_file()` gives a `loaded_dictionary`, whose view is a
`dictionary_view` like a compiled-in one's, and checks it with the same code
as `dictionary<N>`'s consteval constructor (`detail::check_dictionary`),
each error tied to its line; `to_od_file()` writes one, columns aligned. Not
JSON: a web page needs JSON as output, which a `dictionary_view` gives,
while lines are simpler to make, read and compare in a diff. Not EDS
(CiA 306): it knows no categories and no units, and may come later as an
export for third-party tools.

The firmware's table is the only source. `canopen od from-emblib` reads an
emblib firmware's `od.cpp`: emblib states each row's type, although the
binding determines it, "so that a text parser can generate a host's table",
and the access follows from the binding (`od_ro`, `param::rw`, `od_exec`).
The generator stays in cannet for now. A device application compiles the
file in with `cannet_canopen_dictionary(<target> FILE <file.od> NAME
<identifier>)`, which generates a header holding `inline constexpr auto
<identifier> = cannet::canopen::dictionary{...}` with `canopen od header`,
built for the purpose in a project that embeds cannet; the dictionary is
checked again when compiled. `od-verify` catches a file grown stale against
the firmware. The adpt-etk-inverter's table imports whole: 166 objects.

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

- `bsclog`, a hard dependency on the GUI's logger. An optional
  `std::function<void(level, std::string_view)>` in the client options can
  replace it once there is something to log beyond events; stage 2 has
  nothing.
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
| 2026-10-06 | The client runs on the transport's executor and is not thread-safe; only `get_executor()` and the `async_*` initiating functions may be called from any thread |
| 2026-10-06 | `start()`/`stop()` govern only what the host transmits; reception and timeouts run from a node's registration |
| 2026-10-06 | Events keep emission order: an emission from inside a handler waits for the current one to reach every handler |
| 2026-10-06 | A node's heartbeat reports liveness and NMT state apart; every boot-up is an event |
| 2026-10-06 | TPDOs are monitored from setup and time out once per loss; a frame shorter than the mapped length is dropped |
| 2026-10-06 | RPDOs are not gated on the node's liveness; interlocks belong to the application, built on events |
| 2026-10-06 | A node id change resets what the old node reported: the heartbeat status, the TPDO timeouts |
| 2026-10-06 | SYNC off by default and without a counter; a zero period disables a producer |
| 2026-10-06 | COB-ID subscriptions take standard data frames only (`cob_filter()`) |
| 2026-10-06 | A worker coroutine per node serves the SDO queue; each operation is a composed operation that the worker or a cancellation completes |
| 2026-10-06 | `async_exec` completes without a value: emblib's commands are writes whose bytes the device ignores |
| 2026-10-06 | `sdo_error` kinds: aborted (with the code), timeout, cancelled, type_mismatch (an answer's size against the type read), malformed, transport (with the `transport_error`) |
| 2026-10-06 | A string read cut short marks the string, and its next read first runs the device's cursor to the NUL |
| 2026-10-06 | An SDO request in flight during a node id change is cancelled, queued ones go to the new id; with the client gone, requests fail with `transport_error::closed` |
| 2026-10-06 | The SDO timeout is the node's, 500 ms by default |
| 2026-10-06 | The services built on a dictionary are members of `remote_node` and report reads as `object_reading` events: the value or the error, and the time; no formatted text |
| 2026-10-06 | The watch keeps one request in flight; its period separates the starts of passes, zero (the default) stops it; a timeout is reported and the pass goes on, a transport failure ends the pass; `disable()` drops the read in flight |
| 2026-10-06 | A GUI thread reads state through `snapshot<T>`, a triple buffer, so that neither side waits for the other; `watch_snapshot` keeps each watched object's last value, last error and time |
| 2026-10-06 | The config service types reads and writes by the dictionary and does not send a value of another type; storing and restoring all are CiA 301's 1010h:01 and 1011h:01 with "save" and "load"; reading all gives up after three timeouts in a row |
| 2026-10-06 | A service's operations keep their callers' order among the SDO client's: on the client's executor, a service queues its requests without a hop |
| 2026-10-06 | A history signal is a node's object, keyed by the node's name and the object's key; each is a `circular_buffer_space_optimized`; a failed watch read is recorded as NaN |
| 2026-10-06 | A dictionary outside the code is an OD file, a text format cannet defines: one object per line, the access and type by their `name()`s; it is read with `dictionary<N>`'s checks, its errors tied to lines |
| 2026-10-06 | The firmware's table is the dictionary's only source: `canopen od from-emblib` makes the OD file (the generator in cannet for now), `cannet_canopen_dictionary()` compiles it into the application through a generated header, `od-verify` catches a stale file |

## Open questions

1. **Bus error frames and interface state**: how they surface through
   `transport`. Today a failed receive is retried silently after a pause. No
   longer optional: a GUI in a browser sees the bus only through its daemon,
   so bus-off, error-passive and a downed interface must reach it as events.
   Needed by stage 6 if its GUI is a web page.
2. **A web layer in cannet**: the JSON mapping of the dictionary, values and
   errors and the RPC over SDO, NMT, config and watch are not device-specific.
   By the argument that put history in cannet, they are a candidate for an
   optional target over Boost.Beast and Boost.JSON. Revisit after the pilot,
   not before.
