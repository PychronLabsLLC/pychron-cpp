# A fuller example extraction line: design

Status: draft, 2026-10-13 (nightly). Not agreed. Open points: section 8.
Task T076. Owner: add more elements to example extraction line canvas; more
similar to NMGRL, not 1:1, no NMGRL names (bone, jan, ...); example should
represent what pychron-cpp has available.

## 1. Today

| | `configs/examples` | `configs/examples/nmgrl` |
|---|---|---|
| valves | 5 (`A B C P1 P2`) | 30 on canvas, 33 in line |
| manual valves | 1 (`M1`) | 7 |
| rough valves | 0 | 3 |
| gauges | 2 (`IG1 PG1`) | 5 |
| stages | 6 (`bone prep spec turbo rough air_tank`) | 31 |
| pipettes | 1 (`air`) | 2 |
| switches | 1 (`pump_power`) | 0 |
| heaters, cryo | none | 1 heater, cryostat with 4 setpoints |
| connections / elbows / tees / labels | 9 / 1 / 2 / 1 | 63 / 3 / 2 / 4 |

Available in tree and absent from example:

- Stage kinds `getter`, `laser`; symbols `getter`, `ion_pump`, `laser`
  (`libs/systems/src/canvas/loader.cpp:49-63`). Example's `co2` laser driver
  exists in `extraction_line.toml` and has no stage on canvas.
- `[[rough_valve]]` canvas element.
- `[[heaters]]` and `[cryo]` with `[cryo.setpoints]` (heater dock, cryo
  dock); drivers `plc2000_heater`, `lakeshore`.
- A second tank and pipette (cocktail): `compositions.cocktail` is built in
  to simulator (`sim.toml`).
- More than one gauge controller kind (`gp_microion`, `varian_xgs600`,
  `plc2000_gauges` beside `pfeiffer_maxigauge`).

Example already carries one NMGRL name: stage `bone`.

## 2. Who reads the example line

58 files under `tests/` and `apps/` name `configs/examples` or the UI
fixture's example line (`tests/ui` 17, `tests/systems` 12, `tests/integration`
8, `tests/experiment` 8, `apps/elctl/tests` 5, others 8). Held to it:

- `tests/setup/test_profiles.cpp` and `tests/integration/test_install_e2e.cpp`
  (`libs/setup/AGENTS.md`): shipped queue `ba, a, a, a, ba`, run ids, airs'
  raw Ar40/Ar36 within 3 % of 303.2.
- `tests/integration` `ExampleLineSim`, `LabSim` (`TheSameQueueGivesTheSame
  Numbers`, `SuccessiveShotsDeclineWithTheTank`).
- Shipped scripts `configs/examples/scripts`: `sim_extract.py`,
  `sim_blank.py`, `laser_extract.py`, `sim_pump.py` name `A B C P1 P2`.
- Aliases `valves.inlet = "B"`, `valves.outlet = "C"`.
- `sim.toml` numbers (`volumes.air_tank`, `valves.B`, `pumps.turbo`).
- `docs/user/02-extraction-line.md` and its screenshots.
- `tests/core/config_fixtures.hpp` is a copy of its own, not the file.

## 3. Goals

- Example shows one of every element kind the canvas and line config offer.
- Recognisable as an argon line (laser, two-stage cleanup, pipettes,
  spectrometer with ion pump, roughing side), about a third of NMGRL's
  size: learnable in one look.
- Every existing name keeps its meaning. Shipped queue measures what it
  measures now.

## 4. Out of scope

- NMGRL's layout or names. Two spectrometers, furnace, diode.
- New element kinds, canvas features, drivers, simulator physics.
- Changing shipped plans, queue, field tables.
- `configs/examples/nmgrl`: stays as converted.

## 5. Design

### 5.1 The line

```
 laser --L-- stage1 --A-- prep --B-- spec --I-- ion
               |  \        | | \
              G1   D      G2 Y  C --+-- turbo --R1-- M1 -- rough
               |    \      | |      |              |
           getter1   \ getter2 cryo IG1           PG1
                      \
                       +--(to turbo2)--R2--+ (same rough)
 prep also: P1/air/P2 -- air_tank ;  Q1/cocktail/Q2 -- cocktail_tank
 gauges: IG1 (prep turbo), IG2 (stage1), PG1 (foreline), CG1 (rough manifold)
```

| Added | Name | Kind | In line config as |
|---|---|---|---|
| stage | `laser` | `kind = "laser"`, symbol | region of driver `co2`; display "CO2 laser" |
| stage | `stage1` | plain | replaces `bone` (section 8, point 1) |
| valve | `L` | valve | laser chamber to `stage1` |
| valve | `D` | valve | `stage1` to second turbo |
| stage | `turbo2` | `pump` | |
| stages | `getter1`, `getter2` | `getter`, symbol | |
| valves | `G1`, `G2` | valve | getter isolation |
| stage | `cryo` | plain, labelled "Cryo trap" | `[cryo]` driver `cryostat` (`lakeshore`, sim), setpoints `Ar_freeze`, `Ar_release` |
| valve | `Y` | valve | cryo trap isolation |
| stage | `ion` | symbol `ion_pump` | |
| valve | `I` | valve | spectrometer to ion pump |
| stage | `cocktail_tank` | `tank` | `[volumes.cocktail_tank] composition = "cocktail"` |
| pipette | `cocktail` | pipette | `[[pipettes]]` inner `Q1`, outer `Q2`, interlocked as `P1`/`P2` |
| rough valves | `R1`, `R2` | `rough_valve` | turbo and turbo2 forelines |
| manual valve | `M2` | manual | rough manifold vent |
| gauges | `IG2`, `CG1` | gauge | `IG2` on `ig_controller` channel 3; `CG1` on a second controller `cg_controller` (`gp_microion`, sim) |
| heater | `bakeout` | `[[heaters]]` | driver `bakeout_plc` (`plc2000_heater`, sim), units C |
| labels | "Cleanup", "Roughing" | label | |

Result: 13 valves, 2 manual, 2 rough, 4 gauges, 13 stages, 2 pipettes,
1 switch, 1 heater, cryo with 2 setpoints.

Kept exactly: `A B C P1 P2 M1 IG1 PG1 prep spec turbo rough air_tank air
pump_power`, their addresses, interlocks, aliases. `A` stays "first stage to
prep", `B` inlet, `C` prep to turbo.

Interlocks added: `Q1`/`Q2` mutual; `D` with `A` (as `C` with `A`); `L`
none.

### 5.2 Simulator

- Rule: with every added valve closed (their state at start), gas a shipped
  script moves is what it moves today: `prep` and `spec` keep 50 cc,
  `valves.B`, `air_tank`, `pumps.turbo` untouched. Install e2e and `LabSim`
  numbers do not move; if one does, the element that moved it starts
  isolated, the test is not retuned.
- Getters start behind closed `G1`, `G2` for that reason (section 8,
  point 2).
- `sim.toml` gains only: `[volumes.cocktail_tank]`, and comment lines for
  the new names where it lists examples (its commented `[volumes.bone]`
  follows the rename).
- `extraction_line.state.toml`, if shipped state exists for example line:
  new valves closed.

### 5.3 Scripts

- Shipped scripts unchanged (section 8, point 3). One new example script,
  not in shipped queue: `scripts/extraction/sim_cocktail.py`, `sim_extract`
  with `Q1`/`Q2`, so the second pipette is not decoration.

### 5.4 Canvas

- `canvas.size` grows (1000 x 700 to about 1400 x 800); existing elements
  keep relative arrangement so the ASCII sketch at file head and user guide
  still read the same way. Sketch redrawn.
- Elbows and tees where a line turns; no crossing connections.
- A wired gauge is not drawn over a valve (gauge history plan, canvas
  overlap test, covers example canvas).

### 5.5 Install profiles

- `profiles/extraction-line-starter` and instrument profiles: whether they
  copy example line files is checked in the plan; where they do,
  `test_profiles.cpp` is edited in same commit as files
  (`libs/setup/AGENTS.md`).

## 6. Rules

- No name from `configs/examples/nmgrl` in the example.
- Every canvas element that is a device has its entry in
  `extraction_line.toml` and the reverse (`cross_validate`): loading the
  example gives no diagnostic.
- Every test reading the example passes with edits limited to counts and
  the `bone` rename; none retuned, none removed.
- Shipped queue's records: same run ids, same numbers within tests'
  existing tolerances.

## 7. Tests

- `tests/systems`: example line and canvas load with no diagnostic; counts
  of section 5.1.
- `tests/sim`: example `sim.toml` loads; `cocktail` shot gives Ar39 on the
  detector the plan puts it on (new case).
- `tests/integration`: `ExampleLineSim`, `LabSim`, install e2e unchanged and
  passing.
- `tests/ui` `test_canvas_view`: element counts; heater and cryo docks show
  for example line.
- Full `ctest --preset dev-ui`.

## 8. Open points for owner

1. `bone` is an NMGRL name and is the example's first stage, named in about
   19 test files and user guide. Rename to `stage1` (draft), another name,
   or keep?
2. Getters behind valves closed at start (draft, keeps every number), or
   open on the line as in a real lab, with tests' expected values measured
   again?
3. Shipped scripts stay on `A B C P1 P2` only (draft), or should
   `sim_extract` use the new elements (getter time, cryo), which changes
   what the shipped queue does?
4. Laser chamber on canvas as its own stage `laser` (draft), or is `stage1`
   the laser chamber?
5. Heater and cryo in the example line mean heater and cryo docks appear
   for every new user of the example. Wanted?
6. Second gauge controller of another make (`gp_microion`) to show two
   kinds (draft), or all four gauges on the one MaxiGauge?
