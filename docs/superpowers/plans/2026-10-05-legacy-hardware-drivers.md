# Legacy Hardware Drivers: Valve Actuators, Gauges, Cryo — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the legacy Pychron valve actuators, pressure gauge controllers and cryostat controllers that real labs run, so that `elctl import-line` stops replacing them with `sim_valves` and illustration-only gauges, and so that `set_cryo` / `get_cryo_temp` / `get_pressure` work in scripts on an extraction line.

**Architecture:** The same three layers as every other driver (`libs/codecs/CONVENTIONS.md`): a pure codec per vendor, a `Device` in `libs/devices` that composes `Transport&` + codec and implements a capability interface (`IValveActuator`, `IChannelPressureGauge`, and a new `ITemperatureController`), and a `SimTransport` hook wired into `SimSystem::hook_for`. Above the drivers, two systems-layer pieces are missing and are added here: a line `IPressureService` and a line cryo service. The legacy importer (`libs/setup/src/legacy_line.cpp`) learns each new kind as it lands.

**Tech Stack:** C++20, GoogleTest, toml++, asio (UDP), the repo's `Transport` / `SimTransport` / `DriverRegistry` / `GaugeScanner` / `SwitchManager`.

**Sources:** `docs/superpowers/specs/2026-09-30-legacy-config-survey.md` (appendix C: device catalogue, lab counts, shared endpoints), `2026-10-03-legacy-extraction-line-survey.md` (actuator table, importer), and the legacy Python source (`NMGRL/pychron`, `pychron/hardware/{actuators,agilent,arduino,gauges,ionpump,lakeshore}`), read 2026-10-05. AELAMS (not in the Drive survey) runs a PLC2000 for valves and gauges, per the owner. Legacy file and line references below are into that tree. No lab file is copied into the repo; fixtures are synthetic.

## What exists, what is missing

| Area | Have | Missing (labs that need it, from survey C.2/C.6) |
|---|---|---|
| Valve actuators | `ngx_valves` (8 labs), `proxr_relay`, `sim_valves` | Agilent 34903A switch (5), Qtegra valves (3), NMGRL furnace firmware (3), AutomationDirect PLC2000 over Modbus (AELAMS), Pychron-to-Pychron (1), Arduino (1), Agilent 34907A DIO readback (1), LabJack U3 (1) |
| Valve config | interlocks, settle, mandatory read-back | per-valve `inverted`, per-actuator `invert`, `state_source` (readback on another device), `verify = false` (legacy `query_state=false` / `check_actuation_enabled=false`) |
| Gauges | `pfeiffer_maxigauge`, `gp_microion`, `GaugeScanner`, `ExtractionLine::read_gauge` | PLC2000 gauge registers (AELAMS), Varian/Agilent XGS-600 (ldeo), Qtegra gauge readback (ldeo, usgsdenver), MicroIon via furnace host (usgsdenver), MKS 937 (one 2019 lab), SRS IGC100, Gamma SPC ion pump; **no `IPressureService` implementation**, so `get_pressure` in a script is always "not supported" |
| Cryo | `ICryo` as an *extraction-device* feature only; script verbs `set_cryo` / `get_cryo_temp`; run field `cryo_temp` | Any temperature-controller capability; Lakeshore 325/331/335/336 driver (ldeo, hal copy); a line-level cryo service (legacy reaches the cryostat through the extraction line, not the extract device); named setpoints (`cryotemps.yaml`) |
| Transports | serial, tcp, sim, link; Modbus TCP framing (`ReadSpec::modbus_tcp`, `codecs/modbus_adc`) used over a plain `tcp` transport | `udp` (ldeo and felix Qtegra); Qtegra link sharing (one socket for spectrometer + valves + gauges, survey C.5); `modbus_tcp` / `modbus_rtu` kinds still fail at build (`factory.cpp:61-63`); no coil or holding-register codec |

## Owner decisions needed before the marked tasks

1. **Agilent identify command (Task A1).** Legacy sends `*TST?` and accepts `0`. `*TST?` runs a full self-test that can take seconds and cycles relays on some cards. Proposal: `*IDN?` on connect; never `*TST?`.
2. **Agilent at ASU is VISA-USB (Task A1).** No VISA transport exists. Proposal: support serial and TCP (LAN on a 34972A) now; ASU's unit stays on `sim_valves` with a note until a `usbtmc` transport is planned separately.
3. **`verify = false` semantics (Task 0.2).** Legacy `query_state=false` trusts the commanded state. Proposal: record the commanded state, publish `ValveChanged`, and log once at load that the valve is unverified. Interlocks then rely on commanded state for that valve.
4. **Cryo blocking (Task C4).** Legacy `block` waits forever. Proposal: required `timeout_s` (default 600) and the run's `CancelToken`; timeout is an `Io` error that fails the script.
5. **Measured cryo temperature in the run record (Task C5).** Legacy's `cryo_response` blob is always empty (its recorder raises). Proposal: record the measured input temperatures at `end_extract` as `extraction.cryo_measured_k` beside the requested `cryo_temperature`; no time-series blob.
6. **Gauge "off"/over-range readings (Tasks B1–B6).** Legacy maps them to sentinels (MKS `OFF` → 1000, `LO<E-11` → 1e-12, SPC failure → 0.0) or leaves a stale value. Proposal, per codec rule 4: every non-number is a `Protocol` error with a stable message (`"gauge off"`, `"under range"`, `"over range"`, `"no sensor"`), so `GaugeScanner` raises its one Warning alarm and the UI shows no number, never a wrong one. Under-range alone may instead decode as an upper bound if the owner wants a value shown.
7. **AELAMS PLC2000 link and register map (Task 0.4, A7, B7).** Legacy supports both Modbus TCP (`modbustcp_communicator.py`, port 502) and RTU (`modbus_communicator.py`, serial). Which does AELAMS run, which unit id, and is the float word order the legacy default (byte order big, word order little: low word first)? Needed: AELAMS's PLC2000 `.cfg` files (actuator, gauge controller, and `PLC2000Heater` if present). Proposal: TCP first; RTU only if AELAMS uses it.

## Global constraints

- `libs/codecs/CONVENTIONS.md` binds every codec: pure, `Result<T>`, `codec::protocol_error`, no device names, tests with literal bytes.
- One driver per `.cpp`, registered with `REGISTER_DRIVER`; schema keys documented in `schema()` (they are what `elctl list-drivers` prints).
- Every driver gets a `SimTransport` hook and a `SimSystem::hook_for` branch so the same config runs with no hardware. Sim hooks behave like the device on the wire (silent for other bus addresses, error replies for bad syntax).
- Fix legacy bugs, do not port them; each one fixed is named in the task's tests (the surveys found: exact-float setpoint compare, no-timeout blocking, Model330 signature crash, range-band gaps at 10 and 30, unbounded NGX-style re-query recursion, close-readback-of-None-passes, MKS `E+` exponent not parsed, Python-3 float bank math).
- No string expression language is carried forward. Legacy range predicates (`1 = v<10`) and gauge conversions (`IG1=3*v**2-3*v-5`) are converted to numeric tables / coefficient arrays at import; the importer reports any it cannot convert.
- Lifetime rules (AGENTS.md): drivers take `Transport&`; services take `SwitchManager&`, `ExtractionLine&`, `Clock&`. In tests declare fakes before the component.
- AGENTS.md workflow: commit on a branch, rebase on `origin/main`, run the tests, merge to `main`, push. No pull requests. Never skip a failing test.
- Build and test: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8`. Build gcc 13 with `-DPYCHRON_WARNINGS_AS_ERRORS=OFF`.

## Order

```
Phase 0 (shared)      0.1 UDP ─┬─ 0.3 Qtegra link sharing ─┬─ A2 qtegra_valves
                               │                           └─ B2 qtegra_gauges
                      0.2 valve inversion / state_source / verify ─ A1 agilent_switch, A6 agilent_dio
                      0.4 Modbus codec + transport kinds ─┬─ A7 plc2000_valves
                                                          └─ B7 plc2000_gauges
Phase A (actuators)   A1 → A2 → A3 → A4 → A5 → A6, A7     (A3–A7 independent of each other)
Phase B (gauges)      B0 pressure service first; B1–B6 independent
Phase C (cryo)        C1 → C2 → C3 → C4 → C5 → C6
Phase D               importer + docs per kind as each lands; D2 hardware bring-up (manual)
```

Phases A, B and C are independent of each other once Phase 0 is in; they can run in parallel worktrees. Within a phase, priority is by lab count.

---

## Phase 0: Shared infrastructure

### Task 0.1: `udp` transport kind

**Why:** two of three Qtegra labs (ldeo, felix) talk to Qtegra over UDP (survey C.6); the Qtegra spec (2026-10-01, open item 3) deferred it.

**Files:** `libs/core/include/pychron/core/config/system_config.hpp` (`TransportKind::Udp`, `UdpParams{host, port}`), `libs/core/src/config/loader.cpp` (kind table, keys), `libs/transport/include/pychron/transport/udp_transport.hpp`, `libs/transport/src/udp_transport.cpp`, `libs/transport/src/factory.cpp`; tests `tests/transport/test_udp_transport.cpp`, loader cases in the existing config tests.

- [ ] Datagram semantics: one `write` = one datagram; `read(ReadSpec)` assembles datagrams until the spec is satisfied (Qtegra replies fit one datagram, but don't assume it). `open()` connects the socket to the peer so stray datagrams from other hosts are dropped by the kernel.
- [ ] Health, retries, `TraceRecorder` and `poll()` behave as for TCP.
- [ ] Tests against a loopback asio UDP echo/peer in-process: request/reply, reply split over two datagrams, timeout, datagram from a different port ignored.
- [ ] `thermo_qtegra` accepts a `udp` transport (it only needs `Transport&`); add one `tests/systems/test_qtegra_system.cpp` case on UDP.

### Task 0.2: Valve inversion, separate state readback, unverified valves

**Why:** Reston/LDEO actuators set `invert`, NGX/NMGRL valves set `inverted_logic`/`inverted`, LDEO reads state from a different device (`state_device`), and many valves say `query_state=false` (survey §"Reston and LDEO additions"). The importer currently drops all of these and reports it.

**Files:** `system_config.hpp` (`ValveConfig`/`SwitchConfig`: `bool inverted`, `std::optional<StateSource> state_source{driver, address, inverted}`, `bool verify = true`), loader, `libs/systems/include/pychron/systems/switch_manager.hpp` + `src/switch_manager.cpp` (`SwitchSpec` gains the same), `tests/systems/test_switch_manager.cpp`, `tests/systems/test_switch_manager_properties.cpp`.

Semantics (write them into the `switch_manager.hpp` header comment):

- [ ] **Actuator `invert`** is a driver option, handled inside the driver (Agilent swaps OPEN/CLOSE in both the command and the query, exactly as legacy `agilent_gp_actuator.py:115-118`). The manager never sees it.
- [ ] **Valve `inverted`**: the manager sends `close()` to open and `open()` to close, and inverts the read-back, so the recorded state is always the valve's, not the channel's. Legacy double-actuation ignored this flag; ours does not.
- [ ] **`state_source`**: read-back and `refresh()` read `state_source.driver` at `state_source.address` (inverted if its `inverted` is set) instead of the actuator. The read-back step stays mandatory.
- [ ] **`verify = false`**: no read-back; record the commanded state (owner decision 3). `refresh()` leaves such valves at their recorded state rather than Unknown after the first actuation.
- [ ] A failed read-back on **close** fails (legacy passed it: `not None == True`). Test `CloseWithNoReadbackFails`.
- [ ] Interlock property tests extended: inversion and state_source never let two negatively interlocked valves be recorded open together.

### Task 0.3: Qtegra link sharing

**Why:** at melbourne and ldeo the spectrometer link, the valves and the gauge readback are one Qtegra endpoint; Qtegra accepts one client (survey C.5). NGX already solves this with `NgxLinkHandle` + `kind = "link"` transports.

**Files:** `libs/devices/include/pychron/devices/spectrometer/thermo_qtegra.hpp` + `.cpp` (expose a `QtegraLinkHandle` the way `ngx_link.hpp` does: a serialised `ask(command) -> Result<std::string>` over the spectrometer's transport), `tests/devices/spectrometer/test_thermo_qtegra*.cpp`.

- [ ] Follow `ngx_link.hpp` exactly (owner driver opens; borrowers use a `link` transport naming the owner; borrowers' `connect()` is a no-op).
- [ ] An `ask` from a borrower is atomic with respect to acquisition polling (`GetData` and a valve `Open` never interleave on the wire). Test with a sim hook that records interleaving.
- [ ] Spectrometer config may live in another file; same lookup rule as NGX.

### Task 0.4: Modbus codec and transport kinds

**Why:** AELAMS runs its valves and gauges on an AutomationDirect PLC (legacy `PLC2000GPActuator`, `PLC2000GaugeController`). The priorities doc already decided to hand-roll Modbus framing as a codec rather than use libmodbus; `codecs/modbus_adc` does it for function 04 only.

**Files:** `libs/codecs/include/pychron/codecs/modbus.hpp`, `src/modbus.cpp` (new, generic), `codecs/modbus_adc.{hpp,cpp}` (rebuilt on it, public API unchanged), `libs/transport/src/factory.cpp`, `tests/codecs/test_modbus.cpp`, `tests/transport/test_factory.cpp`.

- [ ] Generic codec, host and device side (the device side feeds sim hooks): read coils (01), read holding registers (03), read input registers (04), write single coil (05, `FF00`/`0000`), write multiple registers (16). MBAP framing for TCP; RTU framing with CRC-16/Modbus behind the same request/response types, so one driver serves both.
- [ ] 32-bit float and int from a register pair with explicit byte and word order (`ABCD`, `CDAB`, `BADC`, `DCBA`). Legacy's default (byte order big, word order little) is `CDAB`; `modbus_adc` stays `ABCD`. Test vectors for all four.
- [ ] Exception replies (`0x80 | fn`, code) are Protocol errors naming the code (`illegal data address` etc.); transaction-id and unit-id mismatches are Protocol.
- [ ] `kind = "modbus_tcp"` builds a TCP transport (port default 502); `kind = "modbus_rtu"` builds a serial transport. Unit id is a driver option (`unit`, default 1), not a transport key, because several units share one RTU bus. The kinds exist so `elctl check` can tell a Modbus driver on a non-Modbus transport.
- [ ] RTU waits on owner decision 7; TCP does not.

---

## Phase A: Valve actuators

Every actuator task has the same steps; only the protocol differs.

1. Codec `libs/codecs/{include/pychron/codecs,src}/<vendor>.{hpp,cpp}` + `tests/codecs/test_<vendor>.cpp`: every command, a good reply per decoder, error/garbage/truncated replies.
2. Driver `libs/devices/{include/pychron/devices,src}/<kind>.{hpp,cpp}` implementing `Device, IValveActuator` (+ `IConnectable` when it has a handshake); `schema()`/`create()`; `REGISTER_DRIVER`.
3. Sim hook in the driver's header (`<kind>_sim_hook(model)`) + `SimSystem::hook_for` branch that maps addresses to the line's valves (copy the `ngx_valves` branch).
4. `tests/devices/test_<kind>.cpp`: driver over `SimTransport` scripted bytes, plus the shared `IValveActuator` conformance cases (Task A0).
5. Importer: map the legacy class in `resolve_actuator` (`libs/setup/src/legacy_line.cpp:378`) to the new kind with its transport; delete the matching "no driver yet" note; add an importer test with a synthetic cfg.

### Task A0: `IValveActuator` conformance suite

**Files:** `tests/devices/valve_conformance.hpp` (pattern: `tests/devices/extraction/conformance.hpp`), run against `SimValves`, `ProxrRelay`, `NgxValves` first so the suite is proven before new drivers use it.

- [ ] open→read is Open; close→read is Closed; unknown/invalid address is Config and sends nothing; transport timeout is Io; a reply the codec rejects is Protocol; no call leaves stale bytes that the next exchange would take as its reply.

### Task A1: `agilent_switch` — Agilent 34903A / 34970A switch unit (5 labs)

Protocol (legacy `agilent/agilent_gp_actuator.py`, `agilent_mixin.py`):

- Write terminator LF (`agilent_mixin.py:58`); replies CR/LF terminated.
- Address is the channel verbatim (`101` = slot 1, channel 01); validate `^[1-3]\d\d$`.
- Open valve: `ROUT:OPEN (@101)`; close: `ROUT:CLOSE (@101)`. Actuator `invert` swaps OPEN/CLOSE in commands **and** queries.
- State: `ROUT:OPEN? (@101)` (`ROUT:CLOSE?` when inverted); first char `1` = true.
- After each command drain errors: `SYST:ERR?` until `+0,"No error"`, at most 10; any other error string fails the actuation as Protocol with the instrument's message.
- Several drivers may share one unit (legacy "becker box" reuse by `[Communications] address`): one `[transports.x]`, several drivers, no special code.

- [ ] Owner decisions 1 and 2 first.
- [ ] Driver option `invert` (bool, default false).
- [ ] Tests include `InvertSwapsCommandAndQuery`, `ErrorQueueIsDrainedAndReported`, `ErrorQueueDrainIsBounded`.

### Task A2: `qtegra_valves` — valves through Qtegra RemoteControl (3 labs)

Protocol (legacy `actuators/ascii_gp_actuator.py`; `QtegraGPActuator` is a pass-through subclass):

- `Open <name>` / `Close <name>`, reply `OK`; `GetValveState <name>`, reply `True` / `False`. Name is the address verbatim and may contain spaces (`Valve 1_9 Set`, `Pipet Ref. Out Set`).
- Anything other than exactly `True`/`False` is Protocol (legacy treated it as closed).

- [ ] Depends on 0.3; transport is a `link` to the `thermo_qtegra` driver, or its own TCP/UDP transport when no spectrometer driver is configured.
- [ ] Sim: extend the Qtegra sim to answer the three verbs and move the simulated line's valves.

### Task A3: `nmgrl_furnace_valves` — NMGRL furnace firmware (usgsdenver, felix, jan)

Protocol (legacy `actuators/nmgrl_furnace_actuator.py`), TCP 4567:

- `Open <addr>` / `Close <addr>` → bool token; `GetChannelState <addr>` → bool token.
- Indicator: `{"command": "GetIndicatorState", "name": "<addr>", "action": "Open"|"Close"}` → `open` / `closed`.
- Bool tokens (`strtools.to_bool`): `true t yes y 1 ok open` / `false f no n 0 closed`, case-insensitive; **anything else is Protocol** (legacy: False).

- [ ] `read()` uses the indicator command (it reports the physical valve, not the relay).
- [ ] The furnace host also serves gauges (Task B3) and later the furnace itself; keep the endpoint shareable (plain shared TCP transport; the transport queue serialises).

### Task A4: `pychron_valves` — another Pychron's valve server (felix ↔ jan)

Protocol (legacy `actuators/pychron_gp_actuator.py`, server `tx/protocols/base_valve.py`), TCP 1061:

- `Open <name>` / `Close <name>` → `OK` (changed) or `ok` (already) or an error code; `GetValveState <name>`, `GetIndicatorState <name>` → bool token.
- The address is the **remote valve name**.

- [ ] Scope: actuation and state only. State/lock/owner words (`GetValveStates` with CRC16) are out of scope; legacy's version-1 word format disagrees between its own sender and parser.
- [ ] Note in the header: this is the client half of the RPC service (priorities item 5); when `libs/rpc` lands, its server must answer this driver.

### Task A5: `arduino_valves` — NMGRL Arduino valve box (usgsdenver)

Protocol (legacy `arduino/arduino_gp_actuator.py`, firmware `arduino/sketches/valvebox3.pde`), serial 115200:

- `w <pin> <0|1>\r\n` → `OK`; `r <pin>\r\n` → `0`/`1`.
- Valve address 0..9 maps to output pin `P = {51,48,45,...,24}[addr]`; open indicator `P-1`, closed indicator `P-2`. Make the pin table a driver option with that default.
- State: exactly one indicator high → that state; both or neither → Protocol ("indicators disagree"), not a string smuggled through as legacy did.

- [ ] Actuation waits for the matching indicator (legacy: up to 6 polls, 0.25 s apart) through the driver's injected `Clock`, so tests do not sleep.

### Task A6: `agilent_dio` — Agilent 34907A digital input as a state source (ldeo)

Legacy `agilent/agilent_multifunction.py`: its write path raises TypeError and is off by one, so in practice LDEO used it only to read state.

- Read: `SENS:DIG:DATA:WORD? (@<slot>01)`; bit `int(addr[1:]) - 1` of the 16-bit word; actuator `invert` XORs the word.
- `open()`/`close()` return `not_supported`; used only as a Task 0.2 `state_source`.

- [ ] Shares a codec file with A1 (`codecs/agilent.hpp`).

### Task A7: `plc2000_valves` — AutomationDirect PLC over Modbus (AELAMS)

Protocol (legacy `actuators/plc2000_gp_actuator.py`, `core/modbus.py`):

- Valve address is a 1-based coil number; wire coil = address − 1.
- Open: write single coil `ON`; close: `OFF` (no inversion in legacy; Task 0.2 `inverted` applies if a valve needs it).
- State: read coils, 1 coil at address − 1; bit 0.
- Legacy returned success without checking the write's echo and swallowed `ModbusIOException`; ours checks the function-05 echo (address and value must match) and the manager's read-back runs as for every valve.

- [ ] Driver options `unit` (default 1), `coil_offset` (default −1, so the legacy address convention imports unchanged).
- [ ] Batch state: `refresh()` reads the coil range covering all of the driver's valves in one request when they are contiguous (an `IValveActuator` extension `read_many`, optional, default per-valve). Legacy read one coil per valve.
- [ ] Sim: a coil bank model behind the device-side codec; writes move the simulated line's valves.

### Not in this plan (actuators)

LabJack U3/T4 (needs the LabJack USB library; hal only), WiscAr cRIO (WiSCAr runs NGX), MCC, RPi GPIO, U2351A (all unused by surveyed labs or broken in legacy). The importer keeps `sim_valves` + its note for them.

---

## Phase B: Gauges

### Task B0: `IPressureService` over the extraction line

**Why:** scripts call `get_pressure("Bone", "IG")`; `extraction::IPressureService` is declared but never implemented, and `LabSession` never sets `ExtractionServices::pressure`. Without this, no new gauge is reachable from a script.

**Files:** `libs/systems/include/pychron/systems/line_pressure_service.hpp`, `src/line_pressure_service.cpp`, `libs/experiment/src/lab/session.cpp` (construct and set `line.pressure`), `tests/systems/test_line_pressure_service.cpp`, a scripting test.

- [ ] `get_pressure(controller, gauge)`: resolve by gauge name; `controller` must match the gauge's driver name or be empty (legacy accepts display names too: accept `display_name` if B-phase adds one). Return the latest scanned value (legacy `force=False`), and fail Io if the latest scan failed or is older than 3 scan intervals.
- [ ] `get_manometer_pressure(name)`: same lookup over gauges (a manometer is a gauge here).
- [ ] Conditionals already read `gauge.NAME.pressure` from the line snapshot (`libs/experiment/src/conditionals/expr.cpp`); the experiment importer maps legacy `Hub.IG1.pressure` to `gauge.IG1.pressure`. Add a test that a gauge from a new driver reaches both paths.

### Task B1: `varian_xgs600` — Agilent/Varian XGS-600 (ldeo)

Protocol (legacy `gauges/varian/varian_gauge_controller.py:43-46`), serial 9600, CR:

- Read: `#<aa>02U<label>\r` → `><value>` (e.g. `>1.234E-07`). `aa` is the bus address (`00` for RS-232).
- Gauges are addressed by sensor label (`IG1`, `CNV1`), not a number. Keep `GaugeConfig::channel` an integer: driver option `labels = ["CNV1","IMG1","HFIG1"]`, channel *n* reads `labels[n-1]`.
- Off/over-range/no-sensor replies: per owner decision 6 (codec tests with literal replies from the XGS-600 manual).

- [ ] `IChannelPressureGauge`; several gauges on one bus share one transport.

### Task B2: `qtegra_gauges` — values read through Qtegra (ldeo, usgsdenver)

- `GetParameter <name>` → number; `ERROR...` → Protocol (existing `thermo_qtegra` codec helpers).
- Driver option `parameters = ["Ion Gauge MS Readback", ...]`, channel *n* reads `parameters[n-1]`.

- [ ] Depends on 0.3 (link to the Qtegra driver).

### Task B3: `furnace_microion` — Micro-Ion read through the furnace host (usgsdenver)

Protocol (legacy `granville_phillips/pychron_micro_ion_controller.py:29-44`, server `furnace/firmware/manager.py:525-543`), TCP 4567:

- `GetPressure <remote_device>,<IG|CG1|CG2>` → number. Driver option `remote_device` (legacy used its own local name; the remote names are e.g. `first_stage_gauge`).
- Channels 1/2/3 map to IG/CG1/CG2 as in `gp_microion`.

- [ ] Legacy needed `[Scan] function=get_pressures` to work at all; ours has one read path.

### Task B4: `mks_937` — MKS 937 controller

Protocol (legacy `gauges/mks/controller.py`), serial, RS-485 address 001..253:

- Frame `@<aaa><cmd>;FF`; query `@253PR1?;FF` → `@253ACK7.60E-08;FF`; NAK → `@253NAK<code>;FF`.
- Markers: `LO<E-11`, `NO_GAUGE`, `PROT_OFF`, `OFF` → per owner decision 6 (legacy: 1e-12, 0, 760, 1000).
- On connect, channels 1/3/5 (cold-cathode/ion) are powered with `@aaaCP<n>!ON;FF` **only if** driver option `power_on_connect = true` (default false; switching a gauge on is not a side effect of connecting).

- [ ] Exponents with `+` parse (legacy regex dropped them). Test `PositiveExponentParses`.

### Task B5: `srs_igc100` — SRS IGC100

- `GDAT? <port>` → number. Port map `IGC1=1, IGC2=2, PG1=3, PG2=4, CM1..4=5..8, AN1..4=9..12` (legacy `igc100_gauge_controller.py:19-32`); driver option `ports` maps channel *n* to a port name.

- [ ] Lower priority: in the survey's text but no live `devices/` file references it. Confirm with the owner before starting.

### Task B6: `gamma_spc` — Gamma Vacuum SPC ion pump

Protocol (legacy `ionpump/spc_ion_pump_controller.py:67-73`):

- Telnet/TCP mode: `spc 0B` → `OK 00 4.1E-11 Torr` (prompt `>`); serial mode: `~ <aa> 0B <cs>\r` → `<aa> OK 00 <value> <unit> <cs>`. Checksum = sum of bytes mod 256, two hex digits (legacy sent a double space and `00`: do not copy).
- Units come from the reply; return the value only if it matches the gauge's configured units, else Protocol.

- [ ] TCP with read terminator `>` covers telnet as used (no option negotiation observed in the survey); add a test that a leading IAC sequence is skipped if it ever appears.

### Task B7: `plc2000_gauges` — gauge values in PLC registers (AELAMS)

Protocol (legacy `gauges/plc2000/plc2000_gauge_controller.py`, `core/modbus.py`):

- Gauge `channel` *n* reads 2 holding registers (function 03) from register *n* − 1 and decodes a float32 in the configured word order (default `CDAB`, legacy's big/little).
- The PLC reports whatever its ladder logic computed, in the gauge's configured units. A non-finite float is Protocol (legacy returned it).

- [ ] Driver options `unit`, `word_order` (`ABCD|CDAB|BADC|DCBA`, default `CDAB`), `register_offset` (default −1).
- [ ] One read covering every configured channel when the registers are contiguous; `IChannelPressureGauge` serves each channel from it.
- [ ] Sim: holding-register model filled from `SimSystem` gauge readings.
- [ ] Follow-up, not in this plan: legacy `PLC2000Heater` (`hardware/heater.py:101`) and `BakeoutPLC` use the same codec; list them in the importer report if AELAMS's files name them.

### Not in this plan (gauges)

ADC-backed gauges (`ADCGaugeController`, U3 gauges: need an ADC sensor source; belongs with the survey's "sensor with `source = {driver, channel}` and a conversion" work), MKS670/SRG/TerraNova (dead in legacy). The existing `pfeiffer_maxigauge` and `gp_microion` already handle status fields legacy ignored; no change.

---

## Phase C: Cryo

Legacy reaches the cryostat through the extraction line (`ExtractionLineManager.set_cryo` → `CryoManager` → first cryo device), not through the extract device. pychron-cpp only has `ICryo` as an extract-device feature, so a line cryostat (LDEO's Model 335) cannot be reached. This phase adds a temperature-controller capability, a Lakeshore driver, and a line cryo service the script host can bind `set_cryo`/`get_cryo_temp` to.

### Task C1: `ITemperatureController` capability

**Files:** `libs/devices/include/pychron/devices/temperature_controller.hpp`, `events.hpp` (`TemperatureSample{source, input, value_k, ts}`), `GaugeScanner`-like `TemperatureScanner` in `libs/systems` (or generalise the scanner's reader path; prefer reuse), tests.

```cpp
struct ITemperatureController {
  virtual ~ITemperatureController() = default;
  virtual std::vector<std::string> inputs() const = 0;          // "A", "B", ...
  virtual Result<double> read_temperature(std::string_view input) = 0;  // kelvin
  virtual int outputs() const = 0;                               // loops 1..n
  virtual Result<void> set_setpoint(int output, double kelvin) = 0;     // sets range, writes, verifies
  virtual Result<double> setpoint(int output) = 0;
};
```

- [ ] Kelvin at the interface; a controller configured for °C converts in the driver (explicit, per codec rule 6).
- [ ] Scanner publishes `TemperatureSample` per input at the driver's scan interval.

### Task C2: Lakeshore codec

Protocol (legacy `lakeshore/base_controller.py`, GPIB-style command set; models 325/331/335 identical, 336 adds inputs C/D and outputs 3/4):

- Write terminator LF (legacy forces it, `base_controller.py:281`); replies CR/LF.
- `*IDN?` → `LSCI,MODEL335,<serial>/<serial>,<fw>`; `*CLS`.
- `KRDG? A` → `+077.123` (kelvin); `CRDG? A` for °C. Inputs are letters; legacy sent lowercase, the instrument accepts both: send uppercase.
- `SETP <out>,<value>`; `SETP? <out>` → `+077.000`; `RANGE <out>,<r>`; `RANGE? <out>`.
- Values formatted locale-free with three decimals.
- Serial 7 data bits / odd / 1 stop is in the lab's transport config (Lakeshore's own default is 57600 7O1; LDEO's file says 9600 7O1); the codec does not assume it.

- [ ] Model 330 (`RANG`, `SETP?` without output) and SI 9700 (`SET`, `TA?`) are left out: no surveyed lab uses them.

### Task C3: `lakeshore` driver

**Files:** `libs/devices/{include/pychron/devices,src}/lakeshore.{hpp,cpp}`, sim hook, `SimSystem` branch (a first-order thermal model: input approaches setpoint with a time constant, on the injected clock), tests.

Options: `model` (`325|331|335|336`), `inputs` (default `["A","B"]`), `units` (`K|C`), `[[drivers.<name>.ranges]] {output, range, min, max}` (setpoint bands, numeric), `setpoint_tolerance` (default 0.01 K), `verify_retries` (default 3).

- [ ] `set_setpoint`: pick the band containing the setpoint (half-open `[min, max)`, last band closed; a setpoint in no band is Config), send `RANGE`, send `SETP`, read `SETP?`, compare within tolerance, retry at most `verify_retries`. Range 0 (heater off) is selectable. Tests `BandBoundariesAreCovered` (legacy gaps at 10 and 30), `RangeZeroIsSelectable`, `VerifyUsesTolerance`, `SameSetpointTwiceIsSentTwice` (legacy's trait handler skipped repeats).
- [ ] `connect()` sends `*CLS`, checks `*IDN?` names a Lakeshore of the configured model; mismatch is Config naming what answered.
- [ ] Implements `ITemperatureController` and `IScannable`.

### Task C4: Line cryo service and script binding

**Files:** `libs/devices/include/pychron/devices/extraction/services.hpp` (`ExtractionServices::cryo`, a `ICryo*`), `capability.cpp` (`Cryo` present if either the device or the line has it), `libs/systems/.../line_cryo_service.{hpp,cpp}`, `system_config.hpp` + loader (`[cryo]` section), `libs/scripting/src/python/host_state.cpp` (prefer the line service, fall back to the device's), `libs/experiment/src/lab/session.cpp`, tests.

Config:

```toml
[cryo]
driver = "cryostat"          # a [drivers.*] that is an ITemperatureController
tolerance_k = 1.0            # "at setpoint" band for blocking
timeout_s = 600
[cryo.setpoints]             # legacy cryotemps.yaml, values in kelvin, one per output
He_freeze = [14.0, 0.0]
Ar_freeze = [90.0, 120.0]
```

- [ ] Extend `ICryo` to legacy's script signature: `set_cryo(value_or_name, block, delay)` where the value is a number (output 1) or a setpoint name; `freeze`, `pump`, `release` are prefixed with the run's species (`Ar_freeze`), as legacy `cryo_manager.py:133-156`. Unknown name is Config, checked by the script static check where the name is a literal.
- [ ] Blocking waits on the run's `Clock` and `CancelToken` until every set output's paired input is within `tolerance_k`, polling at `max(0.5 s, delay)`; timeout per owner decision 4.
- [ ] `get_cryo_temp(input)`: input 1 → first configured input (legacy `1 → 'a'`), returns kelvin; a read failure is an error, never 0.0 (legacy returned 0.0).
- [ ] `elctl cryo status|set <K>|setpoint <name>` for bench work.

### Task C5: Cryo in the run record

- [ ] Per owner decision 5: at `end_extract`, record measured input temperatures into the run record beside the requested `cryo_temperature`; persisted by the DVC writer; shown in recall.
- [ ] Experiment rule: `cryo_temp` stays forbidden for non-heating runs (unchanged), and a queue with `cryo_temp` set on a line without a cryo service is a check error.

### Task C6: Cryo in the UI

- [ ] A Cryo dock in `apps/pychron-ui`: inputs, setpoints with readback, a strip chart from `TemperatureSample` (reuse the spectrometer strip-chart ring). Through `ExtractionLine`/`CoreBridge` only (instrument-control design §10).

---

## Phase D: Importer, docs, bring-up

### Task D1: Importer coverage (runs with each driver task, collected here)

- [ ] Actuators: `AgilentGPActuator` → `agilent_switch` (+ `invert`), `PLC2000GPActuator` → `plc2000_valves` on a `modbus_tcp`/`modbus_rtu` transport (`[Communications] host/port/byteorder/wordorder` carried over), `QtegraGPActuator` → `qtegra_valves` (link when the spectrometer importer made a `thermo_qtegra` on the same endpoint), `NMGRLFurnaceActuator`, `PychronGPActuator`, `ArduinoGPActuator`, `AgilentMultifunction` (as `state_source`).
- [ ] Valve keys now carried: `inverted_logic`/`inverted`, `state_device`/`state_address`, `query_state`/`check_actuation_enabled` → `verify`. Remove them from the "not carried over" report list.
- [ ] Gauge controllers (today: drawn for illustration only), including `PLC2000GaugeController` → `plc2000_gauges`: read `devices/<controller>.cfg` `[Gauges] names/channels/lows/highs` (comma lists, zipped; report length mismatches instead of truncating as legacy's `zip` did) into `[drivers.*]` + `[[gauges]]` with `alarm_low/high`. Canvas gauges then show readings.
- [ ] Cryostat: `Model335TemperatureController.cfg` → `[drivers.*] kind = "lakeshore"`, `[Range]` predicates → numeric bands (only the forms `v<a`, `a<v<b`, `v>a`; anything else reported, not guessed), `[IOConfig]` → `inputs`; `cryotemps.yaml` → `[cryo.setpoints]` (comma strings to arrays).
- [ ] Each mapping has a synthetic-cfg importer test; the NMGRL example in `configs/examples/nmgrl/` is regenerated and its diff reviewed.

### Task D2: Docs

- [ ] `docs/legacy_import.md`: the new mappings and what still falls back to sim.
- [ ] Survey C.6 coverage table updated as kinds land.
- [ ] Priorities doc: add this plan as an item with status.

### Task D3: Hardware bring-up (manual, per lab)

- [ ] For each driver: run against the real instrument with `trace = true`, commit the trace (hosts and serials scrubbed) as a codec fixture, and fix any reply the codec rejected. Order by lab availability; ldeo (XGS-600, Lakeshore 335, Qtegra UDP, Agilent) covers the most in one visit. AELAMS: confirm unit id, word order and coil numbering on the live PLC before the importer's defaults are trusted.

## Review focus

1. **Inversion and read-back** (Task 0.2): with `inverted` and/or `state_source`, the recorded state is the valve's, and two interlocked valves are never recorded open together. Property test in `test_switch_manager`.
2. **Modbus word order** (0.4, B7): a float read with the wrong word order is a plausible-looking wrong pressure, not an error. Test vectors for all four orders; bring-up checks one known value.
3. **Shared Qtegra endpoint** (0.3, A2, B2): a valve command never interleaves with an acquisition `GetData` on the wire.
4. **Gauge non-numbers** (B1–B6): no sentinel reaches `PressureSample`; every off/over-range reply becomes one Warning alarm.
5. **Cryo blocking** (C4): cancel and timeout both end the wait promptly; nothing waits forever.
6. **Range bands** (C3): every setpoint in the configured span selects exactly one band.
7. **Importer honesty** (D1): every legacy key not carried over is still listed in the report; nothing is guessed.
