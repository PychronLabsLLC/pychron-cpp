# Flux fitting

How to turn the fluence monitor analyses of an irradiation level into a J for
every position of that level, and save it, with `elctl flux`. The design is
`superpowers/specs/2026-10-06-flux-fitting-design.md`; the code is
`libs/reduction` (`flux.hpp`, the math), `libs/processing` (`flux_fit.hpp`,
the fit of a level) and the `processing_store` adapter (`flux_store.hpp`,
reading and writing the store). The review step (leave an analysis out,
leave a position out) is done in the flux window (below) or with flags.

## Before you start

- The level exists (entry), its holder has a hole for every position, and the
  monitor analyses are in the store and reduce (`elctl import`, or acquired).
- Position N of a level is the Nth hole of its holder (ordinal order,
  N - 1 from zero). The hole's `hole_id` is only a label and is never used to
  find it. A position beyond the holder is an error:
  `position <N> of level <L> of <irradiation> is beyond holder <name> (<M> holes)`.
- Monitors are found by sample name: the analyses of the level whose sample
  equals the monitor set's `sample` (default `FC-2`). Every other position
  that has an identifier is an unknown. A fit saved with `--sample` or
  `--all-positions` chose its monitors another way, and the next fit of the
  level with the same monitor set chooses them the same way (see Options).
  The level's saved fit is its newest saved revision that names a monitor
  set; that revision alone gives the set, the sample and whether every
  position was a monitor, so a level whose positions were saved by
  different fits (some imported later, say) never mixes the set of one
  with the sample of another.
- `F` of a monitor (the 40Ar*/39ArK ratio) comes from the reduction with the
  position's saved flux taken away, so a fit never depends on a J saved
  earlier. An analysis whose reduction reports an error takes no part (it
  is not "omitted": it is back as soon as it reduces); an analysis tagged
  `omit`, `invalid`, `outlier` or `skip` starts omitted.

## The flux window

`Fit ▸ Flux…` in `pychron-ui` opens a window that does what `elctl flux fit`
does, with the review done by eye: the plot shows the monitors and the fit,
and every change refits at once. It needs a store, like entry. The design is
`superpowers/specs/2026-10-07-flux-window-design.md`. The window computes
nothing itself: the fit is `fit_level`, and the plot, the status line, the
warnings and the CSV come from `libs/processing` (`flux_view.hpp`), the same
code `elctl flux` prints from.

Open it from the Fit menu, or from the Packages window: with a level open
there, `Fit flux…` in its tool bar opens the flux window on that level. The
title is `Flux`, and `Flux — NM-300 A` once a level is loaded.

### The level tree

On the left: the irradiations, newest first, each with its levels. The second
column says what the store holds, in one of three words:

| Word | Meaning |
|---|---|
| `no monitors` | No position of the level carries the sample of the standard monitor set. |
| `not fitted` | There are monitor positions, and one or more has no saved J. |
| `fitted` | Every monitor position has a saved J. |

The word is judged against the sample of the default monitor set, whatever
set the level itself uses. Picking a level loads it (the status reads
`Loading NM-300 A…` meanwhile) and fits it with the options it was last saved
with, else the preset in use. The tree is read again whenever anyone saves
something.

### The plot

Above the tables, J against the position round the tray (the hole angle in
degrees, `Hole angle (degrees)`), or against `X` or `Y` for the three
one-dimensional models (`ls1d`, `mean1d`, `bracketing1d`). The legend names
the layers:

- `Analyses`: a small circle for each analysis of a monitor that has a J,
  spread a little about its hole. They have no error bars. A hollow circle,
  named `Analyses (excluded)`, is an analysis that takes no part: omitted by
  its tag, by the saved fit or by you here, or one whose J the weighted mean
  refuses because it has no error (its tooltip says `not used: J has no
  error`). Hover for the record, the J and the reason.
- `Monitor means`: a diamond with error bars for each monitor position, at
  the mean J of its analyses. Hollow when the position is left out of the fit.
- `Unknowns`: a square with error bars at the predicted J of each unknown.
- `Fit`: the curve of the model, with a band of one sigma. The models with a
  curve are `plane`, `bowl`, `weighted-mean`, `ls1d` and `mean1d`; `matching`,
  `nearest`, `bracketing` and `bracketing1d` have none, because each position
  takes its J from its neighbours. Round the tray the curve is drawn at the
  mean radius of the monitors used. With no fit (the fit failed) the analyses
  and means are still drawn, without a curve or the unknowns.
- The monitor selected in the table is drawn again, larger, in the accent
  colour.

To leave an analysis out, click its point; click again to bring it back. A
click reaches the analysis even when a monitor's mean lies over it. To
change several at once, hold Shift and drag a rectangle: every analysis in it
is toggled. An analysis whose reduction failed (`not reduced`) or that gave
no J cannot be toggled. The right-click menu has `Include / exclude`,
`Recall` (only when the application can open a recall window), `Reset view`,
`Copy image`, `Save as PNG...` and `Save as PDF...`. Double-click an empty
place to reset the zoom; a refit keeps your zoom.

### The tables

Below the plot, three tables. A row is a hole; the columns are those of
`elctl flux fit`, with the check boxes added.

**Monitors**: `Fit`, `Save`, `Hole`, `Identifier`, `Sample`, `N`, `Saved J`,
`±`, `Mean J`, `±`, `%`, `MSWD`, `Pred. J`, `±`, `%`, `Dev %`. `Fit` takes the
position into the fit, or leaves it out (it still gets a predicted J, and is
saved). It is unticked and cannot be ticked for a position with no usable
analysis (the tooltip says `No usable analysis`). `Save` says whether the
position is written by Save. A row whose mean MSWD is outside its limits is
tinted; a row out of the fit is dimmed.

**Analyses** (under the monitors, for the selected monitor): `Use`, `Record`,
`Tag`, `J`, `±`, `State`. `Use` leaves the analysis out of its monitor's mean
or brings it back, like a click in the plot. `State` is one of `used`,
`omitted (tag outlier)` (with the tag), `omitted (saved fit)`,
`omitted (here)`, `not reduced` or `no J`; for the last two `Use` is
disabled, and the tooltip of `not reduced` is the reduction's error.

**Unknowns**: `Save`, `Hole`, `Identifier`, `Sample`, `Saved J`, `±`,
`Pred. J`, `±`, `%`, `Dev %`.

J is shown as `%.4e`, percentages and the MSWD with two decimals, a value
that does not exist as a blank. `Dev %` is as on the command line: how far
the J saved now is from the one this fit predicts.

### Every edit refits

A click in the plot, a check box, a change of an option or of a preset
refits at once (150 ms after the last change, so a burst of them fits once) and
redraws the plot, both tables and the status line. There is no Fit button.
What you have changed and not saved is called an edit: omitted analyses,
monitors left out, options that differ from the ones the level was loaded
with, a different monitor group, and unticked `Save` boxes. While any exists
the status line ends `· edited (not saved)`.

### The Fit dock

On the right.

- **Monitors** group: `Monitor set` (a list of the store's monitor sets; its
  tooltip gives the set's sample, age and lambda_k), `Sample` (empty means
  the set's own sample, which the field shows as its placeholder) and
  `All positions` (every position that has analyses is a monitor). These
  belong to the level, not to a preset: they say which positions are
  monitors, so changing one reads the level again (after the question below,
  if you have edits). A level opens with the monitors its saved fit chose.
  Changing the group back to what the level had counts as no change.
- The preset bar, as in the other fit windows. A level with a saved fit opens
  on the saved options and the bar shows `(saved fit)` as its first entry;
  choosing it again returns to them. A level without one opens on the preset
  in use (`Default` to begin with; `Weighted plane` is the other factory
  preset). A preset applies over the current options. What a preset did (it
  loaded with warnings, it was saved) is said in the status line, after the
  fit.
- The options, in the sections Model and Errors, with the names and choices
  of the table under Options. A field that does not apply to the model chosen
  is greyed: `Weighted fit` for the three least-squares models, `Neighbours`
  for `nearest`, `Interpolation` for `bracketing`, `Axis` for `ls1d`,
  `mean1d` and `bracketing1d`, `Degree` for `ls1d`, and `Error of the fit`
  for the models that form it from a fit or a mean. Choosing `sd` for the
  error of the fit of a fitted surface turns the field red; its tooltip is
  the reason, and the status shows it.

### The status line and the warnings

One line at the bottom. A fit reads, for example,
`plane, unweighted · fit MSWD 1.12 (5 dof) · J 1.0012e-03 – 1.0241e-03 (2.24 %)`:
the model, the fit's MSWD and degrees of freedom (left out when the model
has neither), and the lowest and highest J with their spread. When there is
something to look at it ends `· 1 warning` or `· 3 warnings`, and the
warnings are the lines of `elctl flux fit` (an analysis out and why, a
position with no usable analysis, an extrapolated hole, an MSWD outside its
limits, the saved monitor set missing from the store, a saved fit that used
SD), one per line, in the tooltip of the status line.

When the fit cannot be made, the line is red and shows the error as it is
(`flux: bowl needs 6 monitor positions, 5 used`); the predicted columns are
blank, the plot shows the data without a curve, and Save is disabled with
the error as its tooltip. A level with no monitors, or whose holder is
missing, is the same: it loads, and its error is shown. A message from
another action (a preset, a save) follows the line after a `·`; when the
level was changed by someone else while you have edits, it says
`· level changed elsewhere, Reload to see it`.

### Save

`Save` writes the fit on show, exactly as `elctl flux fit --save` would
(below): one changeset, every position or none, the options and the omissions
with it. It is the tool bar's first action, and it is disabled, with the
reason as its tooltip, while the level loads, while a save runs, when no
level is open and when the fit failed. Only the positions whose `Save` box is
ticked are written. The window says one of

```
Saved 12 positions (0 unchanged)
Nothing to save: 12 positions unchanged
Not saved: hole 7 was saved by jsmith at 2026-10-07 14:02:11 UTC since this level was loaded. Reload and fit again.
Not saved: <the error>
```

with `, 2 not saved` added when `Save` boxes were unticked. The conflict
line is red; nothing was written and your edits are kept, so they can be
applied again after a Reload. `Not saved: <the error>` is any other refusal,
such as `hole 5 of NM-300A has J ...: nothing was saved` for a J that is not
a J, and is red too. While it saves the status reads `Saving…` and the plot,
tables and dock are disabled.

After a save that wrote something the window reads the level again, so the
saved J and the revisions are those the next save compares against, and the
tree, the other windows and the Packages window are told. The `Save` boxes
you unticked stay unticked after the save, and are not an edit.

### Revert, Reload, Reset omissions

- `Revert` drops the edits and returns to the options the level was loaded
  with, without touching the store. If you had chosen other monitors, it
  reads the level again with the monitors of its saved fit.
- `Reload` reads the level from the store again (asking first if you have
  edits).
- `Reset omissions` forgets what the saved fit omitted and excluded (tags
  still apply), like `--reset-omits`; it also clears the edits made here. It
  is an edit only when the saved fit had left something out.

### Leaving with edits

Picking another level, `Reload`, changing the monitor group and closing the
window all ask `Save the flux of NM-300 A?` when there are edits, with Save,
Discard and Cancel. Cancel stays where you are. Discard drops the edits and
goes on (on closing, the edits are dropped for good). Save saves, and goes
on only if the save wrote or found nothing to write; a save that conflicts or
is refused keeps you on the level. A monitor group you typed (a sample not
yet entered) is dropped if you answer Save for another purpose; `Save`
itself with a sample typed takes it as a change of group.

### Export CSV and Open in Packages

`Export CSV…` writes the file of `elctl flux fit --csv` for the level on
show (both tables, one row per position, the fit as it is now), under the
name `flux_<irradiation>_<level>.csv` by default. The file is written whole
or not at all, and marked as a file for the user to open elsewhere.
`Open in Packages` shows the Packages window on this level.

### The window and the command line write the same thing

Both call the same `save_level`: the same revisions, the same changeset
message `fit flux for <irradiation><level>`, the same options. A fit saved in
the window is repeated by `elctl flux fit NM-300 A --save`, and the reverse.
The window records its version as the `software` of the revision
(`pychron-ui <version>`). After the window saves, the Packages window shows
the new J of the level it has open, unless it has unsaved edits of its own
(in the grid, the dose table or the level fields): it then keeps them and
does not read the level again.

Known limits: the tree's word for a level is judged against the default
monitor set's sample; the whole tree is read again at every change anyone
makes; and the window has been run on SQLite only.

## The command line: a worked example

Level `A` of `NM-300`, FC-2 in eight holes, unknowns in the rest.

```bash
elctl flux fit NM-300 A --db sqlite:/path/to/store.db --model plane --weighted
```

Nothing is written. The output is a heading, the model line, two tables, the
fit line and one `warning:` line per thing to look at:

```
NM-300 A   holder <holder>   monitors FC-2 (Kuiper 2008): 28.201 +/- 0.046 Ma, lambda_k 5.463e-10
model plane, weighted; mean arithmetic (msem); fit error msem

Monitors
hole  identifier  sample  n  saved J  +/-  mean J  +/-  %  MSWD  pred J  +/-  %  dev %  fit
...

Unknowns
hole  identifier  sample  saved J  +/-  pred J  +/-  %  dev %
...

fit MSWD <m> (<dof> dof)   J min <j>  max <j>  delta <p> %
warning: ...
```

Each J is printed as `%.4e`, its error beside it and then as a percentage
of the J; `-` is an absent value. `saved J` is what the store holds now,
`mean J` the monitor position's own mean, `pred J` what the model says
(F8: a monitor is saved with the model's J, its own mean beside it). `dev %`
is (saved J - predicted J) / predicted J x 100: how far the J now saved differs
from the J this fit predicts; it says nothing of a monitor's own mean, and is
`-` when the position has no saved J. The `fit` column says whether the
position took part in the fit.

Look at the warnings, change what is wrong, run again. Then repeat the same
command with `--save`:

```bash
elctl flux fit NM-300 A --db sqlite:/path/to/store.db --model plane --weighted --save
```

which prints one of

```
saved 24 positions (0 unchanged)
nothing to save: 24 positions unchanged
not saved: hole 5 was saved by <user> at <time> since this fit was loaded
```

Other commands:

```bash
elctl flux fit NM-300 --db ...                 # every level of NM-300, each fitted on its own
elctl flux fit NM-300 A --db ... --save        # no model: repeat the saved fit on the data as it is now
elctl flux fit NM-300 A --db ... --csv fit.csv # both tables, one row per position
elctl flux show NM-300 A --db ...              # what is saved: J, model, who, when; needs no holder, reduces nothing
elctl flux history NM-300 A --db ...           # the saves, newest first, one line per changeset
elctl flux history NM-300 A 5 --db ...         # the revisions of hole 5, with their J
elctl flux monitors list --db ...
```

`elctl flux help` lists every flag. Exit codes: 0 done (warnings included);
1 a level could not be fitted, a save conflicted, or a name asked for does not
exist; 2 usage or a fatal error. With no level, a level that fails is
reported and the rest continue; the exit code is then 1. An unknown
irradiation with no level given exits 2; with a level named, the level fails
and it exits 1.

## Models

A model turns the monitors' mean J into a J at every position. "Needs" is the
number of monitor positions in the fit; fewer is an error that says so
(`bowl needs 6 monitor positions, 5 used`).

| `--model` | Needs | Use it when |
|---|---|---|
| `plane` | 4 | The usual choice: J varies roughly linearly across the tray. Fits `a x + b y + c`. |
| `bowl` | 6 | J curves across the tray. Fits `a x^2 + b y^2 + c x + d y + e` (no `xy` term, as legacy). Needs monitors at more than one distance from the tray's centre: a single ring of monitors (all at one radius) cannot determine a bowl, because `x^2 + y^2` is constant on it, and the fit is refused. |
| `ls1d` | `degree` + 2 | J varies along one axis only (a long tube): a polynomial of `--degree` 1 to 4 along `--axis x` or `y`. |
| `weighted-mean` | 1 | No gradient to speak of: one J for the whole level, the inverse-variance mean of the monitors. |
| `mean1d` | 1 | The same, named for the 1D family; the axis does not change the number. |
| `matching` | 1 | Each position takes the J of its nearest monitor. |
| `nearest` | `--neighbors` N (default 2) | Each position takes the inverse-variance mean of its N nearest monitors. |
| `bracketing` | 2 | Each position lies between its two nearest monitors; `--interpolation weighted` (inverse-variance mean), `average` (mean, sample SD) or `linear` (the projection of the position on the segment between them). |
| `bracketing1d` | 2 | The monitors either side of the position along `--axis`. Always linear; `--interpolation` does not apply to it. |

Nearness is the plain distance in x, y; a tie goes to the monitor first in
hole order. Linear interpolation outside the end monitors extrapolates, and
the position carries a warning (`hole 7 is extrapolated (outside the
monitors)`).

## Options

Every option you give replaces one field of the options of the level's saved
fit; with no saved fit the defaults are `plane`, unweighted, arithmetic mean,
`msem` for both errors (legacy's).

| Flag | Meaning |
|---|---|
| `--weighted`, `--unweighted` | Plane, Bowl, `ls1d`: weight the fit by `1 / j_err^2`. |
| `--mean arithmetic\|weighted` | How the analyses of one monitor position are averaged (J, not F). |
| `--mean-error sem\|msem\|sd` | Error of that mean. `msem` is the SEM scaled by sqrt(MSWD) when the MSWD is above 1. |
| `--fit-error sem\|msem\|sd` | Error of the predicted J, for every model that forms one from a fit or a mean: `sem` or `msem` of a fitted surface (Plane, Bowl, `ls1d`), and `sem`, `msem` or `sd` of the mean models' mean. `sd` on a surface is an error. See "How the errors are formed". |
| `--neighbors N`, `--interpolation`, `--axis`, `--degree` | As the table above. |
| `--monitors NAME` | The monitor set (below). Default: the one the saved fit used, else the store's default. |
| `--sample NAME` | Find monitors by this sample name instead of the set's. Saved with the fit: the next fit of the level under the same monitor set uses it until another `--sample` is given. It does not follow the level to another set: with `--monitors OTHER`, or when the saved set is missing from the store and the default stands in, the monitors are found by that set's own sample (give `--sample` again if you mean otherwise). Given without `--all-positions`, it also undoes a saved `--all-positions`. |
| `--all-positions` | Every position that has analyses is a monitor and appears in the monitor table only. Saved with the fit: the next fit of the level under the same monitor set does the same, unless `--sample` or `--monitor-positions` is given. Like `--sample`, it does not follow the level to another set: with `--monitors OTHER`, or when the default stands in for a saved set the store lacks, the monitors are that set's sample's positions (give `--all-positions` again if you mean otherwise). |
| `--monitor-positions` | The opposite: the monitors are the positions of the monitor sample. This is the default, so it is only needed to undo a saved `--all-positions`. Giving both is a usage error. |
| `--omit RECORD_ID`, `--include RECORD_ID` | Leave one analysis out of its mean, or bring it back (over a tag or a saved omission). A level only. |
| `--exclude-position HOLE` | A monitor position stays out of the fit but still gets a predicted J. Any position of the level; excluding a hole that is not a monitor does nothing and warns `hole <N> is not a monitor position: excluding it changes nothing`; a hole that is not a position is an error listing the level's holes. |
| `--no-save-position HOLE` | Do not save that position. |
| `--reset-omits` | Ignore the omissions and exclusions of the saved fit (all of them: there is no flag to drop one exclusion; `--include` brings one analysis back). |
| `--csv FILE` | Write every position; with no level, every level in one file. The file is replaced at the end of the run and only when at least one level was fitted: a mistyped irradiation, or a level that cannot be fitted, leaves an earlier file as it was. A destination whose directory cannot be written stops the run before anything is saved. |
| `--save [--user NAME]` | Save. The author is `--user`, else `$USER`, else `pychron`. |

A saved fit that used `sd` on a surface (it cannot be made here, but an
import can carry one) is refitted with `msem`, and the command says
`saved fit used SD, which a fitted surface does not have: using msem`
unless you give `--fit-error`.

A saved fit that names a monitor set the store does not have (an imported
level can name `FC Min`) is refitted with the store's default set, and the
command says
`saved fit used monitor set '<name>', which the store does not have: using '<default>'`
unless you give `--monitors`. The monitors are then found by the default
set's own sample, not the saved fit's. Check that the default is the
standard you mean before you save: its age and decay constants are what the
new J is computed with.

## How the errors are formed

- **One analysis**: `J = (exp(lambda_k t) - 1) / F`, the uncertainty of F
  carried through. The monitor's age and lambda_k are plain numbers here. An
  analysis whose F is zero, negative or not finite gives no J: it is rejected
  and named in a warning, never `J = 1`.
- **A monitor position**: the J of its analyses, averaged as `--mean` says.
  With one analysis the error is that analysis's own. The MSWD is taken about
  the mean that is reported. A position whose every analysis is rejected is
  left out of the fit and noted.
- **A surface (Plane, Bowl, `ls1d`)**: least squares on the positions' mean
  J. The error of the J at a point is `sqrt(x C x')` where `C = (X' W X)^-1`,
  and `W` is `1 / j_err^2` when weighted, the identity otherwise. Weighted:
  `sem` is that, `msem` multiplies the variance by the fit's MSWD when it is
  above 1. Unweighted: the residual variance scales it, so `sem` and `msem`
  are the same. The reported MSWD is the scatter of the monitors about the
  fitted surface over `n - q` (q the parameters), using the monitors' errors
  even when they did not weight the fit. In an unweighted fit a monitor with
  zero error is used in the fit but skipped in the reported MSWD, whose
  divisor is then the monitors with a non-zero error minus q.
- **A fit that barely determines itself is refused**, not only an exactly
  degenerate one: when the smallest pivot of the column-equilibrated design
  is below 1e-3 of the largest (monitors nearly in a line, or in a single
  ring under a Bowl), or when a predicted J is not finite and positive, the
  error is `monitor positions do not determine a <model>`. This keeps a ring
  of monitors from yielding a nonsense J.
- **The mean models**: `weighted-mean` and `mean1d` use the inverse-variance
  mean of the monitors with `--fit-error`.
- **Neighbour models**: `matching` gives the monitor's own J and error;
  `nearest` the inverse-variance mean (error `(sum 1/sigma^2)^-1/2`);
  `bracketing` and `bracketing1d` as in the table above, with linear error
  `sqrt(((1-f) e0)^2 + (f e1)^2)`.
- A weighted fit or mean, `nearest`, `bracketing` other than `average`, and
  `bracketing1d` (which always weights by the errors), given a monitor with a
  zero error is an error naming the monitor (its weight would be infinite);
  so is an error so small or so large that its weight `1 / err^2` is not a
  finite number above zero (below about 1e-154 or above about 1e154). A
  weighted mean of a position's analyses leaves such an analysis out and
  names it.
- **The J error is analytical** (F4): it comes from the monitors' F only. The
  age and lambda_k uncertainties are systematic, common to every position of
  the irradiation, and are saved beside the J (`monitor_age_err`,
  `lambda_k_total_err`) for an external-error stage to use. There is no Monte
  Carlo and no position error.
- The fit MSWD is flagged when it is outside the limits for its degrees of
  freedom (`fit MSWD 3.40 is outside its limits`); so is a position's mean
  MSWD.

## What a save writes

`--save` writes one `flux_position` revision per position, all in one
changeset with the message `fit flux for <irradiation><level>` (legacy's, so
history reads the same in both systems). Per position:

- `j`, `j_err`: the predicted J. A monitor position is saved with the
  **model's** J; its own mean is beside it as `mean_j`, `mean_j_err`,
  `mean_j_mswd`.
- `monitor_name`, `monitor_material`, `monitor_age`, `monitor_age_err`,
  `lambda_k_total`, `lambda_k_total_err`: from the monitor set.
- `analyses`: every analysis of a monitor position with whether it was
  omitted. Omitted means by rule: its tag, `--omit`, or an omission carried
  from the saved fit. An analysis that could not be reduced or gave no J is
  saved as not omitted.
- the options (`options_json`): model, weighted or not, error kinds, mean
  kind, neighbours, interpolation, axis, degree, monitor set and sample,
  whether every position was a monitor (`all_positions`), whether the
  position was used in the fit (`used_in_fit`), whether you left it out
  (`excluded`), the fit MSWD and degrees of freedom, and the `software` that
  wrote it. The model strings are legacy's,
  so a fit imported from a legacy meta repository and one saved here read
  alike. `position_jerr` is never written. A position imported from legacy
  with one keeps it in its imported revision, but a save here makes a new head
  with none, and the reduction stops using the imported value for that
  position; it remains only in the older revision.

Then:

- Nothing is saved unless every J to save is a J: finite and above zero,
  with an error that is finite and not negative (a monitor's mean J
  likewise). A neighbour model can extrapolate to a zero or negative J; the
  save is then refused whole, with
  `hole <N> of <irradiation><level> has J <value>: nothing was saved (...)`.
  Change the model, or keep that position out with `--no-save-position`.
- A position whose new value is the same as its head's is not written. A
  save that would write nothing commits nothing and says
  `nothing to save: ... positions unchanged`. The options are compared as
  JSON (key order and spacing do not matter) and the `software` key is not
  part of the comparison: a save after an upgrade, with nothing else
  changed, writes nothing, and the new version is recorded the next time a
  position's value changes.
- Each head moves by compare-and-swap from the revision the fit read. If
  another user saved a position in between with a different value, the whole
  save is a conflict: nothing is written, the output names the position, who
  and when, and the exit code is 1. Run the fit again. A head that moved to
  a value equal to the new one counts as unchanged.
- The omissions, the excluded positions, the options and the choice of
  monitors (`--sample`, `--all-positions`) are in the revision, so
  `elctl flux fit NM-300 A --save` with no flags repeats the fit on the data
  as it is now.
- What a refit carries forward, and what it does not. Carried: the analyses
  you omitted (`--omit`) and the monitor positions you left out
  (`--exclude-position`), until `--reset-omits` discards them all or
  `--include` brings one analysis back. Not carried: a monitor that was out
  of the last fit only because it had no usable analysis then (it joins the
  fit as soon as it has one), and an analysis that did not reduce or gave no
  J then. A revision saved before the `excluded` key existed, or imported,
  is read as excluding a monitor only when it recorded a mean J for it and
  still says it was not used.
- **Unknowns' ages**: the derived cache is keyed by an input fingerprint that
  includes the flux revision, so the ages of the level's unknowns are
  recomputed with the new J the next time they are used. An analysis whose
  flux is **pinned** keeps its pinned J.
- A revision is immutable and nothing entry does can erase a fit. To undo a
  save, the heads can be moved back in the store (`history` shows the
  revisions); there is no `elctl` command for it yet.
- Known limit: a position with no reference object gets one before the
  changeset commits, so a conflicted save on a fresh level can leave
  reference objects with no value. They resolve nothing and are reused by the
  next save.

## Monitor sets

A monitor set names the sample monitors are found by, the monitor's age and
the decay constants. They live in the store as the revisioned document
`pychron/flux_monitors.json`, so every client sees the same sets:

```json
{
  "default": "FC-2 (Kuiper 2008)",
  "monitors": [
    { "name": "FC-2 (Kuiper 2008)", "sample": "FC-2", "material": "sanidine",
      "age_ma": 28.201, "age_err_ma": 0.046,
      "lambda_ec": [5.80e-11, 9.9e-13], "lambda_b": [4.883e-10, 1.4e-12] }
  ]
}
```

A store with no such document behaves as if it held two sets: `FC-2 (Kuiper
2008)` above (age uncertainty 0.046 Ma) and `FC-2 (Renne 1998)` (28.02 Ma,
0.16 Ma; decay constants without uncertainties). The first change writes
them. Pairs are `[value, 1 sigma]` in 1/a; `lambda_k = lambda_ec + lambda_b`,
errors in quadrature.

```bash
elctl flux monitors list --db ...            # names; the default is marked *
elctl flux monitors show "FC-2 (Kuiper 2008)" --db ...   # one set, as JSON
elctl flux monitors default "FC-2 (Renne 1998)" --db ...
elctl flux monitors set sets.json --db ...   # --user NAME names the author
```

`set FILE` replaces the whole document: the file must list every set to keep.
`set` and `default` save over the document as they read it: if someone saved
it in between, nothing is saved, the command says
`not saved: someone else saved the monitor sets since they were read; run the command again`
and exits 1.
A document is checked on load and on save: a non-empty `monitors` list, unique names, each with a
non-empty `sample`, a `default` that names one of them (a document with no
`default` takes the first set), positive ages and decay constants,
non-negative errors. A document that fails is an error naming the key. Only
unknown top-level keys are kept. The level's monitor set is the one its newest
saved revision used; a saved name the document no longer has falls back to the
default set, with the warning described under Options.

## How this differs from legacy Pychron

The numbers differ from legacy's where legacy was wrong. There is no switch
that reproduces a legacy number (F2).

| Legacy | Here |
|---|---|
| The MSWD of a surface fit is the scatter of J about the level mean | About the fitted surface, over `n - q`, so a real gradient does not inflate the error |
| A weighted plane uses unweighted residuals with the weighted covariance | Residuals, covariance and scale are all weighted |
| Bowl and least-squares 1D ignore the weighted option | All three least-squares models honour it |
| With `n <= q` the error is silently 0 | Fewer monitors than the model needs is an error: `bowl needs 6 monitor positions, 5 used` |
| A monitor with a zero error has an infinite weight | A weighted fit or mean given a zero or non-finite error is an error naming the monitor |
| The arithmetic mean of one analysis has error 0 | That analysis's own error |
| The arithmetic mean's MSWD is taken about the weighted mean; the SD of a weighted mean is the unweighted SD | The MSWD about the reported mean; SD as the statistics library defines it |
| Bracketing "Average" uses the population SD of two values | The sample SD (n - 1) |
| An F of zero gives `J = 1` | The analysis is rejected and named |
| Extrapolation past the end monitors is silent | Still extrapolates, with a warning for the position |
| Nearest neighbours needs 3 positions whatever N is | It needs N |
| Plane accepts 3 monitors (an exact fit, error 0) | It needs 4 |
| SD is offered for surface fits (F13) | It is not; an imported fit saved with it refits with `msem` and says so |
| Monte Carlo errors (10 trials, unseeded) and a position error | None (F5). Every model has a closed-form error and results are reproducible |
| Four more models (RBF, GridData, IDW, order 5 polynomial) that assign no J without Monte Carlo | Not ported (F1). A fit imported with one means "no saved options" |
| `j_err` is the analytical error and no age uncertainty exists anywhere | Still analytical (F4); the monitor age error and lambda_k error are recorded beside every J |
| The monitor's name and age are written under `options` and read from a dict nothing writes, so every analysis reports FC-2 at 28.201; material is saved as the monitor name | The monitor's name, material, age and decay constants are saved as fields and read back |
| A save from labnumber entry replaces the whole position and erases the fit's fields; already loaded unknowns keep the old J | A revision is immutable; the ages of the level's unknowns are recomputed on next use |
| The error kind of the mean, axis, degree, neighbours and the positions left out are not saved | All are in the revision, and a fit with no model named starts from them |
| Built-in `FC Min` and `FC SJ`, plus a hand-edited yaml read at import | A shared, revisioned monitor-set document (above) |
| A position can be in both tables with "all positions" | A monitor appears in the monitor table only |

Unchanged: position N is the Nth hole of the holder (as in legacy and in entry; `hole_id` is a label), the J formula, averaging J rather than F, the two mean kinds and
the error kinds of the means, neighbour selection, extrapolation, no
automatic rejection of outliers, a Bowl with no `xy` term, and the commit
message `fit flux for <irradiation><level>`.
