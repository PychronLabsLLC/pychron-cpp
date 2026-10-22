# Publication Workflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One Publish dialog, opened from the Data window's Publish button and from the Publish window, that makes or picks a release and sends it to one or more destinations (release folder, publish in store, EarthBank), with one result per destination.

**Architecture:** `libs/publish` gains the rules (plan, run order, dependencies, refusals, the EarthBank upload condition) and `release_dataset`, all Qt-free and with no dependency on `libs/earthbank`. `pychron-ui` gains `PublishDialog`, owned by `PublishActions` (which already holds the store bridge and `StoreSource`), and hosts today's EarthBank dialog body as `EarthbankPublishPanel` in one of its tabs. The Data window's button is routed to `PublishActions`; `EarthbankPublishDialog` and `DataWorkspace::open_publish` go.

**Tech Stack:** C++20, Qt 6 Widgets, Qt Test (UI suites), GoogleTest (library suites), TinyORM store (SQLite, PostgreSQL when `PYCHRON_TEST_PG_URL` is set).

**Spec:** `docs/superpowers/specs/2026-10-21-publication-workflow-design.md`

## Global Constraints

- `libs/publish` stays Qt-free and gains no dependency on `libs/earthbank` or `libs/earthbank_qt`.
- Destination run order is fixed: `Folder`, `Store`, `EarthBank`, `Doi`, `Hosted`.
- EarthBank upload only when the release is `published`, `public`, its `embargo_until` is null or `<= now`, and no member analysis or member sample is under a live embargo at `now`; or the same run's `Store` step makes a `draft` release public (`make_public = true`) and the embargo conditions hold. Pre-flight and xlsx: any release.
- `Doi` and `Hosted` are always refused: "Not built yet".
- Nothing a step did is undone when a later step fails or is cancelled.
- No migration; the store query count of `check_plan` is fixed (§Task 2), no per-member query loop.
- EarthBank network work stays on `EarthbankWorker` in real time; every message box and file dialog in new UI code is a `std::function` hook tests replace.
- UI work is checked with the `dev-ui` preset; library work with `dev`. Run `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed` (with `--build-dir build/dev-ui` for UI tasks) before each commit.
- Commits: Conventional Commits with component as scope (`feat(publish)`, `feat(ui)`, `feat(elctl)`, `docs`).

## Review Focus

1. Release with zero members left after embargo, chosen for EarthBank pre-flight: expect a refusal naming the reason, not an empty upload or a crash. Test in Task 2 (`check_plan` refuses EarthBank on a release whose every member is embargoed, any mode).
2. Embargo set between check and run (the dialog was left open): upload must be refused at run time. Test in Task 6 (`UploadRecheckedAtRun`).
3. Publish dialog closed while a run is mid-build: no result reaches a destroyed dialog, no crash. Test in Task 6 (`CloseDuringRunCancelsThenCloses`).
4. "New release" chosen with an empty selection: Run disabled with a reason, no empty release created. Test in Task 5 (`EmptySelectionDisablesRun`).
5. Data window in an install without a database: Publish button disabled with tooltip "Publishing needs a database", not a silent no-op. Test in Task 7 (`PublishButtonNeedsAStore`).

---

### Task 1: `release_dataset`, members at their pins, unreduced

**Files:**
- Create: `libs/publish/include/pychron/publish/earthbank_source.hpp`, `libs/publish/src/earthbank_source.cpp`
- Modify: `libs/publish/src/build.cpp:257-410` (extract shared member listing and reduction)
- Create: `libs/publish/src/members.hpp` (private header, shared by `build.cpp` and `earthbank_source.cpp`)
- Test: `tests/publish/test_earthbank_source.cpp`
- Modify: `docs/superpowers/specs/2026-10-21-publication-workflow-design.md` §5.1 (see Step 6)

**Interfaces:**
- Produces:
  - `namespace pychron::publish::detail { Result<std::vector<Uuid>> release_members(persistence::IStore&, const persistence::ReleaseRow&); Result<processing::DatasetPtr> reduce_loaded(const std::vector<processing::AnalysisPtr>&, const ReleaseOptions&); }` in `members.hpp`.
  - `Result<std::vector<processing::AnalysisPtr>> release_dataset(persistence::IStore& store, processing::StoreSource& source, Uuid release);` in `earthbank_source.hpp`.

- [ ] **Step 1: Write the failing tests** (`TEST_P` over the release fixture, as `BuildTest`)
  - `ReleaseDatasetTest.LoadsEveryMemberOnce`: seed (`seed_release_store`), `create_release` over `analysis1..3`; `release_dataset` returns 3 analyses whose `uuid`s equal `{analysis1, analysis2, analysis3}`.
  - `ReleaseDatasetTest.KeepsPinnedRevisionAfterLaterEdit`: after `create_release`, publish a new intercepts revision of `analysis1` (as `test_build.cpp` edits a member); `release_dataset`'s `analysis1` carries the pinned intercept value, not the new one.
  - `ReleaseDatasetTest.KeepsEmbargoedMembers`: sample embargo on `sample1`; all 3 members still returned (local use; upload is refused elsewhere).
  - `ReleaseDatasetTest.UnknownReleaseIsConfig`: random uuid → error kind `Config`.
- [ ] **Step 2: Run, verify they fail** — `cmake --build --preset dev --target pychron_publish_tests && build/dev/tests/publish/pychron_publish_tests --gtest_filter='*ReleaseDatasetTest*'`; expect compile failure (no header).
- [ ] **Step 3: Extract `release_members` and `reduce_loaded`** from `build_release` (members = `bookmark_heads` subjects whose kind is neither `RefValue` nor `InterpretedAge`; reduction = the `select` → `reduce` pipeline with `constants`, `include_decay_error`). `build_release` calls both; no behaviour change.
- [ ] **Step 4: Implement `release_dataset`**: `store.release` (Config "no release <uuid>" when absent), `release_members`, `source.load_at(member, bookmark)` each, return them in member order, unreduced (the EarthBank panel reduces what it publishes).
- [ ] **Step 5: Run** the filter above and `--gtest_filter='*BuildTest*'`; all pass (build output unchanged: `TwoBuildsGiveEqualRowHashesAndEqualNonParquetBytes` and the fixture-hash test stay green).
- [ ] **Step 6: Amend spec §5.1**: replace the `IStore::release_embargoed_members` paragraph with "Embargoed members found with existing `embargoed_analyses(members, at)` only: it unions project, sample and analysis embargoes; one statement, no new store query." Also §9: drop the `tests/persistence/test_publish.cpp` line.
- [ ] **Step 7: Commit** — `feat(publish): load a release's members at their pins for EarthBank`

### Task 2: Plan, order, dependencies, refusals (`publication.hpp`)

**Files:**
- Create: `libs/publish/include/pychron/publish/publication.hpp`, `libs/publish/src/publication.cpp`
- Test: `tests/publish/test_publication.cpp`

**Interfaces:**
- Consumes: `detail::release_members` (Task 1); `find_release`; `IStore::embargoed_analyses`.
- Produces: the types and functions of spec §5.1 verbatim (`Destination`, `EarthBankMode`, `NewRelease`, `ExistingRelease`, `FolderStep`, `StoreStep`, `EarthBankStep`, `PublicationPlan`, `PlanRefusal`, `StepState`, `StepResult`, `check_plan`, `run_order`, `depends_on_failed`), plus:
  - `Result<std::optional<std::string>> earthbank_upload_refusal(persistence::IStore& store, Uuid release, bool made_public_in_run, UtcTime now);` — nullopt: allowed; else the one-sentence reason; a store error is an error, never "allowed". Used by `check_plan`, the dialog's run-time recheck (Task 6), the dialog's in-doubt resume and `elctl` (Task 3).
  - `std::string_view destination_name(Destination);` — "Release folder", "Publish in store", "EarthBank", "DOI", "Hosted".

- [ ] **Step 1: Write failing tests** (`TEST_P` over the release fixture; `now` passed explicitly)
  - `RunOrder.IsFixedWhateverTheTickOrder`: plan with earthbank, store, folder → `{Folder, Store, EarthBank}`.
  - `CheckPlan.DraftUploadWithoutStoreRefused`, reason contains "published".
  - `CheckPlan.DraftUploadWithStorePublicAllowed`; `CheckPlan.DraftUploadWithStorePrivateRefused`.
  - `CheckPlan.PrivatePublishedUploadRefused`; `CheckPlan.PublicPublishedUploadAllowed`.
  - `CheckPlan.LiveMemberEmbargoRefusesUpload` (sample embargo on `sample1`, and analysis embargo on `analysis2`, two cases); `CheckPlan.LiftedEmbargoAllowsUpload`; `CheckPlan.EndedEmbargoAllowsUpload` (`until` < `now`).
  - `CheckPlan.ReleaseEmbargoInFutureRefusesUpload`.
  - `CheckPlan.PreflightAndXlsxAllowedOnDraft`.
  - `CheckPlan.EveryMemberEmbargoedRefusesEarthBankAnyMode` (Review Focus 1), reason contains "no member".
  - `CheckPlan.StoreOnPublishedRefused` ("already published"); `CheckPlan.StoreWithoutFolderOrDirRefused`.
  - `CheckPlan.DoiAndHostedAlwaysRefused` ("Not built yet").
  - `CheckPlan.NewReleaseWithNoAnalysesRefused`.
  - `DependsOnFailed.FolderFailSkipsStoreAndUploadNotPreflight`; `DependsOnFailed.StoreFailSkipsUploadOnlyWhenStoreTicked`; `DependsOnFailed.NothingFailedRunsAll`.
- [ ] **Step 2: Run, verify fail** — `... --gtest_filter='RunOrder*:CheckPlan*:DependsOnFailed*'`.
- [ ] **Step 3: Implement.** For a `NewRelease`, upload is judged with the release as it would be made (draft, private, members = spec's candidates) and embargoes read for those candidates. Refusals collected, not short-circuited: every refusal of the plan returned.
- [ ] **Step 4: Run, verify pass**, both backends when `PYCHRON_TEST_PG_URL` is set.
- [ ] **Step 5: Commit** — `feat(publish): publication plan, run order and the rules a run is checked by`

### Task 3: `elctl earthbank … --release`

**Files:**
- Modify: `apps/elctl/src/earthbank.cpp` (`Args`, `PreflightArgs`, `UploadArgs` parsing near lines 179, 462, 575; dataset load near 419; usage text lines 54-144)
- Modify: `apps/elctl/src/earthbank_stub.cpp` / `earthbank_no_network.cpp` only if they share the usage text
- Test: `apps/elctl/tests/test_earthbank_commands.cpp`

**Interfaces:**
- Consumes: `release_dataset` (Task 1), `earthbank_upload_refusal` (Task 2), `find_release`.

- [ ] **Step 1: Failing tests**
  - `EarthbankCommands.ExportXlsxFromDraftRelease`: workbook written for the release's samples.
  - `EarthbankCommands.ReleaseWithSelectionIsUsageError`: `--release r --sample s` → exit 2, stderr contains "--release".
  - `EarthbankCommands.UploadRefusedOnPrivateRelease`: exit non-zero, stderr equals the `earthbank_upload_refusal` text; fake EarthBank received nothing.
  - `EarthbankCommands.UploadFromPublicReleaseSends` (fake service).
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement**: `--release <name|uuid>` on the three subcommands; with it, analyses = `release_dataset`; upload checks `earthbank_upload_refusal(store, uuid, false, clock.wall_now())` before anything is sent. Usage lines gain `[--release <name|uuid>]`.
- [ ] **Step 4: Run, verify pass**; update `docs/user/08-elctl-reference.md` earthbank section.
- [ ] **Step 5: Commit** — `feat(elctl): earthbank preflight, export-xlsx and upload from a release`

### Task 4: `EarthbankPublishPanel` out of `EarthbankPublishDialog`

**Files:**
- Create: `apps/pychron-ui/src/earthbank_publish_panel.hpp|cpp` (body of `earthbank_publish_dialog.*`)
- Modify: `apps/pychron-ui/src/earthbank_publish_dialog.hpp|cpp` → thin `QDialog` holding a panel and Run (removed in Task 7)
- Modify: `apps/pychron-ui/CMakeLists.txt` (sources)
- Test: `tests/ui/test_earthbank_publish.cpp` (cases retargeted to the panel)

**Interfaces:**
- Produces (`class EarthbankPublishPanel : public QWidget`):
  - ctor `(std::vector<processing::AnalysisPtr>, EarthbankSetup, SettingsFactory, QWidget* parent = nullptr)` and `void set_analyses(std::vector<processing::AnalysisPtr>)` (fills the per-sample table later, for a release made mid-run).
  - every accessor `EarthbankPublishDialog` has today (`action()`, `profile()`, `table()`, …, `result_pane()`), `ask_confirm`, `show_folder`, `ready()`, `busy()`.
  - `void start(Action action);` (the panel's own `Action` enum) `void cancel();` `void set_read_only(bool);` `void resume(const std::string& resolve);` (the in-doubt answer, run by the host)
  - signal `finished(bool ok, QString message)`; `progress(int done, int total)`; `resume_requested(std::string resolve)` (the in-doubt buttons send nothing themselves: the Publish dialog runs the upload gate, then `resume()`).
  - `QPushButton* run_button()` stays on the dialog wrapper, not the panel.

- [ ] **Step 1: Retarget tests**: every case constructing `EarthbankPublishDialog` for its body constructs `EarthbankPublishPanel` and calls `start(mode)`; add `EarthbankPanel.FinishedSignalCarriesResult` (xlsx export to temp dir → `finished(true, …)` once) and `EarthbankPanel.SetAnalysesFillsTable`.
- [ ] **Step 2: Run, verify fail** — `cmake --build --preset dev-ui --target pychron_ui_test_earthbank_publish && build/dev-ui/tests/ui/pychron_ui_test_earthbank_publish`.
- [ ] **Step 3: Move the body**; the dialog wrapper keeps the Data workspace path working until Task 7. `QSettings` keys under `earthbank/` unchanged.
- [ ] **Step 4: Run, verify pass**; whole `ctest --preset dev-ui -R earthbank`.
- [ ] **Step 5: Commit** — `refactor(ui): EarthBank publish body as a panel`

### Task 5: `PublishDialog`: release section, destinations, tabs, refusals

**Files:**
- Create: `apps/pychron-ui/src/publish_dialog.hpp|cpp`
- Modify: `apps/pychron-ui/src/publish_actions.hpp|cpp` (owns dialog; `open_dialog_new(QStringList)`, `open_dialog_existing(Uuid)`)
- Modify: `apps/pychron-ui/CMakeLists.txt`
- Test: `tests/ui/test_publish_dialog.cpp`

**Interfaces:**
- Consumes: `PublicationPlan`, `check_plan`, `destination_name` (Task 2); `EarthbankPublishPanel` (Task 4); `EntryBridge::run<…>`; `release_dialog` (`AskRelease` hook type of `publish_window.hpp:69`).
- Produces (`class PublishDialog : public QDialog`):
  - ctor `(EntryBridge&, processing::StoreSource&, std::string store_url, EarthbankSetup, QWidget* parent)`.
  - `void open_new(const QStringList& analyses);` `void open_existing(Uuid release);`
  - `std::optional<publish::PublicationPlan> plan() const;`
  - accessors for tests: `new_radio()`, `existing_radio()`, `release_combo()`, `destination(Destination) -> QCheckBox*`, `tabs() -> QTabWidget*`, `earthbank_panel()`, `refusals() -> QLabel*`, `run_button()`, `cancel_button()`, `results() -> QTableWidget*`, `progress()`.
  - hooks: `set_ask_release(AskRelease)`, `set_ask_directory(std::function<std::optional<QString>()>)`.
  - `QSettings` key `publish/destinations` (list of ticked names).

- [ ] **Step 1: Failing tests** (store seeded by `release_seed.hpp`, as `test_publish_window.cpp`)
  - `OpenNewPrefillsSelection`: member count label "3 analyses".
  - `OpenExistingSelectsRelease`.
  - `TabsFollowCheckboxes`: tick EarthBank → tab "EarthBank" present; untick → gone.
  - `DoiAndHostedDisabled` with tooltip "Not built yet".
  - `RefusalDisablesRun`: existing draft, EarthBank Upload, Store unticked → `refusals()` text contains "published", Run disabled.
  - `EmptySelectionDisablesRun` (Review Focus 4).
  - `StoreDirShownOnlyWithoutFolder`.
  - `RemembersDestinations` across two dialogs with the same settings factory.
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement**: `check_plan` on bridge worker, debounced 200 ms, result queued to GUI thread through a `QPointer`.
- [ ] **Step 4: Run, verify pass.**
- [ ] **Step 5: Commit** — `feat(ui): Publish dialog with release and destinations`

### Task 6: `PublishDialog` run: steps, results, cancel, recheck

**Files:**
- Modify: `apps/pychron-ui/src/publish_dialog.hpp|cpp`
- Test: `tests/ui/test_publish_dialog.cpp`

**Interfaces:**
- Consumes: `run_order`, `depends_on_failed`, `earthbank_upload_refusal` (Task 2); `create_release`, `build_release`, `publish_built`; `release_dataset` (Task 1); panel `start`/`finished`/`set_analyses` (Task 4).
- Produces: `void run();` `void cancel();` `bool busy() const;` signal `run_finished(std::vector<publish::StepResult>, bool release_ok)`.

- [ ] **Step 1: Failing tests**
  - `RunFolderAndStorePublishes`: new release, Folder (temp dir) + Store public → results rows Ok, Ok; store row `published`, `public`.
  - `FolderFailSkipsStore`: Folder dir is a non-empty directory → Folder Failed, Store Skipped with "skipped: Release folder failed".
  - `XlsxFromDraftAgainstFakeEarthBank`: existing draft, EarthBank Export xlsx → Ok, workbook present.
  - `UploadRefusedOnPrivate` (refusal before run, nothing sent to fake service).
  - `UploadRecheckedAtRun` (Review Focus 2): check passes, embargo on `sample1` added through the store before Run → EarthBank Failed with refusal text, fake service received nothing.
  - `UploadConfirmNoIsCancelled`: `ask_confirm` returns false → EarthBank Cancelled.
  - `CancelBetweenSteps`: cancel during Folder → Store Cancelled; directory left.
  - `CloseDuringRunCancelsThenCloses` (Review Focus 3): close asked yes → dialog gone after the step boundary; no crash under ASan.
  - `NewReleaseFillsEarthBankPanelMidRun`: panel table read-only, filled after creation.
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement**: steps sequenced on GUI thread by results; store steps on bridge worker, `release_dataset` there as well (it reads the store), EarthBank via panel; `earthbank_upload_refusal` rechecked with `CoreBridge`/clock wall time just before the EarthBank step.
- [ ] **Step 4: Run, verify pass**; run this suite under `-DPYCHRON_SANITIZE=thread` once (new cross-thread hand-offs).
- [ ] **Step 5: Commit** — `feat(ui): run a publication plan with a result per destination`

### Task 7: Entry points, removal, docs

**Files:**
- Modify: `apps/pychron-ui/src/data_browser_window.cpp:228-242` (tooltip "Publish…"; disabled without handler, tooltip "Publishing needs a database")
- Modify: `apps/pychron-ui/src/data_workspace.hpp|cpp` (drop `open_publish`, `publish_dialog`, `publish_`, `earthbank_` if unused; add `set_publish_handler(std::function<void(const QStringList&)>)`)
- Modify: `apps/pychron-ui/src/main.cpp:265,650` (handler → `PublishActions::open_dialog_new`)
- Modify: `apps/pychron-ui/src/publish_window.hpp|cpp` (Publish… → `open_dialog_existing`; drop `AskPublish`)
- Delete: `apps/pychron-ui/src/earthbank_publish_dialog.hpp|cpp`
- Modify: `tests/ui/test_data_windows.cpp`, `tests/ui/test_publish_window.cpp`, `tests/ui/test_earthbank_publish.cpp` (workspace cases at lines ~700-770 removed or moved to `test_publish_dialog.cpp`)
- Modify: `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md` §6.3, §11, §13 phase 8; `docs/earthbank.md`; `docs/user/06-data-analysis.md` ("Publishing a release")

- [ ] **Step 1: Failing tests**: `DataWindows.PublishButtonOpensPublishDialog` (handler called with selection); `DataWindows.PublishButtonNeedsAStore` (Review Focus 5); `PublishWindow.PublishOpensDialogOnSelectedRelease`.
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement** wiring and removals; docs as spec §8.
- [ ] **Step 4: Full checks**: `cmake --build --preset dev-ui && ctest --preset dev-ui`; `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --build-dir build/dev-ui`; then `cmake --build --preset dev && ctest --preset dev`.
- [ ] **Step 5: Commit** — `feat(ui)!: one Publish entry point for releases and EarthBank` with footer `BREAKING CHANGE: the Data window's Publish button opens the Publish dialog; EarthBank publishing now starts from a release.`

## Open questions

None.
