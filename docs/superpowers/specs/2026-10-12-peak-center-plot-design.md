# Peak center plot in the experiment window

Status: agreed 2026-10-13 (owner's decisions, section 9). Drafted by an
unattended run 2026-10-12. Plan:
`docs/superpowers/plans/2026-10-13-peak-center-plot.md`; sections 1, 4.1 and
4.4 corrected from code when it was written. Nothing built. Task T042 (`pychron usage notes.md`);
related T004 (minimum signal), T018 and T048 (live plots).

## 1. Problem

Peak center during a run leaves one text line: `EvolutionsView::on_peak_center`
(`apps/pychron-ui/src/evolutions_view.cpp:160`) appends "Ar40 on H1: center
5.123456, table updated" to a list; executor pane adds same as event line
(`executor_pane.cpp:232`). Operator cannot see peak: shape, flat top, where
centre fell, whether other detectors' peaks coincide, why a try failed.

Data already there. `PeakCenterResult` (`libs/systems` `jobs/peak_center.hpp:122`)
carries every try's `SweepPoint{x, map<DetectorId,double> y, ts}`, reference
`PeakShape`, `DetectorPeak` per additional detector; `PeakCenterDone` reaches
UI through `ExperimentBridge::peakCenterDone`. Missing: a view, and points
while sweep runs. (`JobRunner` does publish each `ProgressUpdate` as
`JobProgress`, `job_runner.cpp:56-62`, point included, but keyed by job id,
with no try index, and bridge does not relay it.)

## 2. Legacy (examined 2026-10-12, `~/Programming/pychron`)

- Graph: `spectrometer/jobs/peak_center.py:475-510`. x "DAC (V)", y
  "Intensity". Series 0 reference detector, label `*<det>`; one series per
  additional detector in detector's colour. Legend upper left.
- Shown as page "Peak Center" of run's plot panel, replacing previous page
  and selected (`experiment/plot_panel.py:151-159`).
- Live: redraw each sweep step (`jobs/sweep.py:157-200`).
- Additional detectors scaled onto reference, whole array again each step
  (`sweep.py:244-256`):

      oys = (oys - mir) * R / r + miR

  `R = max(ref) - min(ref)`, `miR = min(ref)`, `r = max(det) - min(det)`,
  `mir = min(det)`, over points so far; applied only if `r` and `R` nonzero.
  Raw values kept beside; fit reads raw.
- Overlays: green markers at low, centre, high (`lx,cx,hx`); orange marker at
  maximum; black vertical rule at start value; rule at found centre; blue
  rule at centre + `dac_offset` when used. No fitted curve.
- Retry (2 tries) clears graph: tries replace, not overlay.
- Saved with analysis: per detector low / centre / high x and signal,
  resolution, resolving powers, packed points (`dvc/dvc_persister.py:942-966`).

## 3. Goal

While and after a peak center in a run, operator sees sweep of reference and
additional detectors on one graph, other detectors scaled to reference, with
low / centre / high marked and outcome stated.

## 4. Design

### 4.1 Events (`libs/systems`)

Two new bus events beside `PeakCenterDone` (`jobs/peak_center.hpp`), published
by `Runner` only when `PeakCenterOptions::bus` is set:

```cpp
struct PeakCenterTryStarted {
  DetectorId detector;                       // reference
  std::string isotope;
  std::vector<DetectorId> additional;
  int try_index = 0;                         // 0-based
  int n_tries = 0;
  double start = 0, end = 0, initial_center = 0;  // native units
};
struct PeakCenterPoint {
  int try_index = 0;
  SweepPoint point;
};
```

- `PeakCenterTryStarted` published before first move of a try
  (`Runner::attempt`, `peak_center.cpp:335`).
- `PeakCenterPoint` published per sweep step. `Sweep::run` takes a
  `Progress&` and a `Progress` gets its sink only when constructed: each try
  gives the sweep its own `Progress`, whose sink publishes the point and then
  reports to the job's. Job's own progress reporting unchanged.
- `PeakCenterDone` unchanged; stays the one authority for outcome. View
  replaces what it drew live with `result.tries.back().points`.
- No new thread, no wait: events published on job's thread, as
  `PeakCenterDone` is now. Clock rules untouched.

### 4.2 Bridge (`apps/pychron-ui`)

`ExperimentBridge`: `relay<jobs::PeakCenterTryStarted>`,
`relay<jobs::PeakCenterPoint>`, signals `peakCenterTryStarted`,
`peakCenterPoint` (queued, as `peakCenterDone`, `experiment_bridge.cpp:26-53`).
Rate is one per sweep step (step's integration, typically 1 s): no batching.

### 4.3 Scaling (pure function)

`apps/pychron-ui/src/peak_center_scale.hpp`:

```cpp
// y of `other` mapped onto the range of `reference`: (y - min_o) * R / r + min_ref.
// Empty when either range is zero or either input is empty: caller draws raw.
std::optional<std::vector<double>> scale_to_reference(std::span<const double> reference,
                                                      std::span<const double> other);
```

- Legacy's formula exactly (section 2). Recomputed over all points of try at
  each new point.
- Display only. Fit, table update and anything stored use raw values.
- Tooltip and legend give raw intensity, not scaled.
- Detector with no value at a step (`SweepPoint::y` lacks it): point left out
  of that series, not drawn as zero.

### 4.4 View

New `PeakCenterView` (`apps/pychron-ui/src/peak_center_view.{hpp,cpp}`), a tab
"Peak center" of Evolutions dock beside kind tabs of `EvolutionsView`. Kind
tabs map to series kinds by index: the new tab is last, outside that map,
and swaps the kind plot, focus box and intercepts table for the view.

- x: magnet value applied, native units, as `SweepPoint::x`. y: intensity of
  reference detector (other series scaled onto it).
- Series: reference, legend `*H1`, drawn heavier; one per additional detector,
  colour from spectrometer config (`EvolutionsView` gets same map), legend
  detector name.
- Title: `Ar40 on H1, try 1 of 2`; when done, outcome: `center 5.123456, table
  updated` or `failed: <message>`.
- Markers from `PeakShape` when done and ok: low, centre, high (one colour),
  maximum (another). Vertical rule at `initial_center` from try's start; rule
  at fitted centre when done; third rule at centre without `dac_offset` only
  when `config.dac_offset != 0`.
- Failed: points stay, no markers, title says why.
- New try (`PeakCenterTryStarted`, `try_index > 0`): earlier tries stay,
  drawn faint (thin, low opacity, no markers, not in legend); the current
  try is drawn full (decision 5). Scaling of 4.3 is computed within a try.
- Tab selected when a try starts (decision 2).
- Existing text list stays under graph; click on a row shows that peak
  center again. Results kept for this: newest 20, in memory.
- Queue start clears view, as `EvolutionsView::clear`.
- Drawing: `QCustomPlot` directly, as `EvolutionsView` (decision 1). No
  `processing::Scene`. View is fed plain data and tested without a session.

### 4.5 Which detectors

Configurable (decision 3): reference and `additional_detectors` of the named
config in the lab's `peak_center.toml`, which the runner already reads and
fits. Shipped `configs/examples/peak_center.toml` gains
`additional_detectors` naming the example spectrometer's other detectors
that carry a peak at the reference position, so the example shows the
scaling; user guide says how to choose them.

## 5. Rules

- Scaling never reaches fit, field table or record.
- With no bus given, runner publishes nothing new and behaves as today
  (headless `elctl` unchanged).
- View shows only what events say: no read of spectrometer from UI thread.
- `PeakCenterDone` without preceding live events (older path, test) still
  draws complete graph.
- Simulated session: no real time anywhere in view (no timers for
  correctness).

## 6. Out of scope

- Storing sweep points in run record and showing them in recall (legacy
  did; record today keeps one `record::Event` text line,
  `libs/experiment/src/run/run.cpp:516`). Follow-up: needs record format
  change, own spec.
- Skipping peak center below minimum signal: T004.
- Peak center from spectrometer window outside a run.
- Interpolated curve (`use_interpolation`), deconvolution of several peaks.
- Mass on x axis.
- Per-isotope stacked evolutions: T018.

## 7. Files

- `libs/systems/include/pychron/systems/jobs/peak_center.hpp`,
  `libs/systems/src/jobs/peak_center.cpp`: two events, published.
- `apps/pychron-ui/src/experiment_bridge.{hpp,cpp}`: relays.
- `apps/pychron-ui/src/peak_center_scale.{hpp,cpp}`: scaling.
- `apps/pychron-ui/src/peak_center_view.{hpp,cpp}`: view.
- `apps/pychron-ui/src/evolutions_view.{hpp,cpp}`,
  `experiment_window.cpp`: tab, wiring, list click.
- `configs/examples/peak_center.toml`: `additional_detectors`.
- `docs/user/04-experiments.md`: what graph shows.

## 8. Tests

- `tests/systems/test_jobs_peak_center.cpp`: with bus, a two-try run
  publishes `TryStarted(0)`, n points, `TryStarted(1)`, n points, `Done`, in
  that order, points equal to `result.tries[i].points`; without bus, nothing.
- `tests/ui/test_peak_center_scale.cpp`: formula against hand numbers; zero
  range gives nullopt; scaled maximum equals reference maximum.
- `tests/ui/test_peak_center_view.cpp`: points fed one by one grow series;
  second detector scaled; done-ok draws markers at `PeakShape` values;
  done-failed draws none and titles message; second try leaves first try's
  series faint and unmarked; `Done` alone draws all tries; tab is selected
  when a try starts; row click restores earlier result.
- `tests/ui/test_experiment_bridge.cpp`: two new signals relayed.
- `tests/ui/test_experiment_window.cpp`: simulated queue with
  `peak_center.after`: tab exists, graph has reference series after run.

Changes `apps/pychron-ui`: final run is `dev-ui`.

## 9. Decisions (owner, 2026-10-13)

1. Draw with `QCustomPlot` directly. (Zoom, save and tooltip for live plots
   stay T048's question.)
2. Jump to the Peak center tab when a peak center starts.
3. Additional detectors are configurable: `additional_detectors` of the
   config in `peak_center.toml` (4.5).
4. Scaling is legacy's range mapping (4.3).
5. Retries stay, faint, behind the current try.

Not asked, kept as drafted: no jump back to Signal when measurement begins.
