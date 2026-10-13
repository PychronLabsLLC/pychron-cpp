# Staged gas release: line steps between the gases of one run

Status: draft 2026-10-13, not agreed; waits for plan v2 (owner 2026-10-13:
agree v2 first, section 8). Written by an unattended run. Nothing built. Task T027 (`pychron usage notes.md`).
Builds on `2026-10-05-measurement-plan-v2-design.md` (itself "Draft": this
cannot be agreed before it) and `2026-09-29-experiment-system-design.md`
sections 3 (run lifecycle) and 4 (plan and engine).

## 1. Problem

Owner's case: extract gas from sample; freeze noble gases on cryotrap; warm
cryo to release one species, equilibrate with mass spectrometer, measure;
warm more, release next species, measure it on the quadrupole; and so on.

One run, several gas fractions, and between fractions the extraction line
does work (cryo setpoint, wait for temperature, getter exposure, pump out
the spectrometer) that depends on which fraction comes next.

## 2. What exists and what is drafted

- Built (v1): a run is one extraction script, then one measurement plan with
  one inlet, one equilibration, one time zero. `cryo_temp` is a run variable
  handed to the extraction script (`docs/user/04-experiments.md:160`);
  script vocabulary has `set_cryo`, `get_cryo_temp` (experiment spec, line
  356; not checked here that the host implements them). Measurement hooks
  (`before_main`, `after_main`, `on_whiff_result`) are Python.
- Drafted, not built (plan v2): `[[gas]]`, several per plan, run in file
  order, each with own `inlet`, `outlet`, `equilibration`, `time_zero`,
  `fits`, segments (v2 section 3.4); worked example He then Ne then Ar
  (v2 section 7.6); item 6 of v2's order of work.
- So v2 already gives "several species measured one after another in one
  run, each with its own inlet and time zero". Two things in owner's case it
  does not give:
  1. What the line does between gases. v2's gas names an inlet and an
     outlet valve, nothing else: releasing the next fraction (warm cryo,
     wait, expose getter, pump spectrometer) has no place in it but a
     Python hook.
  2. A gas measured on another spectrometer. A plan belongs to one
     spectrometer; lab simulator spec treats a second spectrometer stage as
     a warning (`2026-10-06-lab-simulator-design.md:420`).

This spec is those two, on top of v2. It adds no second way to say what v2
says.

## 3. Goal

Owner's case written as one plan file, no Python, validated before the queue
starts, with every fraction's data in one analysis record under its gas.

## 4. Design

### 4.1 `prepare` on a gas

`[[gas]]` gains optional `prepare`: name of an extraction script
(`scripts/extraction`, same resolution as a run's `extraction.script`) run
after the previous gas's last segment and pump-out, before this gas's inlet
opens.

```toml
[[gas]]
name = "He"
prepare = { script = "cryo:release", variables = { cryo_temp = 35, hold_s = 300 } }
inlet = "@valves.inlet"
...
[[gas]]
name = "Ne"
prepare = { script = "cryo:release", variables = { cryo_temp = 75, hold_s = 300 } }
```

- Same script kind, host and cancel token as the run's extraction script:
  one way to drive the line, already has `set_cryo`, valves, waits, and runs
  under the line's clock rules.
- `variables`: table handed to the script, over the run's own extraction
  variables. Plan parameters (`parameters.expose`) reach them, so a run in a
  queue can change a release temperature without a new plan.
- First gas's `prepare` runs after the run's extraction script (extract and
  trap), before first inlet. A plan whose gases have no `prepare` behaves as
  v2.
- Failure of a `prepare`: run ends as an extraction failure does today; data
  of gases already measured is kept and saved (nothing measured is dropped).

### 4.2 `pump_out` on a gas

Optional `pump_out = { valve = "@valves.ion_pump", time_s = 120 }`: after
the gas's last segment, open, wait, close, before the next gas's `prepare`.
Absent: outlet handling as v2. (Kept declarative because every multi-gas
method needs it and a script per lab for one valve is what v2 set out to
remove.)

### 4.3 Run phases

Run state machine today: extracting, then measuring. With staged release a
run alternates. Proposed: no new run states; `prepare` runs inside
Measuring as an engine block (`Block::Prepare`, published with the gas's
name), so executor, overlap and resources see one measurement phase holding
both the spectrometer and the extraction line for the whole run.

- Consequence: such a run cannot overlap with the next run's extraction
  (line is busy until the last gas). Validation says so when a queue asks
  for overlap with such a plan, rather than failing at run time.

### 4.4 A gas on another spectrometer

`[[gas]]` gains optional `spectrometer = "<name>"`: the spectrometer of the
install that measures this gas (default: the plan's). Engine takes that
spectrometer's ports for the gas's segments; record keys series by gas, and
the gas carries the spectrometer's name.

- Needs an install with two spectrometers configured, which nothing has
  today: largest part of this spec, and separable. Proposed as stage 2;
  stage 1 (4.1 to 4.3) is useful alone on one spectrometer.

### 4.5 Record

v2 record (its section 6.1) keys series by gas. Added: per gas, the
`prepare` script name, the variables it ran with, start and end time; the
spectrometer's name when not the plan's.

## 5. Rules

- A `prepare` script is an extraction script: no measurement host calls.
- Nothing in 4.1 to 4.3 changes a plan without `prepare` or `pump_out`.
- Time: scripts and waits on the line's clock; no real sleeps (AGENTS.md,
  Time).
- A gas already measured is saved whatever happens to a later gas.

## 6. Out of scope

- v2 itself (segments, positions, `[[gas]]`): its own spec and order of
  work; this starts after its item 6.
- Reduction of non-argon gases (He, Ne, Kr, Xe ages or abundances).
- Quadrupole driver: none in the tree; 4.4 defines only how a plan names a
  second spectrometer. Owner 2026-10-13: the quadrupole is another kind of
  mass spectrometer; legacy talks to Pfeiffer's Quadera
  (`pychron/spectrometer/pfeiffer/spectrometer/quadera.py`,
  `hardware/quadera_spectrometer_controller.py`,
  `pyscripts/measurement/quadera_measurement_pyscript.py`). A Quadera driver
  is its own spec, before stage 2.
- Conditionals that choose the next release temperature from the fraction
  just measured.

## 7. Tests (stage 1)

- Plan parse: `prepare` and `pump_out` accepted on a gas, unknown keys and
  unknown script refused at validation with the gas's name.
- Engine, fakes: two gases, each with `prepare`: order is prepare, inlet,
  segments, pump out, prepare, inlet, segments; variables reach the script;
  a failing second `prepare` ends the run failed with the first gas's
  series in the result.
- Simulated lab: cryo warmed in two steps releases two fractions; each gas's
  signal appears only after its release.
- Queue validation refuses overlap with a staged plan.

## 8. Owner, 2026-10-13, and what is still open

Answered:

- Order: measurement plan v2 is agreed first; this spec is taken up after
  (its five open decisions, its section 10, come before these).
- The second spectrometer is a quadrupole, another kind of mass spectrometer;
  legacy drives one through Quadera (section 6).

Open, for when v2 is agreed:

1. Is "extraction script per gas" (4.1) the right handle, or should release
   be declarative too (`release = { cryo_temp, hold_s, getter_s }`) with no
   script? Draft: script, because the steps differ by line.
2. Run states (4.3): one Measuring phase holding both line and spectrometer,
   or alternating states the executor and timeline show? Draft: one phase,
   blocks named on the timeline.
3. Is the quadrupole on the same line and install as the noble gas
   spectrometer? Decides whether stage 2 is one install with two
   spectrometers or two installs coordinating.
4. Fold this into the v2 spec as its item 6b rather than a spec of its own?
