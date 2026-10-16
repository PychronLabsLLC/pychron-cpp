# Experiments: queues, runs and conditionals

An **experiment queue** is the list of analyses an instrument runs by itself:
which samples, how each is extracted, how each is measured, and what to do
when something goes wrong. This chapter explains the queue file, the
Experiment window, how to start, pause and stop a queue, and how conditionals
protect your data.

Related chapters: [Getting started](01-getting-started.md),
[Extraction line](02-extraction-line.md), [Spectrometer](03-spectrometer.md),
[Laser extraction](05-laser.md), [Data analysis](06-data-analysis.md),
[Configuration reference](07-configuration-reference.md),
[Scripting](09-scripting.md), [Troubleshooting](10-troubleshooting.md),
[Glossary](11-glossary.md). Messages on a failed run or at the end of a queue
are covered in [notifications](../notifications.md); entering samples and
irradiations is covered in [entry](../entry.md).

## 1. How the pieces fit

A queue only names things. The things themselves live in the **lab
directory**, a folder of plain files that every program in the package reads:

```
<lab>/
  queues/*.toml              queues (any name; the program looks and saves here first,
                             but a queue may be kept anywhere)
  plans/*.toml               measurement plans (how the spectrometer measures)
  scripts/                   extraction, post_equilibration, post_measurement, measurement_hooks;
                             safe_state.py, the lab's safe state (optional)
  conditionals/*.toml        named sets of conditionals (system.toml is always applied)
  identifiers.toml           special identifiers such as "bu" (optional)
  defaults.toml              what the run factory starts a new run with (optional)
  queues/blocks/*.toml       reusable run sequences for the run factory (optional)
  notifications.toml         email / webhook / command messages (optional)
  tray_maps/*.txt            laser sample trays (see the laser chapter)
  patterns/*.toml            laser patterns
  cameras.toml               the camera of each laser
  stage_calibrations/        where each tray sits on each laser's stage
  data/                      written by the program: records/, spool/, executor_state.json
```

`pychron-ui` takes the lab directory from `--lab <dir>`; without it, it uses
the install folder, or the folder of the extraction line config. The data
folder is `--data <dir>`, by default `<lab>/data` (an install may name its own
data folder). Finished runs go to the install's database when it has one, and
to record files under the data folder when there is none (section 8.5).
`configs/examples` in the package is a complete example lab with a
simulated instrument.

A queue, then, says "run identifier `66001` with the extraction script
`laser_extract` and the measurement plan `sim_multicollect`". Scripts are
covered in [Scripting](09-scripting.md), the laser in
[Laser extraction](05-laser.md), and the measurement plans in the
[Spectrometer](03-spectrometer.md) and
[Configuration reference](07-configuration-reference.md) chapters.

## 2. The queue file (`experiment.toml`)

A queue is a text file in TOML. You can write it by hand, or build it in the
Experiment window and save it. The window writes a canonical form of the file:
keys in a fixed order, defaults left out, **comments removed**. If you keep
notes in a hand-written queue, keep them in the `comment` field of each run.

Here is the example queue shipped in `configs/examples/queues/experiment.toml`:

```toml
[queue]
name = "sim-example"
mass_spectrometer = "sim"
username = "example"

[queue.delays]
before_analyses = 5
between_analyses = 10
after_blank = 10

[[runs]]
identifier = "bu"
extraction = { script = "sim_extract", duration = 2 }
measurement = { plan = "sim_multicollect" }
post_measurement = "sim_pump"

[[runs]]
identifier = "66001"
extraction = { script = "sim_extract", value = 5, units = "w", duration = 10 }
measurement = { plan = "sim_multicollect" }
post_measurement = "sim_pump"
sample = { sample = "FC-2", material = "sanidine", project = "examples" }
```

In the simulated lab nothing gives gas off when it is heated. For each run
that is not a blank, the example extraction scripts (`sim_extract.py`,
`laser_extract.py`) let in one pipette of air from the air tank in place of
the sample's gas. So the "unknown" here reads like an air shot: about
9.5e4 fA of Ar40, at the atmospheric 40/36. The blank reads a few fA.

A mistyped key is an error, not silently ignored: the file is refused with a
message such as `runs[1].extraction: unknown key 'durration'`.

### 2.1 The `[queue]` table

| Key | Meaning |
|---|---|
| `mass_spectrometer` | **Required.** The name of the instrument (recorded with every analysis). |
| `name` | A label for the queue; recorded with each analysis. |
| `extract_device` | The extraction device runs use unless a run names its own. For a laser, the name of the driver in `extraction_line.toml` (for example `co2`). |
| `tray` | The sample tray a laser queue uses. A run's `position` is a hole on this tray. See [Laser extraction](05-laser.md). |
| `username` | The operator. |
| `email` | An address that gets the notifications for this queue, in addition to those in `notifications.toml`. |
| `queue_conditionals` | The name of a conditionals file (`<lab>/conditionals/<name>.toml`) applied to every run of this queue. |
| `load`, `repository` | Free text stored with the queue. |
| `schema_version` | Leave out, or `1`. Any other number is refused. |

### 2.2 The `[queue.delays]` table

All values are seconds. They are the pause the instrument takes before a run
starts, so the line can pump down and settle.

| Key | Default | Meaning |
|---|---|---|
| `before_analyses` | 15 | Before the first run. |
| `between_analyses` | 15 | Before a run that follows an ordinary run. |
| `after_blank` | 15 | Before a run that follows a blank. |
| `extract_delay` | 0 | Accepted and written back, but not used by the executor in this version; use `delay_after` on a run for a one-off wait. |

A run's own `delay_after` replaces the delay that would otherwise come after
it. No delay is taken while the previous run is still measuring (see
"overlap" below).

### 2.3 A run (`[[runs]]`)

Each `[[runs]]` entry is one analysis.

| Key | Meaning |
|---|---|
| `identifier` | The sample or special identifier, for example `66001` or `bu`. Decides the run type (section 4). |
| `aliquot`, `step` | Normally leave these out: the program assigns the next free aliquot when the run starts (the `B`, `C` ... of a step heat take the aliquot its `A` was given, so the steps of one heating are one aliquot; a step heat resumed after a restart, or continued in a later queue, joins the aliquot of the identifier's last saved run when its step comes after that run's step (`B`, `C` ... after a saved `A`); otherwise the run takes the next free aliquot), and a step letter only if you give one (`A`, `B`, `AA`: at most two letters). A fixed `aliquot` is allowed for unknowns, and the same identifier, aliquot and step may not appear twice. |
| `extraction` | The extraction (table, see below). |
| `measurement` | `plan` (the plan name), `overrides` (values that change a plan for this run), `hook`, `advanced`. |
| `post_equilibration` | Name of a script that starts when the inlet closes, while the measurement goes on. |
| `post_measurement` | Name of a script that runs after the measurement, usually pumping the prep section. |
| `conditionals` | A list of conditionals files that apply to this run only (section 7). |
| `sample` | `sample`, `material`, `project`, `irradiation`, `level`, `irradiation_position`: recorded with the analysis. |
| `comment` | Free text recorded with the analysis. |
| `weight` | Sample weight, a number of at least 0. |
| `skip` | `true` to leave the run in the file but not run it. |
| `end_after` | `true` to end the queue after this run. |
| `overlap`, `overlap_min` | Seconds. See "Overlapping runs" below. |
| `delay_after` | Seconds to wait after this run, replacing the queue delay. |

The `extraction` table:

| Key | Meaning |
|---|---|
| `script` | Name of the extraction script (a file in `scripts/extraction`, without `.py`; `co2:degas` is `co2/degas.py`). |
| `device` | Extraction device for this run; defaults to the queue's `extract_device`. |
| `position` | A hole or holes on the tray: `4`, `"1-6"`, `"1,3,5-7"`. A script sees only the first hole as `position`, so use one hole per run (the run factory does this for you). |
| `value`, `units` | The heating setting. `units` is `watts` (`w`), `percent` (`%`), `temp` (`c`), `amps` (`a`) or `volts` (`v`). **The default is watts.** A Chromium laser takes only percent, so a laser queue must say `units = "percent"`. A script's `extract()` understands only `percent`, `watts` and `temp`: a run with `amps` or `volts` passes the queue check but fails when the script heats. |
| `duration` | Seconds of heating. |
| `cleanup`, `pre_cleanup`, `post_cleanup` | Seconds of clean-up gettering, available to the script as variables of the same names. |
| `pattern` | Name of a laser pattern (see [Laser extraction](05-laser.md)). |
| `beam_diameter`, `ramp_rate`, `ramp`, `cryo_temp` | Passed to the script as variables of the same name. |
| `options` | A text value for script options. See "Unverified" at the end of this chapter. |

For convenience, `extraction` keys may be written directly on the run
(`duration = 10` instead of `extraction = { duration = 10 }`), but giving the
same setting twice is an error.

Overrides change a measurement plan for one run without editing the plan,
for example `measurement = { plan = "sim_multicollect", overrides = { "main.cycles" = 1 } }`.
Normally only parameters the plan exposes may be overridden;
`advanced = true` lifts that. The plan format itself is in the
[Configuration reference](07-configuration-reference.md).

The plans an install ships expose **Time zero**: where the time axis of the
fits starts, and so the moment the intercepts are taken at. It is
`on_inlet_close` (when equilibration ends, the default) or `on_first_count`,
and a run may choose the other in the **Measurement** tab or with
`overrides = { "main.time_zero" = "on_first_count" }`. A lab that wants time
zero a number of seconds after the inlet opens writes that in its plan,
`time_zero = { offset_s = 20.0 }` under `[main]`; the tab then shows the
seconds as a number a run may change (`"main.time_zero.offset_s" = 35.5`).
Write the number with a decimal point: written `20`, it takes whole seconds
only.

### 2.4 Checks done before a queue runs

Every time you load or edit a queue it is checked, all problems at once:

* required fields, typos, and the rules of the run type (section 4);
* every named plan, script and conditionals file exists;
* a plan that loads with the run's overrides, and conditionals whose names
  (isotopes, detectors, gauges) exist on this instrument;
* in a lab with lasers: the device exists, the tray map exists, each hole is
  on the tray, the tray is calibrated for that laser, the pattern exists, and
  a camera that was meant to be there is there.

A queue with an error will not start. From a terminal:

```bash
elctl -c extraction_line.toml exp validate queues/experiment.toml --lab .
```

prints the queue, the estimated time (`ETA`) and every problem.

## 3. The Experiment window

Open it from **View > Experiment** (Ctrl+Shift+E). Its menus are the submenus of
**Experiment** in the menu bar, live while the window is active: **Queue**,
**Rows**, **Executor**, **Scripts**. The queue's file commands are in **File**.

| Area | What it is |
|---|---|
| Centre | Two tables, one row per run: **Executed** above, **Queue** below (see "Executed and Queue" below). Above them, the queue's conditionals file and an Edit button. |
| Left (tabs) | **Run Factory** (section 5) and **Measurement** (the plan and parameters of the selected run). |
| Bottom | **Executor**: the Start / Stop / Cancel / Abort / Truncate buttons, status, a timeline, a table of events (each with the time it happened, on the line's clock) and tripped conditionals. |
| Right | **Evolutions**: the run being measured, one tab a kind (Signal, Baseline, Sniff). Each series of the tab has a panel of its own, stacked in the order the series began on one time axis, each scaled to its own readings, with its live fit and intercept; with many series the panels scroll. Signals and baselines are a panel an isotope on a detector (an isotope peak-hopped onto a second detector has two), sniffs a panel an isotope. Below, a table of the intercepts (isotope, detector, fit, intercept, error, percent error, points used). A last tab, **Peak center**, draws the run's peak center (below). The panels cannot be zoomed or saved yet. |
| Right, behind Evolutions | **References**: the recent blanks and airs of this spectrometer as time series, redrawn after each saved run (section 3.5). Only when runs are saved to a database. |

**The Peak center tab.** When a run centres a peak (the plan's `peak_center`
block, see "Peak centring" in [the spectrometer chapter](03-spectrometer.md)),
the Evolutions pane switches to its **Peak center** tab by itself and draws
the sweep as it goes: intensity against the magnet position, one point a
step. The reference detector is drawn heavy and named with a star in the
legend (`*H1`). Each of the config's `additional_detectors` is drawn in its
own color, scaled so that its lowest and highest readings span the same
range as the reference's: the graph compares the shapes and positions of the
peaks, not their sizes. Hovering over a point gives its detector, magnet
position and raw intensity. The title names the isotope, the detector and
the try (`Ar40 on H1, try 1 of 2`). When the centring succeeds the title
gives the centre (`center 5.123456, table updated`), circles mark the low,
centre and high points of the peak and a diamond its maximum. Dashed rules
mark where the sweep was centred when it began and the centre found, and,
when the config has a `dac_offset`, the centre without that offset. When it
fails, the points stay, nothing is marked and the title gives the reason
(`failed: ...`). A second try draws over the first, which stays behind it,
thin and faint, without markers or a legend entry.

A peak center skipped because the signal was below `min_signal` draws
nothing (the run's record says it was skipped). One that was cancelled or
stopped by a hardware fault keeps the points it had and its title ends in
`not finished`. When the next run starts, the pane returns to the Signal tab;
the last graph stays in the Peak center tab until another peak center
replaces it. Each finished peak center adds a line to the **Peak centers**
list at the bottom of the pane; clicking a line draws that peak center again
and selects the tab. The list holds the lines of the run being measured
(a plan with a peak center before and after has two) and is emptied when
the next run starts. The graphs of the last 20 peak centers of the queue are
kept in memory only: starting a queue clears them, and the sweep is not
saved with the run.

The panels around the table can be closed, moved and floated (the Executor
can be moved but not closed). **Window > Panels**, **Window > Reset Layout**
and **Window > Arrangements** bring them back, restore the installed layout
and keep layouts under a name: see "Panels and arrangements" in
[the extraction line chapter](02-extraction-line.md).

**Executed and Queue.** The queue is one list of runs, shown in two tables.
**Queue**, the lower one, holds the runs still to do: it is the one you edit,
and the one Start begins at. **Executed**, above it, holds the runs the
executor has reached, oldest first: a run moves there the moment it starts
(and a skipped row the executor has passed moves too, greyed). Executed is
read-only. Its header reads **Executed (N)**, N the number of runs in it;
the arrow beside it folds the table to its header line and opens it again.
The table is hidden while it is empty. It follows the newest run as runs
arrive, unless you have scrolled up in it. Drag the bar between the two
tables to share the room; the fold and the sizes are kept for the next
time the window opens. Both tables have the same columns, and widening a
column in Queue widens it in Executed. A run keeps its number (`#`) in both
tables: runs never change place in the queue, only which table shows them.

Rows that have a problem are colored, and hovering over them lists the
problems. A row that is running or finished has a colored Status
(success, failed, cancelled or aborted).

Table columns: `#`, Status, Identifier, Aliquot, Step, Type, Position,
Extract, Script, Plan, Conditionals, Comment, and **Est.** (the estimated
time of the run). Double-click a cell, or just start typing, to edit
Identifier, Position, Extract (for example `5 w`, `12.5 %`), Script, Plan or
Comment.

### 3.1 Files

**File > Open > Open Queue…** (Ctrl+O) loads a queue; **File > Save Queue**
(Ctrl+S) and **Save Queue As…** write it; **File > New > New Queue** (Ctrl+N)
starts an empty one with no file. Open and New work from any window and
bring the Experiment window up. **Experiment > Queue > Revalidate** checks the queue again. You
cannot open or start another queue while one is running. Closing the window asks about unsaved changes and,
if a queue is running, whether to stop it after the current run. If you
simply hide the window the queue keeps running.

### 3.2 Row operations (Experiment > Rows, and the right-click menu)

| Command | Shortcut |
|---|---|
| Move Up / Move Down | Ctrl+Up / Ctrl+Down |
| Duplicate | Ctrl+D |
| Delete | Delete |
| Toggle Skip | Ctrl+K |
| End After (end the queue after this run) | Ctrl+E |
| Edit Extraction Script / Edit Post-Measurement Script | opens the script editor |
| Set Conditionals... | choose conditionals files for the selected rows |

A skipped row is greyed; a row with End After has a bold identifier. **Set
Conditionals...** lists the lab's conditionals files, with a tick for files
all selected rows have, no tick for none, and a half tick where they differ
(a half-ticked file is left as each row has it).

These commands act on the rows selected in the **Queue** table only; a
selection in Executed is not read. Executed has its own right-click menu:

| Command | What it does |
|---|---|
| Repeat | Same as **Queue > Repeat Last Run** (section 3.4): copies the selected executed runs, or the last one when none is selected, to the front of Queue |
| Open Extraction Script / Open Post-Measurement Script | opens the selected run's script in the script editor, to read (one row selected) |

### 3.3 Editing while the queue runs

You may edit rows the executor has not reached yet: the rows of the Queue
table. A run moves to Executed when it starts; rows in Executed (runs that
have started, a pause being waited, a skipped row already passed) cannot be
changed, and a change that would reach them is refused ("rows the executor
has reached cannot change"). A row whose delay is still counting down is
still in Queue and not yet frozen; the executor re-reads it when the delay
ends. The queue-level settings cannot be changed while running.

When the queue ends, or is stopped, its executed runs stay in Executed and
stay read-only. Rows in Queue can be edited again. To edit an executed run,
or run it again, use **Queue > Reset** (section 3.4).

The executor can also change the queue itself, when a conditional asks it to
skip a run or add a blank (section 7.5). Those changes show in the table
and in the Events list. If you and the executor change the queue at the same
moment, one of the edits is refused rather than lost; make it again.

### 3.4 Reset, Clear Executed, Repeat and Resume (Experiment > Queue)

| Command | What it does | While a queue runs |
|---|---|---|
| **Reset** | Puts every run back in Queue and clears their statuses. The queue file is not changed (runs never moved in it). | refused |
| **Clear Executed...** | Asks "Remove N executed runs from the queue?", then deletes the executed runs from the queue; the queue is marked as changed, to be saved. | refused |
| **Repeat Last Run** | Copies the last executed run (or the runs selected in Executed), aliquot and step cleared, to the front of Queue, so it runs next. | allowed |
| **Resume...** | Offers to resume a queue that stopped before its end (below). | refused |

A command that cannot run says why in a message: "A queue is running.", or
"No run has been executed." for Repeat Last Run with an empty Executed
table, or "Nothing to resume." for Resume....

**Resuming a queue.** While a queue runs, the executor keeps how far it has
got in `executor_state.json` in the data directory. When you open a queue
(**File > Open**, or `pychron-ui --queue <file>` at start-up), and when you
choose **Queue > Resume...**, the window reads that file. If it is about the
same queue (same name) and that queue stopped part way, it asks:

> The queue *name* stopped at row N (*identifier*) at *date and time*.
> Resume from row N?

with a line beneath saying how it stopped: "It was stopped." after a Stop,
Cancel or Abort, or "It did not finish (the program ended while it ran)."
after a crash or a forced quit. **Resume** puts the rows before N in Executed,
with the statuses they had, and starts the queue at row N. **Not now**
changes nothing; Resume... stays in the menu for later. If the queue was
changed since (a row inserted or removed before N), the window compares
each executed row's identifier with the file's; on the first difference it
says so ("Row 3 is 12345-02 now; the file has 12345-01.") and Resume cannot
be pressed. A queue that ran to its end, or stopped before its first run,
is not offered. Nor is it offered in the window that has just stopped the
queue: there its executed runs are still in Executed, and Start goes on
from the first row of Queue.

Resuming does not yet give the remaining steps of a step heat the aliquot
of its earlier steps when the run had none fixed; they are given a new one.

### 3.5 References: recent blanks and airs

The **References** panel shows how the instrument's references have been
behaving while the queue runs: are the blanks falling, is the air steady. It
sits on the right behind **Evolutions**; click its tab to bring it forward.
Like the other panels it can be moved, floated or closed, and **Window >
Panels** brings it back.

It has two tabs, each a time series figure (the same figure as **Time
series** in the data window, [Data analysis](06-data-analysis.md)):

| Tab | Analyses | Starting preset |
|---|---|---|
| **Blanks** | Every kind of blank: `blank_unknown`, `blank_air`, `blank_cocktail`, `blank_extractionline`, and the plain `blank` of imported data. Each kind is a group of the figure, with its own color and legend entry. | Blanks |
| **Airs** | `air` | Air monitor |

Unknowns, cocktails, detector intercalibrations and other run types are in
neither tab. A blank or air tagged invalid is not drawn.

**Which analyses.** Each tab draws the newest analyses of its kind:

* of the queue's mass spectrometer (a queue that names none shows those of
  every spectrometer; opening a queue of another spectrometer reads again for
  that one);
* at most **Last** of them, per tab (default 50, from 5 to 500; the number
  is remembered);
* that started at or after the moment **Reset** was last pressed. Until you
  press Reset there is no lower bound in time. Reset ("from now") empties
  both tabs, and the next blank or air is the first point again. The reset
  point is not remembered: a new Experiment window shows everything again.
  An analysis's time is the time it started, so a run that began before you
  pressed Reset and is saved after it is left out.

**When it reads.** The panel reads the database the first time it is shown,
after each run that was saved (a run whose save failed does not count), and
when you press **Refresh**. Reading is done in the background and never
holds up the queue. A run is sometimes not in the database yet when it
finishes; if that read finds nothing new, the panel looks again 2 and 10
seconds later (on the session's clock, so in a simulation at its speed),
then stops: press **Refresh** if a point is still missing. A read that
finds nothing new leaves the figures as they are.

The note beside the buttons says when the panel last read (`read 14:02:11`,
on the session's clock), or `could not read:` and why. A failed read keeps
the figures it drew before and does not affect the queue.

**Options and presets.** The **…** button on each tab opens that tab's
options: the time series options and the bar of named presets that a figure
window has ("Options and named presets" in
[Data analysis](06-data-analysis.md#options-and-named-presets)). Choose what
each tab plots (for example `Ar40/Ar36` for the airs) or pick a preset; a
change redraws that tab only. The preset chosen for each tab is remembered;
if it is later deleted the tab goes back to its starting preset. Each tab can
be zoomed, panned, saved and copied as any figure can.

**When there is no References panel.** The panel needs a database to read
from, the one the session saves runs to. A queue that writes its runs only
as record files (no database configured, a database that could not be
opened, or a simulation writing files) has no References panel, not an
empty one, and **Window > Panels** does not list it.

## 4. Run types

The type of a run is decided by its identifier. A **special identifier** is
one of a fixed list; any other identifier is an unknown (your sample).

| Type | Default identifier | Use |
|---|---|---|
| `unknown` | anything else, such as `66001` | A sample. |
| `blank_unknown` | `bu` (also `b`) | Procedural blank that mimics an unknown extraction. |
| `blank_air` | `ba` | Blank run between airs. |
| `blank_cocktail` | `bc` | Blank run between cocktails. |
| `blank_extractionline` | `be` | Blank of the extraction line. |
| `background` | `bg` | Instrument background. |
| `air` | `a` | Air shot (atmospheric argon standard). |
| `cocktail` | `c` | Cocktail (mixed-gas standard). |
| `detector_ic` | `ic` | Detector intercalibration. |
| `degas` | `dg` | Heating to degas, usually without much measuring. |
| `pause` | `pa` | A planned wait in the queue. |

A lab can change the prefixes, and the allowed pattern of unknown
identifiers, in `identifiers.toml`:

```toml
[prefixes]
blank_unknown = "bu"
air = "a"

[patterns]
unknown = "^[A-Za-z0-9][A-Za-z0-9_-]*$"
step = "^[A-Za-z]{0,2}$"
```

Rules the checker enforces, per type:

* **Special runs get their aliquot and step assigned at run start.** Giving
  one a fixed `aliquot` or `step` is an error.
* **unknown, blank_unknown, blank_extractionline, degas** may use every
  field: device, script, heating (`value`, `pattern`, `beam_diameter`,
  `ramp_rate`, `ramp`, `cryo_temp`), `position`, and a measurement plan.
* **air, cocktail, blank_air, blank_cocktail, background, detector_ic** may
  extract (device, script, durations) but may not heat (`value`, `pattern`...)
  or have a `position`.
* **pause** may have none of these fields (see the note on pauses in
  "Unverified" below).
* **Every type except pause needs a measurement plan** (or a hook). Giving a
  plan to a pause is an error.

The run factory enforces the same rules: fields a type cannot use are
disabled in the form.

### 4.1 Blanks and special runs in a queue

Blanks and airs are ordinary rows with a special identifier; the executor
never invents runs by itself. To place them every few unknowns use the run
factory's **Frequency** group (section 5), or write them into the file, or
use a block (section 6). The only runs the executor adds on its own are those
a conditional asks for (section 7.5).

## 5. The run factory

The **Run Factory** tab builds runs from a form and inserts them into the
queue. Everything you can do here you can do by editing the file; the factory
only saves typing and checks as you go.

**Run.** Choose the **Type**: choosing a special type fills in its
identifier. Enter the Identifier, and optionally Aliquot (leave blank for
automatic) and Step. When the type changes, the form starts from the lab's
defaults for that type (below).

*Picking the identifier.* When `pychron-ui` was started with `--db`, the Run
group also has **Package** and **Level**, and the Identifier field has a
dropdown. Choose a package (an irradiation) and the dropdown lists the
identifiers of all its levels, each with its sample and where it sits:
`66001  FC-2  (A 3)` is identifier 66001, sample FC-2, level A, position 3.
Choose a level as well to see only that level's. Picking a line puts the
identifier in the field, exactly as if you had typed it. You can still type:
a special such as `bu`, or an identifier the store does not have yet. Nothing
is listed until a package is chosen, and the choice of package and level is
not kept between sessions or in the queue file. If the store cannot be read,
the reason is in the tooltip of the Identifier field; when it could not be
read at all the two selects are not shown, and opening the Experiment window
again tries again. Without `--db` they are never shown.

**Extraction.** Device, Position (`4`, `1-6`, `1,3,5`), **One run per hole**
(on by default: a position of several holes becomes one run per hole, with an
optional "id step" to advance the identifier for each), Value and its units,
Duration, Cleanup, Script, and **Step heat**.

*Step heat* makes one run per heating value, with steps A, B, C...: type the
values (`5, 10, 15`) or `start:increment:count` (`5:2.5:4` gives 5, 7.5, 10,
12.5). It needs a type that heats.

**Measurement.** Plan, Post-equilibration, Post-measurement scripts,
Conditionals (files every added run gets) and Comment.

**Frequency.** Pick a special type (for example `blank_unknown`), "Every N"
unknowns, and whether to add one **Before** the first and/or **After** the
last, then **Insert**. With "Every 0" only the before/after runs are added.
If rows are selected, only those rows are counted. Skipped rows never count.

**Block.** Insert a block (section 6), a number of times.

**Add.** The line above the buttons says what will be added ("Adds 3 run(s):
66001 @1 ... 66001 @3") or why it cannot be. **Add** (Ctrl+Enter) inserts
after the selected rows (or at the end). **Defaults** applies the lab's
`defaults.toml` entry for this type and device; **From Row** copies the first
selected row into the form. "Then advance id / pos" advances the identifier
or position by that much after each Add, handy for working along a tray.

### 5.1 Lab defaults (`defaults.toml`)

An optional file that gives each type and device the plan, scripts and
extraction settings a new run starts with. Tables are
`[<analysis type>.<device>]`, where the device `*` means any:

```toml
[unknown.co2]
template = "sim_multicollect"
script = "laser_extract"
post_measurement = "sim_pump"

[unknown.co2.extraction]
units = "percent"
value = 20
duration = 10

[blank_unknown."*"]
template = "sim_multicollect"
script = "sim_extract"
post_measurement = "sim_pump"
```

Allowed keys are `template`, `script`, `post_equilibration`,
`post_measurement`, `overrides` and an `extraction` table with `units`,
`value`, `duration`, `cleanup`, `pre_cleanup`, `post_cleanup`. A mistyped key
makes the whole file fail to load, and the problem is shown in the Executor
banner.

## 6. Blocks

A **block** is a short reusable sequence of runs, for example a blank, an air
and a second blank. Put a file in `<lab>/queues/blocks/`; its runs use the same keys
as `[[runs]]` in a queue. A lab's old `<lab>/blocks/` folder is not read: move
its files to `<lab>/queues/blocks/`. From
`configs/examples/queues/blocks/blank_pair.toml`:

```toml
[block]
name = "blank_pair"
description = "procedure blank, then a second blank to check memory"

[[runs]]
identifier = "bu"
extraction = { script = "sim_extract", duration = 2 }
measurement = { plan = "sim_multicollect" }
post_measurement = "sim_pump"

[[runs]]
identifier = "bu"
extraction = { script = "sim_extract", duration = 2 }
measurement = { plan = "sim_multicollect" }
post_measurement = "sim_pump"
```

In the run factory, choose the block, a number of **Times**, and Insert. Runs
with no device get the queue's extraction device; runs with no plan get the
lab defaults for their type. A block file that cannot be read is listed as a
lab problem, and with no blocks the Block group is disabled.

## 7. Conditionals

A **conditional** is a rule of the form "if this is true while measuring,
do that". Conditionals protect data and save time: stop a run whose signal is
far too big, end a run that has no gas, cancel the queue when the vacuum is
bad, or add a blank after a run.

### 7.1 Kinds

Each conditional belongs to one kind, which decides what happens when it
trips:

| Kind (table) | When it is checked | What a trip does |
|---|---|---|
| `truncations` | During the main measurement | Ends the measurement early. The run is **saved** and marked truncated. Later blocks (such as baselines) are shortened by `abbreviated_count_ratio`; `truncate:quick` shortens them to 0.25. |
| `terminations` | During the main measurement | Ends the measurement; the data are saved and the queue continues. |
| `cancelations` | During the main measurement | Cancels the run (nothing is saved) **and** ends the queue. |
| `actions` | During the main measurement | Does a named `action`: `truncate`, `terminate`, `cancel`, `set_param NAME=VALUE`, `run_hook NAME`, `notify`. With `resume = true` it keeps measuring and can trip again. |
| `modifications` | During the main measurement | Changes the queue after this run (section 7.5); can also truncate or terminate. |
| `equilibrations` | While the gas equilibrates | Closes the inlet early, ending equilibration. |
| `pre_run` | Before extraction and before measurement | Stops the queue before the run starts. |
| `post_run` | After the run is saved, on its recorded values | Cancels the queue (the default) or changes the queue (section 7.5). |

A conditional trips at most once per run (except an `action` with `resume`).
If several would trip on the same reading, only the first acts, taking the
kinds in this order: modifications, truncations, actions, terminations,
cancelations. Every trip is recorded with the analysis: its name, when it fired, and the
values it was checked against.

### 7.2 Where conditionals come from: system, queue, plan, run

Conditionals come in levels. For each run they are combined in this order,
later levels adding to or replacing earlier ones:

| Level | Where |
|---|---|
| **System** | `<lab>/conditionals/system.toml`: applies to every run. The lab's safety net. |
| **Queue** | The file named by `queue_conditionals` in the queue (the **Queue conditionals** box above the table). |
| **Plan** | The measurement plan: `[conditionals] include = ["@conditionals.NAME"]` plus the plan's own inline truncations. |
| **Run** | The files listed in the run's `conditionals = ["name", ...]`. |

A conditional with the same name at a later level replaces the earlier one.
A file may also say `disable = ["name", ...]` to switch off conditionals of
earlier levels by name (for example, a degas queue that disables the system's
`no_gas`).

The in-run kinds above are evaluated for the **current run** only. `pre_run`
and `post_run` conditionals come only from the system and queue levels,
because they are checked for the queue, between runs; a `post_run` that
applies to a run counts its `ntrips` over consecutive runs.

### 7.3 Writing a conditionals file

A conditionals file is TOML in `<lab>/conditionals/<name>.toml`. Each rule is
a table of its kind. This is `configs/examples/conditionals/system.toml`:

```toml
[[pre_run]]
name = "cdd_off"
check = "CDD.inactive"

[[cancelations]]
name = "vacuum_excursion"
check = "gauge.IG1.pressure > 1e-5"
start = 5
frequency = 5
ntrips = 2

[[truncations]]
name = "huge_signal"
check = "Ar40.cur > 4e6"
start = 3
abbreviated_count_ratio = 0.5
```

and this is `default_unknown.toml`, which a plan can include as
`@conditionals.default_unknown`:

```toml
[[terminations]]
name = "no_gas"
check = "average(Ar40, window=10) < 100"
start = 20
analysis_types = ["unknown"]

[[post_run]]
name = "blank_after_small"
check = "Ar40 < 1000"
action = "run_blank"
analysis_types = ["unknown"]
```

Any kind may appear in any file.

Keys of a conditional:

| Key | Meaning |
|---|---|
| `check` | **Required.** The condition (section 7.4). |
| `name` | Unique within a file. If left out it is the kind and the check. |
| `start` | Readings to ignore before the first check (default 0). The first check is after reading `start + 1`. |
| `frequency` | Check every Nth reading after `start` (default 1). |
| `ntrips` | Consecutive true checks needed to trip (default 1). A false check, or one that cannot be computed, resets the count. |
| `analysis_types` | A list of run types it applies to, for example `["unknown", "air"]`. Empty means all types. `"blank"` means every blank type. |
| `window` | Average the last N points instead of using the live value (default none). |
| `mapper` | An expression in `x` applied to each value read, for example `x + 1000`. |
| `abbreviated_count_ratio` | Truncations, modifications, equilibrations only: after this trip, shorten later blocks to this fraction (a number above 0 and at most 1). |
| `action` | Where the kind allows (section 7.1). |
| `resume` | `actions` only. |
| `truncate` / `terminate` | `modifications` only: also truncate or terminate this run (not both). |

`start` and `frequency` apply to the in-run kinds only; `pre_run` and
`post_run` are checked once per run and take neither. Errors name the file
and the rule (`[[truncations]] #2: ...`). Two rules with the same name in one
file are an error.

### 7.4 The check expression

A check is a small formula that is true or false. Spaces do not matter.

* **Numbers:** `5`, `1e-5`, `4e6`.
* **Comparing:** `<  <=  >  >=  ==  !=`
* **Combining:** `and`, `or`, `not`, and brackets `( )`.
* **Arithmetic:** `+ - * /`.
* **Variables:** `$NAME` takes a value from the script options, a plan
  parameter or a run variable.
* **Isotopes.** A bare isotope, `Ar40`, is the live corrected intensity (fit
  intercept with baseline and detector correction). `Ar40.cur` is the latest
  raw reading; `Ar40.bs` the baseline; `Ar40.bs_corrected`,
  `Ar40.ic_corrected`, `Ar40.intercept`, `Ar40.std_dev` (also `.sd`,
  `.stddev`) are the other forms.
* **Ratios:** `Ar40/Ar39`.
* **Detectors:** `CDD.inactive`, `CDD.deflection`, `CDD.intensity`; use
  `L2(CDD)` when a detector name is qualified.
* **Computed values:** `age`, `instant_age`, `kca`, `cak`, `kcl`, `clk`,
  `radiogenic_yield`, `rad40`, `rad40_percent`, `atm40`, `k39`, `ca37`,
  `ca39`, `ca36`, `cl36`.
* **Gauges and devices:** `gauge.IG1.pressure` (the name is that in
  `extraction_line.toml`), `device.NAME`, and `param.NAME`.
* **Functions over a series of readings:** `min(Ar40)`, `max`, `average`,
  `slope`, `std`, `rsd`, `count`, each optionally `window=N` for the last N
  points: `average(Ar40, window=10)`, `slope(Ar40) > 100`. After a gap in
  the readings (section 8.2) a window starts again: it holds only the points
  after the gap, and a check over it is not made until there are enough (two
  for `slope`, `std` and `rsd`, one for the others). A function without a
  window sees every point, and `elapsed()` counts through the gap.
  `between(value, low, high)`, `abs(value)` and `elapsed()` work on single
  values.

Baseline readings (`Ar40.bs`) are series only: use `average(Ar40.bs)` and so
on rather than comparing them directly.

If a metric cannot be read at the moment (for example the detector is not
on the current plan), the check is skipped for that reading, the trip count
resets, and the problem is recorded once for the run. Checks are verified
against the instrument when the queue loads; an unknown isotope, detector or
gauge is flagged in the queue table.

To test a file from the terminal, including names against the line's gauges
and the spectrometer:

```bash
elctl conditionals-check conditionals/system.toml --spectrometer spectrometer.toml
```

### 7.5 Changing the queue: `modifications` and `post_run`

A queue action acts on the rows after the run that tripped it. Rows already
skipped are left alone.

| Action | Effect |
|---|---|
| `skip_next` | Skip the next run. |
| `skip_n N` | Skip the next N runs. |
| `skip_aliquot` | Skip every following unknown of the same identifier and aliquot. |
| `skip_to_last_in_aliquot` | The same, except the last one. |
| `set_extract STEPS` | The following runs of that aliquot get a changed heating value: `set_extract 1,2,3` adds 1, 2, 3 to the value in turn; `set_extract 10%,20%` raises it by those percentages. Stops when the steps run out. |
| `repeat` | Insert a copy of the run after it, with a new aliquot. |
| `run_blank` | Insert the matching blank after the run: air gets `blank_air`, cocktail `blank_cocktail`, a blank stays the same type, anything else `blank_unknown`. |

`post_run` conditionals may also use `cancel`. Each change appears in the
Events list ("queue: ...") and in the run's summary.

### 7.6 The Conditionals Editor

**Experiment > Scripts > Conditionals Editor...** (or **File > Open > Open
Conditionals…** from any window, or the **Edit...** button beside the
queue conditionals box) edits the files in `<lab>/conditionals` without
touching TOML by hand.

* **Left:** the lab's files, `system` first. Click one to open it, or use
  **File > Open > Open Conditionals…** (Ctrl+O). **+** (also **File > New >
  New Conditionals…**, Ctrl+N) creates a file (a plain
  name: no folders, no leading dot, no `.toml`), **-** deletes the selected
  file (asking first, and telling you if the open queue uses it).
* **Right:** the open file's conditionals grouped by kind. **Add**,
  **Remove**, **Duplicate**, **Up** and **Down** manage them. The selected
  conditional shows a form: Name, Kind, Check, Start and Frequency, Trips
  (ntrips), Window and Mapper, Types (analysis types), Count ratio, Action
  (with `quick`, `percent`, a name or value, or steps as the action needs),
  Resume, and a Run flag (truncate or terminate). Only the fields the kind
  uses are enabled.
* **Disables:** names from earlier levels this file switches off.
* **Diagnostics:** every problem, listed at once. Click one to go to its
  row.

Errors (a check that does not parse, a rule of its kind, a duplicate name)
block saving. About 0.6 seconds after you stop typing the editor also checks
isotope, detector and gauge names against the instrument; those are warnings
only. **File > Save Conditionals** (Ctrl+S) writes the canonical TOML, so a hand-written file
loses its comments; the editor asks first. A saved file takes effect from
the next run (or the next queue start, if it is the queue's conditionals).
A file that does not parse is shown with the reason and cannot be edited
there; fix it in a text editor.

## 8. Running a queue

### 8.1 Start

Check that the queue has no errors (the Start button stays disabled until it
is runnable) and press **Start** (F5, or **Experiment > Executor > Start**).
Start begins at the **first row of the Queue table**, whatever row is
selected: after a Stop, Start goes on from the run after the last one that
started. Runs already in Executed are not run again; to run them again,
choose **Experiment > Queue > Reset** and then Start. Once a queue has run
to its end, Queue is empty and Start stays disabled until Reset (or until
runs are added). The window starts the queue on the instrument; to try
without hardware use a simulation (section 9).

A queue will not start, and the Executor banner says why, when:

* it does not check against the lab (the message names the first error and
  how many more there are);
* another queue is already running;
* a laser is being driven by hand, a laser's beam is on, or its emergency
  stop has not been reset (see [Laser extraction](05-laser.md));
* the lab asks for something this session cannot do, for example a simulated
  camera over a real laser.

While a queue runs, a free-running spectrometer scan is paused (measurement
needs the acquisition engine) and resumed when the queue ends.

### 8.2 What a run does

Each run goes through these states, shown in the Status column and in the
Executor pane:

```
Pending > Preparing > Extracting > Equilibrating > Measuring > PostMeasuring > Saving > Success
```

(`Truncated` appears after Measuring when a truncation tripped.) Any
unfinished run may also end Failed, Cancelled or Aborted.

1. **Delay.** The executor waits the queue delay (section 2.2) and says why in
   the **Waiting** line ("delay before 66001").
2. **Pre-run checks.** The `pre_run` conditionals; a trip stops the queue.
3. **Preparing.** The aliquot is assigned, the plan, conditionals and scripts
   are loaded. Every script is checked first (section 7 of
   [Scripting](09-scripting.md)); an error fails the run before any hardware
   moves.
4. **Extracting.** The extraction script runs. **Whatever the script does, the
   extraction device is stopped and disabled afterwards.** A partial
   extraction record is saved.
5. **Equilibrating, Measuring.** The spectrometer measures according to the
   plan. The post-equilibration script starts when the inlet closes. The
   Evolutions pane draws the signals, and a peak center, when the plan has
   one, in its Peak center tab (section 3).

   **When the spectrometer goes silent** (no reading within three
   integrations plus 5 s, or its link drops), the measurement does not end:
   it waits, for up to 2 minutes, for the readings to come back, and does
   nothing else to the line meanwhile. Readings that resume continue the
   block to its planned end time, so time zero and the block's end do not
   move; the gap is kept in the record and said in the Executor pane. When
   they do not resume, the acquisition is started once more; if that gives
   nothing, the block ends. A main block left with fewer than 20 % of its
   planned counts (and never fewer than its fit needs) fails the run (saved
   as failed, section 8.2); with more, the run is truncated. During a
   measurement, closing the inlet and unprotecting a detector keep trying
   through a dropped link for 10 minutes, with an alarm after 20 s. These
   limits are the lab's to change, in `executor.toml`
   ([Configuration reference](07-configuration-reference.md)).
6. **PostMeasuring.** The post-measurement script runs. A failure here is
   logged but the run still saves.
7. **Saving.** The record is written (section 8.5), and then the `post_run`
   conditionals are checked.

**A run that fails after its extraction script started** has spent its
sample, so nothing it measured is thrown away: its record is saved with
status `failed` and the reason, and stored with the tag `invalid` (left out
of the browser and figures, as any invalid analysis is; the recall window
still opens it). Its aliquot is not given again. A truncated run's record
says `truncated`. A cancelled or aborted run is the operator's choice and is
not saved, as before.

Then the line is put in a **safe state**, in this order: the extraction
device is stopped and disabled, the plan's inlet valve closed and the
detectors the plan protects unprotected. A step that fails because a link is
down keeps trying for 10 minutes; after 20 s an alarm says so, and if it
still fails a critical alarm says the line may not be safe. The steps after a
failed one still run. Last, when the lab has one, `scripts/safe_state.py`
runs: the lab's own sequence to isolate and pump the line ([Scripting](09-scripting.md),
section 1). A run whose post-measurement script failed is saved and then
made safe the same way.

**When a link drops, the queue holds.** Before each run the executor checks
the links the run needs: every transport of the extraction line (its
extraction device is one of them) and, for a run with a measurement plan,
the spectrometer's; a transport marked `optional = true` is left out. It
also checks that the spool's disk has room and that not too many records
wait to be sent. When one of these is not so, or a run has just failed
because of a link, the Executor pane says **Holding** and why. While it
holds, each link that is down is tried again every 15 s; once every link
the next run needs has been up for 30 s, the queue goes on. A run that
failed before its extraction began spent nothing: it is started again, with
the aliquot it was given. A run that failed after it is never run again.
After 2 hours of holding the queue ends, failed, naming the links. Stop,
Cancel and Abort work while it holds. A run whose safe state failed ends
the queue at once, without holding. These times, and whether the queue
goes on after a run failed for a link, are in `executor.toml`
([Configuration reference](07-configuration-reference.md)).

### 8.3 Stop, Cancel, Abort, Truncate

The buttons in the Executor pane (also **Experiment > Executor**):

| Button | Effect |
|---|---|
| **Stop** | Finish the run (or runs) in progress and start no more. The queue ends "Stopped". |
| **Cancel** | Cancel the current run and end the queue (asks to confirm). Scripts stop at their next wait; the post-equilibration and post-measurement scripts still run to leave the line safe. **Nothing is saved for the cancelled run.** |
| **Abort** | Stop immediately (asks to confirm). No further scripts run and the hardware refuses new commands from the script. Use for emergencies. For a laser, the laser window's EMERGENCY STOP is faster and also aborts the queue. |
| **Truncate** | End the current measurement block early. The run continues with its remaining blocks, is saved, and is marked **truncated**. It only acts while a run is equilibrating or measuring. |

A truncation caused by a conditional behaves the same way. Records of
truncated runs are normal analyses with a flag, so you can choose to include
or exclude them in the [data analysis](06-data-analysis.md).

At the command line, the first Ctrl-C stops after the current run, the second
cancels, the third aborts.

A queue ends as one of: **Completed**, **Stopped**, **Cancelled**,
**Aborted** or **Failed**. A failed run ends the queue. A run that was
measured and could not be saved to its final place has not failed: it is a
success with a flag, its record is in the spool, and the queue goes on
(section 8.5).

### 8.4 Overlapping runs

An `unknown` run with `overlap` above zero (seconds) lets the next run begin
while it is still measuring: the next run starts `overlap` seconds after this
run's inlet closes. Other run types never overlap, and neither does the last
runnable row. At most two runs are in flight. They share the extraction
device and the spectrometer: the next run's extraction waits for the device,
and its measurement waits for the spectrometer and for `overlap_min` seconds
of pump time since the previous run's post-measurement began. The timeline in
the Executor pane shows overlapping runs in separate lanes, and the
**Waiting** line says what each is waiting for (the delay, the device, pump
time).

The timeline does not try to show a long queue all at once. It follows the
run in progress, from its start up to now, so its phases stay wide enough to
read however long the queue has been running. When runs overlap it shows
all that are in progress, and between runs the one that started last. To
look further back:

| Do this | To |
|---|---|
| Ctrl+wheel (Cmd+wheel on macOS) | Zoom in or out about the time under the pointer, from ten seconds to the whole queue. A plain wheel scrolls the pane as usual. |
| Drag | Move the window earlier or later. |
| Double-click, or the **Follow** button | Go back to the runs in progress. The button shows whenever the timeline is not following. |
| Right-click | Follow the active run, the last 3, 6 or 12 runs, or show the **Whole queue**. The choice of what to follow is remembered. |

A window you have moved stays where you put it while the queue runs on; the
dashed line that marks now is drawn only when the window includes it.
Starting a queue always returns the timeline to following. The times along
the bottom are elapsed time since the queue's first event.

### 8.5 What is written: run records

Where a finished run goes depends on whether the program has a database.

**With a database.** An instrument install has one (the setup makes it, by
default a SQLite file at `<install>/data/pychron.db`), and both `pychron-ui`
and `elctl exp run` use it; `--db <url>` names another. Each finished run is
written to the database as one analysis before the run is reported as ended,
and the data window of `pychron-ui` shows it when the run ends
([Data analysis](06-data-analysis.md)).
No record file is written for it. These stay files under the data folder:

```
<data>/spool/<uuid>.json                                           a run not yet in the database
<data>/records/<identifier>/<identifier>-<aliquot><step>.extraction.json
<data>/records/artifacts/<uuid>/<name>
```

**Without a database.** The example lab (`--examples`, or a line config given
on the command line), an install made before installs had a database, and a
build without persistence write each finished run as one JSON file instead:

```
<data>/records/<identifier>/<identifier>-<aliquot><step>.json
<data>/records/<identifier>/<identifier>-<aliquot><step>.extraction.json
<data>/records/artifacts/<uuid>/<name>
```

For example `66001-1.json`. The data browser reads these files directly.

Either way, the `.extraction.json` file is written as soon as extraction ends,
so the extraction is kept even if the measurement later fails. The record
holds the identity, sample information, instrument, the extraction settings
and what actually happened (actual duration, positions, pattern), the scripts
that ran with a hash of their text, the effective measurement plan,
spectrometer state, the raw signals, intercepts and baselines, every
conditional that was in force and every trip, and the notes from the run. Ages
and other derived values are not stored; they are calculated when you analyse
the data ([Data analysis](06-data-analysis.md)).

**What the database gets.** The run's identity (identifier, aliquot, step,
type, time, analyst, mass spectrometer, extract device), the extraction
settings, the positions heated, every raw signal and baseline series, the
intercepts and baselines as fitted during the run, the detectors' gains and
deflections and the source settings, the conditionals in force and those
that tripped, the software version, whether the run was simulated, the
queue's name, and the measurement plan as it was run. A fit that failed is
stored as unknown rather than as a number.

An isotope measured on one detector is stored under its name (`Ar40`). When a
plan measures one isotope on several detectors, as the detector
intercalibration plan does, each is stored under the detector and the isotope
(`H1:Ar40`, `AX:Ar40`), which is how the data windows name them. When a plan
takes a detector's baseline more than once (a baseline block and a baseline
hop), the database has one baseline series for that detector, holding all of
its points.

It does not get, in this version: the sample, project, material and
irradiation text typed in the queue (in the database a run belongs to its
identifier, and the identifier's own entry says which sample that is), the
text of the extraction and post-measurement scripts, the peak-center results,
the run's events and notes, the tray and the load, and the readings logged
during extraction. No blank is assigned to a run and no detector
intercalibration factor is computed at acquisition: a run is stored with the
intercepts and baselines fitted during the run, and blanks and IC factors are
fitted and saved afterwards ([Data analysis](06-data-analysis.md)).

**Before a queue starts**, it is checked against the database. The queue's
mass spectrometer and every identifier and extract device it names must
already be there, a step must be letters, and a row whose aliquot and step are
fixed must not be a run the database already has. Only the rows that will run
are checked: a queue started from row 3, or resumed, is not refused for what
its first rows already stored. Rows added to a queue while it runs are checked
in the same way, and an edit the database could not take is refused. Otherwise
the queue is not started and each problem is listed, for example:

```
error: the queue cannot be saved:
error: identifier '66001' is not in the database
```

The program never adds an identifier, a sample or an instrument on its own:
add what is missing with **Entry** or `elctl entry` ([entry](../entry.md)),
then start the queue again. The same happens when the database cannot be
reached at all (`the database cannot be reached: ...`).

**The spool and pending runs.** Every finished run is first written to a
local **spool** (`<data>/spool/`), and then handed on: to the database, or to
its record file. If the second step fails (the database is locked or away,
the disk is full), the run still counts as a success and the queue goes on,
and the failure is not silent:

* the run says `not saved to the data store, kept in the spool:` followed by
  the reason, in the event log and, at the command line, under the run;
* its row in the queue table is flagged, and its tooltip gives that line;
* a notification goes out as for a failed run, with the subject
  `pychron: run <id> not saved (queue <name>)`
  ([notifications](#86-notifications));
* the Executor pane shows "N run(s) waiting to be saved to the database"
  (without a database, "N record(s) in the spool"), followed by the reason
  the last one was refused.

`elctl exp run` marks such a run and ends with the count and the reason:

```
run 0 ba-1: success (not saved)
...
5/5 run(s) succeeded; 0 of them in sqlite:/home/lab/Pychron/argus/data/pychron.db
warning: 5 record(s) waiting in /home/lab/Pychron/argus/data/spool: <record id>: <the database's reason>
```

A pending run is safe on this computer but is not yet in the database, so the
data window does not show it. Pending runs are sent again when the next queue
starts, and a run is never stored twice. One the database refuses for good (its
identifier was removed, say) stays in the spool and is tried at each queue
start until the missing entry is added.

Only one program at a time should run queues into a database.

The executor also keeps `<data>/executor_state.json` up to date. It records
the next row to run, so a queue can be resumed after a crash (a run that has
started counts as consumed and is never repeated):

```bash
elctl -c extraction_line.toml exp run queues/experiment.toml --resume
```

`--from <row>` starts at a given row instead.

### 8.6 Notifications

Pychron can send a message when a run fails (or its record cannot be saved)
and when the queue ends: email, a Slack-style webhook, or a local program.
Set it up in `<lab>/notifications.toml`; see [notifications](../notifications.md).
The Executor pane's **Notify** line lists the configured channels ("off" when
there is no file), and sent messages and failures appear in the Events list.
**Experiment > Executor > Send Test Notification** (or `elctl exp notify`) sends a test on
every channel.

## 9. Simulation and simulated speed

Without an instrument you can try everything above on a simulated lab:

```bash
pychron-ui --sim --lab configs/examples --queue configs/examples/queues/experiment.toml
```

`--sim` forces every transport in the extraction line to the built-in
simulators. The window title then says "(Simulation)". Real time is slow: an
example run takes minutes, mostly waiting. **`--sim-speed <x>`** (with
`--sim`, or on an install set up for simulation) runs the whole program on simulated time `x` times faster than real
time:

```bash
pychron-ui --sim --sim-speed 400 --lab configs/examples --queue configs/examples/queues/experiment.toml
```

The same options exist at the command line, which needs no window:

```bash
elctl -c configs/examples/extraction_line.toml --sim exp run configs/examples/queues/experiment.toml \
      --spectrometer configs/examples/spectrometer.sim-integrated.toml --sim-speed 50
```

**Where simulated runs are saved.** A simulated run is not a measurement, and
its record says so (`"simulated": true`; the database keeps the mark with the
analysis). So that simulated runs do not land among a lab's measurements, a
simulated session writes to a database only when it is the install's own
database and that is a file on this computer (the default of an instrument
install, so the quick start's runs appear in the data window). Pointed at any
other database, a server, or any database given with `--db`, it writes record
files under the data folder instead, and says so once:

```
note: simulated runs are written to /home/lab/Pychron/argus/data/records, not to postgresql://lab@db.example.org/pychron; pass --sim-save to save them there
```

`pychron-ui` puts the same sentence in the log dock, and its data window goes
on showing the database. To save simulated runs there all the same (a test
database, a rehearsal), add **`--sim-save`** to `elctl exp run` or
`pychron-ui`. A session is simulated when it is started with `--sim`, when its
install was set up for simulation, or when its spectrometer file names only
simulators. A real instrument is never affected.

The simulation uses the same queues, scripts, plans and conditionals as the
real instrument, but its spectrometer, valves and lasers are models, so
signal sizes and timings are not those of your instrument. The signals do
follow the valves: gas goes from the example's air tank through the pipette
and the inlet into the spectrometer, and `queues/experiment.sim-air.toml` is a queue
of air shots and blanks that shows it. A laser or furnace releases no gas in
simulation. [The simulated lab](../simulator.md) has the details and the
figures.

## 10. Unverified / not yet implemented

These points were found while reading the source. Test them on your system
before relying on them.

* **Pause rows.** The executor waits `extraction.duration` seconds for a
  `pause` row, but the queue checker also says a `pause` run may not have
  any extraction field, including `duration`. A pause with a duration may
  therefore be refused as an error. Try it with `elctl exp validate` first.
* **`options` / `script_options`.** The queue file reads `options` on a run
  and the scripts have an `opt` object, but the run does not pass the one to
  the other in this version, so `opt.<name>` is empty in an automated run.
* **Script header values.** Lines such as `#! pychron: eqtime=20` in a script
  are checked for syntax, but no setting of the run reads them.
* **`truncate` key on a run.** A run may carry `truncate = "..."` (alias
  `t_o`), but the loader treats each entry as the *name of a conditionals
  file*, not as an inline check. To truncate a run, put a `[[truncations]]`
  rule in a conditionals file and list the file in `conditionals`.
* **The kind of a run's conditionals reference** (`{ name = "x", kind = "..." }`)
  is accepted, but the lookup is by file name only.
* **Queue `extract_delay`** is stored and written back, but nothing in the
  executor reads it (section 2.2).
* **Spool recovery** happens when the next queue starts; there is no separate
  command to resend spooled records.
* **Runs in a database on PostgreSQL.** Writing a queue's runs to an
  install's database (section 8.5) has been tested on SQLite only.
* **Peak center and measurement plan details** are outside this chapter; see
  the [Spectrometer](03-spectrometer.md) and
  [Configuration reference](07-configuration-reference.md) chapters.
* **Measurement plans, version 2** are described in the design notes as a
  draft and are not covered here.
