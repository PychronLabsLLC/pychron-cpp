# Publication workflow: one Publish dialog, many destinations

Date 2026-10-21. Builds on `2026-10-18-data-publishing-and-hosted-sharing-design.md`
(here "publishing spec"): replaces its §11 UI part and phase 8; changes §6.3.
EarthBank side: `2026-10-07-earthbank-design.md`.

## 1. Problem

Two entry points both called Publish, doing different things:

| Entry | Path | Does |
|---|---|---|
| View > Publish… | `publish_actions.cpp:24` → `PublishWindow` | release lifecycle: new, build, verify, publish private/public, withdraw, embargoes; local directory only (`libs/publish`) |
| Data window toolbar "Publish" | `data_browser_window.cpp:234` → `DataWorkspace::open_publish` → `EarthbankPublishDialog` | pre-flight, xlsx, upload to EarthBank from live selection, no release (`libs/earthbank`) |

Data button bypasses release: no pins, no embargo check, no citable unit;
contradicts publishing spec §11 (EarthBank downstream of release). Name clash
confuses user.

## 2. Decisions

- D1. Release is hub. Every publication starts from a release (new from
  selection, or existing). EarthBank and later destinations are targets of
  that release.
- D2. One Publish dialog (`PublishDialog`). Data window button and
  `PublishWindow`'s Publish… both open it. Release section, destination
  checkboxes, one settings tab per ticked destination, run with one result
  per destination.
- D3. EarthBank upload only when release, at upload time, is `published`,
  `public`, and has no member under live embargo (same condition as
  `release_visible`, publishing spec §4.5). EarthBank pre-flight and xlsx
  allowed for any release, draft included: nothing leaves the machine but
  vocabulary lookups.
- D4. No destination plugin interface. Fixed, ordered list; later
  destinations extend enum and list.

## 3. Terms

- Destination: one target a run sends release to. `Folder`, `Store`,
  `EarthBank`; listed not built: `Doi`, `Hosted`.
- Plan: release choice + ticked destinations + each one's settings.
- Run: one execution of one plan.

## 4. Destinations, order, dependencies

Fixed order, whatever order ticked:

| # | Destination | Does | Needs |
|---|---|---|---|
| 1 | `Folder` | `build_release` into directory (options: out dir, with figures, private build) | release exists |
| 2 | `Store` | draft → published via `publish_built(store, actor, release, dir, make_public)`; option private / public | built directory of this release: from step 1 of same run, else directory operator names (refused by `publish_built` when not matching) |
| 3 | `EarthBank` | one of pre-flight, export xlsx, upload; dataset = `release_dataset` (§5.2) | upload: D3 condition, judged after step 2 would have run |
| 4 | `Doi` | not built (publishing spec §10.1, phase 7) | shown disabled, tooltip "Not built yet" |
| 5 | `Hosted` | not built (publishing spec §9.5, phase 6) | shown disabled, tooltip "Not built yet" |

Rules:

- `Store` ticked for a release already `published`: refused at check
  ("already published"); visibility change stays "Make public" in
  `PublishWindow`.
- `Store` ticked without `Folder` and no directory named: refused at check.
- A step that fails marks every later step that depends on it `Skipped`
  ("skipped: <destination> failed"). EarthBank upload depends on `Store` only
  when `Store` is ticked in same run (it is what makes release public);
  EarthBank pre-flight and xlsx depend on nothing but release existing.
  Independent steps still run.
- New release made in run (release section "new"): made first, before step 1.
  When creation fails, nothing else runs.
- Cancel between steps; inside EarthBank between samples/records as today.
  Cancelled step and later ones: `Cancelled`. Nothing undone: a built
  directory stays, a published release stays published, uploaded records
  stay (EarthBank journal rules unchanged).
- At most one run at a time per dialog.
- EarthBank (any mode) refused when no member of release free of embargo at
  `now`: nothing to send.
- EarthBank upload refused for `withdrawn` release.
- New release: refused when it has no analyses, or when an analysis text is
  not a uuid.

## 5. Library: `libs/publish`

Qt-free; no dependency on `libs/earthbank` (EarthBank step's settings are
opaque to the library; UI runs it, §6.3). `elctl` may use the same later.

### 5.1 `publication.hpp`

```cpp
namespace pychron::publish {

enum class Destination { Folder, Store, EarthBank, Doi, Hosted };  // run order
enum class EarthBankMode { Preflight, ExportXlsx, Upload };

struct NewRelease { ReleaseSpec spec; };            // as create_release takes
struct ExistingRelease { std::string name_or_uuid; };  // find_release text

struct FolderStep { BuildOptions build; };
struct StoreStep { bool make_public = false; std::optional<std::filesystem::path> dir; };  // dir: when Folder not ticked
struct EarthBankStep { EarthBankMode mode = EarthBankMode::Preflight; };

struct PublicationPlan {
  std::variant<NewRelease, ExistingRelease> release;
  std::optional<FolderStep> folder;
  std::optional<StoreStep> store;
  std::optional<EarthBankStep> earthbank;
  bool doi = false, hosted = false;  // always refused while not built
};

struct PlanRefusal { Destination destination; std::string reason; };  // one sentence

// Every rule of section 4 and D3 that can be judged before anything runs,
// for the release as the plan would leave it. Empty: plan may run.
// Error (not refusal) only when the store cannot be read.
Result<std::vector<PlanRefusal>> check_plan(persistence::IStore& store, const PublicationPlan& plan, UtcTime now);

enum class StepState { Pending, Running, Ok, Failed, Skipped, Cancelled };
struct StepResult { Destination destination; StepState state = StepState::Pending; std::string message; };

// D3 for EarthBank upload of `release` at `now`: nullopt = may go, else
// one-sentence reason. `made_public_in_run`: same run publishes release
// public (Store ticked, make_public) before upload. Store error = error,
// never "may go".
Result<std::optional<std::string>> earthbank_upload_refusal(persistence::IStore& store, Uuid release,
                                                            bool made_public_in_run, UtcTime now);

// "Release folder", "Publish in store", "EarthBank", "DOI", "Hosted".
std::string_view destination_name(Destination destination);

// Order of section 4 for the destinations ticked.
std::vector<Destination> run_order(const PublicationPlan& plan);
// Given the results so far, whether `next` runs or is skipped (section 4 rules).
bool depends_on_failed(const PublicationPlan& plan, Destination next, const std::vector<StepResult>& so_far);

}  // namespace pychron::publish
```

- `check_plan` for EarthBank upload: release not `withdrawn`; must be
  `published` and `public` now, or `Store` ticked with `make_public = true`
  and release `draft`; and no member under live embargo at `now`, and
  release's own `embargo_until` null or `<= now`. Check recomputed at run
  time, just before EarthBank step (§6.3), with `now` then: a run that took
  long or an embargo set meanwhile is caught.
- One gate: `earthbank_upload_refusal` holds upload rule. Used by
  `check_plan`, dialog's recheck before EarthBank step, in-doubt resume
  (§6.2) and `elctl` (§7). No second copy of rule.
- Embargoed members found with existing `embargoed_analyses(members, at)`
  only: it unions project, sample and analysis embargoes. One statement, no
  new store query. No migration.
- Running Folder and Store stays in existing functions (`build_release`,
  `publish_built`); `publication.hpp` adds no second path to them.

### 5.2 `earthbank_source.hpp`

`release_dataset(persistence::IStore&, processing::StoreSource&, Uuid release)
-> Result<std::vector<processing::AnalysisPtr>>`: release's members loaded at
their pins (`source.load_at(member, release.bookmark)`), unreduced: the
EarthBank panel reduces what it publishes, as it does today. Name kept from publishing spec §11; returns what
`EarthbankPublishDialog` takes today (vector of `AnalysisPtr`), not a
`Dataset`.

Mapping change from publishing spec §11 kept: data point reference = release
DOI when set (none can be set until phase 7, so no hash change yet);
`Samples.igsn` as today.

## 6. UI: `apps/pychron-ui`

### 6.1 `PublishDialog` (`publish_dialog.hpp|cpp`)

Top-level `QDialog`, non-modal, one per window that opens it. Not `QWizard`.

- Release section:
  - "New release from selection": member count, count under embargo now,
    button "Details…" opens existing `release_dialog` (name, title,
    description, authors, licence, publisher, tags, embargo, supersedes) to
    fill `ReleaseSpec`. Default name required before run.
  - "Existing release": combo of `draft` and `published` releases (name@vN,
    state, visibility).
  - Opened from Data window: "new" selected, selection =
    `export_uuids()` (as Export and New release… today). Opened from
    `PublishWindow`: "existing" selected, release = row selected there.
- Destinations: checkboxes Release folder, Publish in store, EarthBank, DOI
  (disabled), Hosted (disabled). Default: none ticked; last choice
  remembered in `QSettings` under `publish/`.
- Tabs, one per ticked destination:
  - Folder: directory, With figures, Private build (fields of today's Build
    dialog).
  - Store: Private / Public; directory field shown only when Folder not
    ticked.
  - EarthBank: today's `EarthbankPublishDialog` body (profile, password,
    package, mode, folder, per-sample table, checkboxes, in-doubt
    there/not-there), moved into widget `EarthbankPublishPanel` (§6.2).
    Mode combo: Pre-flight, Export xlsx, Upload. Panel's own Run button
    gone; dialog's Run drives it. In-doubt there/not-there buttons stay;
    each requests resume that dialog runs through upload gate (§6.2).
- Refusals: `check_plan` run on every change (store read on bridge worker,
  debounced); refusals listed under destinations, Run disabled while any.
- Run: steps in `run_order`; results table, one row per ticked destination
  (destination, state, message). New release with no destination ticked:
  run only creates release, one row "Release". Signal
  `run_finished(steps, release_ok)` once per run: `steps` one per ticked
  destination in run order (empty when none ticked); `release_ok` false only
  when new release could not be made (every step then `Skipped`). progress bar per running step (build
  stages via `IProgress`, EarthBank via worker's progress). Cancel button.
  EarthBank upload confirmation (`ask_confirm`) unchanged: asked when step
  starts, false → step `Cancelled`.
- Hooks as `PublishWindow`: every message box and file dialog a
  `std::function` tests replace.

### 6.2 `EarthbankPublishPanel`

`EarthbankPublishDialog` split: body becomes `QWidget` `EarthbankPublishPanel`
(same accessors for tests, same `EarthbankWorker`, same `QSettings` under
`earthbank/`); `start(mode)` and signal `finished(ok, message)` for dialog
to drive. `EarthbankPublishDialog` removed: no other user once Data button
moves. Panel filled from `release_dataset` after release is chosen (new
release: after creation in run, so per-sample table fills mid-run; choices
remembered per sample name as today let it fill without operator).

In-doubt answer (record whose answer never came): panel's there/not-there
buttons send nothing; they emit `resume_requested(resolve)` (`"written:<id>"`
or `"not-written"`). Panel method `resume(resolve)` finishes the agreed
upload without asking again; `finished` follows once, as for `start`.
Dialog connects `resume_requested` and runs it as run of its own: busy held,
release section and destinations locked, one result row "EarthBank",
`earthbank_upload_refusal(store, release, false, now)` on bridge worker
first. Refusal → `Failed` with reason, nothing sent; allowed →
`panel.resume(resolve)`, `finished` mapped as for EarthBank step;
`run_finished` once. Release may have gone private, withdrawn or embargoed
since the run that left record in doubt.

Rule: new release + EarthBank ticked → per-sample choices taken from
remembered values or rule defaults; table shown read-only during run. To
review per-sample choices first, operator creates release (run with no
destination ticked but release "new", or New release… in window), then
reopens on it.

### 6.3 Threads and lifetime

- Store work (create, check, build, publish) on `EntryBridge` worker of
  `PublishActions` (role `publisher`), as `PublishWindow` today.
- `release_dataset` on the `EntryBridge` worker: it reads the store, whose connection that worker owns.
- EarthBank on `EarthbankWorker` thread, as today, real time (network;
  unchanged).
- Results queued to GUI thread; none reaches a closed dialog (`QPointer`,
  as `DataWorkspace::publish_`). Closing dialog during run: asks; yes
  cancels at next boundary, then closes.
- Quitting during build waits for it (accepted limit, publishing spec §6.3).

### 6.4 Entry points

- Data window toolbar "Publish": tooltip "Publish…", opens `PublishDialog`
  (new from selection). `DataWorkspace::open_publish` opens it in place of
  EarthBank dialog. Needs a store: without one (no database configured),
  button disabled with tooltip "Publishing needs a database".
- `PublishWindow` "Publish…": opens `PublishDialog` on selected release.
  Its old private/public confirmation goes (Store tab has it). Build…, New
  release…, Verify…, Withdraw…, Make public, Embargoes dock unchanged.
- View > Publish… unchanged: opens `PublishWindow`.

## 7. `elctl`

- `elctl earthbank preflight|export-xlsx|upload --release <name|uuid>`
  (publishing spec §11): dataset from `release_dataset`; upload refused by
  D3 condition (`earthbank_upload_refusal`, the one gate of §5.1). Selection
  flags with `--release`: usage error. Selection flags alone keep working
  (command line keeps EarthBank without release).
- No `elctl publish run`. Out of scope.

## 8. Docs

- Publishing spec: §6.3 Publish… row changed to this dialog; §11 UI part and
  phase 8 marked "superseded by 2026-10-21-publication-workflow-design.md".
- `docs/earthbank.md`: UI path is now Publish dialog, EarthBank tab, from a
  release; D3 rule.
- User guide page for publishing: one section "Publishing a release".

## 9. Tests

- `tests/publish/test_publication.cpp` (seeded store, `release_seed.hpp`):
  `run_order`; every refusal of §4 and D3 (draft upload without Store,
  Store public in same run allowed, private release refused, live member
  embargo refused, release embargo in future refused, lifted embargo
  allowed, Store on published release refused, Store without dir refused,
  Doi/Hosted always refused, withdrawn release upload refused, store error
  in gate is error not "may go"); `depends_on_failed` (Folder fail skips Store
  and upload, not pre-flight/xlsx).
- `tests/publish/test_earthbank_source.cpp`: `release_dataset` returns
  pinned revisions after a later edit of a member.
- `tests/ui/test_publish_dialog.cpp`: Data button opens dialog "new" with
  selection; window opens it "existing" on its row; refusal disables Run;
  run Folder+Store makes published release; Folder fails → Store skipped;
  EarthBank xlsx on draft against fake EarthBank (`earthbank_fixture.hpp`);
  upload refused on private release; in-doubt resume refused when release no
  longer public, and allowed resume runs as dialog run (busy, one row).
- `tests/ui/test_earthbank_publish.cpp` → against `EarthbankPublishPanel`;
  cases kept.
- `tests/ui/test_data_windows.cpp`: Publish button opens `PublishDialog`.
- `apps/elctl/tests/test_earthbank_commands.cpp`: `--release`, its usage
  error, upload refused on private release.

## 10. Out of scope

DOI, IGSN, hosted upload, viewer (publishing spec phases 3, 6, 7); several
releases in one run; EarthBank upload from a selection in UI without a
release; `elctl publish run`; changing release creation rules or build
format; undo of any step.

## Open questions

None.
