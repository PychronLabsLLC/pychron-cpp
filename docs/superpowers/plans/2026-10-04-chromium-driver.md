# Chromium CO2 Laser Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `chromium` driver that drives a Photon Machines Chromium CO2 laser system (output, firing, interlocks, XYZ stage, scan positions) over one open TCP connection, with a simulator, proven against the existing extraction-device conformance suites.

**Architecture:** Three layers, as the other drivers are built. A pure codec (`libs/codecs`) formats commands and parses replies. `ChromiumSim` (`libs/devices`) is a model of the laser PC behind a `SimTransport` hook. `ChromiumLaser` (`libs/devices`) is a `Device` that implements `IExtractionDevice`, `ILaserDevice` and `IStage` over a `Transport&`. Nothing above the driver changes: it registers with `DriverRegistry` and `SimSystem` hands it the simulator when its transport is `kind = "sim"`.

**Tech Stack:** C++20, GoogleTest, toml++ (driver options), the repo's `Transport` / `SimTransport` / `DriverRegistry`.

**Spec:** `docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md` (§2a vendor reference, §7 mapping, §8 driver shape). Decisions taken after it by the owner: **CO2 first; keep the connection open.**

## Global Constraints

- Commands end in LF (`\n`). Replies are framed with `ReadSpec::until_any("\r\n")` (the vendor says CR; a CRLF build must not break the framing).
- An error reply is `?<n>` with `n` in 0..4. Map: `?1`, `?2`, `?0` → `ErrorKind::Protocol`; `?3` → `ErrorKind::Config`; `?4` → `ErrorKind::Io` with "not supported by this hardware or failed" in `what`. `Error::code` is `"chromium?<n>"`.
- Action commands answer nothing. Every action is confirmed by a query sent straight after it, inside one `transact()`: write the action, write the query, read one line. A `?<n>` line is the action's error (read and discard one more line, the query's reply). No timing-based "silence means success".
- The connection is opened once and kept open. No per-command connect.
- Stage units: millimetres in `IStage`, integer microns on the wire. Wire value = `round(mm * 1000) * sign`.
- Percent only. `supports(Watts)` and `supports(Celsius)` are false in this plan (watts needs the laser system's power calibration; PID temperature needs a pyrometer, which a CO2 lacks).
- `libs/codecs/CONVENTIONS.md` binds the codec: pure, `Result<T>`, `codec::protocol_error`, no device names.
- No vendor PDF, and no text copied from one, goes in the repo.
- AGENTS.md: commit on a branch, rebase on `origin/main`, run the tests, merge to `main`, push. No pull requests. Never skip a failing test.
- Build and test: `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8`.

## Review Focus

1. **A late or extra line on the wire** (an error for the previous action arriving after its confirm was read): the next exchange must not take it for its own reply. Test in Task 3: `StaleInputIsDiscardedBeforeAQuery`.
2. **Chromium not accepting remote commands / wrong program on the port** (`Sys.ID?` answers something that is not Chromium, or nothing): `prepare()` must fail with a message naming what answered. Test in Task 3: `PrepareRejectsAnythingThatIsNotChromium`.
3. **An interlock tripping after enable** (door opened mid-run): `fire_laser()` must refuse with `Interlock` and name the interlock, not report success. Test in Task 3: `FireChecksInterlocksEveryTime`.
4. **A move outside the stage limits or onto a limit switch**: refused before anything is sent; a limit switch seen while moving is an `Io` error from `moving()`. Tests in Task 4: `MoveOutsideLimitsIsConfigAndSendsNothing`, `LimitSwitchWhileMovingIsAnError`.
5. **Output values a person types**: `12.5`, `100`, `0`, `100.1`, `NaN`. Formatting must be locale-free and `100.1` and `NaN` refused. Tests in Task 1 (`OutputFormatsWithoutLocale`) and Task 3 (`OutputAbove100OrNotFiniteIsConfig`).

---

### Task 1: Codec

**Files:**
- Create: `libs/codecs/include/pychron/codecs/chromium.hpp`, `libs/codecs/src/chromium.cpp`
- Modify: `libs/codecs/CMakeLists.txt` (add `src/chromium.cpp`)
- Test: `tests/codecs/test_chromium.cpp` (globbed by `tests/codecs/CMakeLists.txt`)

**Interfaces:**
- Produces, in `namespace pychron::codec::chromium`:

```cpp
inline constexpr std::string_view kCommandTerminator = "\n";
ReadSpec reply_frame();  // ReadSpec::until_any("\r\n")

// Queries: Command with reply = reply_frame().
codec::Command sys_id();            // "Sys.ID?\n"
codec::Command laser_status();      // "Laser.Status?\n"
codec::Command laser_interlocks();  // "Laser.Interlocks?\n"
codec::Command laser_enabled();     // "Laser.Enable?\n"
codec::Command laser_output_query();// "Laser.Output?\n"
codec::Command stage_position();    // "Stage.Pos?\n"
codec::Command stage_limits();      // "Stage.Status?\n"
Result<codec::Command> scan_in_position(int scan);  // "Scans.InPos? 3\n"; scan < 1 is Config
codec::Command scans_count();       // "Scans.Count?\n"

// Actions: Command::write_only (reply = nullopt).
codec::Command laser_enable(bool on);            // "Laser.Enable 1\n"
Result<codec::Command> laser_output(double percent);  // "Laser.Output 12.5\n"; <0, >100 or not finite is Config
codec::Command laser_fire();                     // "Laser.Fire\n"
codec::Command laser_stop();                     // "Laser.Stop\n"
struct Microns { std::int64_t x = 0, y = 0, z = 0; friend bool operator==(const Microns&, const Microns&) = default; };
codec::Command stage_move_to(Microns target, Microns speed);  // "Stage.MoveTo 1500,-2000,500,5000,5000,100\n"
codec::Command stage_stop();                     // "Stage.Stop\n"
Result<codec::Command> scan_move_to(int scan);   // "Scans.MoveTo 3\n"
codec::Command scans_stop();                     // "Scans.Stop\n"
codec::Command scans_status_verbosity(int v);    // "Scans.Status_Verbosity 1\n"

// Decoders take one framed reply (terminator bytes included).
std::optional<int> error_code(const Bytes& reply);        // "?4\r" -> 4; not an error line -> nullopt
Error to_error(int code, std::string_view command);       // mapping in Global Constraints
Result<std::string> decode_text(const Bytes& reply);      // trimmed of CR/LF/space; an error line -> to_error
Result<bool> decode_flag(const Bytes& reply);             // "1" / "0"
Result<double> decode_number(const Bytes& reply);
Result<Microns> decode_position(const Bytes& reply);      // "1500,-2000,500" (decimals rounded)
Result<std::vector<std::string>> decode_interlocks(const Bytes& reply);  // "" -> {}, "Door,Coolant" -> 2
struct LimitStatus { int x = 0, y = 0, z = 0; };          // -1, 0, +1
Result<LimitStatus> decode_limits(const Bytes& reply);
Result<std::string> decode_id(const Bytes& reply);        // must start "CHROMIUM" (case-insensitive), else Protocol
```

- [ ] **Step 1: Write the failing tests** in `tests/codecs/test_chromium.cpp`:

```cpp
TEST(Chromium, EncodesEveryCommandWithLfAndNoReplyForActions) {
  EXPECT_EQ(to_string(sys_id().tx), "Sys.ID?\n");
  EXPECT_EQ(sys_id().reply, reply_frame());
  EXPECT_EQ(to_string(laser_enable(true).tx), "Laser.Enable 1\n");
  EXPECT_FALSE(laser_enable(true).expects_reply());
  EXPECT_EQ(to_string(laser_fire().tx), "Laser.Fire\n");
  EXPECT_EQ(to_string(stage_move_to({1500, -2000, 500}, {5000, 5000, 100}).tx),
            "Stage.MoveTo 1500,-2000,500,5000,5000,100\n");
  EXPECT_EQ(to_string(scan_move_to(3)->tx), "Scans.MoveTo 3\n");
  EXPECT_EQ(to_string(scan_in_position(3)->tx), "Scans.InPos? 3\n");
  EXPECT_EQ(scan_move_to(0).error().kind, ErrorKind::Config);
}
TEST(Chromium, OutputFormatsWithoutLocale) {
  EXPECT_EQ(to_string(laser_output(12.5)->tx), "Laser.Output 12.5\n");
  EXPECT_EQ(to_string(laser_output(100)->tx), "Laser.Output 100\n");
  EXPECT_EQ(to_string(laser_output(0)->tx), "Laser.Output 0\n");
  EXPECT_EQ(to_string(laser_output(33.333333)->tx), "Laser.Output 33.333\n");  // 3 decimals, trailing zeros cut
  for (double bad : {-0.1, 100.1, std::nan("")}) EXPECT_EQ(laser_output(bad).error().kind, ErrorKind::Config);
}
TEST(Chromium, DecodesRepliesFramedByCrOrCrLf) {
  EXPECT_EQ(*decode_position(to_bytes("1500,-2000,500\r")), (Microns{1500, -2000, 500}));
  EXPECT_EQ(*decode_position(to_bytes("\n1500.4,-2000.6,500\r")), (Microns{1500, -2001, 500}));
  EXPECT_TRUE(*decode_flag(to_bytes("1\r")));
  EXPECT_FALSE(*decode_flag(to_bytes("0\r\n")));
  EXPECT_EQ(*decode_number(to_bytes("12.5\r")), 12.5);
  EXPECT_TRUE(decode_interlocks(to_bytes("\r"))->empty());
  EXPECT_EQ(*decode_interlocks(to_bytes("Door, Coolant Flow\r")), (std::vector<std::string>{"Door", "Coolant Flow"}));
  EXPECT_EQ(decode_limits(to_bytes("0,-1,+1\r"))->y, -1);
  EXPECT_EQ(*decode_id(to_bytes("CHROMIUM 2013.1.1.0\r")), "CHROMIUM 2013.1.1.0");
}
TEST(Chromium, ErrorRepliesMapToKinds) {
  EXPECT_EQ(error_code(to_bytes("?4\r")), 4);
  EXPECT_EQ(error_code(to_bytes("12.5\r")), std::nullopt);
  EXPECT_EQ(decode_number(to_bytes("?3\r")).error().kind, ErrorKind::Config);
  EXPECT_EQ(decode_number(to_bytes("?3\r")).error().code, "chromium?3");
  EXPECT_EQ(decode_flag(to_bytes("?2\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_position(to_bytes("?4\r")).error().kind, ErrorKind::Io);
}
TEST(Chromium, GarbageIsProtocol) {
  EXPECT_EQ(decode_position(to_bytes("1500,abc\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_flag(to_bytes("yes\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_id(to_bytes("NGX 1.0\r")).error().kind, ErrorKind::Protocol);
  EXPECT_EQ(decode_limits(to_bytes("0,2,0\r")).error().kind, ErrorKind::Protocol);
}
```

- [ ] **Step 2: Run** `cmake --build build/dev -j8 --target pychron_codecs_tests` — Expected: fails to compile (`chromium.hpp` not found).
- [ ] **Step 3: Implement** the header and `.cpp`. Numbers are parsed and formatted with `std::from_chars` / `std::to_chars` (never the process locale). Every decoder first checks `error_code` and returns `to_error` (the `command` argument may be empty there).
- [ ] **Step 4: Run** `cmake --build build/dev -j8 --target pychron_codecs_tests && build/dev/tests/codecs/pychron_codecs_tests --gtest_filter='Chromium.*'` — Expected: 5 tests PASS.
- [ ] **Step 5: Commit** `codecs: Chromium command and reply codec`.

---

### Task 2: Simulator

**Files:**
- Create: `libs/devices/include/pychron/devices/extraction/chromium_sim.hpp`, `libs/devices/src/extraction/chromium_sim.cpp`
- Modify: `libs/devices/CMakeLists.txt` (add the `.cpp`)
- Test: `tests/devices/extraction/test_chromium_sim.cpp`; add it to `tests/devices/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 names only for building expected strings in tests.
- Produces, in `namespace pychron::extraction`:

```cpp
// The laser PC: what a Chromium answers. Time comes from the Clock, so a
// stage move takes distance / speed on it.
class ChromiumSim {
 public:
  explicit ChromiumSim(const Clock& clock);
  SimTransport::Hook hook();  // one command per write; "" for an action that succeeds

  // Test and scenario controls (thread-safe).
  void trip_interlock(std::string name);     // Laser.Status? != 0, Laser.Fire answers ?4
  void clear_interlocks();
  void add_scan(codec::chromium::Microns start);  // scans are numbered from 1
  void put_on_limit(char axis, int side);    // Stage.Status? reports it; side -1, 0, +1
  void set_id(std::string id);               // default "CHROMIUM 2013.12.30.0"
  void fail_next(std::string command_prefix, int code);  // e.g. ("Laser.Enable", 4)

  // State, for assertions.
  bool enabled() const;
  bool firing() const;
  double output() const;
  codec::chromium::Microns position() const;  // advanced to clock.now()
  std::vector<std::string> log() const;       // every command received, as sent, without the LF
};
```

Behaviour the tests pin: commands are case-insensitive; an unknown component is `?1`, an unknown command of a known component `?2`, a missing or unparsable parameter `?3`; `Laser.Fire` with an interlock tripped or while not enabled is `?4`; `Laser.Enable 0` and `Laser.Stop` stop firing; replies end in `\r`. A move travels each axis independently at its commanded speed in µm/s (speed `0`: that axis does not move), arriving exactly on target; `Stage.Stop` freezes it where it is. `Scans.MoveTo n` moves to scan `n`'s start at 5000 µm/s; `Scans.InPos? n` is `1` only when stopped there.

- [ ] **Step 1: Write the failing tests** (`ManualClock clock; ChromiumSim sim(clock); auto ask = [hook = sim.hook()](std::string_view c) { return to_string(hook(to_bytes(std::string(c) + "\n"))); };`):

```cpp
TEST(ChromiumSim, QueriesAnswerAndActionsAreSilent) {
  EXPECT_EQ(ask("Sys.ID?"), "CHROMIUM 2013.12.30.0\r");
  EXPECT_EQ(ask("laser.enable 1"), "");            // case-insensitive, silent
  EXPECT_EQ(ask("Laser.Enable?"), "1\r");
  EXPECT_EQ(ask("Laser.Output 12.5"), "");
  EXPECT_EQ(ask("Laser.Output?"), "12.5\r");
  EXPECT_EQ(ask("Laser.Fire"), "");
  EXPECT_TRUE(sim.firing());
  EXPECT_EQ(ask("Laser.Stop"), "");
  EXPECT_FALSE(sim.firing());
}
TEST(ChromiumSim, ErrorsUseTheVendorCodes) {
  EXPECT_EQ(ask("Lazer.Fire"), "?1\r");
  EXPECT_EQ(ask("Laser.Explode"), "?2\r");
  EXPECT_EQ(ask("Laser.Output"), "?3\r");
  EXPECT_EQ(ask("Laser.Output abc"), "?3\r");
  EXPECT_EQ(ask("Laser.Fire"), "?4\r");            // not enabled
  ask("Laser.Enable 1");
  sim.trip_interlock("Door");
  EXPECT_EQ(ask("Laser.Status?"), "1\r");
  EXPECT_EQ(ask("Laser.Interlocks?"), "Door\r");
  EXPECT_EQ(ask("Laser.Fire"), "?4\r");
  sim.fail_next("Laser.Enable", 4);
  EXPECT_EQ(ask("Laser.Enable 1"), "?4\r");
  EXPECT_EQ(ask("Laser.Enable 1"), "");            // once only
}
TEST(ChromiumSim, TheStageMovesAtTheCommandedSpeedAndStops) {
  EXPECT_EQ(ask("Stage.Pos?"), "0,0,0\r");
  ask("Stage.MoveTo 10000,-5000,0,5000,5000,100");
  clock.advance(1s);
  EXPECT_EQ(ask("Stage.Pos?"), "5000,-5000,0\r");   // x half way, y there
  clock.advance(1s);
  EXPECT_EQ(ask("Stage.Pos?"), "10000,-5000,0\r");
  ask("Stage.MoveTo 0,-5000,0,5000,0,0");
  clock.advance(500ms);
  ask("Stage.Stop");
  clock.advance(5s);
  EXPECT_EQ(ask("Stage.Pos?"), "7500,-5000,0\r");
}
TEST(ChromiumSim, ScansAreNumberedFromOne) {
  sim.add_scan({2000, 3000, 0});
  EXPECT_EQ(ask("Scans.Count?"), "1\r");
  EXPECT_EQ(ask("Scans.InPos? 1"), "0\r");
  EXPECT_EQ(ask("Scans.MoveTo 1"), "");
  clock.advance(2s);
  EXPECT_EQ(ask("Scans.InPos? 1"), "1\r");
  EXPECT_EQ(ask("Scans.MoveTo 2"), "?3\r");
  sim.put_on_limit('y', -1);
  EXPECT_EQ(ask("Stage.Status?"), "0,-1,0\r");
}
```

- [ ] **Step 2: Run** `cmake --build build/dev -j8 --target pychron_devices_tests` — Expected: compile failure.
- [ ] **Step 3: Implement.** One mutex; position is advanced lazily to `clock.now()` on every command and accessor (as `SimExtractionDevice` does).
- [ ] **Step 4: Run** `build/dev/tests/devices/pychron_devices_tests --gtest_filter='ChromiumSim.*'` — Expected: 4 PASS.
- [ ] **Step 5: Commit** `devices: a Chromium simulator`.

---

### Task 3: Driver — link, extraction device, laser

**Files:**
- Create: `libs/devices/include/pychron/devices/extraction/chromium.hpp`, `libs/devices/src/extraction/chromium.cpp`
- Modify: `libs/devices/CMakeLists.txt`
- Test: `tests/devices/extraction/test_chromium.cpp` (+ `tests/devices/CMakeLists.txt`)

**Interfaces:**
- Consumes: Task 1 codec; Task 2 `ChromiumSim`; `SimTransport::hooked(hook, options)`.
- Produces, in `namespace pychron::extraction`:

```cpp
struct ChromiumOptions {
  std::array<std::pair<double, double>, 3> limits_mm{{{0, 50}, {0, 50}, {0, 50}}};  // x, y, z
  std::array<int, 3> signs{1, 1, 1};
  codec::chromium::Microns move_speed{5000, 5000, 100};  // µm/s
  double in_position_um = 10;
  bool use_enable = true;  // false: a unit that rejects Laser.Enable; enabling only checks interlocks
};

// Task 4 adds `public IStage` and stage(); until then stage() is the base's nullptr.
class ChromiumLaser final : public Device, public IExtractionDevice, public ILaserDevice {
 public:
  ChromiumLaser(std::string name, Transport& transport, ChromiumOptions options = {}, DeviceOptions device = {});
  // IExtractionDevice and ILaserDevice: every method.
  ILaserDevice* laser() override { return this; }
  const std::string& chromium_id() const;  // what Sys.ID? answered; empty before prepare()
 private:
  // One CR-framed line for a query (stale input discarded first); a ?<n> line is the error.
  Result<Bytes> query(const codec::Command& q);
  // An action and its confirming query in one transact(): write, write, read. See Global Constraints.
  Result<Bytes> act(const codec::Command& action, const codec::Command& confirm);
};
```

Behaviour: `device_name()` is the device's name. `prepare()`: `Sys.ID?` through `decode_id`, then `Scans.Status_Verbosity 1` confirmed by `Sys.ID?`. `enable()`: `Laser.Status?` non-zero → `Interlock` whose `what` lists `Laser.Interlocks?`; with `use_enable`, `act(Laser.Enable 1, Laser.Enable?)` and the flag must read true (else `Io`). `disable()`: `Laser.Stop`, `Scans.Stop`, `Laser.Output 0`, `Laser.Enable 0` (each confirmed; the first error is returned after all were tried), output and firing flags cleared. `is_enabled()`: with `use_enable`, `Laser.Enable?`; otherwise the tracked flag. `extract(v, Percent)`: not enabled → `Interlock`; `act(Laser.Output v, Laser.Output?)`, read-back within 0.1 else `Protocol`. Other units → `not_supported`-style `Config`. `end_extract()`: `Laser.Stop`, `Laser.Output 0`. `fire_laser()`: not enabled → `Interlock`; `Laser.Status?` non-zero → `Interlock` naming the interlocks; then `act(Laser.Fire, Laser.Status?)`. `stop_laser()`: `act(Laser.Stop, Sys.ID?)`. `is_firing()`: tracked. `warmup()`: success, nothing sent. Every public method's result passes through `observe()`.

- [ ] **Step 1: Write the failing tests.** Harness: `ManualClock clock; ChromiumSim sim{clock}; std::unique_ptr<SimTransport> wire = SimTransport::hooked(sim.hook(), {.name = "laser_pc", .clock = &clock}); ChromiumLaser laser{"co2", *wire};` with `wire->open()` in the constructor and `void advance() { clock.advance(250ms); }`.

```cpp
INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, ExtractionDeviceConformance, ::testing::Types<ChromiumHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, LaserConformance, ::testing::Types<ChromiumHarness>);

TEST_F(ChromiumTest, PrepareIdentifiesChromium) {
  ASSERT_TRUE(laser.prepare());
  EXPECT_EQ(laser.chromium_id(), "CHROMIUM 2013.12.30.0");
  EXPECT_EQ(sim.log().front(), "Sys.ID?");
}
TEST_F(ChromiumTest, PrepareRejectsAnythingThatIsNotChromium) {
  sim.set_id("NGX 1.0");
  auto r = laser.prepare();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("NGX 1.0"), std::string::npos);
  EXPECT_EQ(r.error().device, "co2");
}
TEST_F(ChromiumTest, EnableRefusesWithATrippedInterlockAndNamesIt) {
  sim.trip_interlock("Door");
  auto r = laser.enable();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("Door"), std::string::npos);
  EXPECT_FALSE(sim.enabled());
}
TEST_F(ChromiumTest, ExtractSetsAndConfirmsTheOutput) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(12.5, ExtractUnits::Percent));
  EXPECT_EQ(sim.output(), 12.5);
  EXPECT_EQ(*laser.output(), 12.5);
  EXPECT_FALSE(sim.firing());  // extract sets the output; fire_laser opens the beam
}
TEST_F(ChromiumTest, OutputAbove100OrNotFiniteIsConfig) {
  ASSERT_TRUE(laser.enable());
  for (double bad : {100.1, std::nan("")}) EXPECT_EQ(laser.extract(bad, ExtractUnits::Percent).error().kind, ErrorKind::Config);
  EXPECT_EQ(sim.output(), 0.0);
}
TEST_F(ChromiumTest, ARefusedActionIsAnErrorNotSilence) {
  ASSERT_TRUE(laser.enable());
  sim.fail_next("Laser.Output", 4);
  auto r = laser.extract(10, ExtractUnits::Percent);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(r.error().code, "chromium?4");
  EXPECT_EQ(*laser.output(), 0.0);
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));  // the wire is in step again
}
TEST_F(ChromiumTest, FireChecksInterlocksEveryTime) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));
  sim.trip_interlock("Coolant");
  auto r = laser.fire_laser();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("Coolant"), std::string::npos);
  EXPECT_FALSE(*laser.is_firing());
}
TEST_F(ChromiumTest, StaleInputIsDiscardedBeforeAQuery) {
  wire->inject(to_bytes("?4\r"));  // a line nobody is waiting for
  ASSERT_TRUE(laser.prepare());
}
TEST_F(ChromiumTest, AUnitWithoutLaserEnableWorksWithUseEnableOff) {
  ChromiumLaser plain{"co2", *wire, ChromiumOptions{.use_enable = false}};
  sim.fail_next("Laser.Enable", 4);
  ASSERT_TRUE(plain.enable());
  EXPECT_TRUE(*plain.is_enabled());
  EXPECT_TRUE(std::none_of(sim.log().begin(), sim.log().end(), [](auto& c) { return c.starts_with("Laser.Enable "); }));
}
TEST_F(ChromiumTest, DisableStopsFiringZeroesOutputAndDisables) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));
  ASSERT_TRUE(laser.fire_laser());
  ASSERT_TRUE(laser.disable());
  EXPECT_FALSE(sim.firing());
  EXPECT_EQ(sim.output(), 0.0);
  EXPECT_FALSE(sim.enabled());
}
```

If `SimTransport` has no way to inject unsolicited bytes for `StaleInputIsDiscardedBeforeAQuery`, build that one test's transport with `SimTransport::hooked(hook, options, unsolicited)` whose `unsolicited` returns `"?4\r"` once.

- [ ] **Step 2: Run** the devices tests target — Expected: compile failure.
- [ ] **Step 3: Implement** `ChromiumLaser` as declared above.
- [ ] **Step 4: Run** `build/dev/tests/devices/pychron_devices_tests --gtest_filter='*Chromium*'` — Expected: the two conformance suites and the 10 tests PASS.
- [ ] **Step 5: Commit** `devices: Chromium driver: link, output, firing, interlocks`.

---

### Task 4: Driver — stage and scan positions

**Files:**
- Modify: `libs/devices/include/pychron/devices/extraction/chromium.hpp`, `libs/devices/src/extraction/chromium.cpp`
- Test: `tests/devices/extraction/test_chromium.cpp`

**Interfaces:**
- Consumes: Task 3 `ChromiumLaser`, `query`, `act`, `ChromiumOptions`.
- Produces:

```cpp
// Where a named position is on a tray, in mm. The laser system supplies this
// once tray maps exist (sub-project 2); until then, tests and configs do.
struct TrayLookup {
  std::function<std::optional<StagePosition>(std::string_view tray, std::string_view position)> find;
  std::function<std::vector<std::string>(std::string_view tray)> names;
};
// ChromiumLaser now also derives from IStage and implements every method of it.
void ChromiumLaser::set_tray_lookup(TrayLookup lookup);
IStage* ChromiumLaser::stage() override { return this; }
```

Behaviour: `move_to_position(p, autocenter)`: `p` matching `^[sS][0-9]+$` is scan `N`: `act(Scans.MoveTo N, Scans.InPos? N)`, target becomes "scan N". Anything else is looked up on the current tray: unknown (or no lookup) → `Config`; found → an xy move at the looked-up x, y. `autocenter` is accepted and ignored (vision's job). `set_xy(x, y)`: reads `Stage.Pos?` for the present z, then `act(Stage.MoveTo …, Stage.Pos?)` with `move_speed`. `set_axis(axis, v)`: reads the position, changes one axis, same move. Every target is checked against `limits_mm` first: outside → `Config`, nothing sent. `position()`: `Stage.Pos?` → mm, signs undone. `moving()`: one poll. Scan target: `Scans.InPos? N`. Point target: `Stage.Pos?` within `in_position_um` on all three axes. Arrival needs **three consecutive** good polls; then the target is cleared and `moving()` is false. No target → false, nothing sent. Each poll also reads `Stage.Status?`: a non-zero axis → `Io` naming the axis and side, target cleared. `set_tray(t)`: with a lookup whose `names(t)` is empty → `Config`; otherwise stored. `positions()`: `names(current tray)`.

- [ ] **Step 1: Write the failing tests.** The harness gains `ChromiumOptions` with `limits_mm` of `{-50, 50}` on each axis and a lookup for tray `"221-hole"` with `"1"` → (1.5, -2.0, 0) and `"2"` → (10, 10, 0), `set_tray("221-hole")` done; `advance()` moves the clock 250 ms.

```cpp
INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, StageConformance, ::testing::Types<ChromiumHarness>);

TEST_F(ChromiumTest, AnXyMoveKeepsZAndSendsIntegerMicrons) {
  ASSERT_TRUE(laser.set_axis(IStage::Axis::Z, 0.5));
  settle();
  ASSERT_TRUE(laser.set_xy(1.5, -2.0));
  EXPECT_EQ(sim.log().back(), "Stage.Pos?");                       // the confirm
  EXPECT_TRUE(logged("Stage.MoveTo 1500,-2000,500,5000,5000,100"));
}
TEST_F(ChromiumTest, MoveOutsideLimitsIsConfigAndSendsNothing) {
  const auto before = sim.log().size();
  EXPECT_EQ(laser.set_xy(50.001, 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(laser.set_axis(IStage::Axis::Z, -50.5).error().kind, ErrorKind::Config);
  EXPECT_EQ(sim.log().size(), before);
}
TEST_F(ChromiumTest, MovingNeedsThreeGoodPollsInARow) {
  ASSERT_TRUE(laser.set_xy(1.0, 0));       // 1000 µm at 5000 µm/s: 0.2 s
  clock.advance(1s);                        // there
  EXPECT_TRUE(*laser.moving());
  EXPECT_TRUE(*laser.moving());
  EXPECT_FALSE(*laser.moving());            // the third
  const auto n = sim.log().size();
  EXPECT_FALSE(*laser.moving());            // no target: nothing sent
  EXPECT_EQ(sim.log().size(), n);
}
TEST_F(ChromiumTest, LimitSwitchWhileMovingIsAnError) {
  ASSERT_TRUE(laser.set_xy(10, 0));
  sim.put_on_limit('x', +1);
  auto r = laser.moving();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("x"), std::string::npos);
  EXPECT_FALSE(*laser.moving());            // target cleared
}
TEST_F(ChromiumTest, SignsAreAppliedBothWays) {
  ChromiumOptions o = options(); o.signs = {-1, 1, 1};
  ChromiumLaser flipped{"co2", *wire, o};
  ASSERT_TRUE(flipped.set_xy(2.0, 3.0));
  EXPECT_TRUE(logged("Stage.MoveTo -2000,3000,0,5000,5000,100"));
  clock.advance(5s);
  EXPECT_NEAR(flipped.position()->x, 2.0, 1e-9);
}
TEST_F(ChromiumTest, AScanPositionMovesByScanNumber) {
  sim.add_scan({2000, 3000, 0});
  ASSERT_TRUE(laser.move_to_position("s1", false));
  EXPECT_TRUE(logged("Scans.MoveTo 1"));
  clock.advance(5s);
  EXPECT_TRUE(*laser.moving()); EXPECT_TRUE(*laser.moving()); EXPECT_FALSE(*laser.moving());
  EXPECT_EQ(laser.move_to_position("s9", false).error().kind, ErrorKind::Config);  // ?3 from Chromium
}
TEST_F(ChromiumTest, AnUnknownTrayIsConfig) {
  EXPECT_EQ(laser.set_tray("no-such-tray").error().kind, ErrorKind::Config);
  EXPECT_EQ(laser.positions(), (std::vector<std::string>{"1", "2"}));  // unchanged
}
```

- [ ] **Step 2: Run** — Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `build/dev/tests/devices/pychron_devices_tests --gtest_filter='*Chromium*'` — Expected: all PASS, `StageConformance` included.
- [ ] **Step 5: Commit** `devices: Chromium driver: stage moves, limits, scan positions`.

---

### Task 5: Registration, sim wiring, example, docs

**Files:**
- Modify: `libs/devices/src/extraction/chromium.cpp` (`schema()`, `create()`, `REGISTER_DRIVER("chromium", pychron::extraction::ChromiumLaser)`), `chromium.hpp`
- Modify: `libs/sim/src/sim_system.cpp` + `libs/sim/include/pychron/sim/sim_system.hpp` (`hook_for`: kind `"chromium"` → a `ChromiumSim` it owns)
- Create: `configs/examples/laser.chromium.toml.example`
- Modify: `docs/dev_setup.md` (a "Chromium laser" subsection), `docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md` (status line: driver built; §10 items 2 and 8 marked decided)
- Test: `tests/devices/extraction/test_chromium.cpp`, `tests/sim/test_sim_system.cpp` (or the nearest existing sim-system test file)

**Interfaces:**
- Consumes: `ChromiumLaser`, `ChromiumOptions`, `ChromiumSim`.
- Produces: driver kind `chromium` with keys

| Key | Type | Required | Meaning |
|---|---|---|---|
| `x_limits`, `y_limits`, `z_limits` | FloatArray of 2 (declare as needed; add `KeyType::FloatArray` if the registry lacks it) | no | mm; default `[0, 50]` |
| `signs` | IntegerArray of 3 | no | each 1 or -1; default `[1, 1, 1]` |
| `move_speed` | IntegerArray of 3 | no | µm/s, each > 0 except z ≥ 0; default `[5000, 5000, 100]` |
| `in_position_um` | Float | no | > 0; default 10 |
| `use_enable` | Boolean | no | default true |

`configs/examples/laser.chromium.toml.example`:

```toml
# A Photon Machines Chromium CO2 system, driven over TCP. In Chromium, tick
# the TCP/IP interface in View > Control Panels > Remote Control (port 1234).
[transports.laser_pc]
kind = "tcp"          # "sim" runs against the built-in Chromium simulator
host = "laser-pc.lab"
port = 1234
timeout_ms = 2000

[drivers.laser]
kind = "chromium"
transport = "laser_pc"
x_limits = [0, 50]
y_limits = [0, 50]
z_limits = [0, 50]
signs = [1, 1, 1]
move_speed = [5000, 5000, 100]
in_position_um = 10
```

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(ChromiumRegistry, CreatesFromConfigAndRejectsBadOptions) {
  auto& reg = DriverRegistry::global();
  ASSERT_TRUE(reg.contains("chromium"));
  auto ok = toml::parse("x_limits = [-10, 10]\nsigns = [1, -1, 1]\nuse_enable = false\n");
  EXPECT_TRUE(reg.validate("chromium", ok));
  for (const char* bad : {"signs = [1, 2, 1]", "x_limits = [10, -10]", "x_limits = [0]", "move_speed = [0, 5000, 100]",
                          "in_position_um = 0", "bogus = 1"}) {
    auto t = toml::parse(bad);
    ManualClock clock; ChromiumSim sim{clock};
    auto wire = SimTransport::hooked(sim.hook(), {.name = "w"});
    EXPECT_FALSE(reg.create("chromium", *wire, t)) << bad;
  }
}
TEST(ChromiumRegistry, TheDeviceExposesItsCapabilities) {
  // created through the registry, reached as a Device
  auto device = /* reg.create("chromium", *wire, toml::table{}) */;
  auto* extraction = dynamic_cast<extraction::IExtractionDevice*>(device->get());
  ASSERT_NE(extraction, nullptr);
  EXPECT_NE(extraction->laser(), nullptr);
  EXPECT_NE(extraction->stage(), nullptr);
  EXPECT_EQ(extraction->pattern_runner(), nullptr);
  EXPECT_TRUE(extraction->supports(extraction::ExtractUnits::Percent));
  EXPECT_FALSE(extraction->supports(extraction::ExtractUnits::Watts));
}
// tests/sim: a config whose chromium driver sits on a sim transport gets a hook that answers.
TEST(SimSystemChromium, ASimTransportAnswersAsChromium) {
  // system config: [transports.laser_pc] kind = "sim"; [drivers.laser] kind = "chromium", transport = "laser_pc"
  auto hook = sim.hook_for(config.drivers.at("laser"), config);
  ASSERT_TRUE(hook);
  EXPECT_EQ(to_string(hook(to_bytes("Sys.ID?\n"))), "CHROMIUM 2013.12.30.0\r");
}
```

- [ ] **Step 2: Run** — Expected: FAIL (`chromium` not registered).
- [ ] **Step 3: Implement** schema, `create()` (range and length checks beyond the registry's type checks are `Config` errors naming the key), registration, and the `hook_for` branch (the `ChromiumSim` is owned by `SimSystem`, built on its clock).
- [ ] **Step 4: Run the whole suite** `cmake --build build/dev -j8 && ctest --test-dir build/dev -j8` — Expected: 100% pass. Then `build/dev/apps/elctl/elctl list-drivers | grep -A8 chromium` — Expected: the summary and the keys above.
- [ ] **Step 5: Write the docs** (`dev_setup.md`: what the driver does, the example file, the Remote Control tick box, that percent is the only unit, that nothing uses the device in a queue until the laser system exists).
- [ ] **Step 6: Commit** `devices: register the Chromium driver; sim wiring, example config, docs`, then rebase on `origin/main`, run the tests, merge to `main`, push.

---

## Not in this plan

- Watts (power calibration) and temperature (`PID.*`, pyrometer) extraction.
- UV: running scans (`Scans.Run`, status texts), burst, rate, spot size.
- Tray maps, stage calibration, patterns, laser UI, `LabSession` wiring (sub-projects 2 and 3). The driver is reachable by `elctl` and tests only until then.
- Serial (RS232) transport framing (CR terminator).
- Validation against a real Chromium: survey §10 items 1, 3, 4, 6, 7, 9 stay open, and the first real session should confirm the action-then-query assumption.
