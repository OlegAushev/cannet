# cannet

A family of CAN networking libraries for Linux hosts (PC and Raspberry Pi),
built with g++ (C++23) and Ninja — native, no cross-compilation. CMake presets
(`debug`, `release`, `debug-asan`) hold the build trees under
`build/<presetName>`:

```
cmake --preset debug
cmake --build --preset debug
```

Each library is a subdirectory holding its own `CMakeLists.txt`, public
headers under `include/<lib>/`, sources under `src/`, and a CLI diagnostic
tool under `tool/` that is built only in a standalone build (`CANNET_STANDALONE`).

Warning and hardening policy lives in one `cannet_build_config` INTERFACE
target at the root and is attached **PRIVATE** to every cannet target: a
project embedding cannet keeps its own flags, and cannet's headers must
compile cleanly under them. `-Werror` (`CANNET_WERROR`) is on in a standalone
build only, so a newer compiler cannot break a consumer over a new warning.
The set is the fleet's (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wold-style-cast -Wuseless-cast -Wdouble-promotion
-Wsuggest-override`, …) plus host-only hardening: `-fstack-protector-strong`,
`_GLIBCXX_ASSERTIONS` in Debug, `_FORTIFY_SOURCE=3` in optimized builds, and
`CANNET_SANITIZE=ON` for an ASan+UBSan build.

cannet is a product in its own right: it is handed to customers whole, so it
contains nothing device-specific. Per-device applications live outside it and
depend on it.

## Planes

The libraries are layered by *plane* — what part of the stack they touch and
what privileges they need. A plane never reaches upward.

- `canup/` (`namespace cannet::canup`) — config plane: interface bring-up,
  bring-down and status, a thin wrapper over libsocketcan. `up()`/`down()`
  issue rtnetlink writes and require CAP_NET_ADMIN **in the calling process**;
  the library holds no privileges of its own and enforces no interface or
  bitrate whitelist — that is application policy.
- `cansocket/` (`namespace cannet::raw`) — transport plane: CAN frame I/O over
  an already-up interface, unprivileged. `cannet::raw::socket` is CAN_RAW
  today; ISO-TP and J1939 will be sibling types beside it, each in its own
  namespace, never modes of one class: they differ in bind addressing,
  setsockopt space and I/O unit. Every protocol client owns its own fd — the
  kernel fans frames out — so there is no userspace demultiplexer between
  protocols.
- `canopen/` (`namespace cannet::canopen`) — protocol plane over the
  transport, unprivileged. Routing by COB-ID *inside* one protocol is normal
  and belongs here. Present today: the wire data layer only — `types.hpp`
  (node ids, predefined connection set, NMT), `sdo.hpp` (expedited SDO
  codecs), `od.hpp` (the consteval-validated object dictionary and its
  values), `od_format.hpp` (text conversion for UI and CLI). Pure functions
  and constexpr data: no I/O, no state, no dependency on cansocket yet. The
  transport binding, the client and its services build on top.

  Names and semantics mirror the device-side stack
  (`emb::can::canopen` in adpt-etk-inverter's emblib) so both ends of the wire
  can be read side by side; roles are inverted, and PDO names stay the
  device's (TPDO is device -> host).

CMake target names are `cannet::<lib>` aliases (`cannet::canup`,
`cannet::cansocket`); the underlying target keeps the bare name.

Rules:

- Nothing device-specific enters cannet — no object dictionaries, no PDO
  layouts, no device state machines. Those belong to the per-device
  application, which supplies them to cannet as data.
- Public API reports failure through `std::expected` with a per-module error
  enum plus a `to_string()` for it; no exceptions cross an API boundary, and
  errors that are routine and retryable (e.g. a full TX queue) get their own
  enumerator rather than being folded into a generic failure.
- Every public header opens with a doc comment stating what the type is, which
  plane it belongs to, what privileges it needs, and its thread model.
- Public headers are `.hpp` and are included as `<lib/path/name.hpp>`, never
  by relative path.

## Collaboration style

- Work in steps: when asked a design question, answer with a concrete proposal
  and wait for explicit approval before editing. Apply edits without re-asking
  only when the user says to proceed.
- The user often edits/reformats files between turns and stages changes
  themselves — check `git status`/diff before editing and never revert their
  on-disk changes.
- cannet is a library with a carefully reviewed API: prefer minimal,
  well-argued changes; discuss trade-offs (dependencies to be shipped to
  customers, compile time, diagnostics) when proposing API evolution.

## Verification

After any substantive change, verify with:

- Full build: `cmake --build --preset debug` (add `-- -k 0` to list all
  failures). `-Werror` is on — a warning is a failure. WIP files may
  legitimately fail — report, don't "fix" someone else's work in progress.
- Unit tests: `ctest --preset debug` (Catch2, one CTest entry per TEST_CASE).
  Tests are standalone-build only (`CANNET_BUILD_TESTS`); Catch2 comes from
  the system if installed, otherwise `cmake/catch2.cmake` fetches a pinned
  version on the first configure.
- Compile-time contracts (the consteval dictionary validation, `node_id`
  literals) need *negative* checks too: small `g++ -std=c++23 -fsyntax-only`
  programs in the session scratchpad that must fail, one malformed case per
  `-DCASE=n`. A test that only compiles valid input proves nothing about
  validation.
- For anything touching buffers, lifetimes or arithmetic: also
  `cmake --build --preset release` — `-Wconversion` and `_FORTIFY_SOURCE` see
  things `-O0` does not — and, when a runtime path changed, the `debug-asan`
  preset (needs `sudo dnf install libasan libubsan`).
- IDE diagnostics: `clangd --check=<file>
  --compile-commands-dir=build/debug`. The IDE's clangd instead reads
  `build/compile_commands.json`, a copy of the active CMake Tools preset's
  database (`cmake.copyCompileCommands`); pin the preset dir on the CLI for
  reproducible checks. The root `.clangd` rewrites `-std=c++23` to
  `-std=c++2b` so clangd accepts the g++ database.
- Manual driving of the CLIs (`build/debug/cansocket/cansock`,
  `build/debug/canup/canup`)
  against a `vcan0` interface. Creating vcan and any `canup up/down` are
  privileged operations — never run them unless asked.

Formatting: `.clang-format` at the repo root is the fleet-wide style (2-space
indent, 80 columns, east const, Stroustrup braces). Run clang-format on files
you touch, not on the tree.

## Git

- Conventional commits with a scope, messages in English:
  `refactor(raw): ...`, `feat(canopen): ...`, `chore(deps): bump asio`.
- No Co-Authored-By trailers.
- Commit only when asked.
