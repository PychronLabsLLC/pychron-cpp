# Sim Serve, Phase B (Spectrometer on the Wire) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `elctl sim serve --spectrometer <file>` also serves an Isotopx NGX or Thermo Qtegra spectrometer whose readings come from a simulated beam fed by the served line's gas, so `exp run` and `pychron-ui` rehearse an instrument install end to end through their real transports.

**Architecture:** The existing wire emulators (`NgxSimModel`, `QtegraSimModel`, `libs/devices`) gain two seams each: a source for what they measure and an observer of the commands they answer. `libs/sim` binds those seams to a `BeamModel` (settings move the beam, data reads it) and lets `SimSystem` join line valve and gauge drivers to an emulator someone else owns. `LineServer` builds the beam from the spectrometer config, feeds it from the line (`feed_beam_from_line`), and serves each spectrometer transport; transports that reach one vendor PC (by address, or by a line `kind = "link"` transport borrowing the spectrometer's link) share one listener and one emulator. The endpoints file gains a spectrometer section; the assembler reads it.

**Tech Stack:** C++20, toml++, GoogleTest; the phase A pieces (`WireServer`, `LineServer`, endpoints file).

**Spec:** `docs/superpowers/specs/2026-10-09-sim-serve-design.md`, phase B (sections 5 and 6). Phase A plan: `docs/superpowers/plans/2026-10-29-sim-serve-phase-a.md`.

## Global Constraints

- Everything phase A's plan says (real time only, framing from the codec, no state file, loopback default, asio private, errors as values, one commit per task, quality check and compiler sweep before each, TSan for tasks adding threads or shared state).
- In-process sim unchanged: `ngx_sim_hook`, `qtegra_sim_hook` and `SimSystem::hook_for` answer as before when the new seams are empty; every existing test passes untouched.
- Lock order: emulator model mutex, then `BeamModel`'s, then `SimSystem`'s. A beam never calls back into an emulator; an observer seam is called with the model's mutex released.
- Read `libs/sim/AGENTS.md` and `libs/devices/AGENTS.md` before Tasks 1 to 3. A new wire command answered is no new driver command, so `test_repeat_tables.cpp` does not change.
- No test asserts an exact intensity over the wire (readings are at real instants); assertions compare a peak with gas against one without.

## Decisions (settle spec section 10's first point)

- **One Qtegra emulator per PC is small:** `QtegraSimModel` already answers valves (`on_valve`), gauges (`parameter_source`) and acquisition (`GetData`). Merging is giving one model all three seams: `SimSystem::join_qtegra` sets the line's, `feed_qtegra_from_beam` the spectrometer's. NGX likewise (`join_ngx`, `feed_ngx_from_beam`).
- **Who shares an emulator:** served transports whose config names one `host:port`, and a line transport of `kind = "link"` whose `link` is the link name of a spectrometer's `thermo_qtegra` or `isotopx_ngx` driver (its `link` option, else its driver name). Sharing is allowed only when every driver concerned is of one vendor (`thermo_qtegra`, `qtegra_valves`, `qtegra_gauges`; or `isotopx_ngx`, `ngx_valves`); otherwise phase A's refusal stands. Both transport names appear in the endpoints file with one port.
- **Endpoints file, spectrometer section:** `spectrometer = "<abs path>"` and `[spectrometer_transports.<name>]` beside `[transports.<name>]`, so a spectrometer transport may share a name with a line one. A file without it reads as before.
- **`--spectrometer`:** default the install's spectrometer file when `--install` names one that exists; `--no-spectrometer` serves the line alone. Field tables from `setup::field_table_root`, as `exp run` finds them.
- **Beam detectors:** each `[[detectors]]` channel's part after `:` (`ensure_detector`), as the sim drivers give their channels. HV starts at the source's `nominal_hv`, in the beam and in the Qtegra model.

## Review Focus

1. A Qtegra driver that sets HV, deflection or a source parameter before measuring: the reading follows (the observer applies each setting). Test in Task 3.
2. Two `GetData` within one integration period: values may repeat or not, but never error; the driver's own pacing holds. Test in Task 3 (two reads in a row both succeed).
3. NGX reconnect: a second connection gets the banner again (`on_connect` sets `banner_pending`). Test in Task 5.
4. A line valve on a `kind = "link"` transport to the NGX: opening it over the wire moves the served line's valve (it did not in-process). Test in Task 7.
5. A spectrometer file that is not the one the server serves: the client refuses it unless `--sim-endpoints-any`. Test in Task 4.

---

## File Structure

| File | Responsibility |
|---|---|
| `libs/devices/include/pychron/devices/spectrometer/thermo_qtegra_sim.hpp`, `src/spectrometer/thermo_qtegra_sim.cpp` | `data_source`, `on_command`. |
| `libs/devices/include/pychron/devices/spectrometer/ngx_sim.hpp`, `src/spectrometer/ngx_sim.cpp` | `values_at`, `on_command`. |
| `libs/sim/include/pychron/sim/sim_system.hpp`, `src/sim_system.cpp` | `join_qtegra`, `join_ngx`; `hook_for` uses them. |
| `libs/sim/include/pychron/sim/spectrometer/wire_beam.hpp`, `src/spectrometer/wire_beam.cpp` (new) | `feed_qtegra_from_beam`, `feed_ngx_from_beam`. |
| `libs/transport/include/pychron/transport/endpoints.hpp`, `src/endpoints.cpp` | Spectrometer section; `load_spectrometer_endpoints`. |
| `libs/systems/include/pychron/systems/line_server.hpp`, `src/line_server.cpp` | Spectrometer: beam, transports, shared emulators. |
| `apps/elctl/src/sim_serve.cpp`, `exp.cpp`; `apps/pychron-ui/src/main.cpp` | `--spectrometer`, `--no-spectrometer`; clients read the section. |
| Tests: `tests/devices/` (the Qtegra and NGX sim test files), `tests/sim/test_wire_beam.cpp` (new), `tests/sim/test_sim_system.cpp`, `tests/transport/test_endpoints.cpp`, `tests/systems/test_line_server.cpp`, `apps/elctl/tests/test_sim_serve_cmd.cpp`, `tests/integration/test_sim_serve_spectrometer.cpp` (new) | |

---

### Task 1: Emulator seams

**Files:** the two model headers and sources above; their existing test files under `tests/devices`.

**Interfaces:**
- Produces, on `QtegraSimModel`:
  ```cpp
  // When set (before use), GetData reports it (an ERROR reply when it is empty),
  // given the model's integration time; it wins over `intensities`.
  std::function<std::map<std::string, double>(double integration_s)> data_source;
  // When set (before use), called after each command was answered, outside
  // `mutex`, with its verb and arguments: for whoever follows the settings.
  std::function<void(const std::string& verb, const std::vector<std::string>& args)> on_command;
  ```
- On `NgxSimModel`:
  ```cpp
  // When set (before use), an ACQ line's values at its instant, in channel order; wins over `values`.
  std::function<std::vector<double>(TimePoint)> values_at;
  // As QtegraSimModel::on_command; `args` as the command had them, trimmed.
  std::function<void(const std::string& verb, const std::string& args)> on_command;
  ```
- [ ] Tests: `QtegraSim.DataSourceWinsOverIntensities` (source returns {"H1": 5}; GetData reply tags H1 = 5; empty map → ERROR), `QtegraSim.OnCommandSeesSettings` (SetMagnetDAC 4.2 → observer got ("SetMagnetDAC", {"4.2"}), model mutex not held: observer locks `model->mutex` without deadlock), `NgxSim.ValuesAtFeedsAcqLines` (values_at returns {1,2}; StartAcq 1 → ACQ line ends ",2,1#"), `NgxSim.OnCommandSeesSetMass`.
- [ ] Fail, implement, pass; existing devices tests unchanged.
- [ ] Commit `feat(devices): Qtegra and NGX emulators take a data source and a command observer`

### Task 2: `SimSystem::join_qtegra`, `join_ngx`

**Interfaces:**
```cpp
// The line's drivers answered by an emulator another owns (a spectrometer's
// PC): a qtegra_valves driver's Open/Close move its valves, a qtegra_gauges
// driver's parameters read its gauges; an ngx_valves driver's actuations move
// its valves. Chains with what the model was given before (two drivers on one
// model both follow). Other kinds: nothing.
void join_qtegra(spectrometer::QtegraSimModel& model, const config::DriverConfig& driver, const config::SystemConfig& system);
void join_ngx(spectrometer::NgxSimModel& model, const config::DriverConfig& driver, const config::SystemConfig& system);
```
`hook_for`'s `qtegra_valves`, `qtegra_gauges` and `ngx_valves` branches become a new model, a join, and the model's hook (NGX via `on_command`).
- [ ] Tests (`tests/sim/test_sim_system.cpp`): `JoinQtegraBothDriversOnOneModel` (a qtegra_valves and a qtegra_gauges driver joined to one model: Open moves the valve, GetParameter reads the gauge); `JoinNgxMovesValves`. Every existing SimSystem test passes.
- [ ] Commit `refactor(sim): line drivers join an emulator another owns`

### Task 3: Beam-fed emulators

**Files:** create `libs/sim/include/pychron/sim/spectrometer/wire_beam.hpp`, `src/spectrometer/wire_beam.cpp`; test `tests/sim/test_wire_beam.cpp`.

**Interfaces:**
```cpp
namespace pychron::sim {
// The Qtegra answers as the beam: SetMagnetDAC, SetHV, SetDeflection, SetGain,
// ProtectDetector, BlankBeam and SetParameter (hardware name -> codec::qtegra::canonical_name
// -> spectrometer::parse_param_id) move it; GetData reads every beam detector at
// the clock's now over the integration time. Sets model.hv and the beam's HV to
// the beam's nominal. The beam must outlive the model's use.
void feed_qtegra_from_beam(spectrometer::QtegraSimModel& model, std::shared_ptr<BeamModel> beam);
// The NGX: SetMass moves the beam's magnet to the mass (a mass axis, as the
// sim positioner's), SSO sets the source parameter its mnemonic names; ACQ values
// are `channels` read at the line's instant over one second.
void feed_ngx_from_beam(spectrometer::NgxSimModel& model, std::shared_ptr<BeamModel> beam,
                        std::vector<std::string> channels);
}
```
- [ ] Tests (ManualClock, real drivers over `SimTransport::hooked`, beam with default argon gas and `default_table_value`): `QtegraReadsThePeak` (driver moves to Ar40's DAC on H1 → frame's H1 > 100 × its value at DAC 0.1); `QtegraSettingsMoveTheBeam` (SetHV 4000, SetDeflection H1 10 reach the beam: `beam->hv()`, `beam->detector("H1")->deflection`); `QtegraTwoGetDataInARow` (Review Focus 2); `NgxReadsThePeak` (SetMass 39.96 then StartAcq 1: ACQ value for AX ≫ at mass 30).
- [ ] Commit `feat(sim): Qtegra and NGX emulators measure a simulated beam`

### Task 4: Endpoints file, spectrometer section

**Interfaces** (`endpoints.hpp`):
```cpp
struct SimEndpoints {
  ... // phase A fields
  std::filesystem::path spectrometer;                        // empty: none served
  std::map<std::string, Endpoint> spectrometer_transports;   // by the spectrometer file's transport names
};
// read_sim_endpoints, then the spectrometer section, refused (Config) when the
// file serves no spectrometer or another file than `loaded_spectrometer` (canonical), unless `any`.
Result<std::map<std::string, Endpoint>> load_spectrometer_endpoints(const std::filesystem::path& file,
    const std::filesystem::path& loaded_spectrometer, bool any);
```
TOML: top-level `spectrometer = "..."`, tables `[spectrometer_transports.<name>]` as `[transports.<name>]`.
- [ ] Tests: `SpectrometerSectionRoundTrips`; `NoSpectrometerSectionReadsAsBefore`; `LoadSpectrometerEndpointsRefusesOtherFile` (Review Focus 5) and `...RefusesFileServingNone`.
- [ ] Commit `feat(transport): the endpoints file serves a spectrometer too`

### Task 5: `LineServer` serves the spectrometer

**Interfaces:**
- `LineServerOptions`: `std::optional<spectrometer::cfg::SpectrometerData> spectrometer;` (loaded by the caller, field tables found).
- `ServedTransport`: `bool spectrometer = false;` and `std::string shares;` (the served transport whose listener it uses; empty: its own).
- `LineServer`: `std::shared_ptr<sim::BeamModel> beam() const;` (null without a spectrometer); `endpoints()` fills the spectrometer section (`config.source_file` of the spectrometer, canonical).

Building (in `start`, after the line sim):
1. Beam: `BeamModel(clock, beam_settings_from_config(data))`, `ensure_detector` for each `[[detectors]]` channel's part after `:`, then `feed_beam_from_line(beam, system)`; its error fails the start. A `FixedGas` result is a warn note (`the spectrometer is not joined to the line: <why>`, the UI's wording).
2. Groups: each served transport (line or spectrometer; kinds as phase A) is keyed by its config address; a spectrometer driver's link name also keys the line `kind = "link"` transports naming it. A group of more than one transport is allowed only under one vendor (Decisions); else Config error `sim serve: transports '<a>' and '<b>' both name <addr>; ...` (phase A wording) or `... '<a>' borrows the link of '<b>', which is not ...`.
3. Each group served once, under its first transport by name (line before spectrometer): a Qtegra group → one `QtegraSimModel` (`feed_qtegra_from_beam` if a `thermo_qtegra` is in it; `join_qtegra` for each line driver in it), served as `qtegra_sim_hook` with `codec::qtegra::request_spec()`; an NGX group → one `NgxSimModel` with `banner_pending` set by `on_connect` and `ngx_sim_events` as unsolicited, framing `ReadSpec::until(send_terminator)` of the NGX driver; anything else → phase A's `wire_for_transport` for line transports, a silent wire with the warn note for spectrometer transports.
- [ ] Tests (`LineServerSteady`): `ServesQtegraSpectrometer` (example `spectrometer.qtegra.toml` with its tables; served, emulator `thermo_qtegra`, endpoints spectrometer section has `qtegra`); `ServesNgxSpectrometer`; `QtegraLineAndSpectrometerShareOnePort` (a line `qtegra_valves` tcp transport at the spectrometer's address: both names, one port, `shares` set); `LinkTransportJoinsNgx` (line `kind = "link"`, `link = "ngx"` with an `ngx_valves` driver: listed as sharing `ngx`); `MixedVendorsOnOneAddressRefused`; `NgxBannerOnEveryConnection` (Review Focus 3: two raw `TcpTransport` connections each read the banner line first).
- [ ] TSan. Commit `feat(systems): LineServer serves the spectrometer, fed by the line's gas`

### Task 6: Clients and the command

- `elctl sim serve`: `--spectrometer <file>`, `--no-spectrometer`; default the install's spectrometer when it exists (`ExpGlobals::spectrometer`), tables by `setup::field_table_root` as `exp run`. Table and endpoints include the spectrometer's transports (`(spectrometer)` after the name, `shares <x>` where one shares).
- `exp run` and `pychron-ui`: with `--sim-endpoints`, the assembler's endpoints come from `load_spectrometer_endpoints(file, spectrometer file, any)` when the file has a section; without one, the line's map as now (phase A behaviour).
- [ ] Tests (`SimServeCmd`): `ServesTheSpectrometerWhenGiven` (hardware-style example line + `spectrometer.qtegra.toml` copy: endpoints file has the section); `NoSpectrometerLeavesItOut`.
- [ ] Commit `feat(elctl): sim serve serves the spectrometer`, `feat(ui): ...` if the UI change is separate.

### Task 7: Over the wire, end to end

**File:** `tests/integration/test_sim_serve_spectrometer.cpp` (`SimServeSpectrometerSteady`), fixture as phase A's integration test plus the example's Qtegra (then NGX) spectrometer file, its tables, and the client assembled with `load_spectrometer_for_app` and endpoints.
- [ ] `QtegraAirShotReadsAboveBlank`: client moves the magnet to Ar40 on the reference detector; a frame read with the source empty (blank), then air put in the source over the wire (open the pipette path through the client, or `server.system().inject` into the spectrometer volume when the example has no air path): Ar40 reading with air > 10 × blank's. No exact values.
- [ ] `NgxAirShotReadsAboveBlank`: likewise through `NgxSpectrometer`.
- [ ] `NgxLinkValveMovesTheServedLine` (Review Focus 4): line valve on a link transport to the NGX, actuated through the client, moves `server.system()`.
- [ ] TSan, ASan. Commit `test(integration): spectrometers acquire over sim serve`

### Task 8: Docs and checks

- `docs/simulator.md` "The simulated lab on the wire": spectrometer served, `--spectrometer`, shared PCs, the rows of kinds (`isotopx_ngx`, `thermo_qtegra`); remove "Not served yet: the spectrometer". `docs/installation_runbook.md` 2.4 step 3: rehearse `exp run` too. Spec: phase B implemented, section 10's two points settled (merge size: small, see Decisions; NGX poll: measured by Task 7's acquisition passing within its timeouts), endpoints spectrometer section in 4.5. `AGENTS.md` entry: one line on shared emulators and lock order.
- [ ] Quality check, whole-tree compiler sweep (and `--build-dir build/dev-ui --changed`), TSan and ASan on the changed suites, full `ctest --preset dev-ui`.
- [ ] Commit `docs: sim serve phase B`

## Open questions

None.
