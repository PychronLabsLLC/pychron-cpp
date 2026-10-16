# A fuller example extraction line: design

Status: agreed 2026-10-15 (owner settled section 8). Written 2026-10-13 (nightly).
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
                    getter1         getter2  cryo        pump_power
                       |               |       |
                      G1   IG2        G2       Y
                       |   |           |       |
  laser --L-- stage1 --+---+--A-- [     prep     ] --B-- spec --I-- ion
                 |                  |    |     |
                 D                 P1   Q1     C --+-- turbo --+-- R1 -- M1 --+
                 |                  |    |         |           |              |
                 |                 air  cocktail  IG1         PG1   M2 --- rough
                 |                  |    |                           |        |
                 |                 P2   Q2                           |       CG1
                 |                  |    |                           |
                 |           air_tank  cocktail_tank                 |
                 |                                                   |
              turbo2 ---------------------------------- R2 ----------+
```

Drawn (plan decision 3): cleanup above main row, pipettes below `prep`, no
pipe crossing another; canvas 1450 x 800. Gauges: IG1 (prep turbo), IG2
(stage1, joined to it with no valve), PG1 (turbo foreline, ahead of `R1`),
CG1 (on `rough`). `R1`-`M1` and `R2`-`M2` are valve to valve: simulator puts
a pipe volume between each pair, so every valve joins two volumes
(`valves_without_physics()` empty). PG1 stays in turbo's region (tee
`turbo`/`R1`/`PG1`): with `R1` closed, region `C` opens to is the one it was
(prep, turbo, IG1, PG1), so `ExampleLineSim` and shipped numbers hold.
Roughing pump reached only through a manual valve (`M1` or `M2`).

| Added | Name | Kind | In line config as |
|---|---|---|---|
| stage | `laser` | `kind = "laser"`, `symbol = "laser"` | chamber of driver `co2`: canvas stage names no driver (`loader.cpp`), so tie is name and comment only; display "CO2 laser" |
| stage | `stage1` | plain | replaces `bone` (owner, 2026-10-15) |
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
| manual valve | `M2` | manual | turbo 2 foreline (behind `R2`) to `rough`. Not a vent: a dead-end valve joins one volume, has no physics in simulator |
| gauges | `IG2`, `CG1` | gauge | `IG2` on `ig_controller` channel 3; `CG1` on a second controller `cg_controller` (`gp_microion`, sim, address 1, channel 2) |
| heater | `bakeout` | `[[heaters]]` | driver `bakeout_plc` (`plc2000_heater`, sim), units C |
| labels | "Cleanup", "Roughing" | label | |

Result: 13 valves, 2 manual, 2 rough, 4 gauges, 13 stages, 2 pipettes,
1 switch, 1 heater, cryo with 2 setpoints.

Kept exactly: `A B C P1 P2 M1 IG1 PG1 prep spec turbo rough air_tank air
pump_power`, their addresses, interlocks, aliases. `A` stays "first stage to
prep", `B` inlet, `C` prep to turbo.

Interlocks added: `Q1`/`Q2` mutual; `D` with `A` (as `C` with `A`); `L`
none.

Addresses on `actuator1`: `L D G1 G2 Y I Q1 Q2` `"10"` to `"17"`, `R1 R2`
`"18"`, `"19"` (`"9"` is `pump_power`). Rough valves are `[[valves]]` in
line config, `[[rough_valve]]` on canvas.

Found building it (2026-10-16):

- IG2 joined to `stage1` with no valve adds its gauge volume to `stage1`'s
  region: `ExampleLineSim.GasFlowsThroughOpenValvesAndInterlocksHold`
  expands into 14.5 cc, not 13.5 (stage1 12.5, IG2 1, prep 1). No shipped
  script opens `A`; shipped numbers unchanged.
- `elctl`'s own quiet simulator (`apps/elctl/src/line.cpp`, used by `probe`,
  `read`, `state`) answered only `proxr_relay`, `pfeiffer_maxigauge`,
  `plc2000_heater`: a `gp_microion` wire stayed silent and `elctl probe` on
  the example failed. It now answers `gp_microion` as the MaxiGauge (sensor n
  reads n * 1e-9), at the driver's address. No new driver or physics.
- `tests/setup/test_connection.cpp` renames valve A out of a copied line:
  `D` now interlocks with A too, so both references are renamed.

### 5.2 Simulator

- Rule: with every added valve closed (their state at start), gas a shipped
  script moves is what it moves today: `prep` and `spec` keep 50 cc,
  `valves.B`, `air_tank`, `pumps.turbo` untouched. Install e2e and `LabSim`
  numbers do not move; if one does, the element that moved it starts
  isolated, the test is not retuned.
- Getters start behind closed `G1`, `G2` for that reason (owner,
  2026-10-15).
- `sim.toml` gains only: `[volumes.cocktail_tank]`, and comment lines for
  the new names where it lists examples (its commented `[volumes.bone]`
  follows the rename).
- `extraction_line.state.toml`, if shipped state exists for example line:
  new valves closed.

### 5.3 Scripts

- Shipped scripts unchanged (owner, 2026-10-15). One new example script,
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
- `tests/sim`: example `sim.toml` loads (`TheExampleFileIsTheDefaults`:
  new canvas names, `cocktail_tank` a tank, its line in force).
- `tests/integration`: `ExampleLineSim`, `LabSim`, install e2e unchanged and
  passing. New `LabSim` case: `cocktail` shot (`sim_cocktail` in place of an
  air row's `sim_air`) gives Ar39 on detector `sim_multicollect` puts it on
  (`H2`), Ar40 within 1 % of one shot of the cocktail tank, Ar39 that times
  20/298.56 within 3 %. There, not in `tests/sim`: `tests/sim` knows no plan
  and no detector (plan decision 4).
- `tests/ui` `test_canvas_view`: element counts; heater and cryo docks show
  for example line.
- Full `ctest --preset dev-ui`.

## 8. Owner, 2026-10-15

1. `bone` renamed `stage1` everywhere it is named (tests, user guide,
   `sim.toml` comment).
2. Getters behind `G1`, `G2`, closed at start; no expected value measured
   again (5.2).
3. Shipped scripts stay on `A B C P1 P2` (5.3).
4. Laser chamber is its own stage `laser`, behind valve `L` (5.1).
5. Heater and cryo in the example line: wanted, so their docks show for
   every user of the example (5.1, tests 7).
6. `CG1` on a second controller of another make (`gp_microion`) (5.1).

## Open questions

None.
