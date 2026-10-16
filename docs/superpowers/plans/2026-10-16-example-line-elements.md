# A fuller example extraction line: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Grow the example line in `configs/examples` from 5 valves and 6 stages to the line of spec section 5.1
(13 valves, 2 manual, 2 rough, 4 gauges, 13 stages, 2 pipettes, 1 switch, 1 heater, a cryostat with 2 setpoints)
without moving a single number the shipped queue measures.

**Architecture:** Config and test change only: no library code. `canvas.toml` and `extraction_line.toml` change
together (`cross_validate` holds them to each other); `sim.toml` gains one tank; one new extraction script drives
the second pipette. Every added valve starts closed, which is what keeps the shipped queue's gas where it is.

**Tech Stack:** TOML configs, GoogleTest (`tests/systems`, `tests/sim`, `tests/integration`), Qt Test (`tests/ui`),
embedded Python for the new script.

**Spec:** `docs/superpowers/specs/2026-10-13-example-line-elements-design.md`

## Global Constraints

- Kept exactly: names `A B C P1 P2 M1 IG1 PG1 prep spec turbo rough air_tank air pump_power`, their addresses,
  interlocks (but `A`, which gains `D`), aliases `valves.inlet = "B"`, `valves.outlet = "C"`, `extraction.eqtime = 20`.
- `bone` is renamed `stage1` everywhere it names the example's stage; its `volume = 12.5`, `size = [80, 40]`,
  `fill = true` stay.
- No name that identifies NMGRL (`Bone`, `Jan`, `Felix`, `Minibone`, `Microbone`, ...). Generic single letters and
  the driver keys the spec table gives (`cryostat`, `bakeout_plc`) are allowed: the spec names them.
- Every added valve, rough valve and pipette valve is closed at start (sim default). Getters sit behind `G1`, `G2`.
- Shipped scripts (`sim_extract.py`, `sim_blank.py`, `sim_air.py`, `laser_extract.py`, `sim_pump.py`), shipped
  plans, queues and field tables: not edited.
- Tests reading the example: edits only to counts, name lists and the `bone` rename; none retuned, none removed.
  Exception, from owner's item 5: the two "no dock on the example line" assertions move to a line without a heater
  (or cryostat) (Task 4).
- If an install e2e or `LabSim` number moves, the element that moved it starts isolated; the test is not retuned.
- `libs/setup/AGENTS.md`: a profile or shipped file change that `tests/setup/test_profiles.cpp` checks is edited in
  the same commit as that test.
- Inner loop per `AGENTS.md`: build one target, run its binary filtered. UI targets build under preset `dev-ui`
  (`build/dev-ui/tests/ui/pychron_ui_<test>`); everything else under `dev`.

## Decisions this plan takes where the spec is silent or the code disagrees

Each is written back into the spec in the task that makes it, so the two never disagree.

1. **Laser stage and `co2`.** A canvas stage has no key naming a driver (`libs/systems/src/canvas/loader.cpp:374`),
   and adding one is out of scope (spec 4). `laser` is a stage with `kind = "laser"`, `symbol = "laser"`,
   `use_symbol = true`, `display_name = "CO2 laser"`; the tie to driver `co2` is its name and a comment. Spec 5.1
   row edited (Task 2).
2. **`M2`.** A dead-end vent valve joins one volume, so the simulator lists it in `valves_without_physics()`,
   which `expect_the_example_lab` (`tests/systems/test_extraction_line.cpp:1250`) holds empty. `M2` is instead the
   manual valve from `turbo2`'s foreline (behind `R2`) to `rough`. With it there, rough is reached only through a
   manual valve, which `ExampleConfigs.LoadCrossValidateAndBuildGraph` asserts. Spec 5.1 sketch and row edited
   (Task 2).
3. **Cleanup above the main row.** Getters, `G1`, `G2`, `Y`, `cryo` and `IG2` are drawn above the line, pipettes
   below `prep`, so no pipe crosses another. Topology is the spec's; spec sketch redrawn to match (Task 2).
4. **Cocktail test location.** `tests/sim` knows no plan and no detector. The test that a cocktail shot puts Ar39
   on the detector the plan names goes in `tests/integration/test_lab_sim.cpp` (`LabSim`), which runs the example
   lab, its scripts and `sim_multicollect`. Spec section 7 edited (Task 3).
5. **Cross and image.** Not in spec 5.1's table; not added.
6. **Install profiles (spec 5.5), checked 2026-10-16.** `profiles/extraction-line-starter/profile.toml` copies
   `@examples/extraction_line.toml`, `@examples/canvas.toml` and `@examples/sim.toml` (and `cameras.toml`,
   `stage_calibrations/`) when `line_source == "starter"`, the default. No instrument profile copies them itself:
   `instrument-common` includes `extraction-line-starter` (and `lab-common`, which copies all of `@examples/scripts/`,
   so `sim_cocktail.py` ships too), and `argus`, `helix` (through `qtegra`) and `ngx` include `instrument-common`.
   So every instrument install gets the new line. `tests/setup/test_profiles.cpp` reads them in
   `InstrumentProfile.InstallsLoadsAndPassesDoctor` (files exist; `sim.toml` byte-equal to the example's; doctor
   passes, which loads and cross-validates the installed line; `expect_queue_checks`), `AnImportedLineComesWithout
   TheExampleLabsSimToml` (copies the two example files as a lab's own) and `EveryInstalledPlanPutsItsIsotopesOnIts
   Detectors`. None lists valve names or counts, so none needs an edit for the new elements; they are run in Tasks 2
   to 4. If one fails, the cause is found and the test edited in the same commit as the file (`libs/setup/AGENTS.md`).
   The profile `version` (5) is not raised: a raise makes doctor tell every install its profile is newer, and running
   setup again would rewrite a line the lab has made its own; the files are the same files with more in them.
   The profile's `summary` and `help` text, which name "five valves, two gauges", are edited (Task 2).

## Layout (canvas 1450 x 800, centres, y down)

Starting coordinates; Task 2 may move an added element if the overlap test or a crossing pipe needs it, keeping
the topology. A gauge chip runs from about 25 px left of its `pos` to 140 px right and 20 px down: keep valves out
of that box (`aValveUnderAGaugeChipStillTakesItsClick`).

| Element | pos | Other keys |
|---|---|---|
| stage `laser` | [90, 200] | size [80, 60], kind laser, symbol laser, use_symbol, display_name "CO2 laser" |
| valve `L` | [190, 200] | |
| stage `stage1` | [300, 200] | as `bone` was |
| valve `A` | [420, 200] | |
| stage `prep` | [600, 200] | size [240, 40] (was 80 x 40) |
| valve `B` | [780, 200] | |
| stage `spec` | [950, 200] | |
| valve `I` | [1090, 200] | |
| stage `ion` | [1200, 200] | size [60, 40], symbol ion_pump, use_symbol |
| valve `G1`, stage `getter1` | [275, 140], [275, 80] | getter: kind getter, symbol getter, use_symbol, size [60, 40] |
| gauge `IG2` | [325, 130] | |
| valve `G2`, stage `getter2` | [520, 140], [520, 80] | as getter1 |
| valve `Y`, stage `cryo` | [680, 140], [680, 80] | cryo: size [80, 40], display_name "Cryo trap" |
| switch `pump_power` | [850, 80] | |
| valve `D` | [330, 300] | |
| stage `turbo2` | [330, 720] | size [60, 40], kind pump |
| `P1`, `air`, `P2`, `air_tank` | x 540; y 330, 420, 510, 600 | |
| `Q1`, `cocktail`, `Q2`, `cocktail_tank` | x 640; y 330, 420, 510, 600 | tank size [80, 40], kind tank; pipette vlabel "Cocktail Pipette" |
| valve `C` | [700, 300] | |
| gauge `IG1` | [780, 380] | |
| stage `turbo` | [850, 300] | |
| rough valve `R1` | [1060, 300] | |
| gauge `PG1` | [960, 380] | |
| manual `M1` | [1180, 300] | |
| stage `rough` | [1300, 420] | |
| gauge `CG1` | [1300, 520] | |
| rough valve `R2` | [1000, 720] | |
| manual `M2` | [1150, 420] | |
| labels | "Extraction" [100, 50] (was "Furnace side"), "Cleanup" [560, 20], "Roughing" [1200, 640] | |
| legend | [20, 750] | |

Connections (offsets are `start_offset` from the stage centre):

- Main row: `laser`-`L`, `L`-`stage1`, `stage1`-`A`, `A`-`prep`, `prep`-`B`, `B`-`spec`, `spec`-`I`, `I`-`ion`.
- Above, `orientation = "v"`: `stage1`-`G1` [-25, 0], `G1`-`getter1`; `stage1`-`IG2` [25, 0]; `prep`-`G2` [-80, 0],
  `G2`-`getter2`; `prep`-`Y` [80, 0], `Y`-`cryo`.
- Below, `orientation = "v"`: `stage1`-`D` [30, 0], `D`-`turbo2`; `prep`-`P1` [-60, 0], `P1`-`air`, `air`-`P2`,
  `P2`-`air_tank`; `prep`-`Q1` [40, 0], `Q1`-`cocktail`, `cocktail`-`Q2`, `Q2`-`cocktail_tank`; `prep`-`C` [100, 0].
- Roughing: tee `C`/`turbo`/`IG1` (as now); tee `turbo`/`R1`/`PG1` (PG1 stays in turbo's region, so the
  region `C` opens to is unchanged: Task 2 found a tee `R1`/`M1`/`PG1` puts PG1 behind closed `R1` and fails
  `ExampleLineSim`); `R1`-`M1` (valve to valve: the simulator puts a pipe volume between); elbow `M1`-`rough`
  (bend at [1300, 300]); `rough`-`CG1` `orientation = "v"`; `turbo2`-`R2`; elbow `R2`-`M2` (bend at
  [1150, 720]); `M2`-`rough`.

## Review Focus

- A shipped-queue run with every new valve closed: same gas, same numbers (install e2e 3 % of 303.2,
  `LabSim.TheSameQueueGivesTheSameNumbers`, `SuccessiveShotsDeclineWithTheTank`). Held by Task 2 and Task 3 runs.
- Opening `D` while `A` is open (and the reverse) is refused, as `C` with `A`: test added in Task 2.
- `Q1` and `Q2` never open together: test added in Task 2.
- A gauge chip drawn over a valve on the larger canvas: `aValveUnderAGaugeChipStillTakesItsClick` already loops the
  example; run in Task 2.
- The example line in every UI test now carries a simulated cryostat and heater: `test_docks` and the rest of
  `tests/ui` run in Task 4 and the final task, not only the two dock tests.

---

### Task 1: Rename `bone` to `stage1`

**Files:**
- Modify: `configs/examples/canvas.toml` (stage name, connection, sketch, comment line 65), `configs/examples/sim.toml`
  (commented `[volumes.bone]`), `configs/examples/extraction_line.toml` (`A` description "Stage 1 to prep",
  comment "never expose stage 1 to an open turbo line")
- Modify tests: `tests/integration/test_example_line_sim.cpp` (8, 47, 168, 208), `tests/ui/ui_fixture.hpp:35`,
  `tests/ui/test_canvas_view.cpp` (112, 373, 376, 410, 436, 454-471, 547, 557, 693-732, 744, 860: 454-471 and
  860 append to a copy of the example canvas, so they connect to its stage; their gauge `Bone IG` becomes
  `Stage1 IG`), `tests/systems/test_canvas_cross_validate.cpp:195-199`, `tests/sim/test_sim_config.cpp:775, 819, 859`
  (all three in `TheExampleFileIsTheDefaults`)
- Modify docs: `docs/user/02-extraction-line.md:48, 460`, `docs/simulator.md:331, 403, 414`

- [x] **Step 1:** `rg -n '\bbone\b' configs/examples tests apps docs/user docs/simulator.md -g '!configs/examples/nmgrl/**'`;
  rename each hit that names the example's stage; leave fixtures of the tests' own (`test_sim_system.cpp`,
  `test_network_graph.cpp`, `test_canvas_loader.cpp`, ...).
- [x] **Step 2:** Build and run: `cmake --build --preset dev --target pychron_systems_tests pychron_sim_tests pychron_integration_tests`;
  `build/dev/tests/systems/pychron_systems_tests --gtest_filter='ExampleConfigs.*:ExtractionLine.*Example*'`,
  `build/dev/tests/sim/pychron_sim_tests --gtest_filter='SimConfig.TheExampleFileIsTheDefaults'`,
  `build/dev/tests/integration/pychron_integration_tests --gtest_filter='ExampleLineSim*'`. Expected: PASS.
- [x] **Step 3:** `cmake --build --preset dev-ui --target pychron_ui_test_canvas_view`;
  `build/dev-ui/tests/ui/pychron_ui_test_canvas_view`. Expected: PASS.
- [x] **Step 4:** `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
- [x] **Step 5:** Commit `refactor(setup): rename the example line's bone stage to stage1`.

### Task 2: Valves, stages, gauges and the second pipette

**Files:**
- Modify: `configs/examples/extraction_line.toml`, `configs/examples/canvas.toml` (layout table above, new sketch at
  file head, `size = [1450, 800]`)
- Modify: `profiles/extraction-line-starter/profile.toml` (`summary`, `help`: no longer "five valves, two gauges and a
  pipette"; `version` stays 5, decision 6). No other profile file names the example's valves (decision 6).
- Run, edited only if they fail for a cause found: `tests/setup/test_profiles.cpp` (`InstrumentProfile.*`,
  `Profiles.AnImportedLineComesWithoutTheExampleLabsSimToml`, `*EveryInstalledPlan*`)
- Test: `tests/systems/test_canvas_cross_validate.cpp`, `tests/integration/test_example_line_sim.cpp`,
  `tests/ui/test_core_bridge.cpp:39`, `tests/ui/test_canvas_view.cpp:108-120`
- Modify: spec 5.1 (sketch, `laser` and `M2` rows: decisions 1 to 3)
- Found while building (spec 5.1, "Found building it"): `apps/elctl/src/line.cpp` (its quiet simulator answers
  `gp_microion`; test `ElctlTest.ReadsAGaugeOnTheMicroIon`, `ProbePrintsHealthTable` lists `cg_controller`),
  `tests/setup/test_connection.cpp` (renames D's interlock on A too), `profiles/instrument-common/CALIBRATE.md`
  ("five-valve example"), `tests/ui/test_canvas_view.cpp` coordinates of the example in
  `orientedConnectionsAreDrawnSquare`, `elbowsTurnOneSquareCorner`,
  `pipesAreBorderedAndBreakTheBorderOfVolumesTheyEnter`.

**Interfaces:**
- Produces: example names `L D G1 G2 Y I Q1 Q2` (`[[valves]]`, `actuator1`, addresses `"10"` to `"17"` in that
  order: 9 is `pump_power`), `R1 R2` (`[[valves]]` addresses `"18"`, `"19"`, drawn `[[rough_valve]]`), `M2`
  (`[[manual_valves]]`, "Turbo 2 foreline to roughing pump"), gauges `IG2` (`ig_controller`, channel 3: its
  `channels` becomes `[1, 2, 3]`), `CG1` (`cg_controller`, channel 2, units "torr"), pipette `cocktail` (inner `Q1`,
  outer `Q2`), stages `laser stage1 getter1 getter2 cryo turbo2 ion cocktail_tank`.
- Interlocks: `Q1` `["Q2"]`, `Q2` `["Q1"]`, `D` `["A"]`, `A` `["C", "D"]`.
- New transport `[transports.cg_controller] kind = "sim"`, driver `[drivers.cg_controller] kind = "gp_microion"`,
  `address = 1`, `channels = [2]`.

- [x] **Step 1: Write the failing count test** in `tests/systems/test_canvas_cross_validate.cpp`:

```cpp
TEST(ExampleConfigs, TheExampleHasEveryElementKind) {
  // sys, c loaded as in LoadCrossValidateAndBuildGraph
  auto of_kind = [&](ValveKind k) { return std::ranges::count(c->valves, k, &ValveElement::kind); };
  EXPECT_EQ(of_kind(ValveKind::Valve), 13);
  EXPECT_EQ(of_kind(ValveKind::Rough), 2);
  EXPECT_EQ(of_kind(ValveKind::Manual), 2);
  EXPECT_EQ(of_kind(ValveKind::Switch), 1);
  EXPECT_EQ(c->gauges.size(), 4u);
  EXPECT_EQ(c->stages.size(), 13u);
  EXPECT_EQ(c->pipettes.size(), 2u);
  EXPECT_EQ(c->labels.size(), 3u);
  EXPECT_GE(c->elbows.size(), 1u);
  EXPECT_GE(c->tees.size(), 1u);
  EXPECT_EQ(sys->valves.size(), 15u);
  EXPECT_EQ(sys->manual_valves.size(), 2u);
  EXPECT_EQ(sys->gauges.size(), 4u);
  EXPECT_EQ(sys->pipettes.size(), 2u);
  // every stage kind the canvas offers
  for (auto k : {SourceKind::Pump, SourceKind::Laser, SourceKind::Tank, SourceKind::Spectrometer, SourceKind::Getter})
    EXPECT_TRUE(std::ranges::any_of(c->stages, [&](const auto& s) { return source_kind(s) == k; }));
}
```

  `source_kind` is declared in `canvas.hpp:103`. Extend
  `LoadCrossValidateAndBuildGraph` (rename only): `stage1` not joined to `rough` with every `[[valves]]` entry open;
  joined once `M1` opens; `connected_volumes(open).size() == 1` holds.
- [x] **Step 2:** Run `pychron_systems_tests --gtest_filter='ExampleConfigs.*'`: FAIL on the counts.
- [x] **Step 3: Interlock tests** in `tests/integration/test_example_line_sim.cpp` beside
  `PipetteValvesNeverOpenTogether`: `CocktailPipetteValvesNeverOpenTogether` (`Q1` open, opening `Q2` refused, `Q2`
  stays closed) and `DAndANeverOpenTogether` (both orders). Extend the closed-at-start list (154-157) with
  `L D G1 G2 Y I Q1 Q2 R1 R2`, `M2` unknown as `M1`. Run: FAIL (names unknown).
- [x] **Step 4:** Write `extraction_line.toml` and `canvas.toml` per Interfaces and Layout. Laser stage carries a
  comment: "the chamber of driver `co2`". New sketch at the head of `canvas.toml` shows the drawing.
- [x] **Step 5:** Run `pychron_systems_tests --gtest_filter='ExampleConfigs.*:ExtractionLine.*Example*:SpectrometerBringup*'`
  and `pychron_integration_tests --gtest_filter='ExampleLineSim*'`. Expected: PASS, and `expect_the_example_lab`
  sees no warning and `valves_without_physics()` empty.
- [x] **Step 6:** UI counts: `test_core_bridge.cpp:39` becomes `std::size_t{18}` (comment "13 valves, 2 rough, M1,
  M2, pump_power"); `buildsItemsFromCanvas` lists every new valve, stage and gauge, `connection_count() >= 30`.
  `cmake --build --preset dev-ui --target pychron_ui_test_canvas_view pychron_ui_test_core_bridge`; run both
  binaries. Expected: PASS, including `aValveUnderAGaugeChipStillTakesItsClick` (move an added element, not a
  test, if a chip covers a valve).
- [x] **Step 7: Install profiles.** Recheck decision 6 against the tree as it is then:
  `rg -n 'canvas\.toml|extraction_line\.toml|sim\.toml|@examples/scripts' profiles` (expected: only
  `extraction-line-starter/profile.toml` copies the three, only `lab-common/profile.toml` the scripts) and
  `rg -n 'includes' profiles/*/profile.toml` (expected: `argus`, `helix`, `ngx` reach `extraction-line-starter`
  through `instrument-common`). A profile found copying them that decision 6 does not name is added to this task's
  files with its test. `rg -n 'five valves|two gauges|"P1"|"IG1"|"bone"' tests/setup` (expected: no
  hit naming the example's valves or counts). Edit `profile.toml` `summary` and `help`.
  `cmake --build --preset dev --target pychron_elctl_tests pychron_setup_tests`; run
  `pychron_elctl_tests --gtest_filter='*Line*:*Canvas*'`, `pychron_setup_tests --gtest_filter='*Profile*:*Connection*'`
  (`*Profile*` takes in `InstrumentProfile.InstallsLoadsAndPassesDoctor/*`, which installs argus, helix and ngx with
  the new line and runs doctor on it). Expected: PASS. A failure there is fixed in the shipped file, or the test
  edited, in this task's commit.
- [x] **Step 8:** Shipped numbers did not move: `pychron_integration_tests --gtest_filter='LabSim.*'`. Expected: PASS
  untouched. If one fails, find which added element carries gas at start and isolate it.
- [x] **Step 9:** Edit spec 5.1 for decisions 1 to 3. `quality_check.py`, `compiler_sweep.py --changed` (both
  build dirs). Commit `feat(setup): example line gains a laser, cleanup, second pipette and roughing side`.

### Task 3: Cocktail tank and its script

**Files:**
- Modify: `configs/examples/sim.toml` (in force `[volumes.cocktail_tank] composition = "cocktail"` with a comment
  on what one shot gives; commented example names gain `stage1`, `cocktail_tank`)
- Create: `configs/examples/scripts/extraction/sim_cocktail.py` (as `sim_air.py`, with `Q2` then `Q1`; header says
  it is not in the shipped queue)
- Test: `tests/sim/test_sim_config.cpp` (`TheExampleFileIsTheDefaults`: topology gains the new canvas names, with
  `cocktail_tank` role `Tank`; the in-force list gains the cocktail line), `tests/integration/test_lab_sim.cpp`
- Modify: spec section 7 (decision 4)

What the test leans on, checked 2026-10-16 in `tests/integration/test_lab_sim.cpp`:

- `lab.rows({1})` is an air row: `ScratchLab::air_queue()` loads `experiment.sim-air.toml`, blank, air, air, air,
  blank (rows 0 to 4); `AScriptThatSkipsThePipetteMeasuresABlank` (line 710) takes `rows({1, 2})` and asserts
  `queue.runs[0].extraction.script == "sim_air"`. The new test asserts the same before it changes it.
- `extraction.script` is a plain settable field of a `QueueSpec` run: line 717 sets it to `"sim_blank"`, line 766 to
  `"sim_air_both"`, and the run takes the script of that name from the scratch copy's `scripts/extraction/`
  (`ScratchLab` copies all of `configs/examples`, so the new `sim_cocktail.py` is there).
- `sim_multicollect.toml` measures `{ Ar40 = "H1", Ar39 = "H2" }` (its line 49).

Figures, all derived, none tuned. The cocktail tank sets only `composition = "cocktail"` (spec 5.1), so it holds Ar40
at `SimSettings::tank_argon40` (`sim_system.hpp:147`, 3e-5 mbar; `sim_config.cpp:297` uses it for a listed volume
with no `argon40`) and the other species in `cocktail_ratios()` (`gas.hpp:63`: Ar36 1, Ar37 0.5, Ar38 0.1885, Ar39
20, Ar40 298.56). One shot, as `LabNumbers::first_shot()` computes it for air (tank 50 cc, pipette 0.1 cc, prep and
source 50 cc each, sensitivity 1e12 fA/mbar): 3e-5 x 50/50.1 x 0.1/50.1 x 50/100 x 1e12 = about 2.99e4 fA of Ar40,
and Ar39 that times 20/298.56: about 2.0e3 fA. The existing air test holds its Ar40 within 1 % of `first_shot()`
(line 652); H2's field-table peak is flat within 0.02 (`libs/setup/AGENTS.md`). The test computes its expectations
from `lab.settings()`, so no figure above is written into it.

These figures are provisional: Step 4 records what the run gives (`record_value`) and reports it in the commit
message. A measured value outside its band is a cause to find (the tank's Ar40, the pipette's volume, the field
table, fractionation, the blank), not a band to widen; a band is changed only when the cause shows the derivation
above was wrong, and the derivation in this plan and the test's comment are changed with it.

- [x] **Step 1: Failing test** in `test_lab_sim.cpp`. First generalise `numbers_of(const sim::SimSettings&)` to
  `numbers_of(const sim::SimSettings&, std::string_view tank = kTank, std::string_view pipette = kPipette)` (callers
  unchanged; `n.tank`, `n.pipette` and `n.tank_ar40` read the given names). Then:

```cpp
// The second pipette carries the built-in cocktail: Ar39, which air has none
// of, on H2 where sim_multicollect puts it. One shot is the cocktail tank's
// pressure in one pipette spread over prep and the source, as an air shot is
// the air tank's (first_shot); its Ar39 is that times the cocktail's 20/298.56.
TEST_F(LabSim, ACocktailShotPutsAr39WhereThePlanSays) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const LabNumbers cocktail = numbers_of(lab.settings(), "cocktail_tank", "cocktail");
  const auto ratios = sim::cocktail_ratios();
  const double ar39_per_ar40 = ratios[sim::index(sim::Species::Ar39)] / ratios[kAr40];
  QueueSpec queue = lab.rows({1});
  ASSERT_EQ(queue.runs.size(), 1u);
  ASSERT_EQ(queue.runs[0].extraction.script, "sim_air");  // row 1 is an air shot
  queue.runs[0].extraction.script = "sim_cocktail";
  const auto runs = lab.run(queue);
  ASSERT_EQ(runs.size(), 1u);
  const auto ar39 = measured(runs[0].record, "Ar39", "H2");
  const auto ar40 = measured(runs[0].record, "Ar40", kFaraday);
  ASSERT_TRUE(ar39);
  ASSERT_TRUE(ar40);
  record_value("cocktail_ar40_fA", ar40->value());
  record_value("cocktail_ar39_fA", ar39->value());
  record_value("cocktail_expected_ar40_fA", cocktail.first_shot());
  EXPECT_NEAR(ar40->value(), cocktail.first_shot(), 0.01 * cocktail.first_shot());  // as the air shot (line 652)
  const double ar39_expected = cocktail.first_shot() * ar39_per_ar40;
  EXPECT_NEAR(ar39->value(), ar39_expected, 0.03 * ar39_expected);  // H2's peak flat within 0.02, and noise
  EXPECT_NEAR(ar40->value() / ar39->value(), 1 / ar39_per_ar40, 0.03 / ar39_per_ar40);
}
```

  `sim::cocktail_ratios()` and `sim::index` come from `pychron/sim/gas.hpp`, which the file already reaches for
  `kAr40`. Skips as the other `LabSim` cases do without embedded Python.
- [x] **Step 2:** Run `pychron_integration_tests --gtest_filter='LabSim.ACocktail*'`: FAIL (no script).
- [x] **Step 3:** Write `sim.toml` lines and `sim_cocktail.py`; update `TheExampleFileIsTheDefaults`.
- [x] **Step 4:** Run `pychron_sim_tests --gtest_filter='SimConfig.*'`, `pychron_integration_tests --gtest_filter='LabSim.*:ExampleLineSim*'`,
  `pychron_setup_tests --gtest_filter='*Profile*'` (byte-equal `sim.toml`). Expected: PASS; a ratio off by more than
  3 % is a cause to find (fractionation, blank), not a tolerance to widen.
- [x] **Step 5:** Edit spec 7. `quality_check.py`, `compiler_sweep.py --changed`. Commit
  `feat(setup): a cocktail tank and pipette script for the example lab`.

### Task 4: Heater and cryostat

**Files:**
- Modify: `configs/examples/extraction_line.toml`: transports `cryostat`, `bakeout_plc` (`kind = "sim"`,
  `timeout_ms = 2000`); driver `cryostat` (`lakeshore`, model "335", inputs `["A", "B"]`, ranges as NMGRL's
  converted file); driver `bakeout_plc` (`plc2000_heater`, enable 1, use_pid 2, setpoint 11, readback 21);
  `[cryo] driver = "cryostat"`, `tolerance_k = 1.0`, `timeout_s = 600`, `[cryo.setpoints] Ar_freeze = [70.0, 0.0]`,
  `Ar_release = [300.0, 0.0]`; `[[heaters]] name = "bakeout"`, `driver = "bakeout_plc"`, `description = "Line
  bakeout tape"`, `units = "C"`. The `cryo` stage's comment names `[cryo]`.
- Test: `tests/systems/test_canvas_cross_validate.cpp` (`TheExampleHasEveryElementKind` gains heater and cryo
  asserts), `tests/ui/test_heater_dock.cpp:110-118`, `tests/ui/test_cryo_dock.cpp:123-131`

- [ ] **Step 1: Failing tests.** In `TheExampleHasEveryElementKind`: `sys->heaters.size() == 1u`,
  `sys->cryo.has_value()`, its setpoints' keys `{"Ar_freeze", "Ar_release"}`. In the UI: new
  `theExampleLineHasTheDock` in each dock test (`make_example_line()`, `MainWindow`, dock not null); the "plain"
  half of `theMainWindowHasTheDockOnlyWithHeaters` uses `make_cryo_line()`, of `...WithACryostat` uses
  `make_heater_line()`.
- [ ] **Step 2:** Run `pychron_systems_tests --gtest_filter='ExampleConfigs.*'` and the two dock binaries: FAIL.
- [ ] **Step 3:** Write the config.
- [ ] **Step 4:** Run the two dock binaries, `pychron_ui_test_docks`, `pychron_ui_test_core_bridge`,
  `pychron_ui_test_canvas_view`, and `ctest --preset dev-ui -R '^ui\.'`; `pychron_systems_tests --gtest_filter='ExampleConfigs.*:ExtractionLine.*'`;
  `pychron_integration_tests --gtest_filter='LabSim.*:ExampleLineSim*'`. Expected: PASS.
- [ ] **Step 5:** `quality_check.py`, `compiler_sweep.py --changed --build-dir build/dev-ui`. Commit
  `feat(setup): example line has a bakeout heater and a cryostat`.

### Task 5: User guide

**Files:** `docs/user/01-getting-started.md:618-620` ("five valves (A, B, C, P1, P2), a manual valve (M1), ... two
gauges (IG1, PG1) and a pipette": the new counts and names), `docs/user/02-extraction-line.md` (a short tour of the example: what each part shows, the `stage1`
stage list, excerpt at 427-470 gains one heater and `[cryo]`), `docs/simulator.md` (the example sketch at 331 and
the volumes paragraph at 403-416: new tank, closed getters).

- [ ] **Step 1:** Write; check every name against the two config files.
- [ ] **Step 2:** Commit `docs(setup): user guide follows the fuller example line`. Docs only: no build.

### Task 6: Whole branch

- [ ] **Step 1:** `cmake --build --preset dev-ui` (builds what `dev` builds) and `ctest --preset dev-ui`.
  Expected: all pass, `pychron_install_e2e_tests` included (persistence on).
- [ ] **Step 2:** `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.
- [ ] **Step 3:** Review the branch against the spec with `cavecrew-reviewer` on `model: sonnet`; fix findings,
  rerun what they touch.
- [ ] **Step 4:** Spec and code agree (decisions 1 to 4 written in); spec status line notes "built".

## Open questions

None.
