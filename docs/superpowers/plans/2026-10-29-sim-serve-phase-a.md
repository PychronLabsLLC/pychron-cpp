# Sim Serve, Phase A (Line on the Wire) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `elctl sim serve` puts the simulated extraction line behind real TCP/UDP listeners, one per transport of an unchanged hardware config, and `elctl` / `pychron-ui` with `--sim-endpoints <file>` drive that line through their real `TcpTransport` / `UdpTransport`.

**Architecture:** `libs/transport` gains listeners (asio stays private) and the endpoints file with its client-side override in `make_transport`. `libs/sim` gains `WireSim` (a hook plus how one request is cut from a stream) and `WireServer` (listeners, one thread per connection, every connection of a transport on one emulator under one mutex). `libs/systems` gains `build_line_sim` (moved out of `ExtractionLine`) and `LineServer`, which builds the simulated line from a config and serves it; `elctl sim serve` prints it and waits, and phase C's `doctor --rehearse` will call the same `LineServer`.

**Tech Stack:** C++20, asio (inside `libs/transport` only), toml++, GoogleTest, Qt 6 (one `pychron-ui` flag).

**Spec:** `docs/superpowers/specs/2026-10-09-sim-serve-design.md`. Read it first; section numbers below are its. Phase A is its section 6, A: sections 4.1 to 4.6, `elctl sim serve` without `--spectrometer` and `--replay`.

## Global Constraints

- Serve is real time only. `WireServer::start()` refuses a clock that is not a `SteadyClock`; `--sim-endpoints` with `--sim`, or with `--sim-speed`, is refused.
- Its threads follow AGENTS.md "Time" all the same: `Clock::Participant` first, `Clock::Hold` before `std::thread`, done-flag under the owner's mutex then `join()`. Socket waits are real time on purpose and go on the list in `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` section 4.4 (Task 12).
- A request's framing comes from the driver's codec (the same constant or function the encoder uses), never a second copy in `libs/sim`.
- With an endpoints file given, a transport that is not `kind = "sim"` and not `link` and has no entry fails the load. No transport may reach its configured address during a rehearsal.
- Serve binds `127.0.0.1` unless `--bind` says otherwise; a non-loopback bind prints a warning (spec section 5, wording in Task 9).
- In-process sim unchanged: every existing test passes untouched; `SimSystem::hook_for_transport` returns what it did.
- Asio never appears in a public header. Qt never appears in `libs/` outside `libs/persistence` internals and `libs/earthbank_qt`.
- Errors are values (`pychron::Result<T>`). Each task one commit, Conventional Commits with the component as scope. Before each: `python3 tools/quality_check.py` (exit 0), `python3 tools/compiler_sweep.py --changed`, the target's own test binary filtered, then `ctest --preset dev-quick`.
- Tasks 2, 6, 8 and 9 add threads: run their tests under `-DPYCHRON_SANITIZE=thread` before commit. Whole branch under TSan and ASan/UBSan, and full `ctest --preset dev-ui`, before landing (Task 12).
- Read `libs/sim/AGENTS.md` before Tasks 5 and 6, `libs/devices/AGENTS.md` before Task 1.

## Departures from the spec (Task 12 writes them into it)

- `WireSim::request` is a `RequestFramer` (function `Bytes -> optional<size_t>`), not a `ReadSpec`: ProxR requests are 2 or 3 bytes decided by the opcode, and a MaxiGauge ENQ is one unterminated byte after a CRLF line. `framer(ReadSpec)` wraps the `ReadSpec` cases.
- `WireSim` gains `bool close_after_reply`: `pychron_valves` opens one connection per command and reads its reply until the peer closes (`ReadSpec::until_close()`).
- `LineSim::notes` are `LineSimNote{LogLevel, std::string}`, not `config::Diagnostic` (which has no level; the notes are what `ExtractionLine` logs today at info or warn).
- The protocol enum is `WireProtocol` (`libs/transport` `endpoints.hpp`), shared by the server and the endpoints file.
- `elctl sim serve` takes `--canvas <file>` (default: the install's canvas, else `canvas.toml` beside the config if present): the simulated gas needs the canvas, as `ExtractionLine::load` does.
- Serving logic lives in `libs/systems` `LineServer`, so phase C's doctor reuses it.

## Review Focus

1. A client that drops its connection mid-request (half a frame sent): that connection's thread ends, other connections of the same transport keep answering. Test in Task 6.
2. Serve stopped (or crashed) while a client is connected: the client's next call fails `Io` promptly (not after its whole timeout), so its `Link` reconnects. Test in Task 6 (`StopClosesConnections`).
3. A stale endpoints file (serve not running): client `start()` fails `Io` naming the transport. Test in Task 11.
4. Endpoints file whose `config` names the same file by another spelling (relative path, symlinked folder): accepted (canonical compare). Test in Task 3.
5. `pychron_valves` one-connection-per-command: each command is answered and the connection closed, many times in a row. Test in Task 6 (`CloseAfterReply`) and in Task 11 through the real driver.

---

## File Structure

| File | Responsibility |
|---|---|
| `libs/codecs/include/pychron/codecs/proxr.hpp`, `src/proxr.cpp` | `request_length(const Bytes&)`. |
| `libs/codecs/include/pychron/codecs/maxigauge.hpp`, `src/maxigauge.cpp` | `request_length(const Bytes&)`. |
| `libs/codecs/include/pychron/codecs/thermo_qtegra.hpp`, `src/thermo_qtegra.cpp` | `request_spec()`. |
| `libs/transport/include/pychron/transport/listener.hpp`, `src/listener.cpp` (new) | `TcpListener`, `UdpListener`, `ByteStream`. |
| `libs/transport/include/pychron/transport/endpoints.hpp`, `src/endpoints.cpp` (new) | `WireProtocol`, `Endpoint`, `SimEndpoints`, read / write / load-for-config. |
| `libs/transport/include/pychron/transport/factory.hpp`, `src/factory.cpp` | `TransportContext::endpoints` override. |
| `libs/sim/include/pychron/sim/wire_sim.hpp` (new), `sim_system.hpp`, `src/sim_system.cpp` | `WireSim`, `RequestFramer`, `SimSystem::wire_for_transport`. |
| `libs/sim/include/pychron/sim/wire_server.hpp`, `src/wire_server.cpp` (new) | `WireServer`. |
| `libs/systems/include/pychron/systems/line_sim.hpp`, `src/line_sim.cpp` (new) | `build_line_sim`, `topology_of`, sim file resolution. |
| `libs/systems/src/extraction_line.cpp`, `include/.../extraction_line.hpp` | Calls `build_line_sim`; `sim_endpoints` options. |
| `libs/systems/include/pychron/systems/spectrometer/assembler.hpp`, `src/spectrometer/assembler.cpp` | Endpoints passed to `default_transport`. |
| `libs/systems/include/pychron/systems/line_server.hpp`, `src/line_server.cpp` (new) | `LineServer`. |
| `apps/elctl/src/sim_serve.cpp`, `sim_serve.hpp` (new); `cli.cpp`, `exp.cpp`, `laser.cpp`, `line.cpp`, `line.hpp` | Command, globals, plumbing. |
| `apps/pychron-ui/src/command_line.{hpp,cpp}`, `main.cpp` | `--sim-endpoints`, `--sim-endpoints-any`. |
| Tests: `tests/codecs/test_{proxr,maxigauge,thermo_qtegra}.cpp`, `tests/transport/test_listener.cpp` (new), `tests/transport/test_endpoints.cpp` (new), `tests/transport/test_factory.cpp`, `tests/sim/test_wire_sim.cpp` (new), `tests/sim/test_wire_server.cpp` (new), `tests/systems/test_line_sim.cpp` (new), `tests/integration/test_sim_serve.cpp` (new), `apps/elctl/tests/test_sim_serve_cmd.cpp` (new), `tests/ui/test_command_line.cpp` | |
| `docs/simulator.md`, `docs/installation_runbook.md`, `AGENTS.md`, the spec, the virtual clock spec | Task 12. |

Request framing by kind (from the codecs; Task 5 wires it):

| Kind | Framer |
|---|---|
| `proxr_relay` | `codec::proxr::request_length` |
| `agilent_switch` | `ReadSpec::until(codec::agilent::kTerminator)` |
| `qtegra_valves`, `qtegra_gauges` | `codec::thermo_qtegra::request_spec()` (= `until_any("\r\n")`: driver's `terminator` option may be cr, lf or crlf) |
| `pychron_valves` | `ReadSpec::until(codec::pychron_tx::kTerminator)`, `close_after_reply = true` |
| `ngx_valves` | `ReadSpec::until(<driver's send_terminator option, else codec::isotopx_ngx::kDefaultSendTerminator>)` |
| `pfeiffer_maxigauge` | `codec::maxigauge::request_length` |
| `varian_xgs600` | `ReadSpec::until(codec::varian_xgs600::kTerminator)` |
| `gp_microion` | `ReadSpec::until(codec::microion::kTerminator)` |
| `plc2000_*` (alone or bus) | `ReadSpec::modbus_tcp()` |
| `lakeshore` | `ReadSpec::until(codec::lakeshore::kTerminator)` |
| `chromium` | `ReadSpec::until(codec::chromium::kCommandTerminator)` |
| anything else | `ReadSpec::until_any("\r\n")`, `emulator = ""` (silent wire) |

---

### Task 1: Request framing in the codecs

**Files:**
- Modify: `libs/codecs/include/pychron/codecs/proxr.hpp`, `libs/codecs/src/proxr.cpp`
- Modify: `libs/codecs/include/pychron/codecs/maxigauge.hpp`, `libs/codecs/src/maxigauge.cpp`
- Modify: `libs/codecs/include/pychron/codecs/thermo_qtegra.hpp`, `libs/codecs/src/thermo_qtegra.cpp`
- Test: `tests/codecs/test_proxr.cpp`, `test_maxigauge.cpp`, `test_thermo_qtegra.cpp` (whichever exist under those names; else the codec's test file)

**Interfaces:**
- Produces: `std::optional<std::size_t> codec::proxr::request_length(const Bytes& buffer)`; `std::optional<std::size_t> codec::maxigauge::request_length(const Bytes& buffer)`; `const ReadSpec& codec::thermo_qtegra::request_spec() noexcept`. Each: length of the whole first request at the start of `buffer`, nullopt while incomplete.

- [ ] **Step 1: Failing tests**

```cpp
TEST(ProxrCodec, RequestLengthFromOpcode) {
  using codec::proxr::request_length;
  EXPECT_EQ(request_length(Bytes{}), std::nullopt);
  EXPECT_EQ(request_length(Bytes{0xFE}), std::nullopt);
  EXPECT_EQ(request_length(Bytes{0xFE, 0x08}), 2U);              // relay 0 on
  EXPECT_EQ(request_length(Bytes{0xFE, 0x10, 0xFE}), 2U);        // status, next request follows
  EXPECT_EQ(request_length(Bytes{0xFE, 0x31}), std::nullopt);    // select bank needs its bank byte
  EXPECT_EQ(request_length(Bytes{0xFE, 0x31, 0x02}), 3U);
  // Each builder's own bytes frame as one request.
  EXPECT_EQ(request_length(codec::proxr::select_bank(2)->tx), 3U);
  EXPECT_EQ(request_length(codec::proxr::relay_on(3)->tx), 2U);
}
TEST(ProxrCodec, RequestLengthSkipsToPrefix) {
  // Garbage before a prefix is one request of its own length up to the
  // prefix, so a server answers nothing to it and resyncs.
  EXPECT_EQ(codec::proxr::request_length(Bytes{0x00, 0x01, 0xFE, 0x08}), 2U);
}
TEST(MaxiGaugeCodec, RequestLengthLineOrEnq) {
  using codec::maxigauge::request_length;
  EXPECT_EQ(request_length(to_bytes("PR1\r")), std::nullopt);
  EXPECT_EQ(request_length(to_bytes("PR1\r\n")), 5U);
  EXPECT_EQ(request_length(Bytes{codec::maxigauge::kEnq}), 1U);
  EXPECT_EQ(request_length(Bytes{codec::maxigauge::kEnq, 'P'}), 1U);
}
TEST(ThermoQtegraCodec, RequestSpecFramesEveryTerminatorOption) {
  const auto& spec = codec::thermo_qtegra::request_spec();
  EXPECT_EQ(frame_length(spec, to_bytes("Open V1\r")), 8U);
  EXPECT_EQ(frame_length(spec, to_bytes("Open V1\n")), 8U);
  EXPECT_EQ(frame_length(spec, to_bytes("Open V1\r\n")), 8U);
}
```

- [ ] **Step 2: Run, expect FAIL** — `cmake --build --preset dev --target pychron_codecs_tests && build/dev/tests/codecs/pychron_codecs_tests --gtest_filter='*RequestLength*:*RequestSpec*'`: compile error, functions missing.

- [ ] **Step 3: Implement.** ProxR: from `kPrefix` and `kSelectBank` (proxr.hpp:25-29): buffer[0] not `kPrefix` → length up to the first `kPrefix` (or whole buffer when none); opcode `kSelectBank` → 3, else 2. MaxiGauge: first byte `kEnq` → 1, else `frame_length(ReadSpec::until(kTerminator), buffer)`. Qtegra: `request_spec()` returns the same object as `reply_spec()` with a comment saying requests and replies share the framing.

- [ ] **Step 4: Run, expect PASS.**

- [ ] **Step 5: Commit** — `feat(codecs): request framing for a server cutting requests from a stream`

---

### Task 2: TCP and UDP listeners

**Files:**
- Create: `libs/transport/include/pychron/transport/listener.hpp`, `libs/transport/src/listener.cpp`
- Modify: `libs/transport/CMakeLists.txt`, `tests/transport/CMakeLists.txt`
- Test: `tests/transport/test_listener.cpp`

**Interfaces:**
- Produces:

```cpp
namespace pychron {
// One accepted TCP connection. Blocking calls, real time; one thread uses it,
// except close(), which any thread may call and which ends a waiting read.
class ByteStream {
 public:
  virtual ~ByteStream() = default;
  // Bytes now available; empty after `timeout` with nothing. Io when the
  // peer closed or the stream was closed.
  virtual Result<Bytes> read_some(Duration timeout) = 0;
  virtual Result<void> write(const Bytes& bytes, Duration timeout) = 0;
  virtual void close() = 0;
};
class TcpListener {
 public:
  // port 0: the OS chooses. Io when the address cannot be bound (names it).
  static Result<std::unique_ptr<TcpListener>> bind(const std::string& address, std::uint16_t port);
  ~TcpListener();
  std::uint16_t port() const noexcept;
  // Timeout error after `timeout` with no connection; Cancelled once close()d.
  Result<std::unique_ptr<ByteStream>> accept(Duration timeout);
  void close();  // thread-safe; a waiting accept returns within 100 ms
};
class UdpListener {
 public:
  struct Peer;  // opaque sender address
  struct Datagram { Bytes data; std::shared_ptr<const Peer> from; };
  static Result<std::unique_ptr<UdpListener>> bind(const std::string& address, std::uint16_t port);
  ~UdpListener();
  std::uint16_t port() const noexcept;
  Result<Datagram> receive(Duration timeout);  // Timeout when none; Cancelled once close()d
  Result<void> send_to(const Peer& to, const Bytes& bytes);
  void close();
};
}
```

- [ ] **Step 1: Failing tests** (`ListenerSteady`, real loopback, bounds ≥ 5 s):
  - `TcpBindsPortZeroAndReportsPort`: `port() != 0`; a `TcpTransport` to `127.0.0.1:port()` opens.
  - `TcpAcceptedStreamEchoes`: accept, `read_some` gets what the client wrote, `write` reaches the client's `exchange`.
  - `TcpReadSomeTimesOutEmpty`: no data → empty `Bytes` after ~50 ms timeout, not an error.
  - `TcpPeerCloseIsIo`: client closes → `read_some` returns `ErrorKind::Io`.
  - `TcpCloseWhileAcceptWaits`: thread in `accept(10s)`; `close()` from test thread; accept returns `ErrorKind::Cancelled` in under 5 s.
  - `TcpBindTakenPortFails`: second bind of the same port → `ErrorKind::Io`, message contains the port.
  - `UdpReceivesAndRepliesToSender`: `UdpTransport` exchange against `receive` + `send_to(*d.from, ...)`.

- [ ] **Step 2: Run, expect FAIL** (`pychron_transport_tests --gtest_filter='ListenerSteady.*'`, no header).

- [ ] **Step 3: Implement** in `listener.cpp` with asio, one `io_context` per listener and per stream, waits via `detail::run_with_timeout` (`asio_stream.hpp`). `close()` sets an atomic flag; `accept`/`receive` run in slices of at most 50 ms and check it. `ByteStream::close()` from another thread: same flag, `read_some` slices.

- [ ] **Step 4: Run, expect PASS**; then under TSan.

- [ ] **Step 5: Commit** — `feat(transport): TCP and UDP listeners`

---

### Task 3: Endpoints file

**Files:**
- Create: `libs/transport/include/pychron/transport/endpoints.hpp`, `libs/transport/src/endpoints.cpp`
- Test: `tests/transport/test_endpoints.cpp`

**Interfaces:**
- Produces:

```cpp
namespace pychron {
enum class WireProtocol { Tcp, Udp };
struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
  WireProtocol protocol = WireProtocol::Tcp;
  std::string emulator;  // driver kind answered as; "" = silent wire; "replay" later
};
struct SimEndpoints {
  std::filesystem::path config;  // absolute, the served line's file
  std::string host;              // what clients connect to
  std::map<std::string, Endpoint> transports;  // by transport name; host = `host`
};
Result<SimEndpoints> read_sim_endpoints(const std::filesystem::path& file);
Result<void> write_sim_endpoints(const std::filesystem::path& file, const SimEndpoints& endpoints);
// read_sim_endpoints, then refuses (Config) a file whose `config` is not
// `loaded_config` compared as canonical paths, unless `any`. Message names both paths
// and says --sim-endpoints-any skips the check.
Result<SimEndpoints> load_sim_endpoints(const std::filesystem::path& file,
                                        const std::filesystem::path& loaded_config, bool any);
// "<file>" beside the line's file: sim-endpoints.toml
std::filesystem::path default_sim_endpoints_path(const std::filesystem::path& line_file);
}
```

File format exactly spec section 4.5 (`config`, `host`, `[transports.<name>] protocol = "tcp"|"udp"`, `port`, `emulator`).

- [ ] **Step 1: Failing tests**:
  - `RoundTrips`: write then read gives equal `SimEndpoints` (each entry's `host` filled from top-level `host`).
  - `UnknownProtocolIsConfigError`, `PortOutOfRangeIsConfigError` (0 or > 65535), `MissingFileIsConfigError` (names the file).
  - `RefusesOtherConfig`: `load_sim_endpoints(f, other, false)` → Config, message contains `--sim-endpoints-any`; with `any = true` → ok.
  - `SameConfigBySymlinkAccepted` (Review Focus 4): config in `dir/real/line.toml`, symlink `dir/link → dir/real`, file says `dir/link/line.toml`, loaded `real/line.toml` relative to cwd → ok. Skip the symlink half with `GTEST_SKIP()` only where `create_directory_symlink` fails (Windows without privilege).

- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** with toml++ (as `trace_settings` / `libs/core` config readers do). Write through a temporary file and rename, so a client never reads half a file.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** — `feat(transport): sim endpoints file`

---

### Task 4: `make_transport` follows the endpoints

**Files:**
- Modify: `libs/transport/include/pychron/transport/factory.hpp`, `libs/transport/src/factory.cpp`
- Test: `tests/transport/test_factory.cpp`

**Interfaces:**
- Consumes: `Endpoint`, `WireProtocol` (Task 3).
- Produces: `TransportContext::endpoints` — `const std::map<std::string, Endpoint>* endpoints = nullptr;` When set and the config's kind is not `Sim` or `Link`: the entry for `config.name` builds a `TcpTransport` (`WireProtocol::Tcp`) or `UdpTransport` (`Udp`) to its host and port, whatever the config's kind and params (tcp, udp, serial, modbus_tcp, modbus_rtu); no entry → `ErrorKind::Config`, message `transport '<name>' has no entry in the sim endpoints file`. Trace and wire log wrap as before.

- [ ] **Step 1: Failing tests** (`TransportFactory` suite):
  - `EndpointOverridesTcp`: tcp config to `192.0.2.1:1`, entry `127.0.0.1:<listener port>` → result opens against a `TcpListener` (Task 2).
  - `SerialBecomesTcp`: serial config + Tcp entry → opens against the listener.
  - `UdpEntryGivesUdp`: modbus_tcp config with a Udp entry → exchange reaches a `UdpListener`.
  - `MissingEntryIsConfigError`: message as above.
  - `SimAndLinkIgnoreEndpoints`: sim config with an empty map → still a `SimTransport` (hook answers); link likewise builds.

- [ ] **Step 2–4:** fail, implement (lookup before the `switch`), pass.
- [ ] **Step 5: Commit** — `feat(transport): transports follow a sim endpoints file`

---

### Task 5: `WireSim` and `SimSystem::wire_for_transport`

Read `libs/sim/AGENTS.md` first.

**Files:**
- Create: `libs/sim/include/pychron/sim/wire_sim.hpp`
- Modify: `libs/sim/include/pychron/sim/sim_system.hpp`, `libs/sim/src/sim_system.cpp`
- Test: `tests/sim/test_wire_sim.cpp`

**Interfaces:**
- Consumes: Task 1's framers.
- Produces:

```cpp
namespace pychron::sim {
// Length of the first whole request at the start of a buffer; nullopt while incomplete.
using RequestFramer = std::function<std::optional<std::size_t>(const Bytes&)>;
RequestFramer framer(ReadSpec spec);  // frame_length(spec, buffer)
struct WireSim {
  SimTransport::Hook hook;                // one request -> its reply (empty: none)
  SimTransport::Unsolicited unsolicited;  // may be empty
  RequestFramer request;                  // never empty
  std::function<void()> on_connect;       // may be empty
  bool close_after_reply = false;         // pychron_valves: one connection per command
  std::string emulator;                   // driver kind(s) answered as; "" = silent wire
};
// SimSystem:
WireSim wire_for_transport(std::string_view transport, const config::SystemConfig& system);
}
```

`hook_for_transport` becomes `return wire_for_transport(transport, system).hook;` (an empty hook for a transport with no drivers stays empty). `emulator`: the driver's kind when it is one of spec section 2's kinds; for a PLC bus, the distinct kinds of its drivers in name order joined with `+`; `""` for a silent wire and for a transport with no drivers. Framer per kind: table above.

- [ ] **Step 1: Failing tests** (`WireSim` suite, `ManualClock`, example configs via `PYCHRON_EXAMPLE_CONFIGS_DIR` or small in-test `SystemConfig`s):
  - `EveryEmulatedKindNamesItself`: for each kind of the table, a one-driver config gives `emulator == kind` and a non-empty hook.
  - `UnknownKindIsSilentWire`: kind `adc_bank` → `emulator == ""`, hook returns empty.
  - `FramerMatchesDriverBytes`: for proxr (`FE 31 02 FE 08` → 3, then 2), maxigauge (`PR1\r\n` then ENQ), modbus (12-byte read), lakeshore (`KRDG? A\n`), the framer cuts the driver's own encoded commands (build them with the codec functions, not literals).
  - `PychronValvesClosesAfterReply`: `close_after_reply == true` for pychron_valves only.
  - `PlcBusJoinsKinds`: plc2000_valves + plc2000_gauges on one transport → `emulator == "plc2000_gauges+plc2000_valves"` (name order of the drivers decides; assert the order your fixture gives).
  - `NgxUsesDriversSendTerminator`: option `send_terminator = "\n"` → framer cuts on LF.
- [ ] **Step 2–4:** fail, implement, pass; existing `pychron_sim_tests` all pass unchanged.
- [ ] **Step 5: Commit** — `feat(sim): a wire form of every emulated transport`

---

### Task 6: `WireServer`

**Files:**
- Create: `libs/sim/include/pychron/sim/wire_server.hpp`, `libs/sim/src/wire_server.cpp`
- Modify: `libs/sim/CMakeLists.txt`, `tests/sim/CMakeLists.txt` (link asio-free: only `pychron::transport`)
- Test: `tests/sim/test_wire_server.cpp`

**Interfaces:**
- Consumes: `TcpListener`, `UdpListener`, `ByteStream` (Task 2); `WireProtocol`, `Endpoint` (Task 3); `WireSim` (Task 5).
- Produces:

```cpp
namespace pychron::sim {
class WireServer {
 public:
  struct Options {
    std::string bind = "127.0.0.1";
    std::uint16_t port_base = 0;  // 0: OS-assigned; else port_base + i, served transports in byte order of name
    const Clock* clock = nullptr; // must be a SteadyClock (or null: one of its own)
  };
  explicit WireServer(Options options);
  ~WireServer();  // stop()
  Result<void> add(std::string transport, WireProtocol protocol, WireSim sim);  // Config on a name added twice
  // All-or-nothing: on any bind failure every listener is closed and the
  // error names the transport and port. Config for a non-steady clock:
  // "sim serve runs in real time only". Keys: transport names; host = Options::bind.
  Result<std::map<std::string, Endpoint>> start();
  void stop();  // closes listeners and every connection, joins every thread
};
}
```

Behaviour (spec 4.4): per TCP listener one accept thread; per connection one thread: `on_connect` once, then loop `read_some(20 ms)` → append to pending → while `request(pending)` gives n: cut n bytes, call hook under the transport's mutex, write a non-empty reply, close if `close_after_reply`; after each read, poll `unsolicited` under the same mutex and write what it gives. Per UDP listener one thread: each datagram is one request, reply `send_to` the sender. Every connection and datagram of one transport shares one `std::mutex` around hook and unsolicited (the emulators were written for one caller). The mutex is never held across a socket wait.

- [ ] **Step 1: Failing tests** (`WireServerSteady`, real loopback, `SteadyClock`, OS ports, every bound ≥ 5 s; drive with a raw `TcpTransport`/`UdpTransport` and a test `WireSim` whose hook echoes `"<request>!"` and counts calls):
  - `SplitRequestAnswersOnce`: write `"ab"`, then `"c\n"` → one reply `"abc\n!"`, hook called once.
  - `TwoRequestsOneSegmentAnswerInOrder`: `"a\nb\n"` → replies `a`, `b` in order.
  - `UdpDatagramIsOneRequest`.
  - `UnsolicitedArrivesWithoutRequest`: unsolicited yields `"evt\n"` once → client reads it with no write.
  - `TwoConnectionsShareModel`: hook increments a counter in a shared object; two connections each write → counter 2, second sees first's state.
  - `StartAllOrNothingWhenPortTaken`: hold a `TcpListener` on port P; server with transports `a`, `b`, `port_base = P` → `start()` fails `Io` naming `a` and P; then `b`'s port `P+1` refuses a connection.
  - `PortBaseAssignsInNameOrder`: find two free consecutive ports by binding and closing listeners (retry a few times), `port_base` = first → `b` gets base+1, `a` base.
  - `RefusesVirtualClock`: `Options::clock` a `VirtualClock` → Config, "real time only".
  - `HalfRequestThenDisconnect` (Review Focus 1): connection 1 writes `"ab"` and closes; connection 2 still answered.
  - `StopClosesConnections` (Review Focus 2): connected client in `exchange` with 10 s timeout; `stop()` → client fails `Io` in under 5 s.
  - `CloseAfterReply` (Review Focus 5): `close_after_reply` sim; 20 times: open, exchange with `ReadSpec::until_close()`, reply complete.
- [ ] **Step 2–4:** fail, implement (threads per AGENTS.md "Time"), pass; then TSan.
- [ ] **Step 5: Commit** — `feat(sim): WireServer serves emulators on sockets`

---

### Task 7: `build_line_sim` out of `ExtractionLine`

**Files:**
- Create: `libs/systems/include/pychron/systems/line_sim.hpp`, `libs/systems/src/line_sim.cpp`
- Modify: `libs/systems/src/extraction_line.cpp`, `libs/systems/include/pychron/systems/extraction_line.hpp`, `libs/systems/CMakeLists.txt`
- Test: `tests/systems/test_line_sim.cpp`

**Interfaces:**
- Produces:

```cpp
namespace pychron::systems {
struct LineSimNote { LogLevel level; std::string message; };
struct LineSimOptions {
  sim::SimSettings sim;
  std::optional<std::filesystem::path> sim_file;  // as ExtractionLineOptions::sim_file
  std::filesystem::path sim_beside_skipped;       // what load() left alone, for the note
};
struct LineSim {
  std::unique_ptr<sim::SimSystem> system;
  std::vector<LineSimNote> notes;  // what ExtractionLine logged: skipped sim file (info), second spectrometer (warn), valves without physics (info)
};
// Topology from the canvas (none: empty), `~` check, sim.toml, cryotrap check,
// SimSystem, notes, and SimSystem::build_error() as it stands after construction.
// The caller still checks build_error() again once it has made the hooks.
Result<LineSim> build_line_sim(const config::SystemConfig& config, const canvas::Canvas* canvas,
                               const Clock& clock, LineSimOptions options);
// load()'s rule: `[sim] file` relative to the line, else sim.toml beside it (only with a canvas).
// Returns {file to read, file skipped}.
std::pair<std::optional<std::filesystem::path>, std::filesystem::path>
resolve_sim_file(const std::filesystem::path& system_file, const config::SystemConfig& config, bool has_canvas);
}
```

`topology_of`, `role_of`, `share_a_volume`, `pipe_between`, `kPipeMark` move to `line_sim.cpp`. `ExtractionLine::build_sim()` becomes a call plus logging each note at its level; its messages stay word for word.

- [ ] **Step 1: Failing tests**: `BuildsExampleLine` (example config + canvas: system has the canvas's volumes, `spectrometer_volume()` set); `TildeInValveNameRefused`; `NotesSayValvesWithoutPhysics` (a dangling valve → an info note naming it); `ResolveSimFileBesideOnlyWithCanvas`.
- [ ] **Step 2–4:** fail, move, pass. Every existing `pychron_systems_tests` and integration sim test passes unchanged (spec 7: that is the check).
- [ ] **Step 5: Commit** — `refactor(systems): build_line_sim, the simulated lab without the line`

---

### Task 8: `LineServer`

**Files:**
- Create: `libs/systems/include/pychron/systems/line_server.hpp`, `libs/systems/src/line_server.cpp`
- Test: `tests/systems/test_line_server.cpp`

**Interfaces:**
- Consumes: `build_line_sim` (7), `WireServer` (6), `wire_for_transport` (5), `SimEndpoints` (3).
- Produces:

```cpp
namespace pychron::systems {
struct LineServerOptions {
  std::string bind = "127.0.0.1";
  std::string advertise;          // host written to the endpoints file; empty: bind
  std::uint16_t port_base = 0;
  LineSimOptions sim;
};
struct ServedTransport {
  std::string name;
  std::string kind;               // the config's: tcp, udp, serial, modbus_tcp
  WireProtocol protocol;
  std::uint16_t port;
  std::string emulator;           // "" = silent wire
};
class LineServer {
 public:
  static Result<std::unique_ptr<LineServer>> start(const config::SystemConfig& config, const canvas::Canvas* canvas,
                                                   const Clock& clock, LineServerOptions options);
  const std::vector<ServedTransport>& served() const;      // name order
  const std::vector<std::string>& in_process() const;      // kind = "sim": in-process, not joined
  const std::vector<LineSimNote>& notes() const;           // line sim's, then serve's
  SimEndpoints endpoints() const;                          // config = canonical config.source_file
  sim::SimSystem& system();
  void stop();
};
}
```

Rules (spec 4.4, 5): `tcp`, `modbus_tcp`, `serial` → Tcp; `udp` → Udp; `link`, `sim`, `modbus_rtu` → not served (`sim` listed in `in_process()`; `modbus_rtu` gets a warn note `modbus_rtu is not supported`). A `serial` one gets an info note `<name>: serial served as TCP; baud, parity and port name are not exercised`. A served transport with `emulator == ""` gets a warn note `<name>: no emulator for driver kind '<kind>': silent wire`. Two served transports with equal config host and port → Config error naming both (phase A). After the hooks are made, `system().build_error()` fails the start.

- [ ] **Step 1: Failing tests**: `ServesHardwareStyleExample` (example config + canvas with each sim transport rewritten to tcp, or modbus_tcp for a transport whose drivers are `plc2000_*`, host `192.0.2.1`: every transport served, ports non-zero, emulators named); `SimTransportsInProcess`; `SerialNoted`; `UnknownKindWarns`; `SameHostPortRefusedNamingBoth`; `EndpointsCarryAdvertise`.
- [ ] **Step 2–4:** fail, implement, pass; TSan.
- [ ] **Step 5: Commit** — `feat(systems): LineServer serves a line's config on sockets`

(The rewrite helper of `ServesHardwareStyleExample` goes in `tests/support/hardware_style.hpp` for Tasks 9 and 11: `config::SystemConfig hardware_style(config::SystemConfig)`.)

---

### Task 9: `elctl sim serve`

**Files:**
- Create: `apps/elctl/src/sim_serve.hpp`, `apps/elctl/src/sim_serve.cpp`
- Modify: `apps/elctl/src/cli.cpp` (dispatch `sim serve`, usage text), `apps/elctl/CMakeLists.txt`
- Test: `apps/elctl/tests/test_sim_serve_cmd.cpp`

**Interfaces:**
- Consumes: `LineServer` (8), `write_sim_endpoints`, `default_sim_endpoints_path` (3).
- Produces: `int sim_serve_command(const std::vector<std::string>& args, const std::filesystem::path& config, const std::filesystem::path& canvas, Io io, const std::function<bool()>& stop);` `cli.cpp` calls it from `execute` when `argv == {"sim", "serve", ...}` with `stop = [] { return interrupt_count() > 0; }`; `elctl sim` with no argument stays the REPL.

Options: `--config FILE` (over the global `-c`/install line), `--canvas FILE`, `--bind ADDR`, `--advertise HOST`, `--port-base N` (1..65535), `--endpoints FILE` (default `default_sim_endpoints_path(config)`). Unknown option → usage, `kUsage`. Prints:

```
transport           protocol  port   emulator
switch_controller   tcp       40213  agilent_switch
spare_gauge         tcp       40214  (silent)
line_sim            -         -      in-process, not joined
<notes, one per line, "note: " or "warning: ">
endpoints: <path>
serving; interrupt to stop
```

then waits (`SteadyClock::sleep_for(100 ms)` loop on `stop()`), stops the server and removes the endpoints file. A bind that is not a loopback address (`127.0.0.0/8`, `::1`, `localhost`) prints to stderr: `warning: serving on <addr>: the emulators accept any login and have no authentication; anyone who can reach this address can drive the simulated lab`.

- [ ] **Step 1: Failing tests** (`SimServeCmd`, command on a thread, `stop` an atomic flag the test sets):
  - `WritesEndpointsAndRemovesThemOnStop`: hardware-style example written to a temp dir; endpoints file appears (wait ≤ 5 s), parses, every transport has a port; after stop the command returns 0 and the file is gone.
  - `PrintsTable`: output contains each transport name and `in-process, not joined` for a sim one.
  - `WarnsOnNonLoopbackBind`: `--bind 0.0.0.0` → stderr has `no authentication`.
  - `BadPortBaseIsUsage`.
- [ ] **Step 2–4:** fail, implement, pass.
- [ ] **Step 5: Commit** — `feat(elctl): sim serve`

---

### Task 10: Clients take `--sim-endpoints`

**Files:**
- Modify: `libs/systems/include/pychron/systems/extraction_line.hpp`, `libs/systems/src/extraction_line.cpp`; spectrometer assembler options header and `libs/systems/src/spectrometer/assembler.cpp`; `apps/elctl/src/cli.cpp`, `exp.cpp`, `exp.hpp`, `laser.cpp`, `line.cpp`, `line.hpp`; `apps/pychron-ui/src/command_line.{hpp,cpp}`, `main.cpp`
- Test: `apps/elctl/tests/test_line_commands.cpp`, `tests/ui/test_command_line.cpp`

**Interfaces:**
- Consumes: `load_sim_endpoints` (3), `TransportContext::endpoints` (4).
- Produces:
  - `ExtractionLineOptions`: `std::optional<std::filesystem::path> sim_endpoints; bool sim_endpoints_any = false;`. `load()` and `create()` (with `config.source_file`) read it via `load_sim_endpoints`; with `force_sim` → Config `--sim-endpoints and --sim cannot be used together`; every `make_transport` gets `context.endpoints`. A line with an endpoints file builds no `SimSystem` unless a transport is `kind = "sim"`.
  - `AssemblerOptions`: `std::shared_ptr<const std::map<std::string, Endpoint>> sim_endpoints;` passed to `default_transport` (new trailing parameter `const std::map<std::string, Endpoint>* endpoints = nullptr`) and on to its `TransportContext`. Phase A serves no spectrometer, so a spectrometer file with non-sim transports fails its load with the missing-entry error: intended (rule of section 5).
  - elctl globals `--sim-endpoints FILE`, `--sim-endpoints-any`; with `--sim` or a simulation install → usage error `--sim-endpoints cannot be used with --sim (or a simulation install)`; `exp run --sim-speed` with them → `--sim-endpoints runs in real time; --sim-speed is refused`. Passed to `ExpGlobals` (exp, laser) and `LineOptions` (open, close, read, scan, state, probe).
  - pychron-ui `CommandLine`: `std::optional<std::filesystem::path> sim_endpoints; bool sim_endpoints_any = false;`; `parse` refuses with `--sim` and with `--sim-speed`; `main.cpp` sets the line and spectrometer options.
- [ ] **Step 1: Failing tests**: elctl `SimEndpointsWithSimRefused`, `SimEndpointsWithSimSpeedRefused`; `ReadOverEndpoints` (a `LineServer` in the test serving the hardware-style example; `elctl -c <hw file> --sim-endpoints <file> read <gauge>` prints a pressure); UI `SimEndpointsParsed`, `SimEndpointsWithSimRefused`, `SimEndpointsWithSimSpeedRefused`.
- [ ] **Step 2–4:** fail, implement, pass. UI tests need `dev-ui`: `cmake --build --preset dev-ui --target pychron_ui_tests`.
- [ ] **Step 5: Commit** — `feat: --sim-endpoints drives a line served by sim serve` (two commits are fine: `feat(systems)` and `feat(elctl)`/`feat(ui)`).

---

### Task 11: Integration over the wire

**Files:**
- Create: `tests/integration/test_sim_serve.cpp`
- Modify: `tests/integration/CMakeLists.txt` if not globbed

**Interfaces:**
- Consumes: `LineServer` (8), `ExtractionLine` with `sim_endpoints` (10), `hardware_style` (8).

Fixture: hardware-style example written to a temp dir (line file, canvas, sim.toml copied), `LineServer` on OS ports with `valve_conductance` and `pump_speed` large (equilibration in milliseconds), endpoints file written; client `ExtractionLine::load(hw file, canvas, {sim_endpoints = file})`. `SteadyClock`, bounds ≥ 5 s.

- [ ] **Step 1: Failing tests** (`SimServeSteady`):
  - `OpenValveOverWire`: client actuates a valve open → `server.system().valve_open(name)` true.
  - `GaugeRisesOverWire`: open the valve between a gauge's volume and the tank; poll `read_gauge` until it exceeds 10× its first reading (≤ 5 s). No exact value.
  - `ReadbackMatchesServer`: client 1 opens a valve and is destroyed; client 2 `start()` reads the valve `Open`.
  - `PychronValvesThroughDriver` (Review Focus 5): a `pychron_valves` transport added to the fixture config; open and close it 10 times through the client.
  - `LoadRefusedWithEntryMissing`: endpoints file with one entry removed → load fails Config naming it.
  - `LoadRefusedWithForceSim`.
  - `StaleEndpointsFailStart` (Review Focus 3): server stopped, file kept → client `start()` fails `Io`, message contains a transport name.
- [ ] **Step 2–4:** fail if anything is missing, fix the owning task's code, pass; TSan.
- [ ] **Step 5: Commit** — `test(integration): the example line driven over sim serve`

---

### Task 12: Docs, spec, whole-branch checks

**Files:**
- Modify: `docs/simulator.md` (section "The simulated lab on the wire": purpose, emulated kinds table from Task 5, not exercised: serial settings, timing, vendor quirks; phase A limits: no spectrometer, no replay), `docs/installation_runbook.md` (in 2.4: rehearse before connecting: `elctl sim serve`, then `elctl --sim-endpoints ... exp run`), `AGENTS.md` (under Build and test: endpoints rule, real time only, request framing comes from the driver's codec; written compressed), `docs/superpowers/specs/2026-10-09-sim-serve-design.md` (Departures above, phase A status, section 10's chromium/NGX poll point: 20 ms measured enough for phase A's kinds, none of which sends unsolicited), `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` section 4.4 (WireServer socket waits on real time).

- [ ] **Step 1:** Write the docs.
- [ ] **Step 2:** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py` (whole tree) and `--build-dir build/dev-ui`.
- [ ] **Step 3:** TSan build: transport, sim, systems, integration, elctl targets, run Tasks 2, 6, 8, 9, 11 suites. ASan/UBSan build: same.
- [ ] **Step 4:** `cmake --build --preset dev-ui && ctest --preset dev-ui`: all pass.
- [ ] **Step 5: Commit** — `docs: sim serve phase A`

## Open questions

None.
