# Spectrometer Control — Design

Date: 2026-09-29
Status: Approved (design), pending implementation plan
Depends on: `2026-09-29-instrument-control-design.md` (core, transport,
device_kit, config conventions, sim/testing conventions, UI boundary).
Scope: noble-gas mass spectrometer control — positioning, acquisition,
source/detector parameters, tuning jobs — for integrated vendor backends
(Thermo Qtegra, Isotopx NGX, later Nu, Pfeiffer Quadera) and legacy split
systems (MAP 215-50, VG 5400/1200/3600) where source, magnet and detectors
are separate devices on separate interfaces.

## 1. Intent

Pychron's spectrometer layer is vendor-software mediated end to end: every
complete backend (Qtegra, NGX firmware, Quadera) owns timing and integration,
all three vendor classes share one `microcontroller` transport, and the only
"direct" hardware code is a write-only serial DAC for MAP. Supporting legacy
instruments is therefore not a port; it is the new part of this design.

Requirements (owner):

- Thermo and Isotopx first; legacy systems must be cheap to add.
- Legacy systems have independent interfaces for source, magnet and detector
  measurement; the design must compose them, not special-case them.
- Milestone = full tuning suite: position, read, peak center, source and
  detector calibration, HV sweep, CDD voltage scan, coincidence.
- This spec stops at instrument primitives plus an acquisition engine.
  Multicollect / peak-hop / baseline sequencing belongs to the experiment
  engine spec, built on these primitives.
- Existing lab calibration files are imported once into new formats.

Design principle carried from the instrument-control spec: code owns rules
and hardware access; each unit is small, Qt-free, and fakeable in isolation.

## 2. Roles and composition

### 2.1 Role interfaces

`libs/devices/include/pychron/devices/spectrometer/`. Each is a capability
interface in the sense of the instrument-control spec section 4.4.

```cpp
struct IMassPositioner {
  enum class Axis { Dac, Field, Mass };              // what the hardware natively accepts
  virtual Axis           native_axis() const = 0;
  virtual Result<void>   set(double value) = 0;      // native units
  virtual Result<double> read() = 0;                 // may be cached for write-only hardware
  virtual Result<bool>   moving() { return false; }
  virtual Limits         limits() const = 0;
};

struct IBeamSource {
  virtual Result<void>   set_hv(double v) = 0;
  virtual Result<double> read_hv() = 0;
  virtual std::span<const ParamSpec> params() const = 0;   // supported params + vendor names
  virtual Result<void>     set_param(ParamId, double) = 0;
  virtual Result<Readback> read_param(ParamId) = 0;        // {setpoint, actual?}
};

struct IIntensityAcquirer {
  virtual std::vector<ChannelId> channels() const = 0;
  virtual bool integrates() const = 0;               // true: vendor integrates; false: raw samples
  virtual Result<void> configure(Duration integration) = 0;   // integrated mode: snaps to legal value
  virtual Result<void> start() = 0;
  virtual Result<void> stop() = 0;
  virtual Result<void> trigger() { return {}; }      // NGX StartAcq; no-op for polled backends
  virtual Result<std::optional<Frame>> next(Duration timeout) = 0;
};

struct IDetectorControl {
  virtual Caps caps() const = 0;                     // gain | deflection | protect | cdd_voltage
  virtual Result<void>   protect(ChannelId, bool) = 0;
  virtual Result<void>   set_deflection(ChannelId, double);
  virtual Result<double> read_deflection(ChannelId);
  virtual Result<void>   set_gain(ChannelId, double);
  virtual Result<double> read_gain(ChannelId);
  virtual Result<void>   set_cdd_voltage(ChannelId, double);
};

struct IBeamBlank { virtual Result<void> blank(bool) = 0; };
```

### 2.2 Two composition shapes, one `Spectrometer`

| | Integrated vendor (Qtegra, NGX, Quadera, Nu) | Legacy split (MAP-215, VG) |
|---|---|---|
| drivers | one driver implements all roles on one transport | one driver per role, each on its own transport |
| example | `QtegraSpectrometer : IMassPositioner, IBeamSource, IIntensityAcquirer, IDetectorControl, IBeamBlank` | `DacPositioner`, `AdcBank : IIntensityAcquirer`, `PulseCounter : IIntensityAcquirer`, `SerialHv : IBeamSource` |
| integration | `integrates() == true` | `integrates() == false`; host integrates |
| acquirers | exactly one | N, merged by timestamp |

`Spectrometer` (`libs/systems/spectrometer/`) is built by a
`SpectrometerAssembler` from config: it resolves role -> driver bindings,
validates that every configured detector maps to exactly one acquirer
channel, and constructs `DetectorSet`, `FieldTable`, `AcquisitionEngine`.
Managers, jobs and UI see only `Spectrometer`, never roles.

## 3. Acquisition engine

One `IntensityStream` contract for all consumers regardless of who
integrates.

### 3.1 Frame

```cpp
struct Frame {
  TimePoint ts;           // instrument time if provided (NGX), else host monotonic
  uint64_t  seq;          // acquirer-local sequence; gaps = dropped frames
  std::vector<std::pair<ChannelId, double>> values;   // raw units (V, fA, counts)
  bool      integrated;   // vendor already averaged over the integration period
  Duration  span;         // integration period (integrated) or sample period (raw)
};
```

### 3.2 Engine (`libs/systems/spectrometer/acquisition.hpp`)

- Owns 1..N acquirers; one Scheduler job per acquirer runs its `next()`
  loop (non-overlap guaranteed by the Scheduler).
- Integrated path: frame -> unit conversion -> `Reading` -> publish. The
  engine records the actual snapped integration time reported by
  `configure()` (Thermo binary series, NGX 1 s multiples) and exposes it.
- Host-integration path: raw frames are binned per channel to the requested
  integration time on the host clock; each bin emits mean, sigma, n. Counter
  channels sum counts, apply dead-time correction (`tau` per channel), and
  report cps. Bins align to a shared epoch so readings from a Faraday bank
  and a counter for the same period pair up.
- Merge: readings from multiple acquirers with |dt| <= half the integration
  time form one `Reading` row. A missing channel yields `nullopt` for that
  detector; the row is still emitted. Detectors are never silently dropped.
- `Reading = {ts, integration, {DetectorId -> Value{mean, sigma?, n?, saturated?}}}`
  in detector units after `software_gain`, using the detector -> channel
  binding from config. Published as `IntensityReading` on `SignalBus` and
  available via `IntensityStream` (bounded queue with `next(timeout)`) for
  synchronous consumers such as peak center.
- `acquire(n)` / `acquire(duration)`: `start()`, `trigger()` per period for
  NGX-style backends, returns after n readings or `cancel()`. Stale-frame
  guard: frames older than the request start are discarded.
- Health: no frame within `timeout_factor * integration + 3 s` ->
  `Error{Timeout}` on the stream and an `Alarm` event. Consumers decide.
- Saturation: engine flags `saturated` when a value exceeds the detector's
  configured `saturation`; `Spectrometer` applies protection policy. Policy
  is not in the engine.
- Push-fed backends (Quadera): the acquirer returns frames as they arrive.
  Recording to disk is a `SignalBus` subscriber, not a spectrometer method.

Dropped from pychron: `get_intensities` tuples `(keys, signals, t, inc)`,
the `NoIntensityChange` heuristic (replaced by seq/ts staleness), per-vendor
`read_intensities` overrides, `sink_data`.

## 4. Mass positioning, field table, corrections

### 4.1 Position request

```cpp
struct PositionTarget { std::variant<Isotope, Mass, NativeUnits> target; DetectorId on; };
struct PositionOptions { std::optional<Duration> settle; ProtectPolicy protect; bool wait_moving; bool confirmed; };
Result<PositionResult> Spectrometer::position(PositionTarget, PositionOptions = {});
```

### 4.2 Pipeline

`target -> mass -> table value -> corrections -> native -> IMassPositioner::set`

1. Isotope -> mass via `MolecularWeights` (config; default table ships).
2. Mass -> value via `FieldTable` for detector `on`, in the positioner's
   native axis.
3. Corrections, each toggleable, fixed order:
   - deflection: `+ sign * poly_det(deflection)` (per-detector polynomial
     from config)
   - HV: `* sqrt(HV_actual / HV_nominal)`, only for `Axis::Dac`; HV
     readback cached <= 1 s.
4. `IMassPositioner::set(native)` followed by the move protocol (4.4).

`uncorrect` is the exact inverse and is unit-tested as a round-trip.

### 4.3 FieldTable (`libs/systems/spectrometer/field_table.hpp`, pure)

- Per detector: control points `(isotope, mass, value)` and fit kind
  `discrete | linear | quadratic | cubic`.
- `value_for(mass, det)`: fit evaluated. `discrete` = nearest within
  0.15 amu, else `Error{Config}`. No silent fallback to the current position.
- `mass_for(value, det)`: bracketed numeric inverse over the table range.
- `update(det, isotope, new_value, propagate)`: shifts that isotope; with
  `propagate`, applies the same offset to every detector. Explicit flag,
  default from config.
- Persistence: `tables/<name>/<timestamp>.toml` plus `current` pointer;
  `FieldTableStore::restore(version)`. Importer for `mftable.csv`.
- Named tables (`ic`, HV) selected via `Spectrometer::with_table(name)`
  RAII scope.

### 4.4 Move protocol (`Spectrometer::move_native`)

Vendor-blind replacement for pychron's Thermo-only `set_dac`:

1. If any protected detector would see a beam above its
   `protection.threshold` along the path, or |delta| exceeds
   `beam_blank_threshold`: `protect(det, true)` for those detectors and
   `blank(true)` if an `IBeamBlank` is bound.
2. Optional AF demagnetization: decaying sinusoid of `set()` calls with
   `period`, `duration`, `start_amplitude`, `threshold` from config.
3. `set(target)`; if the positioner reports motion, poll `moving()` until
   false (bounded by `max_wait`); otherwise wait `settle` (skipped when
   |delta| < epsilon).
4. Unprotect and unblank in reverse order. On any failure, still attempt
   unprotect/unblank, then return the first error. A detector is never left
   protected by accident.
5. Publish `MagnetMoved{from, to, axis, mass_on_reference, elapsed}`.

Large-move confirmation (`confirmation_threshold_mass`) is a UI concern:
interactive callers pass `confirmed = true`; the core never blocks on a
dialog.

### 4.5 HV positioning

`Spectrometer::position_hv(mass, det)` uses the HV table and
`IBeamSource::set_hv` through the same pipeline shape.

Dropped: dual `dac`/`mass` properties with vendor-dependent meaning; Traits
change handlers driving hardware; `relative_position`; silent table-miss
fallback.

## 5. Source and detector parameter model

### 5.1 Canonical parameter registry

```cpp
enum class SourceParam { HV, TrapCurrent, TrapVoltage, Emission, ElectronEnergy, IonRepeller,
                         ExtractionLens, ExtractionFocus, ExtractionSymmetry, YSymmetry, ZSymmetry,
                         ZFocus, HorizontalSymmetry, Flatapole, RotationQuad, PoleN, PoleS,
                         ESAPlus, ESAMinus };
struct ParamSpec { ParamId id; Unit unit; Range range; bool readable, writable; std::string vendor_name; };
```

- Each `IBeamSource` advertises the subset it supports with its own vendor
  names (Qtegra `"Y-Symmetry Set"`, NGX `"YF"`, a legacy supply exposes only
  `HV`).
- `ParamId` also admits `Custom(name)` for vendor extras; UI shows them by
  vendor name.
- `Readback{setpoint, actual?}`; consumers never assume `actual` exists.

### 5.2 Detector model

```toml
[[detectors]]
name = "H1"   kind = "faraday"   channel = "qtegra:H1"   units = "fA"
software_gain = 1.0   isotope = "Ar40"   active = true
deflection = { control = true, correction = [0.0, 0.0012], sign = 1, max = 800, per_volt = 0.0031 }
protection = { threshold = 5e5, on_move = true }
saturation = 4.9e6

[[detectors]]
name = "CDD"  kind = "counter"   channel = "counter:0"   units = "cps"
dead_time_ns = 25   cdd_voltage = 1450
```

- `channel` binds detector -> acquirer channel; the assembler validates 1:1.
- `kind` in `{faraday, counter, cdd, atona}` selects host-integration math
  and applicable `IDetectorControl` caps.
- Runtime state (`active`, `isotope`, deflection/gain readbacks) lives in
  `DetectorSet` and is published as `DetectorState`; config is not mutated
  at runtime.

### 5.3 Profiles: apply and verify

- `SpectrometerProfile` = named desired values: source params, per-detector
  deflection/gain, CDD voltage, magnet settings, per-param tolerance
  (`abs` or `pct`). Stored under `profiles/<name>.toml`.
- `Spectrometer::apply(profile, ApplyOptions{ramp, verify})`:
  1. writes only params the bound driver declares writable; unsupported
     params are skipped with a warning, never an error;
  2. ramped params (trap current, HV) step at the configured rate through a
     generic `Ramper`;
  3. with `verify`, reads back each param and returns
     `VerifyReport{ok, mismatches[]}`.
- `Spectrometer::snapshot()` -> `SpectrometerState` (all readable params,
  detector states, magnet native value, integration time), content-hashed
  so persistence can dedupe.

### 5.4 Detector control ops

`set_deflection` (clamped to `max`), `set_gain`, `protect`,
`set_cdd_voltage`, each guarded by `caps()`. Unsupported ops return
`Error{Config}`; UI hides the controls.

Dropped: per-vendor Traits parameter classes; `config.cfg` sections
`[SourceOptics] [SourceParameters] [Deflections] [Protection]`;
`readout.yaml`.

## 6. Tuning jobs

Every job is a pure algorithm plus a thin runner.

```cpp
struct SweepPoint { double x; std::map<DetectorId, double> y; TimePoint ts; };
struct SweepSpec  { Axis axis; double start, stop, step; Duration integration; Duration settle;
                    std::vector<DetectorId> record; bool bidirectional = false; };
class Sweep { Result<std::vector<SweepPoint>> run(Spectrometer&, SweepSpec, Progress&, CancelToken&); };
```

`Axis` covers magnet, HV, a source parameter, and CDD voltage. One `Sweep`
serves every job; job code computes start/stop and fits the result.

| Job | Algorithm (pure) | Runner behavior |
|---|---|---|
| Peak center | `find_peak_center(points, percent, min_height) -> PeakShape{low, center, high, height, resolution, resolving_power_lo/hi}`; pychron's max / percent-crossing / edge guards ported with fixture scans | position to start, wait for signal to fall below tolerance (10 s cap), sweep, fit; on success `uncorrect` -> `FieldTable::update` -> position at center; retries with scaled window (`n_tries`); failure keeps points for UI |
| Coincidence | delta per detector -> deflection via `deflection.per_volt`, clamp to `max` | peak-centers reference then each active detector on the same isotope; proposes deflections; applying is a separate explicit call |
| HV sweep | same peak fit | `Sweep{axis = hv}`; writes HV table on request |
| CDD voltage scan | plateau detection -> `{start, end, recommended = start + 0.6 * width}` | `Sweep{axis = cdd_voltage}` on a counter at a fixed isotope |
| Deflection calibration | linear fit -> `dac_per_volt` | sweep deflection at fixed magnet; writes `per_volt` on request |
| Rise rate | linear slope per detector | `acquire(n)` at fixed position |
| IC / hop table | reuses peak center | for each (isotope, detector) pair under `with_table(name)` |

Job framework (`libs/systems/jobs/`): `Job{id, kind, spec, state, progress,
result}`; `JobRunner` runs one job at a time per spectrometer with queue
depth 1 (a second submit returns `Error{Interlock, "spectrometer busy"}`).
Jobs are the only way tuning touches hardware; UI and the future experiment
engine submit the same `JobSpec`s. Every job records a `SpectrometerState`
snapshot before and after. Results are `SignalBus` events; persistence is a
subscriber (separate spec).

Dropped: the `BaseSweep / MagnetSweep / AccelVoltageSweep` mixin lattice;
Traits-driven scanners; graph objects inside jobs.

## 7. Configuration and importer

Conventions from the instrument-control spec apply: one file, schema
validation, cross-references, `*.local.toml` for ports/hosts/credentials.

### 7.1 `spectrometer.toml` (integrated vendor)

```toml
[system]
name = "jan-argus"
reference_detector = "H1"
integration_time_s = 1.0

[transports.qtegra]
kind = "tcp"  host = "192.168.0.10"  port = 1069  timeout_ms = 2000

[drivers.qtegra]
kind = "thermo_qtegra"
transport = "qtegra"
roles = ["positioner", "source", "acquirer", "detector_control", "beam_blank"]

[magnet]
positioner = "qtegra"
native_axis = "dac"
limits = { min = 0.0, max = 10.0 }
settle_ms = 500
field_table = "argon"
hv_table = "argon_hv"
corrections = { deflection = true, hv = true }
protection = { detectors = ["CDD"], beam_blank_threshold = 0.5 }
af_demag = { enabled = false, period_s = 0.5, duration_s = 10, start_amplitude = 0.5, threshold = 0.5 }

[source]
driver = "qtegra"
nominal_hv = 4500
ramp = { TrapCurrent = 10.0, HV = 200.0 }

[acquisition]
acquirers = ["qtegra"]
stale_frame_guard = true
timeout_factor = 2.0

[detector_control]
driver = "qtegra"

[[detectors]]
# see section 5.2
```

### 7.2 Legacy split (binding differs, nothing else)

```toml
[transports.dac]      kind = "labjack_u3"
[transports.adc]      kind = "modbus_tcp"  host = "192.168.0.20"
[transports.counter]  kind = "serial"      port = "/dev/tty.usbserial-C"  baud = 115200
[transports.hv]       kind = "serial"      port = "/dev/tty.usbserial-H"  baud = 9600

[drivers.magnet_dac]  kind = "dac_positioner"  transport = "dac"      roles = ["positioner"]  channel = 0
[drivers.faradays]    kind = "adc_bank"        transport = "adc"      roles = ["acquirer"]    sample_hz = 100  channels = ["AX", "H1", "L1"]
[drivers.multiplier]  kind = "pulse_counter"   transport = "counter"  roles = ["acquirer"]    channels = ["EM"]
[drivers.spellman]    kind = "serial_hv"       transport = "hv"       roles = ["source"]

[magnet]       positioner = "magnet_dac"  native_axis = "dac"
[source]       driver = "spellman"
[acquisition]  acquirers = ["faradays", "multiplier"]  host_integration = true  bin_epoch = "shared"
# no [detector_control]: UI hides deflection/gain/protect
```

### 7.3 Assembler validation (all errors collected)

- every referenced role exists on the named driver's `roles`
- each detector `channel` resolves to exactly one acquirer channel; every
  acquirer channel is bound or listed in `acquisition.ignored_channels`
- `field_table` exists and has a column for every active detector in the
  positioner's native units
- `protection.detectors` is a subset of detectors with `protection` config;
  `[detector_control]` is present when any detector enables deflection
  control or protection
- `native_axis = "mass"` positioners cannot enable the HV correction

### 7.4 Data directory

```
spectrometer/
  spectrometer.toml
  tables/<name>/<timestamp>.toml, tables/<name>/current
  profiles/<name>.toml
  molecular_weights.toml
```

`tables/*.toml`: `fit = "quadratic"`, `axis = "dac"`,
`[[points]] isotope = "Ar40" mass = 39.962 H2 = 5.123 H1 = 5.001 ...`.

### 7.5 Importer `tools/pychron_spectrometer_import.py`

One-shot: `detectors.yaml|.cfg` -> `[[detectors]]`; `mftable.csv`,
`ic_mftable.csv`, `avftable` -> `tables/`; `config.cfg` sections plus
`readout.yaml` -> `profiles/imported.toml` and `[magnet]`;
`deflections/<det>` -> `per_volt` and `correction`; device `.cfg`
`[Communications]` -> `[transports.*]`; `af_demagnetization.yaml` ->
`[magnet.af_demag]`. Anything unmapped is reported, not guessed.

Dropped: `.hidden/spectrometer_config_name`, preferences-held credentials,
`molecular_weights` csv, duplicate deflection sections.

## 8. Simulation and testing

### 8.1 BeamModel (`libs/sim/spectrometer/beam_model.hpp`)

- State: magnet native value, HV, source params, per-detector deflection and
  geometry offset, protection flags, beam blank.
- Gas: `isotope -> abundance` with optional exponential decay/growth per
  isotope; baseline at mass 34.2 is zero plus noise.
- Peak shape per detector: flat-top trapezoid centered at
  `table_value(mass, det)` shifted by deflection, scaled by
  `sqrt(HV / HV_nominal)`; source params scale sensitivity (trap current)
  or shift/skew the shape (symmetries) so tuning jobs have something to
  optimize.
- `intensity(det, t)` sums isotopes x shape x sensitivity x gain plus noise
  (Gaussian for Faraday, Poisson with dead time for counters), clamps at
  saturation, and flags an unprotected counter that sees a large beam.
- Deterministic RNG per test; injected `Clock`.

### 8.2 Sim drivers (all bound to one BeamModel)

| kind | roles | mimics |
|---|---|---|
| `sim_integrated` | all five | integrated vendor box; `moving()` true for a configurable time |
| `sim_dac_positioner` | positioner | write-only DAC with cached readback |
| `sim_adc_bank` | acquirer | raw frames at `sample_hz` |
| `sim_pulse_counter` | acquirer | raw count frames |
| `sim_hv_supply` | source | HV only |

Two example configs ship, `spectrometer.sim-integrated.toml` and
`spectrometer.sim-legacy.toml`; the whole test matrix runs against both.

### 8.3 Test tiers

```
tests/codecs/spectrometer/     Qtegra, NGX (event demux), Quadera framing; legacy HV/DAC/ADC fixtures
tests/devices/spectrometer/    each driver vs SimTransport scripted + replay; conformance suite
tests/systems/spectrometer/
  field_table_test        fits, inverse round-trip, update/propagate, discrete miss, versioning
  corrections_test        correct/uncorrect inverse across axes; HV correction disabled for mass axis
  move_protocol_test      protect/unprotect ordering incl. failures; AF demag trajectory; settle skip
  acquisition_test        pass-through; host binning; multi-acquirer merge; missing channel; stale guard;
                          timeout -> Alarm; dead-time correction
  params_test             vendor mapping; apply skips unsupported; ramp steps; verify tolerances
  jobs/peak_center_test   pure fit on fixture scans (tests/data/scans/*.csv) + BeamModel end to end
  jobs/coincidence_test   proposals within clamp; reference unchanged
  jobs/cdd_scan_test      plateau detection on synthetic + fixture
  assembler_test          one failing fixture per validation rule
tests/integration/spectrometer/
  both sim configs: load -> apply profile -> position Ar40 on H1 -> acquire 10 -> peak center
  -> table updated -> re-position lands on center
```

- Property-style: random position/protect/blank sequences never leave a
  detector protected after an error; random table updates keep
  `mass_for(value_for(m))` within tolerance of `m`.
- Timing tests use `ManualClock` + `Scheduler::run_pending()`; no sleeps.
- Conformance suite (`tests/devices/spectrometer/conformance.hpp`): templated
  gtest suite any new role driver instantiates (positioner contract, acquirer
  seq/ts monotonic, source readback shape). Adding a legacy instrument = a
  driver plus one suite instantiation.
- Hardware traces captured with `elctl trace on` at first Qtegra/NGX
  bring-up are committed under `tests/traces/<vendor>/` and replayed in CI.

## 9. Milestones, CLI, UI

### 9.1 Definition of done

Thermo Argus (Qtegra) and Isotopx NGX drive from `spectrometer.toml`; the
MAP-style legacy composition runs under `sim-legacy` with identical behavior;
every tuning job runs from UI and CLI; identical results under sim.

### 9.2 Stages (each green in CI before the next)

| Stage | Units | Depends on |
|---|---|---|
| S1 position + read | `spec_roles` (role interfaces, Frame, DetectorSet, param registry), `field_table`, `acquisition_engine`, `spectrometer_core` (assembler, pipeline, move protocol), `sim_beam` (BeamModel + sim drivers), `spec_config` | instrument-control `core`, `transport`, `device_kit` |
| S2 vendors | `thermo_qtegra`, `isotopx_ngx`, `legacy_kit` (`dac_positioner`, `adc_bank`, `pulse_counter`, `serial_hv`) | S1 |
| S3 peak center | `jobs_framework` (Job, JobRunner, Sweep), `peak_center`, `hv_sweep` | S1 |
| S4 calibration | `profiles_apply_verify`, `coincidence`, `deflection_cal`, `cdd_scan`, `ic_table_gen`, `rise_rate` | S2, S3 |
| S5 tooling | `spec_importer` (Python), `elctl spec-*`, `spectrometer_ui` | S4 |
| S6 bring-up | Argus and NGX traces captured and committed (manual) | S5 |

Nu Noblesse, VG 5400/1200/3600 and Quadera codecs follow after; `legacy_kit`
plus the conformance suite is what makes them cheap.

### 9.3 `elctl` additions

`spec validate`, `spec state`, `spec position <iso|mass> --on <det>`,
`spec read --n N --integration S`, `spec params [get|set]`,
`spec apply <profile> [--verify]`, `spec table [show|import|restore]`,
`spec job peak-center|coincidence|cdd-scan|hv-sweep|deflection-cal|rise-rate`,
`spec sim` (BeamModel REPL).

### 9.4 UI (`apps/pychron-ui`, via `CoreBridge` only)

- Readout dock: live per-detector intensities with sigma when
  host-integrated, integration selector showing the snapped actual, active
  toggles, protect badges, stale indicator.
- Magnet panel: position by isotope/mass/native, current value on the
  reference detector, big-move confirmation dialog, table selector.
- Scan view: live sweep plot with switchable x axis, peak-center fit overlay
  (low/center/high, resolution), history of recent scans.
- Parameters panel: generated from `params()`; setpoint, readback, tolerance
  status; profile pick with Apply / Verify and a mismatch list.
- Detectors panel: deflection/gain/CDD voltage editors (hidden when caps are
  absent), coincidence proposal table with per-row accept.
- Jobs dock: queue, progress, cancel, result summaries; reused later by the
  experiment engine.
- Plotting library (Qt Charts or QCustomPlot) decided in the implementation
  plan.

### 9.5 Deferred

Measurement sequencing (experiment spec); persistence of scan and peak-center
results (persistence spec; they are `SignalBus` events here); Nu, VG and
Quadera codecs; automatic full-table generation across all isotopes; source
auto-tune.

## 10. Open decisions carried into the plan

Non-blocking: exact `ParamId` representation for `Custom`; whether
`IntensityStream` is a ring buffer or an unbounded queue with backpressure
metrics; plotting library; whether `legacy_kit` ships `labjack_u3` (M2 of the
instrument-control spec) or a `sim_dac_positioner` only in S2.
