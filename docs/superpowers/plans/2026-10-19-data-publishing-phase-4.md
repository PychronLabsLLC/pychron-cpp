# Data publishing phase 4: the Publish window — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In pychron-ui, View > Publish opens a window that lists a store's releases and creates, builds, verifies, publishes, withdraws and makes public a release, with a dock that lists, sets and lifts embargoes; everything `elctl publish` and `elctl embargo` do, without a terminal.

**Architecture:** A `PublishActions` owner (as `FitActions`) contributes View > Publish… and owns a store bridge opened as a *publisher* client, a `StoreSource` for builds, and the `PublishWindow`. All store work runs on the bridge's worker thread (a store connection belongs to the thread that opened it); results and build progress come back to the GUI thread queued. The checks `elctl publish publish` and `verify --db` make move into `libs/publish`, so the command and the window share them. Dialogs are reached through `std::function` hooks that tests replace.

**Tech Stack:** C++20, Qt 6 Widgets, QtTest (tests/ui), GoogleTest (tests/publish). No new dependency.

**Spec:** `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md` (sections 4.4, 4.5, 6.2, 6.3, 13 phase 4).

## Global Constraints

- Qt stays out of `libs/`; `libs/publish` gains no Qt.
- A store connection belongs to the thread that opened it: every `IStore` call runs on the bridge's worker.
- Writes are made as a client of role `publisher` (as `elctl publish`), software `pychron-ui/<PYCHRON_VERSION>`; a release's `software_version` is `pychron-cpp/<PYCHRON_VERSION>` and `reduction_version` is `reduction::kReductionVersion`, as `elctl publish create` writes them.
- Licence default `kDefaultLicence` (`release.hpp`, `CC-BY-4.0`); visibility stays private unless asked (R1).
- Every file a build writes goes through `mark_as_user_file` (the build does this already).
- In phase 4 the window has no DOI…, IGSN… (phase 7) or Publish release to EarthBank… (phase 8) buttons, and no with-viewer option (phase 3 not done).
- Dialogs in tests are never driven modally: each is a `std::function` hook with a dialog default.
- Commits: Conventional Commits, scope `publish`, `elctl`, `ui` or `docs`.

## Review Focus

- A window closed while a build runs on the worker must not be called back (results go through the bridge with a `QPointer` guard), and the build's `IProgress` must not touch a destroyed window (Task 5 `ClosingDuringABuildIsSafe`).
- Publishing a directory built from another release, from a private build as public, or after a file was changed must be refused with a message that says which (Task 1 tests, Task 6 `PublishRefusesAChangedDirectory`).
- A store that cannot be opened as publisher (read-only file, wrong URL) must say so in the window once, not crash or block the GUI (Task 3 `AStoreThatCannotBeOpenedIsSaid`).
- New release with nothing selected in the Data browser must say so, not create an empty release (Task 4 `NothingSelectedIsSaid`).
- Embargo by a name that matches nothing must be refused with the name in the message (Task 7 `AnUnknownScopeIsSaid`).

---

## File structure

| File | Responsibility |
|---|---|
| `libs/publish/include/pychron/publish/workflow.hpp`, `src/workflow.cpp` | `publish_built(...)`: the checks before draft -> published; `verify_against_store(...)`: rebuild to a scratch directory and compare |
| `apps/elctl/src/publish.cpp` | uses `workflow.hpp` |
| `tests/publish/release_seed.hpp` | the release fixture's store seed as a free function (no gtest), for gtest and QtTest |
| `apps/pychron-ui/src/entry_bridge.*` | `Options` gains `role` and `software` |
| `apps/pychron-ui/src/publish_actions.*` | View > Publish…; owns bridge, source, window; selection hook |
| `apps/pychron-ui/src/publish_window.*` | releases table, buttons, status line, progress bar, embargo dock |
| `apps/pychron-ui/src/release_dialog.*` | New release dialog |
| `apps/pychron-ui/src/data_browser_window.*` | `export_uuids()`: the selection Export uses, public |
| `apps/pychron-ui/src/main.cpp` | makes `PublishActions` in both installs |
| `apps/pychron-ui/CMakeLists.txt` | the new sources and `pychron::publish` in the store block |
| `tests/ui/test_publish_window.cpp`, `tests/ui/CMakeLists.txt` | the window's tests |
| `docs/user/…` (the data browser chapter), spec | docs |

---

### Task 1: The publish and verify checks in `libs/publish`

**Files:**
- Create: `libs/publish/include/pychron/publish/workflow.hpp`, `libs/publish/src/workflow.cpp`
- Modify: `apps/elctl/src/publish.cpp` (`publish`, `verify` use them)
- Test: `tests/publish/test_workflow.cpp`

**Interfaces:**
- Produces:

```cpp
// The draft -> published step from a built directory (spec 4.4, 6.2): reads
// <dir>/manifest.json; refuses a private build made public, a directory that
// does not match its manifest, and a manifest whose release, bookmark or
// pinned heads are not `release`'s; then publish_release with the manifest's
// SHA-256. Config with a sentence for each refusal.
Result<persistence::ReleaseRow> publish_built(persistence::IStore& store, const persistence::Actor& actor,
                                              const std::string& release, const std::filesystem::path& dir,
                                              bool make_public);
// verify_files, then a rebuild of the manifest's release (same private flag,
// figures when the directory has them) into a scratch directory under the
// system temp, removed after, compared with verify_rebuild. The two reports
// are returned apart.
struct StoreVerify { VerifyReport files, rebuild; };
Result<StoreVerify> verify_against_store(persistence::IStore& store, processing::StoreSource& source,
                                         const std::filesystem::path& dir, const std::string& store_url);
```

- [ ] **Step 1: Write the failing tests** (`ReleaseFixture`-based, both engines, as `test_build.cpp`): `Workflow.PublishBuiltPublishesAndRecordsTheManifestSha` (state published, `manifest_sha256` = sha of the bytes); `Workflow.APrivateBuildCannotBePublic` (message contains "private build"); `Workflow.ADirectoryOfAnotherReleaseIsRefused` (build r1 and r2, publish r2 from r1's directory: message contains "not this release"); `Workflow.AChangedFileIsRefused` (message contains "does not match its manifest"); `Workflow.VerifyAgainstStoreAgreesAndNamesAChangedTable` (ok; then change a cell of `tables/release.csv`: `files.changed` names it).
- [ ] **Step 2: Run** `pychron_publish_tests --gtest_filter='Engines/Workflow*'`: compile failure.
- [ ] **Step 3: Implement**, moving the code of `elctl publish publish` and of `verify --db`'s rebuild; `elctl` calls them, its messages and exit codes unchanged (`ElctlPublishTest.*` stay green).
- [ ] **Step 4: Run** `--gtest_filter='Engines/Workflow*:Engines/ElctlPublishTest.*'` with `PYCHRON_TEST_PG_URL` set: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `refactor(publish): the publish and verify checks of elctl in libs/publish`.

### Task 2: The release seed as a free function

**Files:**
- Create: `tests/publish/release_seed.hpp`
- Modify: `tests/publish/release_fixture.hpp` (uses it)

**Interfaces:**
- Produces: `namespace pychron::publish::testing { struct ReleaseSeed { Lab lab; Uuid project, project2, material, sample1, sample2, sample3, position1, position2, production, analysis1, analysis2, analysis3; }; Result<ReleaseSeed> seed_release_store(persistence::IStore& store); }` — what `ReleaseFixture::SetUp` makes today, failures as `Result` instead of `EXPECT_*`.

- [ ] **Step 1:** Move the seeding into `seed_release_store`; `ReleaseFixture::SetUp` does `ASSERT_TRUE(seeded)` and copies the ids.
- [ ] **Step 2: Run** the whole `pychron_publish_tests`: unchanged, pass.
- [ ] **Step 3: Commit** `test(publish): the release fixture's seed is a function a Qt test can call`.

### Task 3: The bridge as publisher, the window, View > Publish…

**Files:**
- Modify: `apps/pychron-ui/src/entry_bridge.hpp/.cpp` (`Options::role = "reduction"`, `Options::software = "pychron-ui"`), `apps/pychron-ui/CMakeLists.txt` (store block: `publish_actions`, `publish_window`, `release_dialog`, link `pychron::publish`), `tests/ui/CMakeLists.txt` (`test_publish*` sees `tests/` and `tests/publish`, links `pychron::publish`, `TinyOrm`, `Qt6::Sql` as the publish tests do)
- Create: `apps/pychron-ui/src/publish_actions.hpp/.cpp`, `apps/pychron-ui/src/publish_window.hpp/.cpp`
- Test: `tests/ui/test_publish_window.cpp`

**Interfaces:**
- Produces:

```cpp
class PublishActions : public QObject {  // View > Publish…, for as long as `owner` lives
 public:
  // `selection` gives the analyses a new release is made of (the Data
  // browser's, as Export takes them); may be empty.
  PublishActions(QWidget* owner, std::string store_url, std::function<QStringList()> selection = {});
  PublishWindow* window();  // made on first use; nullptr when the store cannot be opened
  QAction* action() const noexcept;  // "Publish…"
  void set_report_error(std::function<void(const QString&)> report);
};
class PublishWindow : public QMainWindow {
 public:
  PublishWindow(EntryBridge& bridge, processing::StoreSource& source, std::string store_url,
                std::function<QStringList()> selection, QWidget* parent = nullptr);
  void refresh();                       // reloads releases and embargoes
  QTableView* releases() const;         // columns: Name, Version, State, Visibility, Members, DOI, Created
  QLabel* status() const;
  QProgressBar* progress() const;
  bool busy() const;                    // a job is on the worker
  QString selected_release() const;     // uuid of the current row, empty when none
  void select_release(const QString& uuid);
  // hooks (Tasks 4-7)
};
```

Member order of `PublishActions`: the `StoreSource` first, the bridge second, the window last, so the window goes first, then the bridge joins its worker, then the source goes (fit_actions.hpp's rule).

- [ ] **Step 1: Write the failing tests**: `TestPublishWindow::listsTheStoresReleases` (seed with `seed_release_store`, create a release with `create_release`, open the window: one row, name `r`, state `draft`, visibility `private`, members `3`); `TestPublishWindow::theViewMenuOpensIt` (`PublishActions` on a bare `QMainWindow`: its action's text is "Publish…" and triggering it shows the window); `TestPublishWindow::aStoreThatCannotBeOpenedIsSaid` (URL of a missing directory: `window()` is nullptr and `report_error` was called once with the URL in the text).
- [ ] **Step 2: Run**, expect compile failure (`cmake --build --preset dev-ui --target pychron_ui_test_publish_window`).
- [ ] **Step 3: Implement.** The bridge is `EntryBridge::open({url, "", "", "publisher", "pychron-ui/" PYCHRON_VERSION})`; refresh runs `store.releases({})` and `list_embargoes(store, true, UtcTime::now())` on the worker and fills both tables on return.
- [ ] **Step 4: Run** `QT_QPA_PLATFORM=offscreen build/dev-ui/tests/ui/pychron_ui_test_publish_window`: pass; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(ui): a Publish window listing the store's releases (View > Publish…)`.

### Task 4: New release

**Files:**
- Create: `apps/pychron-ui/src/release_dialog.hpp/.cpp`
- Modify: `publish_window.*`
- Test: `tests/ui/test_publish_window.cpp`

**Interfaces:**
- Produces: `struct ReleaseForm { publish::ReleaseSpec spec; };` and on `PublishWindow`: `using AskRelease = std::function<std::optional<publish::ReleaseSpec>(const QStringList& analyses, std::size_t embargoed)>; void set_ask_release(AskRelease);` (default: `ReleaseDialog`). `void new_release();` (the New release… button).

The dialog: name, title, description, authors table (name, ORCID, affiliation; at least one row), licence combo (`CC-BY-4.0`, `CC-BY-SA-4.0`, `CC0-1.0`, editable; preselected `kDefaultLicence`), publisher, tags (comma list), embargo date (optional), supersedes (combo of the store's releases, none by default), and a line "N analyses, M under embargo". The spec's `analyses` are the selection, `options_json` the default `ReleaseOptions`, versions as Global Constraints. OK runs `create_release` on the worker, selects the new row and says "release <name> v<N>: <members> members, <samples> samples".

- [ ] **Step 1: Write the failing tests**: `newReleaseFromTheSelection` (selection hook gives the seed's three analyses; the ask hook receives them and an embargoed count of 0 and returns a spec named `r`: one row appears, selected, status names `r v1`); `nothingSelectedIsSaid` (selection empty: the ask hook is not called, status says "Select analyses in the Data browser first"); `embargoedCountIsShown` (embargo sample FC-1, then New release: the hook receives embargoed 2); `aRefusedReleaseIsSaid` (the hook returns a spec with an empty title: status has the store's message, no row added).
- [ ] **Step 2–5:** run (fail), implement, run (pass), quality_check, commit `feat(ui): New release from the Data browser's selection`.

### Task 5: Build with progress

**Files:** `publish_window.*`; test file.

**Interfaces:**
- Produces: `struct BuildRequest { std::filesystem::path out; bool figures = false; bool private_build = false; }; using AskBuild = std::function<std::optional<BuildRequest>(const QString& release_name)>; void set_ask_build(AskBuild); void build();` Default dialog: directory chooser, With figures, Private build.

The job runs `build_release` on the worker with an `IProgress` that queues each stage to the window (`progress()` steps through the nine stages of `build.hpp`; label shows the stage). On return: status "built <dir>" plus " (no Parquet: built without Apache Arrow)" when `!parquet_written`, warnings appended to the status's tooltip; the window remembers the directory per release for Verify and Publish.

- [ ] **Step 1: Write the failing tests**: `buildWritesTheDirectoryAndSaysWhere` (the ask hook returns a temp dir, figures true: `manifest.json` and `figures/FC-1/ideogram.svg` exist; status contains the directory; progress reached its maximum); `buildingAReleaseNotSelectedIsSaid` (no row selected: "Select a release first"); `closingDuringABuildIsSafe` (start a build, delete the window at once, wait until the bridge's worker is idle (`run_sync` of a no-op): no crash under ASan; the directory is complete).
- [ ] **Step 2–5:** run, implement, run, quality_check, commit `feat(ui): Build a release from the Publish window, with progress per stage`.

### Task 6: Verify, Publish, Withdraw, Make public

**Files:** `publish_window.*`; test file.

**Interfaces:**
- Consumes: `publish_built`, `verify_against_store` (Task 1).
- Produces on `PublishWindow`: `using AskDirectory = std::function<QString(const QString& suggested)>; void set_ask_directory(AskDirectory);` (default `QFileDialog::getExistingDirectory`, suggested = the last build of the release), `using AskText = std::function<std::optional<QString>(const QString& question)>; void set_ask_text(AskText);` (withdraw reason), `using Confirm = std::function<bool(const QString& question)>; void set_confirm(Confirm);` (publish public or private, make public), and slots `verify()`, `publish()`, `withdraw()`, `make_public()`. Buttons are enabled by the row's state: Build and Publish for a draft, Withdraw for a published, Make public for a published private.

Verify: `verify_against_store`; status "verified: files and rebuild agree" or each difference listed, one per line, in a message box (hook `set_report` for tests: `std::function<void(const QString&)>`). Publish: confirm "Publish <name> v<N> as private?" with a Public check (the confirm hook returns `std::optional<bool>` make_public: nullopt cancels — use `using AskPublish = std::function<std::optional<bool>(const QString& release)>`). Withdraw: the reason is required.

- [ ] **Step 1: Write the failing tests**: `verifyAgreesWithTheStore`; `verifyNamesAChangedFile` (append to `README.md`: report contains "README.md"); `publishMakesItPublished` (state column `published`, visibility `private`); `publishRefusesAChangedDirectory` (status contains "does not match its manifest", state stays draft); `withdrawNeedsAReason` (text hook returns empty: nothing changes; returns "error in J": state `withdrawn`); `makePublicChangesVisibility`; `buttonsFollowTheState` (draft: Publish enabled, Withdraw disabled; published: the reverse).
- [ ] **Step 2–5:** run, implement, run, quality_check, commit `feat(ui): Verify, Publish, Withdraw and Make public in the Publish window`.

### Task 7: The embargo dock

**Files:** `publish_window.*`; test file.

**Interfaces:**
- Produces on `PublishWindow`: `QTableView* embargoes() const;` (columns: Scope, Name, Until, Reason, Set), a dock titled Embargoes, object name `embargoes`, on the right; `struct EmbargoForm { persistence::EmbargoScopeType type; QString name; std::optional<QDate> until; QString reason; }; using AskEmbargo = std::function<std::optional<EmbargoForm>()>; void set_ask_embargo(AskEmbargo); void set_embargo(); void lift_embargo();` (lifts the selected row). Default dialog: scope combo (project, sample, analysis), name field with the store's projects or samples as completions, optional date, reason.

- [ ] **Step 1: Write the failing tests**: `setAndLiftAnEmbargo` (sample FC-1 until 2030-01-01: a row "sample, FC-1, 2030-01-01"; lift it: no row); `anUnknownScopeIsSaid` (sample "NOPE": status contains "NOPE", no row); `aNewReleaseCountsTheEmbargo` (covered by Task 4's `embargoedCountIsShown`, set here through the dock instead of the store).
- [ ] **Step 2–5:** run, implement, run, quality_check, commit `feat(ui): an embargo dock in the Publish window`.

### Task 8: Wiring in both installs

**Files:**
- Modify: `apps/pychron-ui/src/data_browser_window.hpp/.cpp` (`QStringList export_uuids() const;` the selected rows, else every shown row; `request_export` uses it), `apps/pychron-ui/src/main.cpp` (a `PublishActions` beside `FitActions` in `run_data_reduction` and in the instrument install, its selection from the data workspace's existing browser's `export_uuids()`, empty when there is no browser)
- Test: `tests/ui/test_data_windows.cpp` (`exportUuidsAreTheSelectionElseTheShownRows`)

- [ ] **Step 1: Write the failing test**; **Step 2:** run (fail); **Step 3:** implement; **Step 4:** run the data window and publish window suites: pass; build `pychron-ui` and open View > Publish… on a seeded store by hand (`pychron-ui --sim` with the example install's store, or the data install), screenshot; quality_check exit 0.
- [ ] **Step 5: Commit** `feat(ui): View > Publish… in the data and instrument installs`.

### Task 9: Docs and spec

**Files:** the data browser chapter of `docs/user/` (a section "Publishing a release"), `docs/release_format.md` (one line: the window does what `elctl publish` does), the spec.

Spec amendments (compressed, as the spec is): 6.3 as built — a window (`PublishWindow`) over `PublishActions` (View > Publish…), not a page of a Data workspace; no `ExportBridge` exists: store work runs on an `EntryBridge` opened as role `publisher`, builds through a `StoreSource`; Set public is `set_release_public` (private -> public only); the publish and verify checks shared with `elctl` are `workflow.hpp`; DOI…, IGSN… and EarthBank buttons come with phases 7 and 8; with-viewer absent until phase 3. Section 13: phase 4 done, with date and plan path.

- [ ] **Step 1:** write; **Step 2:** check every name against the code; **Step 3:** commit `docs(publish): the Publish window in the user guide and the spec`.

### Landing

- [ ] Rebase on `origin/develop`; `cmake --build --preset dev-ui`; `ctest --preset dev-ui` with `PYCHRON_TEST_PG_URL` set (PG suites at `-j2` if refused).
- [ ] The window's tests under `-DPYCHRON_SANITIZE=address,undefined` once (the bridge adds no thread of its own, but closes a window under a running job): `cmake --preset dev-ui -B build/asan-ui -DPYCHRON_SANITIZE=address,undefined`, build `pychron_ui_test_publish_window`, run it.
- [ ] `python3 tools/quality_check.py` exit 0; `python3 tools/compiler_sweep.py --build-dir build/dev-ui` 0 warnings.
- [ ] Sonnet review of the whole branch; fixes as their own commits.
- [ ] Push `feat/publish-page:develop`.

## Open questions

None.
