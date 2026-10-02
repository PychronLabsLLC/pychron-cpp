# Thermo Qtegra Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `thermo_qtegra` spectrometer driver (positioner, source, acquirer, detector control, beam blank on one TCP transport) plus the infrastructure it needs: opened and reconnectable spectrometer transports and an acquisition engine that is safe to restart.

**Architecture:** Infrastructure first (engine quiescence, transport keys, `IConnectable` + assembler open/connect/close, `Reconnector`), then one `QtegraSpectrometer` device built on the merged `pychron::codec::qtegra` codec, tested against a stateful sim hook, then system tests through `Spectrometer` and `ScanService`.

**Tech Stack:** C++20, existing transport/devices/codecs/systems libraries, GoogleTest, `SimTransport` (scripted, hooked, replay), `ManualClock`.

**Spec:** `docs/superpowers/specs/2026-10-01-qtegra-driver-design.md`

## Global Constraints

- All wire text comes from `pychron::codec::qtegra` (`libs/codecs/include/pychron/codecs/thermo_qtegra.hpp`). The driver never formats or parses command strings itself. If the codec lacks something, add it to the codec with a byte-fixture test.
- Only commands in spec section 5.2 are sent. Never send `SetIonCounterVoltage`, `Reset`, `SetSubCupConfiguration`, or the `SetParameter ProtectDetector,...` form.
- Config errors (limits, unknown channel or parameter, non-positive integration) never touch the wire: tests assert nothing was written.
- Driver methods return through `Device::observe(...)`; `Error::device` is the driver name. Follow `libs/codecs/CONVENTIONS.md` "Driver side".
- No sleeps in tests: timing uses `ManualClock`. Acquirer `seq` is never reset on start (conformance `RestartsAfterStop`).
- `Frame::ts` is the injected Clock's time (steady-clock domain), never wall time.
- Registry kind `thermo_qtegra`; declared keys exactly as spec 5.1: `roles`, `channels` (default `H2 H1 AX L1 L2 CDD`), `limit_min` 0, `limit_max` 10, `terminator` (`cr`|`lf`|`crlf`, default `cr`), `settle_periods` 2.0.
- Ranges: HV 0..10000 V; every other source parameter `-1e6..1e6`, `Unit::None`, documented as unverified.
- `libs/*` stays Qt-free. `PYCHRON_WARNINGS_AS_ERRORS=ON`.
- Sources and tests in `libs/devices/CMakeLists.txt` and `tests/devices/CMakeLists.txt` are listed explicitly; `libs/systems` and `tests/systems`, `tests/integration` are globbed.
- Verify with `cmake --build --preset dev --parallel && ctest --preset dev --output-on-failure`; the last task also runs `dev-ui`.
- One commit per task, repo style (`feat(spectrometer): ...`, `feat(devices): ...`, `fix(spectrometer): ...`), ending with the trailer the implementing agent's own attribution guidance names.

## Review Focus

1. `GetData` replies an instrument could plausibly send that the spec does not list: empty reply, trailing comma, duplicate detector name, a detector value of `nan`/`inf`, lower-case names. Expected: a clear `Protocol` error or a documented tolerant parse, never a crash or a silently wrong channel (Task 6).
2. Transport open succeeds but the instrument never answers `GetIntegrationTime`: load fails with a timeout naming the driver, and every transport opened so far is closed (Task 3).
3. A reconnect while a scan is running: the acquirer reports the error, the scan status shows it, Restart recovers, and nothing deadlocks (Tasks 4, 7).
4. `stop()` called while `next()` is blocked in a slow transport read: `stop()` returns once that read ends and the acquirer is left stopped, with no frame delivered after stop (Tasks 1, 6).
5. A config that names `thermo_qtegra` but whose `[magnet].limits` disagree with the driver's `limit_min`/`limit_max`: the stricter bound wins or the load is refused with a clear diagnostic, never a silent out-of-range DAC write (Task 7).

---

### Task 1: Acquisition engine quiescence

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/acquisition.hpp`, `libs/systems/src/spectrometer/acquisition.cpp`
- Test: `tests/systems/test_acquisition.cpp` (append)

**Interfaces:**
- Produces: `AcquisitionEngine::stop()` returns only when no `poll()` is executing (spec 4.4). Each poll registers itself for its duration; `stop()` clears `running_`, cancels the scheduler jobs, waits until the active-poll count is zero, then calls each acquirer's `stop()`. A `stop()` invoked from inside a poll on the same thread does not wait for itself. No public signature changes.

- [ ] **Step 1: Write failing tests** using a fake acquirer whose `next()` blocks on a latch (extend `tests/systems/spectrometer_fakes.hpp` if needed) and a `Scheduler` with 2 threads, started:
  - `StopWaitsForInFlightNext`: start; wait until `next()` is entered; call `stop()` on another thread; assert `stop()` has not returned after 50 ms of real time; release the latch; `stop()` returns; the acquirer's `stop()` was called after `next()` returned (record an order log).
  - `StartAfterStopNeverOverlapsPreviousNext`: with the same fake, `stop()` then `start()`; assert the fake never observed `configure()`/`start()` while a `next()` was active (the fake counts overlaps; expect 0) across 50 iterations.
  - `StopFromInsidePollDoesNotDeadlock`: a fake whose `next()` calls `engine.stop()`; the test finishes within 2 s.
  - `StopWithNothingRunningIsImmediate`.
- [ ] **Step 2: Run** `ctest --preset dev -R Acquisition` → the first two FAIL.
- [ ] **Step 3: Implement** with a mutex, condition variable and active count; record the polling thread id to detect self-stop.
- [ ] **Step 4: Run** `ctest --preset dev -R "Acquisition|ScanService|Spectrometer"` → PASS; repeat `-R Acquisition --repeat until-fail:10`.
- [ ] **Step 5: Commit** `fix(spectrometer): engine stop waits for an in-flight poll`.

### Task 2: `retries` and `trace` keys on spectrometer transports

**Files:**
- Modify: `libs/systems/include/pychron/systems/spectrometer/config.hpp` (`cfg::TransportConfig` gains `std::int64_t retries = 0; bool trace = false;`), `libs/systems/src/spectrometer/config_loader.cpp` (`parse_transport` allowed keys), `libs/systems/include/pychron/systems/spectrometer/spectrometer.hpp` (`SpectrometerOptions` gains `std::filesystem::path trace_dir = "traces";`), `libs/systems/src/spectrometer/assembler.cpp` (`default_transport` passes `retries`, `trace` and `trace_dir`, creating the directory on demand as `extraction_line.cpp` does)
- Test: `tests/systems/test_spectrometer_config.cpp`, `tests/systems/test_spectrometer_assembler.cpp` (append)

**Interfaces:**
- Produces: `cfg::TransportConfig::retries`, `::trace`; `SpectrometerOptions::trace_dir`. `*.local.toml` still may override only `host`, `port`, `baud`, `timeout_ms`.

- [ ] **Step 1: Write failing tests:** `TransportRetriesAndTraceParsed`; `TransportRetriesDefaultZeroTraceFalse`; `NegativeRetriesIsDiagnostic`; `TraceMustBeBoolean`; `LocalFileCannotSetRetriesOrTrace`; assembler `TraceKeyWritesTraceFile` (sim transport with `trace = true`, `trace_dir` in a temp directory → `<dir>/<transport>.trace` exists after assembly); `TraceDirCreationFailureIsIoError`.
- [ ] **Step 2: Run** `ctest --preset dev -R "SpectrometerConfig|SpectrometerAssembler"` → new tests FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset dev` → PASS.
- [ ] **Step 5: Commit** `feat(spectrometer): retries and trace keys on spectrometer transports`.

### Task 3: `IConnectable`; assembler opens, connects and closes transports

**Files:**
- Create: `libs/devices/include/pychron/devices/connectable.hpp`
- Modify: `libs/systems/src/spectrometer/assembler.cpp`, `libs/systems/src/spectrometer/spectrometer.cpp` (destructor), headers as needed
- Test: `tests/systems/test_spectrometer_assembler.cpp` (append)

**Interfaces:**
- Produces:
  ```cpp
  namespace pychron {
  struct IConnectable {
    virtual ~IConnectable() = default;
    virtual Result<void> connect() = 0;   // after every transport open; blocking
  };
  }
  ```
  `SpectrometerAssembler::assemble` final step: `open()` each transport in config order, then `connect()` each device that is `IConnectable` (via `capability<IConnectable>`), in driver config order. First failure: close every transport opened so far, return the error with `Error::device` set to the transport or driver name if empty. `Spectrometer`'s destructor body stops the engine and then closes its transports, before any member (devices, transports) is destroyed.

- [ ] **Step 1: Write failing tests** with injected `TransportMaker` / `DriverMaker` (see existing assembler tests) and `SimTransport::fail_open_next()`:
  - `AssembleOpensEveryTransport` (each sim transport reports open afterwards).
  - `OpenFailureClosesOpenedTransportsAndReturnsError` (second of two transports fails → first is closed, error kind `Io`, names the transport).
  - `ConnectCalledAfterOpenInDriverOrder` (fake device implementing `IConnectable` records order and that its transport was open).
  - `ConnectFailureClosesTransportsAndReturnsError` (Review Focus 2: fake returns `Timeout`; error names the driver).
  - `DestructorClosesTransports`.
  - `ExistingSimConfigsStillAssemble` (both example sim configs).
- [ ] **Step 2: Run** `ctest --preset dev -R SpectrometerAssembler` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset dev` and `ctest --preset dev-ui` → PASS (UI and integration tests build spectrometers through the assembler).
- [ ] **Step 5: Commit** `feat(spectrometer): open transports and connect drivers at assembly`.

### Task 4: `Reconnector`

**Files:**
- Create: `libs/devices/include/pychron/devices/reconnect.hpp`, `libs/devices/src/reconnect.cpp`
- Modify: `libs/devices/CMakeLists.txt`, `tests/devices/CMakeLists.txt`
- Test: `tests/devices/test_reconnect.cpp`

**Interfaces:**
- Produces (namespace `pychron`):
  ```cpp
  class Reconnector {
   public:
    Reconnector(Transport& transport, const Clock& clock, Duration min_interval = std::chrono::seconds(1));
    // Runs `op`. On Io or NotConnected, and if `min_interval` has passed since
    // the last attempt (or there was none), closes and reopens the transport,
    // runs `on_connect`, then runs `op` once more. Other errors pass through.
    template <class T>
    Result<T> run(const std::function<Result<T>()>& op, const std::function<Result<void>()>& on_connect);
    std::uint64_t reconnects() const noexcept;   // successful reopen + on_connect
  };
  ```
  Thread-safe: concurrent `run` calls perform at most one reconnect between them (one mutex around the reconnect step only, never around `op`).

- [ ] **Step 1: Write failing tests** on `SimTransport` with `ManualClock`: `SuccessPassesThroughWithoutReconnect`; `IoTriggersReopenOnConnectAndRetry` (op fails once with Io then succeeds → result ok, `reconnects() == 1`, on_connect called once); `NotConnectedAlsoTriggers`; `ProtocolAndTimeoutErrorsPassThrough` (no close/open); `RateLimitedWithinMinInterval` (second Io 0.5 s later returns the original error without touching the transport; after advancing past 1 s it reconnects again); `FailedReopenReturnsItsError` (`fail_open_next`); `FailedOnConnectReturnsItsError`; `RetryFailureReturnsRetryError`; `VoidResultSupported`; `ConcurrentRunsReconnectOnce` (4 threads, op fails with Io until a reopen happened).
- [ ] **Step 2: Run** `ctest --preset dev -R Reconnector` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset dev -R Reconnector --repeat until-fail:5` → PASS.
- [ ] **Step 5: Commit** `feat(devices): Reconnector for reconnect-on-demand drivers`.

### Task 5: `QtegraSpectrometer`: device, registry, connect, positioner, beam blank, detector control, sim model

**Files:**
- Create: `libs/devices/include/pychron/devices/spectrometer/thermo_qtegra.hpp`, `libs/devices/src/spectrometer/thermo_qtegra.cpp`, `libs/devices/include/pychron/devices/spectrometer/thermo_qtegra_sim.hpp`, `libs/devices/src/spectrometer/thermo_qtegra_sim.cpp`
- Modify: `libs/devices/CMakeLists.txt`, `tests/devices/CMakeLists.txt`
- Test: `tests/devices/spectrometer/test_thermo_qtegra.cpp`

**Interfaces:**
- Consumes: `IConnectable` (Task 3), `Reconnector` (Task 4), `codec::qtegra::*`, `Device`, `DriverArgs`, `legacy::kRolesKey`, role interfaces in `roles.hpp`.
- Produces (namespace `pychron::spectrometer`):
  ```cpp
  struct QtegraOptions {
    std::vector<ChannelId> channels{"H2", "H1", "AX", "L1", "L2", "CDD"};
    Limits limits{0.0, 10.0};
    codec::qtegra::Terminator terminator = codec::qtegra::kDefaultTerminator;
    double settle_periods = 2.0;
  };
  class QtegraSpectrometer final : public Device, public IConnectable, public IMassPositioner, public IBeamSource,
                                   public IIntensityAcquirer, public IDetectorControl, public IBeamBlank {
   public:
    QtegraSpectrometer(std::string name, Transport& transport, QtegraOptions options, const Clock* clock = nullptr);
    static DriverSchema schema();
    static Result<std::unique_ptr<QtegraSpectrometer>> create(const DriverArgs& args);
    std::uint64_t reconnects() const noexcept;
    // + every role method
  };
  // Stateful simulated wire.
  struct QtegraSimModel {            // all access under its own mutex
    double dac = 0.0;  TimePoint moving_until{};  Duration move_time{};
    bool blank = false;
    std::map<std::string, bool> protect;  std::map<std::string, double> deflection, gain;
    double hv = 0.0;  std::map<std::string, double> params;   // by hardware name
    double integration_s = 1.048576;
    std::map<std::string, double> intensities;   // by detector name
    std::string data_override;                   // when non-empty, GetData replies with this verbatim
    const Clock* clock = nullptr;
  };
  SimTransport::Hook qtegra_sim_hook(std::shared_ptr<QtegraSimModel> model);
  ```
  In this task the source and acquirer methods exist and compile but return `Config` "not implemented" (Task 6 replaces them); mark them with a comment naming Task 6. `REGISTER_DRIVER("thermo_qtegra", QtegraSpectrometer);`

- [ ] **Step 1: Write failing tests** (scripted via `legacy_test::open_scripted` / `step` from `tests/devices/spectrometer/legacy/sim_util.hpp`, plus hooked where state matters):
  - Registry: `CreatesFromTomlTable`; `SchemaListsDeclaredKeys` (exactly the six keys); `UndeclaredKeyRejected`; `BadTerminatorRejected`; `InvertedLimitsRejected`; `EmptyOrDuplicateChannelsRejected`.
  - connect: `ConnectSendsGetIntegrationTime` (`"GetIntegrationTime\r"` → `"1.048576\r\n"`); `ConnectNonNumericReplyIsProtocol`; `ConnectTimeoutIsReturned`.
  - Positioner: `SetSendsSetMagnetDac` (`"SetMagnetDAC 5.001\r"`); `SetOutsideLimitsIsConfigAndWritesNothing`; `ReadParsesGetMagnetDac`; `MovingUsesBoolVocabulary` (`True`, `false`, `1`); `NativeAxisIsDac`; `LimitsFromOptions`.
  - Beam blank: `BlankSendsTrueAndFalse` (`"BlankBeam True\r"`, `"BlankBeam False\r"`).
  - Detector control: `CapsAreGainDeflectionProtect`; `ProtectSendsMagnetMoveForm` (`"ProtectDetector CDD,On\r"`, `...,Off`); `DeflectionRoundTrip`; `GainRoundTrip`; `UnknownChannelIsConfigAndWritesNothing`; `SetCddVoltageIsUnsupportedConfig`.
  - Terminator: `LfTerminatorUsedWhenConfigured`.
  - Errors: `ProtocolErrorCarriesDeviceName`; `ErrorReplyIsProtocol` (`"ERROR: bad\r\n"`).
  - Reconnect: `IoErrorReconnectsRunsConnectAndRetries` (hooked sim that drops once; `reconnects() == 1`).
  - Sim hook: `SimHookAnswersPositionerBlankAndDetectorCommands`.
- [ ] **Step 2: Run** `ctest --preset dev -R Qtegra` → FAIL (missing header).
- [ ] **Step 3: Implement.** One private `exchange(Result<codec::Command>)` helper does encode-check, `Reconnector::run` around `transport_.exchange(cmd.tx, *cmd.reply)`, returning the raw reply for the caller to decode.
- [ ] **Step 4: Run** `ctest --preset dev -R Qtegra` → PASS; full `ctest --preset dev` → PASS.
- [ ] **Step 5: Commit** `feat(devices): Qtegra driver core (positioner, blank, detector control)`.

### Task 6: `QtegraSpectrometer`: source, acquirer, conformance, replay

**Files:**
- Modify: `libs/devices/src/spectrometer/thermo_qtegra.cpp`, `.hpp`, `thermo_qtegra_sim.cpp`
- Create: `tests/devices/spectrometer/test_thermo_qtegra_acquire.cpp`, `tests/devices/spectrometer/test_thermo_qtegra_conformance.cpp`, `tests/traces/thermo/qtegra_session.trace` (header comment `# SYNTHETIC: hand-written from pychron Python, not a bench capture`)
- Modify: `tests/devices/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 5's class and sim model; `tests/devices/spectrometer/conformance.hpp` harness contract (`positioner()`, `acquirer()`, `source()`, `detector_control()`, `channel()`, `advance()`, `timeout()`).
- Produces: the source and acquirer behaviour of spec 5.2, verbatim:
  - Source: `set_hv` → `SetHV v` expecting `ok`; `read_hv` → `GetHighVoltage`; `params()` from `codec::qtegra::param_names()` with the ranges in Global Constraints; `set_param` → `SetParameter <hardware>,v` expecting `ok`; `read_param` → `Readback{GetParameter <hardware>, GetParameter <readback> if the map has one}`; `Custom(name)` read/write through the same commands; unadvertised canonical ids are `Config`.
  - Acquirer: `integrates()` true; `configure` snaps with `snap_integration_time`, sends `SetIntegrationTime` only when the snapped value changed, and then sets next-due to `now + settle_periods * snapped`; `start`/`stop` local; `next` returns `Config` when not started, nullopt with no wire traffic when not due (waiting on the clock up to `timeout`), else one tagged `GetData`; next-due advances by one period, resetting to `now + period` when more than one period behind; frame `ts` = clock at reply, `seq` incremented per attempt and never reset, `integrated = true`, `span` = snapped period, values only for configured channels the reply names.

- [ ] **Step 1: Write failing tests:**
  - Source (in `test_thermo_qtegra.cpp`): `SetHvExpectsOk`; `SetHvNonOkIsProtocol`; `ReadHv`; `ParamsCoverCodecMapWithNominalRanges`; `SetParamUsesHardwareName` (`"SetParameter Trap Current Set,200\r"` or the codec's exact text for `trap_current`); `ReadParamWithReadbackName` (two exchanges → setpoint and actual); `ReadParamWithoutReadbackNameHasNoActual`; `CustomParamRoundTrip`; `UnadvertisedParamIsConfig`; `OutOfRangeHvIsConfigAndWritesNothing`.
  - Acquirer (`test_thermo_qtegra_acquire.cpp`, hooked sim + `ManualClock`): `NextBeforeStartIsConfig`; `ConfigureSnapsAndSendsOnlyOnChange` (1.0 s → `SetIntegrationTime 1.048576`; same again → nothing written); `NonPositiveIntegrationIsConfig`; `NotDueReturnsNulloptWithoutTraffic` (zero timeout; `written()` unchanged); `FirstFrameAfterChangeWaitsSettlePeriods` (due at 2 × 1.048576 s); `FramesOnFixedCadenceWithoutDrift` (advance by irregular steps; frame count over 20 periods is 20 and due times are multiples of the period); `MoreThanOnePeriodBehindResetsCadence` (no burst of frames); `FrameShape` (`integrated`, `span`, host `ts`, increasing `seq`); `SeqNotResetAcrossStopStart`; `MissingDetectorIsAbsentNotError`; `ExtraDetectorIgnored`; `ErrorReplyIsProtocolAndConsumesSeq`; `NonNumericValueIsProtocol`; `StopDuringBlockedNextLeavesStopped` (Review Focus 4: sim `delay_next`; `stop()` from another thread; no frame after stop).
  - Review Focus 1 (`GetDataEdgeReplies`, table-driven through `QtegraSimModel::data_override`): empty reply, trailing comma, duplicate name, `nan`, `inf`, lower-case names → each either `Protocol` or the documented tolerant result (decide per case from the codec's `decode_data` behaviour, state the rule in a comment above the table, and keep it consistent with the codec tests); none crashes or maps a value to the wrong channel.
  - Conformance (`test_thermo_qtegra_conformance.cpp`): a harness owning a hooked `SimTransport`, `QtegraSimModel`, `ManualClock` and the driver (connected, acquirer configured at the minimum period); `advance()` steps the clock one period; instantiate `PositionerConformance`, `AcquirerConformance`, `SourceConformance`, `DetectorControlConformance`.
  - Replay: `ReplaysSyntheticSession` drives connect, a magnet move, `GetData` and an HV read against `SimTransport::replay(PYCHRON_TRACE_DIR "/thermo/qtegra_session.trace")`, and `expect_verified`.
- [ ] **Step 2: Run** `ctest --preset dev -R Qtegra` → new tests FAIL.
- [ ] **Step 3: Implement.** All acquirer state under one mutex; the wire read happens outside the mutex only if `stop()` can then still mark the acquirer stopped and suppress the frame.
- [ ] **Step 4: Run** `ctest --preset dev` → PASS; `-R Qtegra --repeat until-fail:5`.
- [ ] **Step 5: Commit** `feat(devices): Qtegra source and polled acquirer, conformance and replay`.

### Task 7: System tests, example config, docs, units

**Files:**
- Create: `tests/systems/test_qtegra_system.cpp`, `configs/examples/spectrometer.qtegra.toml`, `configs/examples/spectrometer.qtegra.local.toml.example`
- Modify: `docs/dev_setup.md`, `tools/spec_router/spec_router/units.toml` (`thermo_qtegra_driver` goal → delivered, pointing at the spec; `isotopx_ngx_driver` goal gains "see docs/superpowers/specs/2026-10-01-ngx-driver-notes.md before starting"), `libs/systems/src/spectrometer/config_validate.cpp` only if Review Focus 5 needs a rule
- Test: `tests/systems/test_qtegra_system.cpp`

**Interfaces:**
- Consumes: `qtegra_sim_hook`, `QtegraSimModel` (Task 5), assembler `TransportMaker` injection, `ScanService`, `cfg::load_spectrometer`.
- Produces: example config in the shape of `configs/examples/spectrometer.sim-integrated.toml` with `[transports.qtegra] kind = "tcp" host = "192.168.0.10" port = 1069 timeout_ms = 2000`, `[drivers.qtegra] kind = "thermo_qtegra" transport = "qtegra" roles = [...] channels = [...]`, the `argon` field table, detector `channel = "qtegra:<name>"`; the `.local.toml.example` with `[transports.qtegra] host/port`.

- [ ] **Step 1: Write failing tests:**
  - `ExampleConfigLoadsAndValidates` (`cfg::load_spectrometer` on the new example; no transport opened).
  - `ExampleConfigIsNotSimulated` (`is_simulated` false, so `--sim` refuses it).
  - `ConfigParityThroughSpectrometer`: load `spectrometer.sim-integrated.toml`, swap the driver kind to `thermo_qtegra` (dropping keys the Qtegra schema does not declare), assemble with a `TransportMaker` returning `SimTransport::hooked(qtegra_sim_hook(model))`; then `position(Isotope{"Ar40"}, H1)` reaches the model's DAC, `set_hv`/`read_hv` round-trip, `acquire(3)` returns three readings at the snapped integration with a value for every detector.
  - `MoveProtocolProtectsAndBlanks`: a large move with the example protection settings sends `ProtectDetector CDD,On`, `BlankBeam True`, `SetMagnetDAC`, then the reverse, in that order (check the sim's command log).
  - `ContinuousScanDeliversAtSnappedPeriod`: `ScanService::start(1 s)` → `IntensityReading`s arrive with `integration == 1.048576 s`; `set_integration(0.5 s)` → later readings report `0.524288 s`; no overlap of `configure` with `next` (the model counts concurrent calls; expect 0).
  - `ReconnectDuringScanSurfacesErrorAndRestartRecovers` (Review Focus 3): drop the sim connection mid-scan → a `ScanStatus` with a non-empty error; `ScanService::start` again → readings resume; finishes within the test timeout.
  - `MagnetLimitsDisagreeWithDriverLimits` (Review Focus 5): `[magnet].limits` wider than the driver's `limit_min`/`limit_max` → a move outside the driver's limits is refused with `Config` and nothing is written; state in the test comment which layer refuses it.
- [ ] **Step 2: Run** `ctest --preset dev -R Qtegra` → new tests FAIL.
- [ ] **Step 3: Implement** the example files, docs (pointing a config at an instrument, `trace = true`, the bring-up checklist from spec section 8) and the `units.toml` edits. Fix any defect the system tests expose in Tasks 1–6 code, and list each such fix in the report.
- [ ] **Step 4: Run** `ctest --preset dev --output-on-failure` and `ctest --preset dev-ui --output-on-failure` → PASS.
- [ ] **Step 5: Commit** `feat(spectrometer): Qtegra system tests, example config and docs`.

---

## Self-review notes

- **Spec coverage:** 4.1 → Task 3; 4.2 → Task 4; 4.3 → Task 2; 4.4 → Task 1; 5.1–5.3 → Tasks 5–6; 6 → Task 7; 7 (errors) → tests in Tasks 5–7; 9 (testing) → all; rollout order matches spec section 10.
- **Order and dependencies:** 1 and 2 are independent; 3 builds on 2's assembler edits; 4 is independent; 5 needs 3 and 4; 6 needs 5; 7 needs all.
- **Left to the NGX spec:** secret keys, redaction, `try_read`, `SimTransport::inject`, valves over the spectrometer link.
