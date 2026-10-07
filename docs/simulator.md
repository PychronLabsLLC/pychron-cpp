# The simulated lab

What pychron simulates when it runs with no hardware, how to give the
simulated line your own numbers (`sim.toml`), and what the example lab reads
when it runs air shots and blanks. The code is `libs/sim`; the design is
`superpowers/specs/2026-10-06-lab-simulator-design.md`.

## What it is for

- Trying the program, a queue, an extraction script or a conditional without
  an instrument, and seeing signals that follow what the valves did: an air
  shot reads an air shot, a blank reads a blank, and a script that forgets
  the pipette reads a blank too.
- Rehearsing and teaching. A queue of five analyses that takes fifty minutes
  on a lab runs in a minute and a half at `--sim-speed 50`.
- Tests: the same lab, started the same way, gives the same numbers every
  time.

A line is simulated when its transports are `kind = "sim"` in
`extraction_line.toml`, or when the program is started with `--sim`, which
makes every transport one. A spectrometer is simulated when every transport
and driver in its file is a simulator's (`spectrometer.sim-integrated.toml`).
When both are, the simulated spectrometer measures the gas in the simulated
line. Nothing has to be switched on for that, and `--sim` need not be given
if the files are simulated already.

## What is modelled, and what is not

Modelled:

- **Gas by species.** Six of them: Ar36, Ar37, Ar38, Ar39, Ar40 and `active`,
  which stands for everything a getter removes (N2, O2, H2O, CO2) as one gas
  of mass 28. Air is 298.56 Ar40 and 0.1885 Ar38 to one Ar36, and active gas
  106 times the argon (argon is 0.93 % of air). A volume's pressure is the
  sum over the six, and that is what its gauge reads.
- **Volumes and valves.** Each volume holds a partial pressure of each
  species. An open valve carries each species from the higher partial
  pressure to the lower at the valve's conductance, so two volumes come to
  their volume-weighted mean with the time constant
  `V1 V2 / ((V1 + V2) C)`. Flow is molecular: a species of mass M moves
  `sqrt(39.962 / M)` times as fast as Ar40, so Ar36 arrives 5 % faster and an
  inlet closed too soon fractionates. A closed valve carries nothing.
- **Pumps.** A pump takes its volume towards its base pressure at
  `speed / V`. What is left at base is air. Whatever is open to the pump
  follows through the valves between.
- **Getters.** A getter is a pump of active gas only (1 L/s, to nothing); it
  leaves argon alone.
- **Walls.** Every volume gives off gas in proportion to its size: argon in
  the ratios of air (the blank), and far more active gas. A volume shut off
  from its pumps rises on its gauge and builds up a blank.
- **Leaks.** Air into one volume at a constant rate.
- **Tanks and pipettes.** Nothing special: a tank is a volume that starts
  full, a pipette a small volume between two valves. The script's own valve
  sequence loads and delivers the shot, so the shot is the tank's pressure in
  the pipette's volume, each shot leaves the tank at
  `V_tank / (V_tank + V_pipette)` of what it was, a pipette opened before it
  has filled delivers less, and one whose tank valve is left open delivers
  the tank.
- **The ion source.** The spectrometer stage is a volume like the others,
  with two terms of its own: it uses up each argon isotope at a small
  constant fraction a second (a large signal falls during a static
  measurement), and it gives a little Ar40 back (memory: a small signal
  rises).
- **The beam.** A detector on a peak reads that isotope's partial pressure in
  the source times the sensitivity, plus its baseline, plus noise. Peak
  shapes, magnet moves, deflections, dead time and the ion counter's plateau
  are the simulated spectrometer's, as before.
- **Noise that repeats.** See [Reproducibility](#reproducibility).

Between two valve movements the equations are solved exactly, with no time
step. A valve that equilibrates in a millisecond and walls that take hours
to matter are both right, and a reading does not depend on how often
anything else was read.

Not modelled:

- **Gas from a sample.** Firing the laser or heating the furnace releases
  nothing. The example's stock scripts let in a pipette of air in its place
  (see [the example lab](#the-example-lab)).
- **Ages.** Amounts are plausible, not calibrated: there is no irradiated
  sample, no J, and a simulated analysis has no meaningful age.
- **Anything but argon and one active gas.** No helium, neon, krypton or
  xenon, no hydrocarbons, no HCl or other interferences on the argon masses.
- **Viscous flow, adsorption, temperature.** A valve's conductance is the
  same at any pressure. Heaters and the cryostat are simulated as devices
  (they reach their setpoints) and do nothing to the gas.
- **Fractionation in the source.** The source uses up every argon isotope at
  the same rate, and its memory is Ar40 only.
- **A getter on the example line.** The model has getters; the example canvas
  draws none, so the active gas of an air shot stays in the source until it
  is pumped out.
- **A tee whose three ends are all valves.** Those three valves carry no gas
  (see below).
- **Units on gauges.** A simulated gauge reports the model's number, which is
  mbar, under whatever unit the gauge is configured with (the example's say
  torr). Nothing is converted.

The numbers are the model's defaults or the ones you give it. They are not
your line's until you make them so: do not read a real pump-down or
equilibration time off the simulator.

## How the canvas becomes the model

The simulated line is built from `canvas.toml`. What each thing on the canvas
is to the gas:

| On the canvas | In the model | Size if the canvas gives none |
|---|---|---|
| `[[stage]]` with `kind = "pump"` (or the `turbo` or `ion_pump` symbol) | a volume with a pump: 50 L/s to 1e-9 mbar | 50 cc |
| `[[stage]]` with `kind = "getter"` (or the symbol) | a volume with a getter | 50 cc |
| `[[stage]]` with `kind = "tank"` | a volume that starts with 3e-5 mbar of Ar40 and the rest of air with it (3.2e-3 mbar in all) | 50 cc |
| `[[stage]]` with `kind = "spectrometer"` (or the `spectrometer` or `quadrupole` symbol) | the ion source: the volume the beam reads | 50 cc |
| any other `[[stage]]` (`volume`, `laser`, no kind) | a plain volume | 50 cc |
| `[[pipette]]` (or a stage with `kind = "pipette"`) | a small volume between its two valves | 0.1 cc |
| `[[gauge]]` | a small volume; it reads the pressure of whatever it is joined to | 1 cc |
| `[[valve]]`, `[[manual_valve]]` | a valve of conductance 0.1 L/s for Ar40 | |

A stage's `volume` is its size in cc. Every volume that is not a tank starts
at 1e-8 mbar of air, and every valve starts closed; the line then puts back
the valve states it remembered from its last run
(`extraction_line.state.toml`), since a simulated controller has nothing to
read back. A manual valve moves in the model when you report it moved.

Things joined on the canvas with no valve between them are one volume: its
size is the sum, and each name reads the pressure of the whole. On the
example canvas the turbo and the two gauges on its pipe are one volume of
52 cc.

If the canvas has two spectrometer stages, the first by name is the source
and the log has a warning for the other. If it has none, the simulated
spectrometer does not read the line and shows its fixed argon instead
(1e6 fA of Ar40).

A gauge that `extraction_line.toml` has and the canvas does not is a 1 cc
volume of its own, joined to nothing and with no walls: it reads the pressure
it started at (1e-8 mbar), whatever the valves do.

### Which valves carry gas

A valve carries gas when it stands between exactly two volumes. Real
canvases also draw pipe straight from one valve to the next, and that is
handled:

- **Two valves joined directly** get the pipe between them as a volume of
  1 cc, named for the two with `~` between, the names in byte order (capitals
  before small letters): valves `V2` and `V1` joined make the volume
  `V1~V2`. Both valves then carry gas. The pipe can be sized or filled in
  `sim.toml` like any volume.
- **Two valves and a volume on one tee** are both on that volume. No pipe is
  made.
- **Anything else carries no gas**: a valve joined to nothing, to one volume
  only, to three or more, or with the same volume on both sides. It still
  opens and closes, and interlocks and scripts see its state. It only moves
  no gas.

Because `~` names a pipe, a simulated line refuses a canvas valve with `~` in
its name:

```
sim: valve 'V1~V2': a valve's name may not contain '~' on a simulated line (the pipe between two valves joined directly is named '<a>~<b>')
```

The valves that carry no gas are listed once, in one log line at `info`, when
the line is built. It is in the Log dock of `pychron-ui`, and in `pychron.log`
if `extraction_line.toml` has `[logging] dir`. The example line has none. The
full-size example line (`configs/examples/nmgrl`) logs:

```
[info] extraction_line: sim: 7 valve(s) carry no gas in the simulation: FE (it is joined to no volume), FF (it is joined to no volume), FG (it is joined to no volume), G (it is joined to one volume only ('CO2')), GP50Manual_Rough (it is joined to one volume only ('GP502')), NP-10CRough (it is joined to one volume only ('NP-10C')), RDiode (it is joined to one volume only ('Diode'))
```

To give such a valve its gas, draw what is on its far side: a stage, or a
connection to the volume it leads to.

## sim.toml

Optional. Without it the defaults in the tables below give a working lab.
With it you change numbers, for the whole line or for one volume, valve, pump
or detector by name. It cannot change what a stage is: that is the canvas's
`kind`.

### Where it is found

- `extraction_line.toml` may name it, relative to its own folder:

  ```toml
  [sim]
  file = "sim.toml"
  ```

  A file named there and not found is an error.
- Otherwise a file called `sim.toml` beside `extraction_line.toml` is used if
  there is one.
- It is read only when the line is loaded with its canvas, since its names
  are the canvas's. `pychron-ui` and `elctl exp run` load the canvas. A tool
  that loads the line alone does not read it.

### What is in force

Three layers, each over the one before: the simulator's built-in defaults;
whatever the program that loads the line sets for itself (the applications
set nothing; tests do); the keys present in the file. A key that is not in
the file changes nothing. `configs/examples/sim.toml` lists every key with
its default, each commented out, and sets five numbers of its own at the
end.

### Keys

Units throughout: pressure mbar, size cc, conductance and pump speed L/s, gas
given off mbar L/s, time s. A number outside its range is refused.

`[defaults]`: the whole line.

| Key | Unit | Default | Range | Meaning |
|---|---|---|---|---|
| `pressure` | mbar | 1e-8 | 0 to 1e4 | what every volume starts at, as air, unless `[volumes.*]` says otherwise (a tank starts full) |
| `volume_cc` | cc | 50 | 1e-6 to 1e9 | a stage the canvas gives no `volume` |
| `pipe_cc` | cc | 1 | 1e-6 to 1e9 | the pipe between two valves joined directly |
| `gauge_cc` | cc | 1 | 1e-6 to 1e9 | a gauge, on the canvas or off it |
| `valve_conductance` | L/s for Ar40 | 0.1 | 0 to 1e9 | every valve not in `[valves.*]`. Two 50 cc volumes equilibrate through 0.1 L/s with a time constant of 0.25 s |
| `outgassing` | mbar L/s of Ar40, per litre of volume | 5e-13 | 0 to 1e3 | what the walls give off: the blank. Ar36 and Ar38 come with it as in air. With the default sensitivity 5e-13 is a rise of 0.5 fA/s of Ar40 in any volume shut off from its pumps |
| `noise` | fraction of the reading | 0.01 | 0 to 10 | 1-sigma noise on every gauge reading; 0 for none |
| `seed` | whole number, 0 or more | 0x5eed | | the seed of the noise: of every gauge reading, and of every detector of a simulated spectrometer joined to the line. See [Reproducibility](#reproducibility) |

The walls also give off active gas, 1e-10 mbar L/s per litre. It has no key.
It is what a gauge on a static volume sees rise: 1e-10 mbar a second.

`[compositions.<name>]`: a gas by its ratios to Ar36, to be named by a
volume. Keys are the species; one not given is 0.

| Key | Unit | Range |
|---|---|---|
| `Ar36`, `Ar37`, `Ar38`, `Ar39`, `Ar40`, `active` | ratio to Ar36 | 0 to 1e9 |

Two are built in and need no table:

| Name | Ar36 | Ar37 | Ar38 | Ar39 | Ar40 | active |
|---|---|---|---|---|---|---|
| `air` | 1 | 0 | 0.1885 | 0 | 298.56 | 31773 (106 times the argon) |
| `cocktail` | 1 | 0.5 | 0.1885 | 20 | 298.56 | 0 |

A table called `air` or `cocktail` replaces the built-in one for the volumes
that name it. It does not change the air of the walls, of leaks, of pump
base pressures or of a volume that names no composition.

`[volumes.<name>]`: one volume, by its canvas name. That is a stage, a
pipette, a gauge (also one of `extraction_line.toml` that is not on the
canvas), or a pipe (`[volumes."V1~V2"]`).

| Key | Unit | Default | Range | Meaning |
|---|---|---|---|---|
| `composition` | name | `air` | `air`, `cocktail` or a `[compositions.*]` name | what gas it starts with |
| `argon40` | mbar of Ar40 | 3e-5 on a tank | 0 to 1e4 | how much: this much Ar40, and the rest in the composition's proportions |
| `pressure` | mbar in all | `[defaults] pressure` | 0 to 1e4 | how much, as a total instead. Give `argon40` or `pressure`, not both |
| `leak` | mbar L/s of Ar40 | 0 | 0 to 1e3 | air leaking in; the rest of the air comes with the Ar40 |
| `volume_cc` | cc | the canvas's `volume`, else by kind | 1e-6 to 1e9 | its size |

A `composition` with neither `argon40` nor `pressure` is at a tank's 3e-5
mbar of Ar40 on a tank and at `[defaults] pressure` in all anywhere else.
`argon40` needs a composition with Ar40 in it. No species may come out above
1e4 mbar.

`[valves.<name>]`: one valve of the canvas.

| Key | Unit | Default | Range | Meaning |
|---|---|---|---|---|
| `conductance` | L/s for Ar40 | `[defaults] valve_conductance` | 0 to 1e9 | smaller is slower: `tau = V1 V2 / ((V1 + V2) C)` |

`[pumps.<name>]`: the pump on a volume. A stage of kind `pump` has one
without being asked; a section here changes it, or puts a pump on any other
volume. A key left out stays as it was.

| Key | Unit | Default | Range | Meaning |
|---|---|---|---|---|
| `speed` | L/s | 50 | 0 to 1e9 | the pump's own volume empties with `tau = V / speed` |
| `base` | mbar in all | 1e-9 | 0 to 1e4 | the pressure it ends at. It is air, so 0.93 % of it is Ar40 |

`[spectrometer]`: the ion source.

| Key | Unit | Default | Range | Meaning |
|---|---|---|---|---|
| `sensitivity` | fA per mbar | 1e12 | above 0, to 1e30 | beam at the peak top for each mbar of an isotope in the source |
| `consumption` | 1/s | 2e-5 | 0 to 1e6 | the fraction of each argon isotope the source uses up a second: 2.4 % in twenty minutes |
| `memory_fA_per_s` | fA/s | 0.01 | 0 to 1e30 | what the source gives back, as a rise of the Ar40 beam. As gas (divided by the sensitivity) it may not exceed 1e3 mbar/s |

`[detectors.<name>]`: one detector of the simulated spectrometer, by its name
in the spectrometer's file.

| Key | Unit | Default | Range | Meaning |
|---|---|---|---|---|
| `baseline` | fA (cps on a counting detector) | 0 | -1e30 to 1e30 | what it reads with no beam on it |
| `baseline_drift_per_h` | the same, per hour | 0 | -1e30 to 1e30 | how that changes, from when the program started |

Nothing else may be in the file: the seven sections above, and under each
the keys listed. A section may be given once.

### Errors you will meet

Every problem in the file is reported at once, each as
`file:line:key: message`, and the line does not load. After a name or key
that is not known the message lists the ones that are (it counts them when
there are more than twelve). From the example lab:

```
error: sim.toml:3:defaults.noise: value -1 out of range, must be from 0 to 10 as a fraction of the reading
sim.toml:5:volumes.perp: unknown volume 'perp'; known: IG1, PG1, air, air_tank, bone, prep, rough, spec, turbo
sim.toml:9:valves.B.conductence: unknown key; known: conductance
sim.toml:12:spectrometer.sensitivity: value 0 out of range, must be above 0 to 1e+30 fA per mbar
```

```
error: sim.toml:3:volumes.air_tank.pressure: give argon40 or pressure, not both
```

A section written twice is a TOML error:

```
error: sim.toml:3:toml: syntax error: Error while parsing table header: cannot redefine existing table 'defaults'
```

Detector names are the spectrometer's, so they are checked when the
spectrometer is loaded, not with the rest:

```
error: sim.toml: detectors.H3: unknown detector 'H3'; known: H2, H1, AX, L1, L2, CDD
```

`elctl exp validate` checks the queue, not the simulator: these errors come
when the line is loaded, from `elctl exp run` or `pychron-ui`.

## The spectrometer side

A detector sitting on the peak of an isotope reads

```
partial pressure in the source (mbar) x sensitivity (fA/mbar) + baseline + noise
```

- **Sensitivity** is one number for every isotope and detector. With the
  default 1e12 fA/mbar, 1e-7 mbar of Ar40 in the source is 1e5 fA.
- **A Faraday** reads fA, with Gaussian noise of 1 fA plus 0.1 % of the
  signal on each reading.
- **An ion counter** reads counts per second, one for each fA that reaches
  it, less what it does not count, with counting (Poisson) noise for the
  integration time and 25 ns of dead time.
- **Consumption and memory** move the signal during a static measurement.
  With the defaults a signal of S fA changes by `-2e-5 x S + 0.01` fA/s,
  plus what the source's own walls give (the `outgassing` rise). A shot of
  1e5 fA falls 2 fA a second. A blank rises.
- **Baselines** are what a detector reads off the peak. The example plan
  measures them at mass 34.2, where there is no gas. They are 0 unless
  `[detectors.*]` sets them.

### Why 40/36 reads high as counted

In the example Ar40 is measured on a Faraday (H1) and Ar36 on the ion
counter (CDD). The simulated counter counts a fraction of the ions that reach
it, set by its voltage on the multiplier's plateau: 98.47 % at the example's
1450 V. So air's 298.56 reads `298.56 / 0.9847 = 303.2` as the two detectors
report it. That is the counter's yield, which a lab measures and a reduction
corrects for. It is not in the gas: the ratio of the two partial pressures
in the source is 298.56.

### A spectrometer simulated on its own

When the line is real, or its canvas has no spectrometer stage, a simulated
spectrometer has no simulated gas to read and shows a fixed argon instead:
1e6 fA of Ar40, 1e4 of Ar39, 1e3 each of Ar38 and Ar37, 3e3 of Ar36, whatever
the valves do. `sim.toml`'s `[spectrometer]` then does nothing. Its
`[detectors.*]` baselines still apply wherever the line is simulated.

## The example lab

`configs/examples` is a small lab that runs air shots and blanks:

```
bone --A-- prep --B-- spec
            |  \
           P1   C -- turbo (IG1, PG1) -- M1 -- rough
            |
           air        (the pipette, 0.1 cc)
            |
           P2
            |
         air_tank
```

`prep`, `spec` and `air_tank` are 50 cc, `bone` 12.5 cc. `B` is the inlet to
the spectrometer and `C` the valve to the turbo. `P1` and `P2` are
interlocked: never both open.

Its `sim.toml` sets five numbers, so that the signals are of a lab's size:

| Key | Value | Default | What it does |
|---|---|---|---|
| `[defaults] pressure` | 1e-10 | 1e-8 | the lab starts pumped down |
| `[defaults] outgassing` | 1.5e-13 | 5e-13 | a static volume rises 0.15 fA/s of Ar40: a blank of about 10 fA |
| `[volumes.air_tank] argon40` | 9.5e-5 | 3e-5 | one pipette of it, spread over prep and the source, is 9.5e-8 mbar: 9.5e4 fA of Ar40 |
| `[valves.B] conductance` | 0.02 | 0.1 | the inlet equilibrates with a time constant of 1.25 s, slow enough to see |
| `[pumps.turbo] base` | 1e-10 | 1e-9 | a pumped source reads about 1 fA of Ar40, inside a Faraday's noise |

### Running the air queue

`experiment.sim-air.toml` is five analyses: blank, air, air, air, blank. From
`configs/examples`, with a built `elctl`:

```bash
elctl -c extraction_line.toml --sim exp validate experiment.sim-air.toml \
    --spectrometer spectrometer.sim-integrated.toml
```

```
queue sim-air: 5 run(s), ETA 0:39:30
    0  ba          blank_air      sim_multicollect    0:07:45
    1  a           air            sim_multicollect    0:07:45
    2  a           air            sim_multicollect    0:07:45
    3  a           air            sim_multicollect    0:07:45
    4  ba          blank_air      sim_multicollect    0:07:45
ok: experiment.sim-air.toml
```

```bash
elctl -c extraction_line.toml --sim exp run experiment.sim-air.toml \
    --spectrometer spectrometer.sim-integrated.toml --sim-speed max \
    --data ~/pychron-sim-data
```

`--data` keeps the records out of the examples folder. The run prints each
analysis as it goes and ends with

```
queue completed
5/5 run(s) succeeded; records in /Users/you/pychron-sim-data/records
```

`--sim-speed` (it needs `--sim`) is how fast the lab's clock runs:

| | The five analyses (50 minutes of the lab's time) take |
|---|---|
| no `--sim-speed` | 50 minutes |
| `--sim-speed 50` | a minute and a half |
| `--sim-speed max` | about 5 seconds: the clock does not wait at all |

The speed does not change what is measured. `max` is `elctl`'s only;
`pychron-ui` takes a number.

In the window:

```bash
pychron-ui --examples --sim --sim-speed 50 --data ~/pychron-sim-data \
    --queue configs/examples/experiment.sim-air.toml
```

(In a build tree the program is `build/dev-ui/apps/pychron-ui/pychron-ui`,
or `build/dev-ui/apps/pychron-ui/Pychron.app/Contents/MacOS/Pychron` on
macOS; `elctl` is `build/dev/apps/elctl/elctl`.) Start the queue from the
experiment window and watch the canvas and the signal plot.

### What one air analysis does

The extraction script is `scripts/extraction/sim_air.py`, the measurement
plan `plans/sim_multicollect.toml`, the post-measurement script
`scripts/post_measurement/sim_pump.py`. Times are seconds from the start of
the analysis.

| t (s) | Who | What | In the model |
|---|---|---|---|
| 0 | script | close `C` | prep is off the turbo |
| 0 | script | open `P2`, wait 2 s, close `P2` | the pipette fills from the tank (in a millisecond) |
| 2 | script | open `P1`, wait 2 s, close `P1` | the shot expands into prep: 1.9e-7 mbar of Ar40 there |
| 4 | script | wait 30 s (the queue's `duration`) | the gas sits in prep |
| 36 | plan | sniff starts; 3 s later open `B` | Ar40 in the source goes from 10 fA to 5.2e4 fA in the first second and 9.3e4 fA in five |
| 59 | plan | close `B`: time zero | 9.46e4 fA of Ar40, 317 fA of Ar36, 40/36 of 298.56 |
| 59 to 542 | plan | five cycles, baseline, peak center | the source is static: Ar40 falls to 9.38e4 fA |
| 542 | script | open `C` and `B`, wait 50 s | the source empties through prep to the turbo, time constant 3 s: 1.4e4 fA after 6 s, 23 fA after 26 s |
| 592 | script | close `B` | the source is shut at 1.5 fA; prep is left pumping |

A blank (`sim_blank.py`) is the same with the pipette valves left alone.
Prep and the source are static for the same time, so what it measures is
what the walls gave off.

### What it reads

The time-zero intercepts of one run of the queue from a fresh copy of the
example lab, each less its baseline (they are under `results` in each
`records/<identifier>/<identifier>-<aliquot>.json`):

| Analysis | Ar40 on H1 (fA) | Ar36 on CDD (cps) | 40/36 as counted |
|---|---|---|---|
| `ba-1` | 10.31 ± 0.28 | 0.03 | |
| `a-1` | 94 670 ± 22 | 317.2 ± 2.0 | 298.5 |
| `a-2` | 94 387 ± 21 | 308.2 ± 1.9 | 306.2 |
| `a-3` | 94 208 ± 23 | 310.1 ± 2.1 | 303.8 |
| `ba-2` | 10.19 ± 0.23 | -0.01 | |

To print them:

```bash
python3 - ~/pychron-sim-data/records/*/*-[0-9].json <<'EOF'
import json, sys
for name in sys.argv[1:]:
    r = json.load(open(name))["results"]
    ar40 = r["intercepts"]["Ar40"]["value"] - r["baselines"]["H1"]["value"]
    ar36 = r["intercepts"]["Ar36"]["value"] - r["baselines"]["CDD"]["value"]
    print(name.split("/")[-1], round(ar40, 2), round(ar36, 2))
EOF
```

What to see in them:

- **The shot.** The tank holds 9.5e-5 mbar of Ar40. The pipette takes
  `50 / 50.1` of that, it expands into prep (`0.1 / 50.1`), and prep shares
  it with the source (a half): 9.462e-8 mbar, 94 621 fA.
- **The decline.** Each shot is `50 / 50.1 = 0.998` of the one before, 189 fA
  less. Three shots show it.
- **The ratio.** Ar36 is about 312 counts a second, so one analysis has its
  intercept to 0.6 % and its 40/36 scatters by that. The three together give
  302.8 as counted, where air over the counter's yield is 303.2, and 298.2
  once divided by the yield, where air is 298.56.
- **The slope.** During the measurement Ar40 falls about 1.7 fA/s: the
  source uses 1.9 fA/s of it and the walls and the memory give 0.16 back.
- **The blank.** About 10 fA: a pumped source holds about 1 fA of Ar40, and
  prep and the source then rise 0.15 fA/s for the minute they are static
  before time zero.
- **The gauges.** IG1 and PG1 are on the turbo's side of `C`, so they read
  1.0e-10 with 1 % of noise before, during and after the shot, and see it
  only when the source is pumped out: about 1e-8 for the first second, back
  to 1.0e-10 within half a minute. Nothing measures the source's own
  pressure, which is 1.0e-5 mbar after a shot. Nearly all of that is the
  air's active gas, since the example has no getter.

Things to try, each by editing a copy of the example:

- Take the pipette lines out of `sim_air.py`: the analysis reads a blank.
- Shorten `extraction.eqtime` in `extraction_line.toml` from 20 s to 2 s: the
  inlet closes before the gas has equilibrated. Ar40 reads 7.5e4 fA where it
  read 9.5e4, and 40/36 about 2 % low, because the lighter Ar36 is ahead.
- Take `open('B')` out of `sim_pump.py`: the source is never pumped, and the
  next analysis starts on top of the last.
- Give prep a leak (`[volumes.prep]`, `leak = 1e-12` in `sim.toml`): each
  blank reads about 600 fA where it read 10, however long prep is pumped
  between analyses.

### The stock queues

`experiment.toml` and `experiment.laser.toml` are the example's ordinary
queues: a blank and two unknowns. The simulator has no sample gas, so their
extraction scripts (`sim_extract.py`, `laser_extract.py`) let in one pipette
of the tank's air for every analysis that is not a blank. An unknown there
reads about 9.5e4 fA of Ar40 with the 40/36 of air, and the blank about
7 fA. It is not a sample.

### The model without a queue

The gas model belongs to the line as `pychron-ui` and `elctl exp run` load
it. `elctl`'s own hardware commands (`open`, `close`, `read`, `state`,
`scan`, and the `sim` session) simulate the controllers only: valves answer
and gauges read a fixed number whatever the valves do. To watch the gas
move by hand, use the canvas in `pychron-ui --examples --sim`.

## Reproducibility

The same lab, started in the same state and given the same queue, measures
the same numbers, to the last digit, at any `--sim-speed` and however busy
the computer is. Two runs of the air queue from two fresh copies of the
example lab differ only in each record's id, its time of day and its
checksum.

- The noise on a reading is worked out from three things: a seed, the name
  of what is read (the detector, or the gauge's volume), and the lab's time
  of the reading counted from when the program started. It is not the next
  number from a shared generator, so it does not depend on what else was
  read, how often, or in what order.
- `[defaults] seed` is the one seed of the simulated lab: of the gauges'
  noise and of the detectors'. Change it and every reading scatters
  differently: another run of the same lab, as a second day's would be. The
  gas does not change, so intercepts and ratios agree within their errors.
  With no seed in the file both use 0x5eed, and so does a spectrometer
  simulated on its own, beside a real line, which has no `sim.toml`.
- The gas itself has no randomness.

What does change the numbers:

- **The starting state.** A line remembers its valve states in
  `extraction_line.state.toml`, and the air queue ends with `C` open. Run it
  a second time from the same folder and prep is on the turbo for the five
  seconds before the first analysis, so the first blank reads 10.0 fA where
  it read 10.3. Delete the state file, or start from a fresh copy, to repeat
  a run exactly.
- **Another computer.** Faraday and gauge noise go through the maths
  library's `log`, `sqrt` and `cos`, which may differ in the last digit
  between platforms. A count on the ion counter can then differ by one, very
  rarely.

## Limits and approximations

- Everything under [what is not modelled](#what-is-modelled-and-what-is-not).
- The solution is exact to the rounding of the largest pressure among
  volumes that are open to each other. A volume holding 1e13 times less
  than its neighbour through an open valve is right only to that.
- A pump's base pressure is air, so a pumped volume always holds a little
  Ar40: 0.93 % of the base.
- An ion counter's noise is exact Poisson below 30 counts in an integration
  and a rounded normal distribution from 30 up: the mean and the scatter are
  right, the slight skew of a true count is left out.
- A detector saturates as its configuration says (4.9e6 fA in the example).
  A tank valve left open delivers the tank, the Faraday saturates, and the
  analysis still ends as a success.
- Only the first spectrometer stage is a source, and one sensitivity serves
  every isotope and detector.
- A gauge the canvas does not draw reads a steady pressure whatever the
  valves do.
