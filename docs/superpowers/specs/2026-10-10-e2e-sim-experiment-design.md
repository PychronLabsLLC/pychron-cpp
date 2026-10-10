# End-to-end simulated experiment: from a fresh install to dated runs in the data window

Status: approved 2026-10-10. Plan 1 of section 13 (store path) implemented
2026-10-10 (`../plans/2026-10-10-e2e-store-path.md`, "As built"); plans 2
and 3 not started. Sections 5, 7.1, 7.3, 7.4, 7.6, 8, 10.3, 10.4, 10.6 and
14 state plan 1 as built. Not done in plan 1: PostgreSQL not run for this
plan; UI path not driven by a person at the screen;
unknowns, ages, simulated samples, step rule, example irradiation (plans 2
and 3).

## 1. Goal

Someone with built pychron and nothing else can, in three steps, entering
nothing by hand:

1. make install (`elctl init <profile> --yes`, or setup wizard),
2. run queue install shipped with, on simulated hardware,
3. see each run appear in data window as it finishes, and open it.

Queue: lab's day, in small: air shots between blanks, one sample
step-heated, one sample dated by series of total fusions. Both samples have
ages: step-heated one draws spectrum with plateau, fusions draw ideogram,
both at age simulated sample was given.

"Run": whole experiment process as on instrument: queue, executor,
extraction script, laser, measurement plan, post-measurement script, save.
Only transports simulated. "See": run is in install's database (DVC store),
because data window of an install reads that.

Same path is first automated test from profile to reduced age: also tells
us pieces still fit together.

## 2. Where things stand

Works today:

- `elctl init <profile> --yes` and wizard write install: line, canvas,
  spectrometer, plans, scripts, defaults, identifiers, example queue, SQLite
  database at `<root>/data/pychron.db`, seed (`<root>/seed.toml`) applied to
  it: project `references`, eight reference samples with special
  identifiers (`bu`, `ba`, `bc`, `be`, `bg`, `a`, `c`, `ic`), reactor
  `Triga`.
- With `--sim` (or install whose `simulation` answer is true) line and
  spectrometer simulated, beam fed from line's gas (`feed_beam_from_line`),
  queue runs to end. Example line has CO2 laser (`[drivers.co2]`, Chromium
  driver on simulated transport), tray (`example-9`), its place on stage.
- Finished run saved as JSON record,
  `<data>/records/<identifier>/<runid>.json`, by `FilePersister`, through
  `SavePipeline` (spool first, then hand-off, re-sent at next queue start).
- Data window reads records folder by default, a store when `--db` given.
  Figures include ideogram and spectrum; recall view shows J and age.
- `libs/entry` can make package with levels, chronology and production in
  one changeset (`create_package`), put samples on positions
  (`LevelSheetEdit`), allocate identifiers.

Missing:

1. Nothing in experiment writes to store. `libs/experiment` does not link
   `libs/persistence`; `IStore::ingest` called only by legacy importer.
   Database persister named in experiment spec (section 8) and outbox of DVC
   schema spec (section 8.1) never built. So install has seeded database no
   run ever reaches.
2. Simulator has no sample. Every non-blank run lets in one pipette of
   tank's air, whatever queue says is heated; laser heats nothing. No Ar39
   in gas, so no F, no age (`docs/simulator.md`: "Gas from a sample ...
   releases nothing"). `SimExtractionDevice` has release model (gas per
   stage position, logistic in peak temperature) no program uses.
3. Shipped queue (`profiles/instrument-common/experiment.toml`) runs `bu`
   and two steps of `66001` (sample `FC-2`, project `examples`). Nothing
   creates `66001`, `FC-2` or `examples`; store refuses analysis whose
   identifier is not in its catalog.
4. Unknown in store has age only when its identifier is on irradiation
   position with a J, level with a production, irradiation with a
   chronology. Nothing ships any of that; J can be written only by flux fit
   over monitor analyses already in store.
5. Seed has no mass spectrometer, no extract device. Store refuses analysis
   whose mass spectrometer or extract device is not in its catalog.
6. Consecutive steps of one identifier each get new aliquot unless queue
   fixes aliquot on every row (`-01A`, `-02B`, ...): step-heat runnable
   twice cannot be written in queue file.
7. No profile ships `sim.toml`, tray map or stage calibration: installed
   simulated line runs on `SimSettings`' defaults, its laser has no tray.
8. `pychron-ui` on instrument install reads records folder, not install's
   database. Only `--db` or data-reduction install opens a store.
9. Data window refreshes when opened and when "Rescan" pressed, not when
   run finishes.
10. `elctl exp run` cannot open a store.
11. No test runs a queue and then reads the store; no test asserts an age
    on anything a queue measured.

## 3. Decisions (owner, 2026-10-10)

1. Finished run written to store directly: store-backed persister calling
   `IStore::ingest`. No outbox, no uploader in this work.
2. Shipped queue has blanks and airs (`ba`, `a`, `a`, `a`, `ba`).
3. When a save to the store fails in the middle of a queue, the record
   stays in the spool, the run is flagged `save_error`, the queue goes on,
   and the spool is re-sent at the next queue start. `SavePipeline` already
   does this for file persister.
4. Both programs covered: `pychron-ui` and `elctl exp run` write same
   store; UI's data window shows each run when it finishes.
5. Unknowns in scope: shipped queue also step-heats one example sample and
   fuses series of grains of another; both come out with ages.
6. J for example unknowns written by seed, not fitted. Seed makes example
   irradiation, its level, positions, production, chronology, and J for
   each position.
7. Simulator gets samples in holes of tray under simulated laser. What hole
   gives off depends on what is in it and how hot laser has made it; step
   heating and fusion are same mechanism at different powers.

## 4. Out of scope

- Outbox, uploader, leases, more than one writer per database. One
  acquisition program at a time per database, as for importer.
- Flux monitors in queue and flux fit as part of path. Seeded J stands
  until someone fits one; `docs/flux.md` unchanged. (Running FC-2 in
  simulated tray and fitting the level: natural next step; sample model of
  section 6 can hold monitor as well as unknown.)
- Assigning blanks or IC factors to runs automatically. Run stored with
  intercepts and baselines fitted at acquisition; unknown's age reduced
  with blank of zero and IC factors of one until someone assigns them.
  Section 10.1 says what that does to asserted ages.
- Age column or age filter in data browser's list: that is
  `2026-10-09-age-cache-filter-design.md`. Here age seen in recall view and
  figures.
- Calcium and chlorine in simulated sample (Ar37, Ar36 and Ar39 from Ca,
  Ar38 from Cl). Example samples sanidine-like, with none (section 6.3).
- Simulated furnace. Laser is the one extraction device that gets samples.
- Copying install's existing `<data>/records` into its database.
- Artifacts (snapshots, video) in store: stay files (section 5.6).
- Example lab without install (`pychron-ui --sim` on machine with no
  install, `elctl -c extraction_line.toml --sim exp run`): no database,
  keeps writing JSON records. Does get simulated samples (section 6.6), so
  its laser queue measures real sample gas.
- Windows: CI builds there without persistence, so store path not built or
  tested there. File persister remains what Windows has. Simulated samples
  built and tested everywhere.
- Simulated time as default of an install. Quick start names `--sim-speed`
  (section 9).

## 5. The store persister

### 5.1 Where it lives

New library `libs/experiment/adapters/store` (target
`pychron::experiment_store`, namespace `pychron::experiment::store`), laid
out like store adapter of `libs/processing`. Links `pychron::experiment` and
`pychron::persistence`; built only when persistence is
(`-DPYCHRON_PERSISTENCE=OFF` and missing Qt6 Sql skip it, like
`libs/ingest`).

`libs/experiment` itself gains no dependency: keeps seeing only
`persist::IAnalysisPersister`. No Qt in new library's public headers.

Two headers, conversion kept apart so it is testable without database and
reusable by uploader later:

- `record_ingest.hpp`: pure conversion from `AnalysisRecord` to what store
  ingests.
- `store_persister.hpp`: `StorePersister`, an `IAnalysisPersister` owning
  nothing but reference to an `IStore` and conversion's inputs;
  `Acquisition`, what a program opens (section 5.3); `without_password`.

Public compile definition `PYCHRON_EXPERIMENT_HAS_STORE=1`.

### 5.2 Conversion: `AnalysisRecord` to ingest items

```cpp
struct RunIngest {
  std::vector<persistence::IngestItem> blobs;  // one per distinct raw series
  persistence::IngestItem analysis;
};

Result<RunIngest> to_ingest(const record::AnalysisRecord& record, persistence::Uuid client);
```

Rules:

- **Everything is derived from the record; nothing is drawn at random and
  nothing reads a clock.** Analysis uuid: `record.identity.uuid`. Changeset
  uuid, six root revision uuids (`signals`, `intercepts`, `baselines`,
  `blanks`, `icfactors`, `tags`) and each item's idempotency key: each
  `persistence::Uuid::v5(<record uuid>, label)`. Labels: `changeset`,
  `signals`, `intercepts`, `baselines`, `blanks`, `icfactors`, `tags`,
  `item` (analysis item), `blob:<sha hex>` (blob item).
  `AnalysisIngest::created` and `timestamp`: record's timestamp (run took it
  from `clock.wall_now()`, so simulated run stamped in simulated time).
  Same record converted twice gives same bytes.
  This makes re-send safe: a record left in the spool by a crash between
  the store's commit and the spool's removal is sent again as the same
  item, and the store answers `duplicate` instead of making a second
  analysis.
- `IngestItem::payload_sha256` of analysis item and
  `AnalysisIngest::record_sha256`: record's own `provenance.sha`;
  `record_schema_version`: `provenance.schema_version`. Blob item's
  `payload_sha256`: blob's own sha.
- **Refused**, each a `Config` error, record stays in spool: record not
  finalized (`provenance.sha` not 64 hex characters); uuid or timestamp
  that does not parse; step that is not letters; series whose `t`, `v` or
  sigma differ in length; two series with same kind and key (one isotope on
  two detectors, or two baseline series on one detector: store's keys hold
  one); intercept whose isotope has no series.
- **Not finite.** Intercept or baseline value or error that is NaN or
  infinite: stored as unknown (`nullopt`). Run still saved.
- **Identity.** `identifier`, `aliquot`, `analysis_type`, `run_index` from
  `record.identity`. `increment`: `increment_of(step)`, inverse of
  `make_runid`'s step letters: empty step is `-1`, `A` is `0`, `Z` is `25`,
  `AA` is `26`; case folded (`a` is `A`). Step not letters: error (run not
  saved to store, stays in spool).
- **Instrument.** `mass_spectrometer`, `laboratory` (unset when empty),
  `analyst` from `record.instrument`. `extract_device` is `nullopt` when
  record's is empty or `(none)`.
- **Sample.** Record's sample, project, material, irradiation, level and
  position text *not* written. In store analysis belongs to its identifier;
  identifier's catalog rows say what sample that is and where irradiated.
  Record's text is what operator typed in queue; catalog is authority.
  (Shipped queue: the two agree; a test holds seed, `defaults.toml` and
  queue together.)
- **Extraction.** `ExtractionFields` from `record.extraction`: value,
  units, duration, cleanup, pattern, cryo temperature from spec; beam
  diameter from actuals. Fields record does not carry (`pre_cleanup`,
  `post_cleanup`, `weight`, ramp fields, `light_value`) stay unset. `tray`
  unset: record carries none.
- **Positions.** Each hole in `record.extraction.spec.positions` becomes
  `MeasuredPositionRow` with that position. No load: record carries none,
  and load is not made up from tray name.
- **Time zero.** `AnalysisIngest::time_zero` unset. `Identity::timestamp`
  is wall time taken when run is prepared, not origin of
  `record.data.time_zero`; field left unset rather than guessed.
- **Raw series.** Each `DataSeries` becomes one blob: codec `f32le-tvs/1`
  when it has a sigma, else `f32le-tv/1`; address
  `blob_sha256(codec, bytes)`. Two series with same bytes: one blob item.
  Each series gets `SignalRefRow`: `series_kind` is series' `kind`,
  `series_key` is isotope (detector for baseline), `detector`, `blob_sha`,
  `n_points`. Blobs listed in first-use order.
- **Isotopes and detectors.** One `IsotopeRow` per distinct (isotope,
  detector) among series of kind `signal`, sorted by isotope; one
  `DetectorRow` per detector among all series, sorted, with deflection and
  gain from `record.spectrometer` when present.
- **Intercepts and baselines.** `Results::intercepts` (by isotope) and
  `Results::baselines` (by detector) become root `InterceptRow`s and
  `BaselineRow`s: value, error, `n`, fit and error type, outlier filter.
  Text form is what `StoreSource` reads: `fit` is `to_string(FitKind)`,
  `error_type` is `SD` or `SEM`, `filter_outliers_json` is
  `{"filter_outliers":0|1,"iterations":N,"std_devs":X}`. Intercept's
  detector: its isotope's signal series' detector.
- **Blanks, IC factors, tag.** No blank rows. IC factor row for each
  detector record names one for, error `0`. Tag `ok`.
- **Scripts.** Only `ScriptBodies::measurement` written, from
  `record.measurement.plan.effective_plan_toml` when non-empty. Record
  carries name and sha of other scripts, never text (`ScriptRef::text_ref`
  never set): extraction, post-equilibration and post-measurement script
  texts not stored.
- **Spectrometer snapshot.** `SpectrometerSnapshot` from
  `record.spectrometer`: gains, deflections, and settings (source
  parameters plus `integration_time`) as JSON objects with sorted keys, so
  snapshot's content address is stable; `spectrometer_json` is `{}`.
- **Meta.** `AnalysisMetaRow`: installed and tripped conditionals, whiff
  result, grain polygons, pipette counts, software version and git sha,
  queue name, each as JSON, each only when record has it; no meta row when
  record has none of them.
- **Peak centers.** Record keeps peak center as event with text detail, not
  numbers. Not written by this work.
- **Queue.** No `QueueRow`. `record.identity.queue_uuid` holds queue's
  *name*, not a uuid; written as `AnalysisMetaRow::queue_names_json`,
  one-element JSON array, when non-empty.

Carried by record, not stored (nothing dropped without being written down
here): sample/project/material/irradiation text, `time_zero`, tray, load,
queue row, extraction/post-equilibration/post-measurement script texts,
peak centers, events and notes, `pid_params`, extraction `series`,
`snapshot_refs`, `cryo_measured`, `manometer_pressure`, `data.counts`,
`spectrometer.state_hash` and `field_table_version`, hook reference, plan
overrides.

### 5.3 `StorePersister`

```cpp
class StorePersister final : public persist::IAnalysisPersister {
 public:
  StorePersister(persistence::IStore& store, persistence::Uuid client, std::filesystem::path data);
  ...
};
```

- `save_analysis(record)`: convert; ingest each blob; ingest analysis.
  Blobs go first, so the analysis is stored with `signals_state = complete`
  and never shows as "signals uploading". Blob already there: not an error.
  `duplicate` acknowledgement of analysis: success. Record with no series:
  analysis with no blobs, `signals_state = complete`.
- Blobs ingested before an analysis the catalog then refuses stay in store
  as unreferenced content-addressed rows; re-send finds them there.
- `next_aliquot(...)`: section 5.4.
- `begin_run`, `save_extraction`, `save_artifact`: as today, files under
  `<data>` (section 5.6).
- `flush()`: nothing to do; returns success.
- `check_queue(...)`: section 5.7.

`StorePersister` is the direct, one-thread class: holds no connection of its
own; `store` outlives it.

**A store handle works only on the thread that opened it.** Queue's thread
is not the one that opens the program's store, so a program does not use
`StorePersister` directly:

```cpp
class Acquisition {
 public:
  persist::IAnalysisPersister& persister() noexcept;
  ...
};
Result<std::unique_ptr<Acquisition>> open_acquisition(const std::string& url, std::filesystem::path data,
                                                      const std::string& software_version);
std::string without_password(const std::string& url);
```

- `open_acquisition` opens `url` with `migrate = false` and registers the
  client (section 8.4) on a worker thread the `Acquisition` owns. Failure
  to open or register: returned, no thread left running. Schema behind:
  error text gains `Run: elctl db migrate --db <url without password>`.
- `Acquisition::persister()` is the `IAnalysisPersister&` a session is
  given. Every call on it runs on the worker, one at a time, in order of
  arrival; caller waits. Calls may come from any thread.
- Worker is a plain thread on a `std::condition_variable`, not a clock
  participant: to simulated clock a caller blocked on it looks busy, so
  simulated time stands still for length of call, as for a file write.
- `open_acquisition` is called from program's main thread: Qt application
  object the store needs is made there, on calling thread, by
  `persistence::ensure_host_application()`, not on worker.
- Destroying it serves calls already queued, closes store on worker, joins.
  No call may be in progress or made once it is being destroyed, so an
  `Acquisition` is declared before the session that saves through it.
- `without_password(url)`: `url` as it may be shown; password of a
  `postgresql://user:password@host` (or `postgres://`) url removed, any
  other url unchanged.

### 5.4 Aliquots and steps

Two changes: where numbers are read from (plan 1, built: "The store read"
and `StorePersister::next_aliquot` below), what a step does (plan 3, not
yet built).

**The rule for a step (plan 3, not yet built).** Today row with step and no fixed aliquot gets new
aliquot like any other row, so step-heat written without aliquots comes out
as `-01A`, `-02B`, `-03C`. Queue fixing aliquot on every row works once:
run again, its run ids already exist. Shipped queue must be runnable any
number of times, so rule becomes:

> A row with a fixed aliquot uses it, as today. A row with no step and no
> fixed aliquot takes the next aliquot of its identifier, as today. A row
> with a step and no fixed aliquot continues the identifier's last aliquot
> when that aliquot's last run has a step that comes before this row's;
> otherwise it takes the next aliquot.

So `A` always starts a new aliquot (nothing comes before `A`), `B` after `A`
continues it, and `B` after a `C`, or after a run with no step, starts a new
one. The decision is made from what has been saved and issued, never from
what the allocator remembers of the queue, so a step-heat that is stopped
after `C` and resumed in another session continues the same aliquot at `D`.

Change to experiment, not only store path: applies to `FilePersister` too,
so queue means same thing with and without database.
`docs/user/04-experiments.md` (aliquot and step rule) to be rewritten with
plan 3; today it states the old rule, which is what the code does. Queue that
today relies on steps splitting into aliquots can afterwards say so only by
fixing aliquots; none known to exist.

**The interface.** Plan 1: `IAnalysisPersister::next_aliquot(identifier)`
unchanged; `last_run`, `LastRun` and step rule come with plan 3.
`StorePersister::next_aliquot` is one more than highest of (a) store
(`IStore::last_analysis`), (b) records waiting in spool, (c) what this
persister issued. Store cannot be read: (a) is value last read for that
identifier by this persister; none read yet: the error is returned and run
does not start. Spool cannot be listed: error. Spooled record that cannot
be read is not counted (it can never be sent). Plan 3, not yet built:
`next_aliquot(identifier)` becomes

```cpp
struct LastRun { int aliquot = 0; std::string step; };  // step empty: none
// The identifier's highest aliquot and the last step of that aliquot;
// nullopt for an identifier with no run.
virtual Result<std::optional<LastRun>> last_run(const std::string& identifier) = 0;
```

and rule above lives once, in `AliquotAllocator`, over that answer (plan
3). For `StorePersister` answer is latest of (a) store, (b) records waiting in
spool, (c) what was handed out this session. (b) matters: a run whose save
failed is not in the store yet, and its number must not be given to the
next run. If the store cannot be read, (a) is the value last read for that
identifier this session; with nothing read yet it is an error and the run
does not start.

**The store read (plan 1, built).** `IStore` gains one read:

```cpp
struct LastAnalysis { int aliquot = 0; int increment = -1; };
// The highest aliquot among the analyses of `identifier`, and the highest
// increment within it; nullopt when it has none.
virtual Result<std::optional<LastAnalysis>> last_analysis(const std::string& identifier) = 0;
```

Lookup by identifier (through identifier's uuid), not a search. Runs once
per run, so held to "Schema and queries": plan reads its query plan on
`SchemaIndexes.TheLastAnalysisOfAnIdentifierIsASeek` requires index by name
and no `SCAN`. No migration: `analysis` has
`UNIQUE (identifier_uuid, aliquot, increment)`. Unknown identifier:
`nullopt`, like one with no run.

### 5.5 Failures

`SavePipeline` unchanged. The record is spooled first; then
`StorePersister::save_analysis` is called; on success the spool copy is
removed; on any failure it stays, the run ends `save_error`, and the queue
goes on. The executor's `recover()` at the next queue start sends it again.

Two kinds of failure, told apart only in what operator is told:

- **The catalog refuses it** (`is_unknown_catalog_reference`): identifier,
  mass spectrometer or extract device not in store. Sending it again will
  fail again until someone adds the row. Run's message: store's text
  (names the row), then `: not in the database. Add it (elctl entry ...)
  and start a queue to send the run again`. Store's words stay first, and
  error's kind and code stay store's, so `is_unknown_catalog_reference`
  still tells it. Preflight (section 5.7) exists so this does not happen in
  a queue that started.
- **Anything else** (database locked, server away, disk full, run id
  already taken): message is store's.

Record the store refuses stays in spool, so `recover()` tries it at each
queue start and fails each time. Accepted: one cheap transaction, pending
count shows it, removing record from spool by hand is a person's decision.

Hand-off stays inline (session builds `SavePipeline` without a `post`). So
a run's analysis is in the store before `RunFinished` is published, which
is what lets the data window refresh on that event (section 8.2). Cost:
ingest's time is inside run: about 4 ms a run on SQLite (shipped queue,
fresh argus install). Not measured on PostgreSQL.

### 5.6 What stays on disk

With store configured:

- `<data>/spool/<uuid>.json`: the record, until the store has acknowledged
  it. Unchanged.
- `<data>/records/<identifier>/<runid>.extraction.json`: partial record
  written after extraction, as today. What is left of run that died in
  measurement.
- `<data>/records/artifacts/<uuid>/<name>`: artifacts, as today.
- `<data>/executor_state.json`: unchanged.

No longer written: finished record,
`<data>/records/<identifier>/<runid>.json`. Store is the one home of
finished run. `StorePersister` composes a `FilePersister` for the two
things that stay files rather than repeating its code.

Review point: if second, human-readable copy of every finished run on
instrument's own disk is wanted while store path is new, alternative is to
keep writing JSON record too. Offered, not chosen (decision 3); means two
copies that can disagree.

### 5.7 Preflight

`IAnalysisPersister` gains one method:

```cpp
// What would stop this queue's runs from being saved, one line each;
// empty when nothing would. FilePersister: always empty.
virtual Result<std::vector<std::string>> check_queue(const QueueSpec& queue) = 0;
```

`StorePersister` answers, in this order:

1. `the queue names no mass spectrometer`, or
   `mass spectrometer '<name>' is not in the database`;
2. `extract device '<name>' is not in the database`, for queue's device and
   then each run's, each name once;
3. `identifier '<identifier>' is not in the database`, each once, written
   exactly as in queue (`BU`, `bu ` are not `bu`);
4. `<runid> is already in the database`, once for each row whose aliquot
   is fixed and whose (identifier, aliquot, step) the store has.

Skipped rows: identifier and run id not checked (a row's extract device
is). A store read that fails ends the check with one
line and nothing else: `the database cannot be reached: <store's text>`.

Session calls it in `LabSession::start`, after queue's own check, before
`QueueStarted` and before executor touches line. Non-empty answer refuses
start with `Config` error `the queue cannot be saved:` followed by the
lines, one per line (`the queue cannot be saved: <error>` when
`check_queue` itself fails). UI shows it where it shows a start that
failed; `elctl exp run` prints each as `error:` line and exits 1,
`--dry-run` prints them too. Queue edited while it runs is not checked
again: row added later with identifier catalog lacks is caught by section
5.5.

Why refuse, not warn: on instrument, queue that runs all night and saves
nothing to database is worse outcome; check costs few lookups by key.

## 6. Simulated samples

### 6.1 What is there and what is not

Example line's laser: real Chromium driver talking to simulated Chromium
over simulated transport. Arrangement to keep: driver, stage, tray map and
calibration under test are ones a lab uses. Missing: anything under beam.

`SimExtractionDevice` (libs/sim) has right physics, deliberately simple:
sample's temperature follows laser's power with first-order lag; fraction
of hole's gas that has come off depends only on hottest hole has been, by
logistic in temperature. So holding a power releases nothing more and
stepping hotter does: that is step heating, and one step far past curve is
total fusion. Used by no program, knows one gas amount for every position,
reports amount with no composition.

### 6.2 The model

New pure type in `libs/sim`, `SampleTray`: clock-free, standard library and
`core/error.hpp` only, like `GasNetwork`.

- Tray: set of holes at stage coordinates, each empty or holding one
  sample. Sample: named material and amount.
- Material: what gas is made of (section 6.3) and how it comes off: one or
  more components, each a fraction of material's gas with own release curve
  (logistic's midpoint and width, in degrees). One component gives flat
  spectrum. Two, small low-temperature one mostly air and main one mostly
  radiogenic, give first steps low radiogenic yield and same age: what real
  spectrum looks like, costs nothing more.
- Tray is told where beam is, its power, how much time passes (`advance`);
  answers with gas released in that time, by species. Thermal lag and
  release fraction are `SimExtractionDevice`'s, moved here so there is one
  copy; `SimExtractionDevice` is rebuilt on it or, if the plan finds
  nothing left that needs it, removed.
- A hole is heated when the beam is within the hole's radius. Beam between
  holes, or on empty one, heats nothing.
- Hole remembers hottest it has been. Its gas is finite: second queue over
  same holes in same session finds them spent. New session starts from full
  tray, as simulated tank starts full.
- No noise in it: what comes off is exact. Noise stays where it is, on
  readings, keyed by (seed, name, time).

`SimSystem` owns tray, as it owns device sims: on each query moves tray to
`clock.now()` with simulated laser's present position and power, puts what
came off into named volume of gas network with `SimSystem::inject`. Sample
gas therefore enters network as all gas does, reaches detectors through
valves, volumes and inlet like air shot; nothing touches beam.

How tray learns laser's position and power: the one piece of wiring to find
in plan; simulated Chromium behind transport holds both. Rule: tray reads
simulated hardware's state, never driver's, queue's or script's: hardware
does not know what an identifier is.

### 6.3 What a sample's gas is

Material described as geochronologist would; simulator works out species:

| key | meaning |
|---|---|
| `age_ma` | age sample will be found to have |
| `j` | J of where it was irradiated |
| `ar39` | Ar39 from potassium, per unit of sample: sets size of every signal |
| `radiogenic` | fraction of Ar40 that is radiogenic, per component |

From these: F = (exp(lambda * age) - 1) / J; radiogenic Ar40 = F * Ar39;
Ar40 from potassium by production ratio K4039; atmospheric Ar40 from
radiogenic fraction, with Ar36 and Ar38 at ratios of air (`air_ratios`,
already in `gas.hpp`); Ar38 from potassium by K3839. `lambda` and the two
production ratios: keys of `sim.toml` with defaults reduction and seed use.

This is reduction run backwards, written out in `libs/sim` rather than
called from `libs/reduction`: simulator stays free of code it is there to
test. Test in `tests/integration` (section 10.2) holds the two together, by
reducing what simulator made.

Not modelled: calcium and chlorine (no Ar37; no Ar36, Ar38 or Ar39 from
them), and decay of Ar39 between irradiation and measurement (few parts in
ten thousand per year). Example samples sanidine-like, so first is true to
them. Ar37 channel reads baseline and noise; reduction's correction for
calcium, scaled up for decay of Ar37 since irradiation, multiplies that
noise: why example chronology is recent (section 7.2); section 10.1
measures what is left.

### 6.4 `sim.toml`

New tables, read and range-checked in `sim_config.cpp`, listed in
`configs/examples/sim.toml`, given rows in `docs/simulator.md`. Key names
fixed in plan; shape:

```toml
[materials.example-sanidine]
age_ma = 28.201
j = 0.00350
ar39 = ...                 # per unit of sample
[[materials.example-sanidine.components]]
fraction = 0.05            # of the Ar39
radiogenic = 0.40
release_c50 = 500
release_width_c = 60
[[materials.example-sanidine.components]]
fraction = 0.95
radiogenic = 0.98
release_c50 = 900
release_width_c = 80

[trays.co2]                # the laser this tray is under
tray = "example-9"         # tray_maps/, stage_calibrations/
into = "..."               # the volume the chamber is part of
[[trays.co2.holes]]
hole = 1
material = "example-sanidine"
amount = 1.0
```

- No `[trays]` table: nothing changes; laser heats nothing, as today. That
  is `SimSettings`' default.
- `SimConfig.TheExampleFileIsTheDefaults` holds example file to defaults;
  example's materials and tray added to what that test already pins as
  example's own numbers.
- Hole tray map does not have, material not defined, volume network does
  not have, fractions not summing to one, radiogenic fraction outside
  (0, 1]: each a config error naming the key.

### 6.5 Scripts

`laser_extract.py` and `sim_extract.py` today let in pipette of air "in
place of" sample gas for every non-blank run. With samples under laser that
stand-in goes: `laser_extract.py` isolates chamber's section, moves, fires,
waits, lets gas settle, for blank and unknown alike. Blank is same script
on hole tray has left empty: what laser blank is on an instrument.

`sim_extract.py` (valves only, no laser) keeps its pipette: line with no
laser has no sample. Its line number pinned by script-editor test left
alone.

### 6.6 The example lab

`configs/examples/sim.toml` gets materials and tray;
`configs/examples/experiment.laser.toml` then measures sample gas; its
header comment and `docs/simulator.md` ("Gas from a sample", "Ages", "The
stock queues") rewritten. Tests asserting laser queue's runs read like air
shots changed to assert what they now read.

## 7. What an install ships

### 7.1 Seed: the mass spectrometer and extract devices

`seed.toml` gains two optional keys; `apply_seed` ensures what they name
with `add_mass_spectrometer` and `add_extract_device`:

```toml
mass_spectrometers = ["{{ family }}"]
extract_devices = []
```

Instrument template renders mass spectrometer from profile's `family`
(`argus`, `helix`, `ngx`), same value shipped queue's `mass_spectrometer`
is rendered from. `extract_devices` ships empty in plan 1 (shipped queue
names no device); `co2`, example line's laser, comes with plan 3, listed
only when install takes example line. Seed still only adds: mass spectrometer or
device already there kept as is. `SeedReport` and `describe()` count the
two new kinds. `parse_seed` refuses empty or repeated name.

### 7.2 Seed: the example irradiation

`seed.toml` gains tables for what unknown needs; `apply_seed` makes them
through `libs/entry`'s own functions (`create_package`, `LevelSheetEdit`),
not writes of its own:

```toml
[examples]
project = "examples"
irradiated_days_ago = 30

[[examples.samples]]
sample = "example-sanidine-A"
material = "sanidine"

[[examples.samples]]
sample = "example-sanidine-B"
material = "sanidine"

[examples.irradiation]
name = "EX-1"
reactor = "Triga"
level = "A"
hours = 8

[[examples.irradiation.positions]]
position = 1
sample = "example-sanidine-A"
identifier = "..."
j = 0.00350
j_err = 0.0000035

[[examples.irradiation.positions]]
position = 2
sample = "example-sanidine-B"
identifier = "..."
j = 0.00350
j_err = 0.0000035
```

- **What is made**: project, samples, irradiation with its reactor's
  production (seed's own `Triga`) and one dose, level `A`, positions with
  their samples, identifier on each position, J on each position.
- **Only adds, as a whole.** If the irradiation exists, nothing under it is
  made or changed: not a level, not a position, not a J. The report says it
  was kept. A lab that has refitted its example level keeps its fit.
- **The chronology is relative**: one dose of `hours`, ending
  `irradiated_days_ago` days before the time the seed is applied.
  `apply_seed` takes that time as argument (caller passes clock's wall
  time; test passes fixed one). Reason is section 6.3: reduction scales
  Ar37 reading by its decay since irradiation, and irradiation years old
  turns noise on empty channel into a correction. Thirty days: factor under
  two. Review point: install kept for years has example irradiation that
  old, its example ages degrade; remedy is new database, user guide says
  so.
- **The identifiers** must not disturb lab's own numbering:
  `allocate_identifiers` continues from highest it finds. Plan settles
  their form by reading allocator and `identifiers.toml`; the rule is that
  after the seed, the first identifier a lab generates is the one it would
  have got without it.
- **J without a fit.** Store today has no way to write J but flux fit. Seed
  writes, per position, position's flux reference object and one revision
  whose value is `FluxValue{j, j_err}` with decay constant simulator used,
  no monitor, no analyses, `options_json` saying it was seeded. One
  changeset for level, message `seed flux for EX-1A`. Function of
  `libs/entry` (`seed_flux`), not general "set J" command: no `elctl` verb,
  no UI added for it. To check in plan: that flux window and
  `elctl flux show` open a level whose J has no fit behind it and say so,
  rather than fail; what they need changed to do that is part of this work.
- **When.** The example tables are in the instrument template only when the
  install is simulated and takes the example line: they describe example
  tray. `parse_seed` accepts seed without them, as today.

### 7.3 `sim.toml`, the tray and the laser

`extraction-line-starter`, when the line it installs is the example line,
also copies `@examples/sim.toml` (plan 1: this file only) and, with plan 3,
`tray_maps/example-9.txt`, `stage_calibrations/co2.example-9.toml` and
whatever else plan finds example laser needs to run a queue (patterns,
`cameras.toml`). Imported or
legacy line gets none of it, as today: example's numbers describe example's
volumes and stage.

Tray in `sim.toml` loaded with the two example materials: one hole of
sample A, to be step-heated; several holes of sample B, one grain each, to
be fused; rest empty, one of the empty ones is blank's. J and age in
`sim.toml` and J in `seed.toml` are same numbers written twice; section 7.5
keeps them same.

### 7.4 The queue

Plan 1 ships rows 1 to 5 only: `ba, a, a, a, ba`, scripts `sim_blank` and
`sim_air`, plan `multicollect` with no overrides, post-measurement
`sim_pump`, no extract device, no tray, queue name `first-queue`,
`mass_spectrometer = "{{ family }}"`; 19 min 15 s of simulated time on a
fresh argus install. Plan 3:
`profiles/instrument-common/experiment.toml`, for install on example line,
becomes:

| rows | identifier | what |
|---|---|---|
| 1 | `ba` | blank for airs |
| 2-4 | `a` | three air shots |
| 5 | `ba` | blank |
| 6 | `bu` | laser blank: empty hole |
| 7-12 | sample A | one hole, steps `A` to `F` at rising power, no aliquot fixed |
| 13 | `bu` | laser blank |
| 14-18 | sample B | five holes, full power, no step |
| 19 | `bu` | laser blank |

Queue-level `extract_device = "co2"` and `tray = "example-9"`. Airs and
their blanks use `sim_air` and `sim_blank` and no position; plan confirms
that a queue with an extract device may hold rows that do not use it, and
if not, this is two shipped queues (airs; unknowns) run one after the
other, and quick start says so. Plan `multicollect`, the one install builds
from its own detectors; post-measurement `sim_pump`. `66001`, `FC-2` and
stand-in go.

Step powers chosen in plan from release curves so six steps each carry
useful share of Ar39 and at least three consecutive ones make a plateau;
tuning pinned by test of section 10.1 like example lab's other numbers.

Review point: nineteen runs is long first queue in simulated time. One file
so quick start is one command. Alternative: short default
(`experiment.toml`: rows 1 to 5) and second file for unknowns.

Install on imported or legacy line has no simulated tray: its shipped queue
is rows 1 to 5 only, with whatever plan finds those lines can run (today's
queue assumes example's valves too).

### 7.5 The tests that hold them together

`tests/setup/test_profiles.cpp`
(`ShipsASeedThatTheDefaultsAndTheExampleQueueAgreeWith`) widened. For every
instrument profile with default answers:

- every identifier of shipped queue is one seed makes (reference identifier
  or example position's);
- queue's mass spectrometer and extract device are seed's;
- every script, plan and tray queue names is a file of install;
- each run's sample and project are seed's for that identifier;
- every hole queue heats for sample A or B holds, in install's `sim.toml`,
  a material whose `j` is J seed gives that sample's position; and queue's
  blank hole is empty there;
- simulator's production ratios and decay constant are seed's reactor's and
  reduction's preset.

### 7.6 Field tables (found in plan 1)

Not foreseen above. Placeholder field tables profiles copied from
`configs/examples/tables/` disagreed with installed one-hop `multicollect`
plan: fresh simulated install never measured Ar36. `profiles/qtegra` and
`profiles/ngx` now ship own placeholder tables (`tables/argon`,
`tables/argon_hv`; `tables/ngx_argon`), built so every isotope of the
multicollection sits on its detector: collectors one mass unit apart on
reference detector's own position-against-mass curve. Example tables
unchanged.

`tests/setup/test_profiles.cpp`
(`EveryInstalledPlanPutsItsIsotopesOnItsDetectors`): for every instrument
profile, every installed plan, every hop, each active detector's isotope is
within 0.01 of where the magnet is for hop's target. 0.01 because simulated
peak is flat within 0.02 of centre and each run's peak center moves table
by a few thousandths.

Profile versions: `qtegra` 2, `ngx` 2, `extraction-line-starter` 4,
`instrument-common` 3.

Measured, fresh argus install, shipped queue: air Ar40 about 94 500 fA;
Ar36 about 309 cps on CDD; raw Ar40/Ar36 about 305.8, 0.54 % scatter (air's
298.56 over simulated counter's yield 0.9847 is 303.2); blanks about 11 fA.

Unconfirmed: that ngx placeholder table, or profile's detector layout,
matches a real NGX.

## 8. Wiring

### 8.1 The session takes its persister

`LabSession` always builds a `FilePersister` today. `SessionOptions` gains

```cpp
persist::IAnalysisPersister* persister = nullptr;  // not owned; nullptr: files under <data>/records
```

With one given, session's `SavePipeline`, `AliquotAllocator` and
`RunServices::persister` use it; spool stays `<data>/spool` either way.
Must outlive session.

### 8.2 `pychron-ui`

- **Which database.** One rule for whole program: the database is `--db`
  when given, else the chosen install's `database` (with password from
  install's credentials, as `database_url()` already does). That one url is
  what data window, Entry and Fit menus, run factory's identifiers and
  saving of runs use (`choose_database`, `database_choice.hpp`).
- **An instrument install with a database** therefore reads its store in
  data window and writes its runs to it. Without database (example lab,
  install from before installs had one) nothing changes.
- **`--db` with no install** (`pychron-ui --sim --db <url>`) also *saves*
  runs to that database; before, `--db` only browsed.
- **Two handles.** Data window's `StoreSource` used on UI thread; runs saved
  through an `Acquisition` (section 5.3), whose worker owns second handle.
  Both open with `migrate = false`. `Acquisition` declared before session.
- **A database that cannot be opened** (or whose password cannot be read)
  does not stop instrument install from starting: line and spectrometer
  must be operable without it. Data window shows banner with the reason and
  the records folder it shows instead; log has `ERROR [ui]` line; session
  is given a persister whose `check_queue` answers
  `the database cannot be reached: <reason>`, so no queue starts. (Data
  window's handle opens but acquisition's does not: log line and refusal,
  no banner.) Schema that is behind is such a failure; its message names
  `elctl db migrate --db <url>`. Data-reduction install still refuses to
  start, as before: nothing without its store. `--db` given explicitly
  that does not open for browsing: fatal, exit 2, as before.
- **Refresh.** `ExperimentBridge` relays `RunFinished` and queue start to
  UI thread; data window refreshes on both (queue start: `recover()` may
  have just sent older records). No timer. Refresh only if store changed
  (`StoreSource::refresh` reads only change cursor: one statement when
  nothing new). Hidden data window refreshes when next shown. Refresh keeps
  rows loaded, filters, selection, scroll position and column widths; new
  run filters admit appears at top, since list is newest first. Open figure
  window not redrawn by new run: shows what it was opened on.
- **Pending saves.** Executor pane's pending text, with a database:
  "N run(s) waiting to be saved to the database"; without:
  "N record(s) in the spool".
- **`--sim-speed`** no longer needs `--sim` when install is simulated:
  check moved from command-line parsing to after install is chosen.

### 8.3 `elctl exp run`

- `--db <url>` added. Without it, `--install NAME` supplies an instrument
  install's database, as it already supplies line, spectrometer and data
  folder. With neither, records written as files, as before.
- Store opened through `open_acquisition`, on main thread, before anything
  is printed or started. Failure to open is fatal here, exit 1: no line to
  keep alive for an operator. Outdated schema's message names
  `elctl db migrate --db <url>`.
- Install's database whose password cannot be read: fatal,
  `error: the installation's database: <reason>`; no fall back to files.
  `--db` overrides it.
- Queue the database cannot take: `error: the queue cannot be saved:` and
  one `error:` line each (section 5.7), exit 1, nothing run; `--dry-run`
  prints them too.
- Last line says where runs went:
  `N/M run(s) succeeded; in <database>` (`without_password`), without a
  database `N/M run(s) succeeded; records in <data>/records`. When any are
  pending: `warning: K record(s) waiting in <data>/spool` (without a
  database: `... still in the spool`).
- `exp.cpp` split: store part `exp_store.cpp` with stub
  `exp_store_stub.cpp`, like `import` and `export`; build without
  persistence refuses `--db` with usual "built without the DVC store", and
  an install there writes files.

### 8.4 The client

Store stamps every analysis with acquisition client that sent it.
`open_acquisition` registers
`{<HOSTNAME env, else "localhost">, "acquisition", nullopt, <software version>}`.
`register_client` is ensure-by-(hostname, role), so every session on one
host is one client; no file kept in install. Open: `HOSTNAME` is rarely
exported, so in practice every machine registers as `localhost`.

### 8.5 SQLite, two connections

UI reads while queue's thread writes, on two connections to one SQLite
file. Store already sets `journal_mode = WAL`, under which reader does not
block writer; busy timeout is 10 s (`libs/persistence/src/tiny/db.cpp`).
Test: `AcquisitionTest.AReaderDoesNotLockOutTheWriter`
(`tests/experiment/test_store_persister.cpp`). Same test is instantiated on
PostgreSQL when `PYCHRON_TEST_PG_URL` is set; not run there yet.

## 9. The quick start

User guide (`docs/user/01-getting-started.md`) and `README.md` get path
this spec makes true; nothing they say is left untested:

```bash
elctl init argus --yes
```

```bash
pychron-ui --install argus --sim-speed 20
```

Then: open experiment window, open `experiment.toml`, start. Open data
window: each run is there when it ends. Select steps of sample A, draw
spectrum; select fusions of sample B, draw ideogram. Both come out at age
`sim.toml` gave samples.

Without UI:

```bash
elctl --install argus --sim exp run ~/Pychron/argus/experiment.toml --sim-speed max
```

`pychron-ui --sim-speed` needs no `--sim` beside it when install's
`simulation` is true (section 8.2). After plan 1 quick start holds for
shipped queue of airs and blanks: five runs in data window, no spectrum, no
ideogram, no age. Queue named relative to install's folder is found there:
`elctl --install argus --sim exp run experiment.toml --sim-speed max`.

Plan 1 brought in line: `docs/user/01-getting-started.md`, `README.md`,
`docs/user/04-experiments.md` ("What is written"),
`docs/user/08-elctl-reference.md` (`exp run`), `docs/dev_setup.md`
(sections 3, 5 and 6), `docs/installation_runbook.md` (2.3 and 2.4),
`docs/entry.md` (the seed); `AGENTS.md` got plan 1's rules of section 11,
compressed. Plan 2 will: `docs/simulator.md` (samples; stock queues). Plan
3 will: `docs/user/04-experiments.md` (aliquots and steps), `docs/flux.md`
(level whose J was seeded), quick start with spectrum and ideogram, rest of
section 11's rules in `AGENTS.md`.

## 10. Tests

### 10.1 The end-to-end test

`tests/integration/test_install_e2e.cpp`, built only with persistence. One
instrument profile (argus), in temporary root with `PYCHRON_SITE_CONFIG`
pointing into it:

1. plan and apply install with default answers, create and migrate its
   database, apply its seed: same calls `elctl init --yes` makes;
2. bring up install's line, laser and spectrometer simulated, on a
   `VirtualClock`, and a `LabSession` with a `StorePersister` on install's
   database;
3. run install's own `experiment.toml` to end.

Plan 1 as built (`pychron_install_e2e_tests`, suite `InstallE2E`,
`AFreshInstallsQueueLandsInItsDatabase` and `RunningItAgainNumbersOn`),
on shipped queue of five: run ids newest first `ba-02`, `a-03`, `a-02`,
`a-01`, `ba-01`, each `signals_state = complete`; each loads and equals
record that was saved; airs' raw Ar40/Ar36 within 3 % of 303.2 (air's
298.56 over simulated counter's yield 0.9847: no IC factor applied); airs'
Ar40 over 1000 times blanks'; spool empty; 19 min 15 s on the clock, real
time under its bound; second session on same database numbers on (`ba-03`,
`a-04` to `a-06`, `ba-04`). Rest below is plan 3.

Then, through a `StoreSource` opened on same database as data window opens
it:

- nineteen analyses, newest first, with run ids aliquot rule gives:
  `ba-01`, `a-01` to `a-03`, `ba-02`, `bu-01`, sample A `-01A` to `-01F`,
  `bu-02`, sample B `-01` to `-05`, `bu-03`; each `signals_state =
  complete`, each under seed's project and sample;
- each loads (`load` and `load_raw`) without error, with isotopes and
  detectors of install's plan;
- each loaded analysis has intercepts, baselines and raw series of the
  record that was saved (the spool's copy is captured by a persister
  wrapper before it is removed), compared through `analysis_from_record`:
  store path and records path give same analysis from same run. This is
  the test that conversion of section 5.2 lost nothing data window reads;
- airs' Ar40 is orders of magnitude above blanks'; their 40/36 is simulated
  air's within a tolerance;
- every step of sample A and every fusion of sample B reduces to an age,
  with J, production and chronology found through store;
- `build_spectrum` over sample A's aliquot finds plateau whose age is
  material's `age_ma` within a tolerance; steps' Ar39 sums to what hole
  held, within a tolerance;
- `build_ideogram` over sample B's fusions gives weighted mean at
  material's `age_ma` within a tolerance;
- first step of sample A has lower radiogenic yield than last;
- laser blanks small against fusions;
- spool empty, and run took no real time (upper bound of real seconds, per
  "Time").

Tolerances fixed in plan by measuring shipped queue; they are where things
this spec leaves out show up: no blank subtracted, Ar37 channel is noise,
Ar39's decay not modelled. If the ages do not come out within a tolerance
that means something (the aim is a percent), that is a finding about one of
those, and it is settled before the test is loosened.

Then the queue is run a second time in a new session on the same database:
it is not refused, sample A's steps are `-02A` to `-02F`, sample B's
fusions `-06` to `-10`.

Derives from `VirtualTimeTest`, follows its rules. Runs on PostgreSQL too
when `PYCHRON_TEST_PG_URL` is set.

### 10.2 The simulated samples

`tests/sim/test_sample_tray.cpp`, pure:

- a hole held at one power releases a share and then nothing more; a hotter
  step releases more; shares over rising steps sum to the whole and no
  further;
- beam off hole, on empty hole, or at no power releases nothing;
- two components come off at own temperatures; composition of what comes
  off at each step is mix of what each released;
- species of a material: for given age, J and radiogenic fraction, released
  Ar40, Ar39, Ar38 and Ar36 are those of section 6.3, to rounding;
- same sequence of advances cut at different times gives same total;
- each config error of section 6.4.

In `tests/integration`, without store: material's gas, injected and
measured through example line and spectrometer and reduced with seed's
production and material's J by `libs/reduction`, gives material's age.
Tests that simulator's arithmetic and reduction's are inverses, apart from
store and queue.

Example lab's laser queue: its unknowns have Ar39, and no air shot is let
in for them.

### 10.3 The conversion

`tests/experiment/test_record_ingest.cpp` (executable
`pychron_experiment_store_tests`, built only with the adapter), no database:

- same record converted twice is identical, ids and bytes;
- two records differing only in uuid share no id;
- step letters to increment, and bad step refused;
- series with sigma and one without take the two codecs; two identical
  series are one blob;
- each field of section 5.2, a row each, so field added to record later has
  obvious place to be added;
- each refusal of section 5.2; non-finite intercept stored as unknown;
  record with no series converts.

### 10.4 The persister and the aliquot rule

`tests/experiment/test_store_persister.cpp` (same executable), on SQLite
(and PostgreSQL when set):

- record saved twice is one analysis, and second save succeeds;
- identifier, mass spectrometer or extract device catalog lacks: the save
  fails as a catalog refusal, nothing is written, and after the row is
  added the same record saves;
- `next_aliquot` (plan 3: `last_run`) across sessions (new persister on
  same store), with record waiting in spool, with the store made unreadable
  after one successful read, with spool that cannot be listed;
- `check_queue` names exactly what is missing, and a fixed run id that is
  taken;
- with `SavePipeline` and an `IStore` wrapper that fails on demand: the run
  is `save_error`, the queue continues, the spool holds the record, and
  `recover()` empties it once the store answers again;
- a crash between the store's commit and the spool's removal (wrapper
  reports failure after a real commit): `recover()` leaves one analysis;
- draft record stays in spool, next queue still starts;
- `Acquisition`: calls from other threads and from two at once served on
  worker; reader does not lock out writer; Qt application belongs to
  caller's thread; schema behind and failure to open returned; one client
  per host. `without_password`.

Plan 3, in `tests/experiment/test_persist.cpp`, for `AliquotAllocator` over
a fake and over `FilePersister`: each line of rule of section 5.4 (`A` starts an
aliquot; `B` after `A` continues; `B` after `C` starts one; a step after a
run with no step starts one; a fixed aliquot is used as given; no step
takes the next), and a step-heat stopped after `C` and resumed with a new
allocator continues at `D`.

### 10.5 The seed and the profiles

`parse_seed` and `apply_seed` for new keys and tables: added, kept, dry
run, refusals; example irradiation kept whole when it exists; chronology at
time given; seeded position's J read back through `resolve_refs`; first
identifier a lab generates after the seed. Section 7.5.

`tests/processing/test_flux_store.cpp`: level whose J was seeded loads in
`load_level`, and a fit saved over it replaces it.

### 10.6 `elctl`

In `apps/elctl/tests/test_exp_commands.cpp`: `elctl init argus --yes` into
temporary root, then
`elctl --install argus --sim exp run ... --sim-speed max`: last line names
database, store has queue's analyses (five in plan 1, nineteen after plan
3). `--db` overrides install; database that cannot be opened, schema that
is behind, install whose password cannot be read: fatal, nothing run. Queue with identifier store lacks
exits non-zero before any run, naming it. Build without persistence refuses
`--db`.

### 10.7 The UI

- Data window's refresh on `RunFinished`: with a `StoreSource` on SQLite
  store, ingest an analysis, emit bridge's signal, model has one more row;
  filters and selection kept.
- Choice of database: `--db` over install's, install's over none, and
  instrument install whose database cannot be opened still starts.
- Flux window on seeded level (section 7.2).

### 10.8 Concurrency and cost

- Two-connection test of section 8.5.
- Anything that adds a thread or shares state with one is run under
  `-DPYCHRON_SANITIZE=thread` before it lands. Plan 1 added one thread:
  `Acquisition`'s worker (section 5.3). Tray moved under `SimSystem`'s
  mutex like network.
- Query-plan test of section 5.4.
- Ingest's time per run, measured, in commit message (section 5.5).

## 11. Rules that must hold

1. `libs/experiment` does not depend on `libs/persistence`. Store is
   reached only through `IAnalysisPersister`.
2. Conversion of record to ingest items is pure function of record and
   client: no clock, no random id, no store read. Changing an id's recipe
   makes a re-sent record a second analysis: it is a file format.
3. A record is spooled before the store is tried, and leaves the spool only
   after the store has acknowledged it.
4. A save that fails never stops the queue and never blocks the next run.
5. The store is written before `RunFinished` is published.
6. Persister never creates a catalog row that says what a run *is*: not an
   identifier, a sample, a project, a mass spectrometer or an extract
   device. (Store's ingest creates analyst's user row and queue row, as it
   does for every ingest.) A queue that names something the catalog lacks
   is refused at its start.
7. One acquisition program per database at a time.
8. An instrument install starts and its hardware is operable when its
   database cannot be opened.
9. Whatever shipped queue names, shipped seed makes; whatever it heats,
   shipped `sim.toml` holds; and J the seed gives a position is J the
   simulator gave the sample in it.
10. Without a database, everything behaves as it does today, except the
    aliquot rule for steps (section 5.4), which is the same with and
    without one.
11. An aliquot is decided from what has been saved and issued, never from
    what an earlier row of the queue happened to leave in memory.
12. A simulated sample's gas enters through `SimSystem::inject`, from what
    simulated hardware is doing: tray never learns an identifier, a sample
    name from the queue, or what a script intends.
13. Simulator does not call the reduction. Its arithmetic is its own, and a
    test reduces what it made.
14. Seed only adds, and example irradiation is added whole or not at all.
15. Shipped queue can be run any number of times on one database.
16. A store handle is used only on the thread that opened it. A program
    saves runs through an `Acquisition`, which owns that thread, and
    declares it before the session, so that no call is in progress or made
    once it is being destroyed.
17. An install's plans and its field table are held together by a test:
    every isotope a plan's hop measures sits on its detector, within 0.01,
    where the magnet is for that hop (section 7.6).
18. `--db`, or an install's database, that is named and cannot be used never
    falls back to record files: `elctl exp run` stops, `pychron-ui` starts
    no queue.

## 12. Earlier specs this touches

Changed in same branch, so they do not disagree with this one:

- `2026-09-29-experiment-system-design.md`, section 8: database persister
  it names is `StorePersister`, in adapter library, not in
  `libs/experiment`; `IAnalysisPersister` gains `check_queue` (plan 1,
  noted there); `last_run` and aliquot rule for steps are plan 3, note to
  be added with it.
- `2026-10-01-dvc-schema-design.md`, section 8.1, and
  `2026-09-29-persistence-adr.md`, ADR-0002 option B: outbox and uploader
  still the design for instrument that must keep running while server is
  away. Until they exist, the spool is what covers that gap, and conversion
  of section 5.2 is what an uploader will send. Note says so in each.
- `2026-10-07-install-defaults-design.md`: its decision 2 ("the experiment
  does not read the store") becomes "the experiment reads the store only to
  number aliquots and to check a queue against the catalog"; seed's new
  keys and tables.
- `2026-10-06-lab-simulator-design.md`: samples under laser; what becomes
  of `SimExtractionDevice`.
- `2026-10-06-flux-fitting-design.md` and `2026-10-07-flux-window-design.md`:
  a J that no fit made.
- `../plans/2026-10-02-dvc-persistence.md`, D3: note that direct path came
  first.

## 13. Order of work

One spec, three plans, each landing on `develop` on its own with its tests:

1. **The store path.** Sections 5, 7.1, 8, and shipped queue of rows 1 to 5
   (airs and blanks). After it, fresh install runs a queue into its
   database and data window shows it. No sample, no age.
2. **Simulated samples.** Section 6. After it, example lab's laser queue
   measures sample gas, and material's gas reduces to its age outside
   store.
3. **The example irradiation and the unknowns.** Sections 5.4's step rule,
   7.2 to 7.5, rest of 10.1. After it, goal of section 1 holds.

1 and 2 do not depend on each other. 3 needs both.

## 14. To settle while planning

Collected from above; each is answered by reading or running, not by a
decision, unless it turns out otherwise:

1. Settled: ids are `persistence::Uuid::v5(<record uuid>, label)`, labels
   in section 5.2.
2. Settled: `Identity::timestamp` is wall time when run is prepared, not
   origin of `Data::time_zero`; `AnalysisIngest::time_zero` unset. Record
   carries no tray. `Identity::queue_uuid` holds queue's name: no
   `QueueRow`, name in `queue_names_json`.
3. Settled: no script text in record (`ScriptRef::text_ref` never set);
   only effective measurement plan stored as a script.
4. Settled: client is `{<HOSTNAME env or "localhost">, "acquisition",
   nullopt, <program version>}`, ensured by (hostname, role); no file in
   install (section 8.4).
5. Settled: SQLite busy timeout already 10 s, with WAL (section 8.5).
6. Settled: `analysis` has `UNIQUE (identifier_uuid, aliquot, increment)`;
   `last_analysis` needs no index, no migration.
7. How tray reads simulated Chromium's position and power, which volume
   chamber is, what becomes of `SimExtractionDevice` (section 6.2).
8. `sim.toml` key names, materials' numbers, step powers and every
   tolerance of section 10.1, by measurement.
9. What reduction does with Ar37 channel that is only noise, at thirty days
   and at a year (sections 6.3 and 7.2).
10. Form of example identifiers (section 7.2).
11. Whether flux window and `elctl flux show` open a level with seeded J
    (section 7.2).
12. Whether one queue may mix rows that use queue's extract device with
    rows that do not, and queue's simulated duration (section 7.4).
13. What example laser needs installed beside tray map and calibration
    (section 7.3), and what install on imported line can run (section 7.4).
14. Settled: `--sim-speed` refused without `--sim` only after install is
    chosen, and not when install is simulated (section 8.2).
15. Settled: list at end of section 5.2.
16. Settled: fit text store reads is `parse_fit_kind(lower(fit))`, error
    type `sd` else SEM, outliers from JSON keys `filter_outliers`,
    `iterations`, `std_devs`; conversion writes those (section 5.2).
17. Settled: tests of adapter live in `tests/experiment/` as second
    executable `pychron_experiment_store_tests`, not in
    `tests/experiment_store/`.
18. Open (owner): every machine registers as client `localhost` (section
    8.4). Peak center after a blank centres on a peak of about 11 fA and
    updates live field table. ngx placeholder table and detector layout
    unconfirmed against a real NGX (section 7.6).
