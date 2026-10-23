# Domain Seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put every Ar-Ar-specific part of pychron (figures, reduction past the IC stage, the age cache, the Schaen report, EarthBank, flux) behind a `Domain` interface, with a noble-gas core that needs none of them, so a second domain is one library and no UI edit.

**Architecture:** A Qt-free `processing::Domain` supplies units, figure entries, derived quantities, recall sections and exporters; a Qt `ui::DomainUi` supplies Ar-Ar windows and pages. Ar-Ar code moves to a new `libs/arar` that links `processing`; `processing` never links back. The install's `site.toml` entry picks the domain.

**Tech Stack:** C++20, CMake presets `dev` / `dev-ui`, GoogleTest, Qt 6 (UI only), toml++.

**Spec:** `docs/superpowers/specs/2026-10-22-domain-seam-design.md`

## Global Constraints

- Four branches, one per phase, each landing on `develop` with Ar-Ar behaviour unchanged (same figures, same report bytes, same cache rows) until Phase 3 adds the second domain. Branch names: `feat/domain-seam`, `refactor/arar-library`, `feat/noble-gas-domain`, `refactor/domain-ui`.
- Phase 2 (library move) lands before the noble-gas abundance spec's items 2-11 add code to the files it moves. Its item 1 (`make_signal(label)`, tag fix) is folded into Task 5 here; the noble plan then skips it.
- `libs/processing` and `libs/reduction` outside the files that stay Ar-Ar by design name no isotope, no age, no J after Phase 2. CMake enforces the link direction: `pychron_processing` lists no `arar` target.
- `reduction_version()` of the `arar` domain is `"arar-1"` (today's `kReductionVersion`): an input fingerprint computed after the move equals one computed before.
- Every C++ change: `python3 tools/quality_check.py` before tests; `python3 tools/compiler_sweep.py --changed` while working. Before each phase merges: `cmake --build --preset dev-ui` and `ctest --preset dev-ui` (every phase touches `apps/pychron-ui`), never `dev-quick` as the final run.
- Commit messages follow Conventional Commits with the component as scope, as root `AGENTS.md` says.
- The spec changes in the same branch whenever code departs from it.

## Review Focus

1. A `site.toml` entry with `domain = "u_pb"`: `pychron-ui` and `elctl` stop with a message listing the registered ids, no crash and no silent fall-back to `arar`. Test in Task 2 and Task 3c.
2. A figure preset saved before Phase 2 whose `reduce` node carries `constants = "legacy"`: the pipeline loads, warns once naming the key, and `arar_reduce` runs with legacy constants. Test in Task 11.
3. An analysis with no argon isotopes (a He-only run) opened in an `arar` install: `arar_reduce` sets the item's error, the figure reports "did not reduce", nothing dereferences an absent `ArArResult`. Test in Task 8.
4. A browser filter bound saved in QSettings for a derived name the active domain does not list (an `age` bound in a `noble_gas` install): ignored, no column, no join. Test in Task 14.
5. Rows cached in `derived_value` before the move: the page statement still finds them (fingerprint and marker name unchanged). Test in Task 11 (fingerprint equality) and Task 12 (name-parameterised join against a fixture written with `'age'`).

---

## Phase 1: `feat/domain-seam` (no file moves)

### Task 1: `Domain` interface, registry, and the two domains over today's code

**Files:**
- Create: `libs/processing/include/pychron/processing/domain.hpp`, `libs/processing/src/domain.cpp` (registry, `make_noble_gas_domain`), `libs/processing/src/domain_arar.cpp` (`ArArDomain`, moved to `libs/arar` in Task 9)
- Modify: `libs/processing/include/pychron/processing/source.hpp:90-101` (`DerivedAge` → `DerivedValue`, `DerivedValues`; `IDerivedSink::put(std::vector<DerivedValues>)`), `libs/processing/adapters/store/src/store_source.cpp:669-678` (`derived_rows` over `DerivedValues`), `libs/processing/src/derived.cpp`, `libs/processing/src/units_builtin.cpp:164-169`, `apps/pychron-ui/src/recall_window.cpp:305`, `apps/elctl/src/import_age.cpp:195`
- Test: `tests/processing/test_domain.cpp` (new), `tests/processing/test_derived.cpp`, `tests/processing/test_store_source.cpp` (the `put` callers)

**Interfaces:**
- Produces: `Runner(const UnitRegistry&, IAnalysisSource*, const Domain* domain = nullptr)` and `RunContext::domain` (`units.hpp:40-49,119`); `ReduceUnit` writes the cache through `ctx.domain->derive()` when both the domain and `ctx.source->derived()` exist (`units_builtin.cpp:164-169`), else nothing. Spec §4 verbatim: `FigureEntry`, `DerivedQuantity`, `DerivedValue`, `DerivedValues`, `Exporter`, `Domain` (pure virtuals `id`, `title`, `units`, `reduction_chain`, `figures`, `quantities`, `default_quantity`, `derived`, `derive`, `reduction_version`, `recall_sections`, `error_budget`, `exporters`), `DomainRegistry` (`add`, `find`, `ids`), `std::unique_ptr<Domain> make_noble_gas_domain()`, and for this phase `std::unique_ptr<Domain> make_arar_domain()` declared in `domain.hpp` too (moves to `pychron/arar/domain.hpp` in Task 9).
- `arar` answers from today's code: `units()` = `UnitRegistry::builtin()`; `reduction_chain()` = `{"reduce"}`; `figures()` = the eight entries of `data_browser_window.cpp:239-248` in order, with `separator_before` on `isotope_evolution_fit`, group keys from `figure_window.cpp:57-61`, `window` `ReferenceFit` for `blank_fit` and `icfactor_fit`, `IsotopeEvolution` for `isotope_evolution_fit`; `quantities()` = the `reduced` names of `quantity.cpp:17-41` plus `decay_corrected`, `interference_corrected`; `default_quantity()` = `"age"`; `derived()` = `{age "Age (Ma)" Ma filterable, age_w_j, kca}`; `derive()` wraps `derived_age(reduced, {})` into `DerivedValues` with rows `age`, `age_w_j`, `kca` as `store_source.cpp:669-678` makes them today (valueless `age` row when none); `reduction_version()` = `reduction::kReductionVersion`; `recall_sections()` = `make_recall_model(reduced).computed` and `.ratios`; `error_budget()` = `make_recall_model(reduced).error_budget`; `exporters()` = one `{"schaen", "Export", today's tooltip, "CSV report (*.csv);;JSON report (*.json)", write}` whose `write` is `data_workspace.cpp:63-68` (pick the aliquot grouping when any step, `make_report`, `save_report`).
- `noble_gas`: `units()` = builtin; `reduction_chain()` = `{"reduce"}`; `figures()` = `time_series` (group key `none`), `isotope_evolution_fit`, `blank_fit`, `icfactor_fit`; `quantities()` empty; `default_quantity()` = `"Ar40"` placeholder until the noble spec lands (`group_stats` needs a string); `derived()` empty; `derive()` nullopt; `reduction_version()` = `"noble-1"`; recall sections, error budget, exporters empty.

- [ ] **Step 1: Write the failing tests in `tests/processing/test_domain.cpp`**

```cpp
TEST(Domain, RegistryFindsByIdAndListsIdsSorted)  // add noble_gas and arar; find("arar")->id() == "arar"; find("u_pb") == nullptr; ids() == {"arar","noble_gas"}; add(arar again) replaces, ids().size() == 2
TEST(Domain, ArArFiguresAreTodaysToolbarInOrder)  // kinds == {time_series, ideogram, spectrum, inverse_isochron, spectrum_isochron, isotope_evolution_fit, blank_fit, icfactor_fit}; separator_before only on isotope_evolution_fit; group_key ideogram=="identifier", spectrum=="aliquot", time_series=="none"; window of blank_fit == ReferenceFit
TEST(Domain, NobleGasHasNoAgeNoExportNoRecallExtras)  // figures kinds == {time_series, isotope_evolution_fit, blank_fit, icfactor_fit}; derived().empty(); exporters().empty(); derive(reduced).has_value() == false; reduction_version() == "noble-1"
TEST(Domain, ArArDerivesTheSameRowsAsDerivedAge)  // reduce fixtures.hpp's unknown; derive() rows names == {"age","age_w_j","kca"}, units "Ma","Ma",""; age value == derived_age(...).age nominal
TEST(Domain, ArArMarkerRowIsValuelessWithoutAnAge)  // an air: rows == {age valueless}
TEST(Domain, EveryFigureKindIsInTheDomainsRegistry)  // for both domains, units().find(entry.kind) != nullptr
TEST(Domain, NobleGasRunsEveryCoreUnitOnAnArgonFreeFixture)  // MemorySource with two analyses of He3, He4, Ne20, Ne21, Ne22 (one blank_unknown); Runner over noble_gas; pipeline select→reduce→filter→group→edits→group_stats and select→reduce→group→graph→edits→time_series: both run with no error, group_stats rows have n_included 0 and an error text, time_series scene has one series per isotope; no row written to the sink
TEST(Domain, ReduceWritesThroughTheRunnersDomain)  // Runner with arar and a SinkSource: put called with age/age_w_j/kca; Runner without a domain: put never called
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset dev --target pychron_processing_tests 2>&1 | tail -3`
Expected: compile error, `domain.hpp` not found.

- [ ] **Step 3: Write `domain.hpp` and `domain.cpp`, then `domain_arar.cpp`**

`DerivedValues` replaces `DerivedAge` everywhere it is used (the four modify sites above); `derived_age()` stays and `arar`'s `derive()` calls it. `derived_rows` in `store_source.cpp` iterates `values` and writes a valueless row when `value` is absent.

- [ ] **Step 4: Run the processing tests**

Run: `build/dev/tests/processing/pychron_processing_tests --gtest_filter='Domain.*:Derived*'`
Expected: all PASS. Then `ctest --preset dev-quick` for the `put` callers.

- [ ] **Step 5: Commit**

```bash
git add libs/processing apps/pychron-ui/src/recall_window.cpp apps/elctl/src/import_age.cpp tests/processing
git commit -m "feat(processing): Domain interface with noble_gas and arar over today's code"
```

### Task 2: The install's `domain` key

**Files:**
- Modify: `libs/setup/include/pychron/setup/site.hpp:33` (`SiteInstall::domain`), `libs/setup/src/site.cpp:62-110` (`load_site`, `save_site`), the profile top-table struct and its loader (`libs/setup`, where `kind` and `summary` are read; `setup_wizard.cpp:493` reads it as `profile_->top`), the installer where `SiteInstall::kind` is set from the profile, `libs/setup/src/doctor.cpp:194-236`, `profiles/argus/profile.toml`, `profiles/helix/profile.toml`, `profiles/ngx/profile.toml`, `profiles/data-reduction/profile.toml`
- Test: `tests/setup/test_site.cpp` (or the file that covers `load_site`), `tests/setup/test_profiles.cpp`, `tests/setup/test_doctor.cpp` (or where doctor checks are tested)

**Interfaces:**
- Produces: `std::string SiteInstall::domain = "arar"`; profile top key `domain` (default `"arar"`); question id `domain` with choices `["arar", "noble_gas"]`, labels "Ar/Ar geochronology", "Noble gas (abundances, no age)", default `arar`, group "Data"; doctor check named `domain`.
- Consumes: `DomainRegistry::ids()` is not available to `libs/setup` (it does not link `processing`): doctor checks against the constant list `setup::known_domains()` = `{"arar", "noble_gas"}` declared in `site.hpp`, and Task 3c asserts at startup that every registered id is in it.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Site, DomainRoundTripsAndDefaultsToArAr)  // save_site with domain "noble_gas", load_site reads it back; an entry without the key loads as "arar"
TEST(Profiles, EveryShippedProfileWritesADomain)  // install() each of argus, helix, ngx, data-reduction with defaults: site entry domain == "arar"
TEST(Profiles, DataReductionAsksForTheDomainAndTheAnswerWins)  // install data-reduction with answer domain=noble_gas: entry domain == "noble_gas"; the question's choices == {"arar","noble_gas"}
TEST(Doctor, RefusesAnUnknownDomain)  // entry domain "u_pb": a Fail check named "domain" whose detail lists arar and noble_gas; "noble_gas" passes
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset dev --target pychron_setup_tests && build/dev/tests/setup/pychron_setup_tests --gtest_filter='Site.Domain*:Profiles.*Domain*:Doctor.Refuses*'`
Expected: FAIL (no `domain` member, question not found).

- [ ] **Step 3: Implement**

Follow `load_site`'s `(*i)["kind"].value_or(...)` form for the key; the profile loader reads `domain` like `kind`; the installer copies `top.domain` then the `domain` answer when the profile has that question. Doctor: `failed("domain", "unknown domain '<id>'; known: arar, noble_gas", reconfigure_hint(install))` before the kind branch at `doctor.cpp:228`.

- [ ] **Step 4: Run the setup tests**

Run: `build/dev/tests/setup/pychron_setup_tests`
Expected: PASS, including the existing `InstrumentProfile.InstallsLoadsAndPassesDoctor` suite.

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(setup): the install entry names its domain"
```

### Task 3a: `ProcessingBridge` and the figure windows take the domain

**Files:**
- Modify: `apps/pychron-ui/src/processing_bridge.hpp:62`, `processing_bridge.cpp:99`; `figure_window.hpp:46-55`, `figure_window.cpp:57-111,183,294,459-463`; `reference_fit_window.hpp:47-51`, `reference_fit_window.cpp:46-72`; `isotope_evolution_window.hpp:53`, `isotope_evolution_window.cpp:56-72`; `reference_series.cpp:36`
- Test: `tests/ui/test_data_windows.cpp` (every `FigureWindow w(bridge, presets, ...)`, `ReferenceFitWindow w(...)`, `IsotopeEvolutionWindow` construction), `tests/ui/test_reference_series.cpp`

**Interfaces:**
- Produces: `ProcessingBridge(processing::IAnalysisSource& source, const processing::Domain& domain, QObject* parent = nullptr)` with `const processing::Domain& domain() const noexcept`; the runner is `Runner(domain.units(), &source, &domain)`. `FigureWindow(ProcessingBridge&, PresetStore&, const processing::FigureEntry& entry, QStringList uuids, QWidget* parent = nullptr)`; `default_group_key` removed, the group key is `entry.group_key`; the pipeline is `select` → each kind of `bridge.domain().reduction_chain()` (node ids equal to the kinds, each fed by the previous) → `group` → `graph` → `edits` → figure, the group node reading from the last chain node. `ReferenceFitWindow` and `IsotopeEvolutionWindow` take the registry from `bridge.domain().units()` and build the same chain; `ReferenceFitWindow::handles` removed (dispatch is Task 3c's).
- The `kind_ == "ideogram"` dock at `figure_window.cpp:183` and the `spectrum` export branch at `:459-463` stay as they are: Phase 2 moves them under `arar` with the units.

- [ ] **Step 1: Update the tests to construct through a domain**

Add to the fixture at `test_data_windows.cpp:579`: `processing::DomainRegistry domains_` holding both domains, `const processing::Domain& arar_ = *domains_.find("arar")`, and a helper `const processing::FigureEntry& entry(const char* kind)` that finds the entry in `arar_.figures()`. Replace `FigureWindow w(bridge, presets, "ideogram", ids)` with `FigureWindow w(bridge, presets, entry("ideogram"), ids)` at every site; construct every `ProcessingBridge` with `arar_`. Add:

```cpp
void figure_pipeline_follows_the_domains_reduction_chain();  // FigureWindow over entry("ideogram"): pipeline().nodes kinds in order == {select, reduce, group, group, edits, ideogram}; group key == "identifier"
```

- [ ] **Step 2: Build to verify it fails**

Run: `cmake --build --preset dev-ui --target pychron_ui_test_data_windows 2>&1 | grep -m3 error`
Expected: no matching constructor.

- [ ] **Step 3: Implement the signatures above**

- [ ] **Step 4: Run the UI data-window tests**

Run: `build/dev-ui/tests/ui/pychron_ui_test_data_windows`
Expected: PASS, every existing figure test unchanged.

- [ ] **Step 5: Commit**

```bash
git commit -am "refactor(ui): figure windows build their pipeline from the domain"
```

### Task 3b: The browser toolbar and export come from the domain

**Files:**
- Modify: `apps/pychron-ui/src/data_browser_window.hpp:66,128-140,171-173`, `data_browser_window.cpp:229-262,815`
- Test: `tests/ui/test_data_windows.cpp:1631-1666,2775-2790`

**Interfaces:**
- Produces: `DataBrowserWindow(processing::IAnalysisSource& source, const processing::Domain& domain, QWidget* parent = nullptr)`. One toolbar action per `domain.figures()` entry in order (`data_icon(entry.icon)`, `entry.label`, `entry.tip`, `setData(kind)`, a separator before an entry flagged so), then a separator, Recall, then one action per `domain.exporters()` (`data_icon("export")`, `e.label`, `e.tip`, `setData(e.id)`), then Publish. `plot_action(kind)` unchanged. Signal `export_requested(const QString& exporter_id, const QString& path, const QStringList& uuids)`; `ask_export_path(const QString& suggested, const QString& filters)`; `export_action()` returns the first exporter's action or null. `data_icon` of an unknown name returns the `time_series` glyph (the generic figure).
- Consumes: Task 1's `FigureEntry`, `Exporter`.

- [ ] **Step 1: Write the failing tests**

Change `browser_toolbar_offers_every_figure_recall_and_export` to compare the toolbar's `data()` list with `arar_.figures()` kinds followed by `recall`, `schaen`, `publish`. Add:

```cpp
void browser_toolbar_under_noble_gas_has_four_figures_and_no_export();  // DataBrowserWindow over *domains_.find("noble_gas"): data() list == {time_series, isotope_evolution_fit, blank_fit, icfactor_fit, recall, publish}; export_action() == nullptr
void browser_export_names_the_exporter();  // spy on export_requested; trigger export with ask_export_path returning "/tmp/x.csv"; first argument == "schaen", filters passed == the exporter's
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset dev-ui --target pychron_ui_test_data_windows && build/dev-ui/tests/ui/pychron_ui_test_data_windows`
Expected: FAIL at the constructor.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run, expect PASS**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(ui): browser toolbar and export built from the domain"
```

### Task 3c: Workspace, main windows and `main.cpp` pick the domain

**Files:**
- Modify: `apps/pychron-ui/src/data_workspace.hpp:32-60`, `data_workspace.cpp:35-82,89,122-173`; `recall_window.hpp:50`, `recall_window.cpp:120-149,231-232,299-336`; `main_window.hpp`, `main_window.cpp:189,756,865-867`; `data_main_window.hpp`, `data_main_window.cpp:17-24`; `main.cpp:258-301,410-426,568-646,675-691`
- Test: `tests/ui/test_data_windows.cpp:2844,3030-3046`, `tests/ui/test_earthbank_publish.cpp:120`, `tests/ui/test_preferences.cpp:448`, `apps/elctl` untouched here

**Interfaces:**
- Produces: `DataWorkspace::set_source(IAnalysisSource*, PresetStore*, const Domain*)`; `open_figure(kind, uuids)` finds the entry in `domain->figures()` (null when absent) and dispatches on `entry.window`; `export_report(exporter_id, path, uuids)` runs `select` → `reduction_chain()` → the two group nodes as today and calls the exporter's `write` with the chosen grouping; `DataMainWindow(IAnalysisSource&, PresetStore&, const Domain&, const QString& title, QWidget* = nullptr)`; `MainWindow::set_data(..., const Domain&)`. `RecallWindow(IAnalysisSource&, const Domain&, QWidget* = nullptr)`: the Summary tab is the five header rows then one table per `domain.recall_sections(reduced)`; the Error budget tab is added only when `domain.error_budget(reduced)` is non-empty; the cache write at `:305` goes through `domain.derive()`. `main.cpp`: a function `const processing::Domain& choose_domain(processing::DomainRegistry&, std::string_view id)` that registers `make_noble_gas_domain()` and `make_arar_domain()`, asserts every id is in `setup::known_domains()`, and on an unknown id calls `fatal("unknown domain '<id>'; known: arar, noble_gas")`; the id is `install->domain` when an install was chosen, else `"arar"`.

- [ ] **Step 1: Write the failing tests**

```cpp
void workspace_opens_only_the_domains_figures();  // under noble_gas: open_figure("time_series") non-null; open_figure("ideogram") null
void workspace_opens_a_window_of_each_kind();  // under arar: open_figure of ideogram, blank_fit, isotope_evolution_fit return a FigureWindow, ReferenceFitWindow, IsotopeEvolutionWindow (qobject_cast)
void recall_under_noble_gas_has_no_error_budget_tab();  // tabs == {Summary, Isotopes, Evolutions, Identity, Extraction, Spectrometer, History}; under arar recall_shows_every_tab unchanged
void workspace_export_writes_the_report();  // existing: now export_report("schaen", path, ids); bytes equal today's fixture
```

And in `main_window_opens_and_tears_down_data_windows` keep both assertions (`"ideogram"` true, `"no_such_figure"` false) under `arar_`.

- [ ] **Step 2: Build, expect constructor errors**

- [ ] **Step 3: Implement; keep `set_source(nullptr, nullptr, nullptr)` as the disable path**

- [ ] **Step 4: Run the whole UI suite**

Run: `ctest --preset dev-ui -R '^ui\.'`
Expected: PASS.

- [ ] **Step 5: Manual check**

Run: `build/dev-ui/apps/pychron-ui/pychron-ui --sim --examples` and open View > Data: the toolbar is unchanged from today.

- [ ] **Step 6: Commit**

```bash
git commit -am "feat(ui): the data windows run on the install's domain"
```

### Task 3d: `elctl` resolves the domain

**Files:**
- Modify: `apps/elctl/src/cli.cpp:188,754-833` (`Globals::domain`, `--domain`), `apps/elctl/src/setup.cpp:191` (install's domain), `apps/elctl/src/flux.cpp:566`, `apps/elctl/src/export.cpp:188`, `apps/elctl/src/db.cpp:63,97`, `apps/elctl/src/import_run.cpp:116`, `apps/elctl/CMakeLists.txt:59` (link `pychron::processing` already; nothing new this phase)
- Test: `apps/elctl/tests/test_flux_cmd.cpp` (or the file covering `flux`), `apps/elctl/tests/test_cli.cpp`

**Interfaces:**
- Produces: `Globals::domain` (`std::string`, `"arar"`), set from `SiteInstall::domain` by `resolve_install`, overridden by `--domain <id>`; `Session::domain()` returning `const processing::Domain&` from a registry built as in Task 3c (shared helper `elctl::choose_domain` in a new `apps/elctl/src/domain.cpp`, with the same fatal message as the UI, exit `kUsage`). `flux_command`, `export_command`, `db derive`, `import` receive the domain; `flux` returns `fatal(io, "needs domain arar (this install: <id>)")` when `domain.id() != "arar"`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Cli, DomainFlagAndInstallEntry)  // --domain noble_gas → session domain id "noble_gas"; --domain u_pb → exit kUsage, stderr contains "known: arar, noble_gas"
TEST(FluxCmd, RefusesOutsideArAr)  // `flux fit --db ... --domain noble_gas` exits kUsage, stderr "elctl flux: needs domain arar (this install: noble_gas)"
```

- [ ] **Step 2: Run, expect FAIL (unknown flag)**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `ctest --preset dev-quick -R elctl`, expect PASS**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(elctl): commands run on the install's domain"
```

### Task 4: Phase 1 close

- [ ] **Step 1: Docs**: `docs/user/07-configuration-reference.md` "The site file" (`:91`) gains the `domain` key with both values; `docs/user/06-data-analysis.md` "The toolbar" (`:260`) says the buttons are the install's domain's and lists the Ar/Ar set; `docs/user/08-elctl-reference.md` gains `--domain`. Root `AGENTS.md` component table gains the row `libs/processing` `domain.hpp`, `apps/pychron-ui` data windows → "a new figure, exporter, derived quantity or recall section is a `Domain` entry, never a literal in a window".
- [ ] **Step 2: Static analysis and sweep**: `python3 tools/quality_check.py` (status 0), `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.
- [ ] **Step 3: Full run**: `cmake --build --preset dev-ui && ctest --preset dev-ui`. Expected: all pass.
- [ ] **Step 4: Land** per root `AGENTS.md` (rebase on `origin/develop`, rerun, push `feat/domain-seam:develop`).

---

## Phase 2: `refactor/arar-library`

### Task 5: Neutral signal types in `libs/reduction`

**Files:**
- Create: `libs/reduction/include/pychron/reduction/signal.hpp`, `libs/reduction/src/signal.cpp`
- Modify: `libs/reduction/include/pychron/reduction/arar_types.hpp:23-69` (move `Measured`, `StoredValue`, `resolve`, `IsotopeSignal`, `MeasuredSignal`, `make_signal`), `arar_reduction.hpp:37-61` (move `baseline_corrected`, `non_detector_corrected`, `corrected_intensity`, `deadtime_correct`, `deadtime_corrected_intercept`), `src/arar_types.cpp`, `src/arar_reduction.cpp:90-150`, `src/arar_kernels.hpp` → `include/pychron/reduction/detail/arar_kernels.hpp`, `libs/processing/src/reduced.cpp:107`
- Test: `tests/reduction/test_isotope_arithmetic.cpp`, new cases in `tests/reduction/test_signal.cpp`

**Interfaces:**
- Produces: in `signal.hpp`: `IsotopeSignal make_signal(std::string_view label, const MeasuredSignal& m)` minting tags `<label>`, `<label> bs`, `<label> bk`, `<label> IC`; the `ArgonIsotope` overload stays in `arar_types.hpp` and calls it with `to_string(iso)`. `reduced.cpp:107` calls the label overload with `iso.isotope`.

- [x] **Step 1: Write the failing test** (noble spec §3.3): `TEST(Signal, TagsCarryTheLabel)`: `make_signal("He3", m)` stages' `error_components` tags are `He3`, `He3 bs`, ..., none `Ar40`; `make_signal(ArgonIsotope::Ar40, m)` bit-equal to `make_signal("Ar40", m)` in value and sigma.
- [x] **Step 2: Run, expect FAIL** (`signal.hpp` missing).
- [x] **Step 3: Move the declarations and bodies; `arar_types.hpp` includes `signal.hpp`** so no caller changes yet.
- [x] **Step 4: Run `build/dev/tests/reduction/pychron_reduction_tests` and the golden suites, expect PASS** (bit-equal: same operations).
- [x] **Step 5: Commit** `refactor(reduction): neutral signal types in signal.hpp`

### Task 6: `libs/arar` holds the Ar-Ar reduction (`pychron::arar_reduction`)

**Files:**
- Create: `libs/arar/CMakeLists.txt` (target `pychron_arar_reduction` STATIC from `src/reduction/*.cpp`, alias `pychron::arar_reduction`, `PUBLIC pychron::reduction`, `-ffp-contract=off` as `libs/reduction/CMakeLists.txt:25-27`; the second target comes in Task 7), `libs/arar/include/pychron/arar/`, `libs/arar/src/reduction/`, `tests/arar/CMakeLists.txt` (`pychron_arar_tests` from `test_*.cpp`, golden dir defines copied from `tests/reduction/CMakeLists.txt:7-10`, links `pychron::arar_reduction` now and `pychron::arar` from Task 7), `libs/arar/AGENTS.md` and `CLAUDE.md` (`@AGENTS.md`; content: `libs/processing/AGENTS.md` moved, title "Ar-Ar: reduction, figures, report and flux (`libs/arar`)")
- Move (`git mv`): `libs/reduction/include/pychron/reduction/{arar_types,arar_reduction,flux}.hpp` → `libs/arar/include/pychron/arar/`; `libs/reduction/src/{arar_types,arar_reduction,decay,flux}.cpp`, `least_squares.hpp` → `libs/arar/src/reduction/`; `tests/reduction/{test_age,test_arar_types,test_atmospheric,test_calculate_f,test_chlorine,test_decay,test_flux,test_interference,test_reduce,test_golden_reader}.cpp`, `golden.hpp`, `reduce_golden.hpp`, `flux_golden.hpp`, `golden/` → `tests/arar/`; `libs/processing/AGENTS.md` → `libs/arar/AGENTS.md`
- Stay in `libs/reduction`: `arar.hpp`, `arar.cpp` (`compute_arar`, live conditionals: `libs/experiment/CMakeLists.txt:20` keeps linking `pychron::reduction` only), `detail/arar_kernels.hpp`, `test_arar.cpp`, `test_isotope_arithmetic.cpp`, `test_stats.cpp`, `test_fits.cpp`, `test_ufloat.cpp`. Whatever `arar.cpp` needs from `arar_types.hpp` stays with it in `signal.hpp` or a new `pychron/reduction/arar_live.hpp`.
- Modify: top `CMakeLists.txt:31` (`arar` after `processing`, before `earthbank`); `libs/processing/CMakeLists.txt:15` (`PUBLIC pychron::arar_reduction` added, removed again in Task 10); `tools/flux_reference.py` output path; `tools/spec_router/spec_router/units.toml:159-160`; every `#include "pychron/reduction/arar_types.hpp"`, `arar_reduction.hpp`, `flux.hpp` → `pychron/arar/...` (`grep -rl 'reduction/arar_types\|reduction/arar_reduction\|reduction/flux\.hpp' libs apps tests`)

- [x] **Step 1: Move, fix includes and CMake.**
- [x] **Step 2: `cmake --build --preset dev-ui`**. Expected: links; `libs/experiment` still builds against `pychron::reduction` alone.
- [x] **Step 3: `ctest --preset dev-ui`**. Expected: PASS, the `tests/arar` golden suites included, bytes unchanged.
- [x] **Step 4: Commit** `refactor(arar): Ar-Ar reduction in its own library`

Landed differently: `least_squares.hpp` stays in `libs/reduction` as `detail/least_squares.hpp` (the neutral `fits.cpp` uses it); `K3739Mode` went to a new neutral `pychron/reduction/arar_live.hpp`, `corrects_for_blank` to `signal.hpp`, and `to_live_constants` to `libs/arar` `live_parity.hpp`; the golden JSON reader shared by `tests/reduction` and `tests/arar` is `tests/support/golden_json.hpp`.

### Task 7: Figures, report, group ages, flux fit and the domain move to `pychron::arar`

**Files:**
- Create: second target in `libs/arar/CMakeLists.txt`: `pychron_arar` STATIC from `src/*.cpp`, alias `pychron::arar`, `PUBLIC pychron::processing pychron::arar_reduction`; `pychron_arar_store` from `adapters/store/src/*.cpp` (`PUBLIC pychron::arar pychron::processing_store`) when `pychron_processing_store` exists
- Move (`git mv`): `libs/processing/include/pychron/processing/{arar_figures,arar_groups,group_ages,report,derived,flux_fit,flux_view}.hpp` → `libs/arar/include/pychron/arar/`; `libs/processing/src/{arar_figures,arar_groups,group_ages,report,derived,flux_fit,flux_view,ideogram,spectrum,isochron,spectrum_isochron,figure_common,domain_arar}.cpp` → `libs/arar/src/`; `libs/processing/adapters/store/{include/pychron/processing/flux_store.hpp,src/flux_store.cpp}` → `libs/arar/adapters/store/`; `tests/processing/{test_arar_figures,test_derived,test_flux_fit,test_flux_store,test_flux_view,test_report,test_r_values}.cpp`, `flux_level_inputs.hpp`, `flux_seed.hpp`, `flux_store_fixture.hpp` → `tests/arar/`
- Modify: `libs/processing/src/units_builtin.cpp:451-471` (`builtin()` drops ideogram, spectrum, inverse_isochron, spectrum_isochron), `libs/processing/include/pychron/processing/domain.hpp` (drop `make_arar_domain`; declared in new `libs/arar/include/pychron/arar/domain.hpp`), `libs/arar/src/domain_arar.cpp` (`units()` = `UnitRegistry::builtin()` plus the four figure units and the flux units; `reduction_chain()` still `{"reduce"}`), `libs/earthbank/CMakeLists.txt:63`, `libs/publish/CMakeLists.txt:22`, `apps/pychron-ui/CMakeLists.txt:216-234`, `apps/elctl/CMakeLists.txt:59`, `tests/ui/CMakeLists.txt:18-37` (`flux_seed.hpp` path), `tests/earthbank`, `tests/publish`, `tests/integration` CMake link lines (`pychron::arar`, `pychron::arar_store`); every `#include "pychron/processing/report.hpp"` and kin in `apps`, `libs/earthbank`, `libs/publish` → `pychron/arar/...`
- Test: the moved suites; new `tests/arar/test_domain_arar.cpp`: `TEST(ArArDomain, HoldsTheFigureUnitsAndCoreDoesNot)` (`make_arar_domain()->units().find("ideogram") != nullptr`; `UnitRegistry::builtin().find("ideogram") == nullptr`; `tests/processing/test_domain.cpp`'s Ar-Ar cases move here)

- [x] **Step 1: Write the registry test, run, expect FAIL.**
- [x] **Step 2: Move the files, fix includes, CMake and `units_builtin.cpp`.** The files still read `ReducedAnalysis::arar`: that is Task 8's change.
- [x] **Step 3: `cmake --build --preset dev-ui && ctest --preset dev-ui`**, expect PASS.
- [x] **Step 4: Commit** `refactor(arar): figures, report, group ages and flux in libs/arar`

Landed differently: `make_arar_domain()` became `arar::make_domain()` in `pychron/arar/domain.hpp`; `figure_common.cpp` stays in `libs/processing` (the neutral figures use it, and there was no `arar_figures.cpp`); `r_values`, `flux_monitors.cpp` and `flux_options.cpp` moved as well; the Ar-Ar cases of `test_options.cpp` went to `tests/arar/test_options_arar.cpp`.

### Task 8: Neutral `ReducedAnalysis`, `DomainReduced`, `arar_reduce`

**Files:**
- Modify: `libs/processing/include/pychron/processing/reduced.hpp:14,19-65` (spec §3.2: `Stage` ends at `IcCorrected`; `ReductionSettings` loses `preset`, `include_decay_error`, `use_irradiation_endtime`; `ReducedAnalysis{analysis, signals, isotopes, domain, reduction_error}`, `gases` arriving with the noble spec; `DomainReduced` with the spec's virtuals plus `virtual std::string units(std::string_view name) const`), `libs/processing/src/reduced.cpp:87-204` (`reduce_analysis` stops after `:126`, keeps the signals), `libs/processing/src/units_builtin.cpp:129-177` (`ReduceUnit` schema has no options; `execute` no longer writes the cache; `reduction_tag` prefix is `ctx.domain ? ctx.domain->reduction_version() : "core"`), `libs/processing/include/pychron/processing/recall.hpp` and `src/recall.cpp:76,96` (`RecallModel` loses `computed`, `ratios`, `error_budget`), `libs/arar/src/domain_arar.cpp` (`units()` adds `make_arar_reduce_unit()`; `reduction_chain()` = `{"reduce", "arar_reduce"}`; `recall_sections` and `error_budget` computed here from `ArArReduced`, the code of today's `recall.cpp` computed/ratios/budget blocks), every file under `libs/arar/src` and `libs/earthbank/src/mapping.cpp:1281` reading `.arar`, `.j`, `.constants`, `.lambda_k_total` (→ `ArArReduced` through `analysis->domain`), `apps/pychron-ui/src/recall_window.cpp:87-112,143-146,299-306` (`kStages` and the stage combo list the core stages only; runs `select` → `reduction_chain()` through a `Runner(domain.units(), &source, &domain)` on the UI thread as today's `reduce_analysis` was, no `derived_age` call: `arar_reduce` writes), `apps/elctl/src/selection.cpp:66`, `import_age.cpp:195` (`AgeFlags::reduction()` builds `ArArSettings`)
- Create: `libs/arar/include/pychron/arar/reduce.hpp`, `libs/arar/src/reduce.cpp` (`ArArReduced`, `ArArSettings`, `reduce_arar`), `libs/arar/src/unit_reduce.cpp` (`make_arar_reduce_unit`)
- Test: `tests/arar/test_reduce_unit.cpp` (new), `tests/processing/test_units.cpp`, `tests/processing/test_recall.cpp` (neutral parts stay), `tests/arar/test_recall_arar.cpp` (the computed-value cases, through the domain)

**Interfaces:**
- Produces: `struct ArArReduced final : processing::DomainReduced { reduction::ArArResult arar; ReductionConstants constants; std::optional<reduction::UFloat> j; std::optional<reduction::Measured> lambda_k_total; std::vector<std::map<std::string, reduction::UFloat>> stages; }` (per isotope: `decay_corrected`, `interference_corrected`), `named()` over `age`, `age_w_j`, `age_w_position`, `F`, `kca`, `cak`, `kcl`, `clk`, `radiogenic_yield`, `rad40`, `k39`, `j`; `stages()` = `{"decay_corrected","interference_corrected"}`. `struct ArArSettings { ConstantsPreset preset; bool include_decay_error; bool use_irradiation_endtime; }`. `Result<std::shared_ptr<const ArArReduced>> reduce_arar(const processing::ReducedAnalysis&, const ArArSettings&)` = `reduced.cpp:128-204` moved, reading `signals` rather than re-minting them, and `analysis->context` for flux, production, chronology until Task 10. `std::unique_ptr<processing::Unit> make_arar_reduce_unit()`: kind `arar_reduce`, title "Ar-Ar reduction", schema `unit.arar_reduce` with the three options moved from `unit.reduce` (same keys, choices and defaults), Dataset in and out; per item sets `item.analysis->domain` or `reduction_error`; writes the cache through `ctx.source->derived()` with `ctx.domain->derive()`; diagnostic "N analyses did not reduce".

- [x] **Step 1: Write the failing tests**

```cpp
TEST(ArArReduceUnit, MatchesTheGoldenReduce)  // select→reduce→arar_reduce over reduce_golden.hpp's inputs: ages->age nominal and sigma equal the golden to tests/arar/test_reduce.cpp's tolerances
TEST(ArArReduceUnit, StagesShareVariablesWithTheCoreStages)  // Ar39 decay_corrected / Ar39 ic_corrected: sigma equals today's reduce_analysis ratio to 1e-15 (recorded from Phase 1's binary into the test as a literal)
TEST(ArArReduceUnit, AHeliumOnlyAnalysisDoesNotReduce)  // Analysis with He3, He4 only: reduction_error non-empty, domain == nullptr, diagnostic "1 analyses did not reduce"; run under ASan
TEST(ArArReduceUnit, WritesTheCacheOnceWithTheArArVersion)  // SinkSource: put called once with rows age/age_w_j/kca; reduction_tag starts with "arar-1/"
TEST(Units, ReduceHasNoOptionsAndNoDomain)  // builtin reduce schema fields empty; reduce_analysis output: domain == nullptr, stages end at IcCorrected, signals.size() == isotopes.size()
TEST(ArArRecall, SectionsAndBudgetMatchTodaysModel)  // make_arar_domain()->recall_sections(reduced) rows equal the computed and ratios rows test_recall.cpp asserted today; error_budget non-empty for an unknown, empty for an air
```

- [x] **Step 2: Run, expect FAIL** (no `arar_reduce` kind, no `ArArReduced`).
- [x] **Step 3: Implement.**
- [x] **Step 4: `ctest --preset dev-ui`, expect PASS** (UI recall and figure tests unchanged in what they assert).
- [x] **Step 5: Commit** `refactor(processing): neutral ReducedAnalysis; arar_reduce carries the Ar-Ar result`

Landed differently: `signals` is named `isotope_signals` (Qt's `signals` macro); `ReductionSettings` was removed outright rather than emptied; `DomainReduced::units` was added here and removed again in Task 9; a neutral `run_reduction_chain` was added for the derive pass and the recall window. Fix commit `fix(arar): the j quantity stays an independent variable` followed.

### Task 9: Quantities resolve domain terms through `DomainReduced`

**Files:**
- Modify: `libs/processing/include/pychron/processing/quantity.hpp:26-57,84`, `libs/processing/src/quantity.cpp:17-41,209-410`, `libs/processing/include/pychron/processing/units.hpp:69-76` and `src/units_builtin.cpp:184,399,451` (`UnitRegistry::builtin()` → `UnitRegistry UnitRegistry::core(std::string default_quantity)`; `FilterUnit` and `GroupStatsUnit` take the default in their constructors), `libs/processing/src/domain.cpp` (`noble_gas` units = `core(default_quantity())`), `libs/arar/src/domain_arar.cpp` (`core("age")` plus its units), `apps/elctl/src/export.cpp:155`, `selection.cpp` (registry from the domain), every `UnitRegistry::builtin()` in tests (`tests/processing`: `core("")`; `tests/arar`, `tests/ui`: the domain's)
- Test: `tests/processing/test_quantity.cpp`, `tests/arar/test_quantities.cpp` (new, the Ar-Ar cases moved)

**Interfaces:**
- Produces: `Named` = `{Timestamp, Aliquot, StepIndex, ExtractValue, ExtractDuration, CleanupDuration, Weight}`; `Term::Kind::Domain` with `name` (a bare name) or `label` + `stage` (an isotope with a non-core stage); `Quantity::eval` resolves them through `a.domain->named(name)` / `->stage(label, stage)`, nullopt when `a.domain` is null; `needs_reduction()` true for any `Domain` term; `term_units` asks `a.domain->units(name)`; `available_quantities(analyses, const std::vector<std::string>& domain_quantities)`.

- [x] **Step 1: Tests**: `TEST(Quantity, ADomainNameHasNoValueWithoutADomain)` (`parse("age")` ok; `eval` on a core-only reduced nullopt, no throw); `TEST(Quantity, ADomainStageParses)` (`Ar40.interference_corrected` is `Kind::Domain`, `needs_reduction()`); `TEST(Units, CoreRegistryTakesTheDefaultQuantity)` (`core("He4").find("group_stats")->schema()` default of `quantity` == "He4"); `tests/arar`: `TEST(ArArQuantities, AgeAndRatiosEvaluateAsBefore)` (every value of today's Ar-Ar cases, through `arar_reduce`).
- [x] **Step 2: FAIL; implement; `ctest --preset dev-ui` PASS.**
- [x] **Step 3: Commit** `refactor(processing): quantities name no isotope; domain terms resolve through DomainReduced`

Landed differently: `UnitRegistry::core(const std::string&)` is static and returns a fresh registry by value; labels and units come from a new `Domain::quantity_info` at parse instead of `DomainReduced::units`; `arar::shared_domain()` serves the Ar-Ar figures' own parsing; `OptionsEditor::set_domain` refuses quantity typos on entry (fix commit `fix(ui): quantity typos are refused on entry under the domain`).

### Task 10: `Analysis::extension` and the Ar-Ar loaders

**Files:**
- Modify: `libs/processing/include/pychron/processing/model.hpp:14,74-96` (`DomainAnalysisData`; `Analysis::extension`; `ReductionContext::flux`, `production`, `chronology`, `reactor`, `constants` removed; the `arar_types.hpp` include removed), `libs/processing/adapters/store/include/pychron/processing/store_source.hpp:46,121-132`, `store_source.cpp:363-513,718,776,799,917` (`StoreSourceOptions` gains `std::string reduction_version` and `ExtensionLoader extension_loader`; `analysis_from_store` calls the loader with the parts; `kReductionVersion` uses → `options.reduction_version`), `libs/processing/adapters/records/include/pychron/processing/record_source.hpp:43-89` and `src/record_source.cpp:204-279` (`RecordExtensionLoader`; `set_extension_loader`; `flux_`, `production_`, `chronology_` removed), `libs/processing/CMakeLists.txt:15` (`pychron::arar_reduction` link removed), `libs/arar/src/reduce.cpp` (reads `ArArAnalysisData` from `analysis->extension`; error `"no Ar-Ar reference data for this analysis"` when absent), `libs/earthbank/src/mapping.cpp` (reads the extension), `apps/pychron-ui/src/main.cpp:260-268,568-582`, `apps/elctl/src/selection.cpp:148-157`, `flux.cpp:517`, `db.cpp:65`, `import_run.cpp:119` (pass `domain.reduction_version()` and, when `domain.id() == "arar"`, `arar::store_extension_loader()`; records sources `set_extension_loader(arar::record_extension_loader(references_toml))`)
- Create: `libs/arar/include/pychron/arar/analysis_data.hpp` (`struct ArArAnalysisData final : processing::DomainAnalysisData { std::optional<reduction::Flux> flux; std::optional<ProductionRatios> production; std::vector<Dose> chronology; std::string reactor; std::optional<ReductionConstants> constants; }`), `libs/arar/adapters/store/src/store_loader.cpp` (`processing::StoreSourceOptions::ExtensionLoader arar::store_extension_loader()` = `store_source.cpp:477-509` over `parts.refs`), `libs/arar/adapters/records/src/record_loader.cpp` (`processing::RecordExtensionLoader arar::record_extension_loader(const std::filesystem::path& references_toml)` = `record_source.cpp:204-279`; target `pychron_arar_records`, `PUBLIC pychron::arar pychron::processing_records`)
- Test: `tests/processing/test_store_source.cpp` (Ar-Ar reference-data cases → `tests/arar/test_store_loader.cpp`), `tests/processing/test_record_source.cpp` (references cases → `tests/arar/test_record_loader.cpp`), `tests/arar/fixtures/fingerprint_before_move.txt`

**Interfaces:**
- Produces: `using ExtensionLoader = std::function<std::shared_ptr<const DomainAnalysisData>(const StoreAnalysisParts&)>`; `using RecordExtensionLoader = std::function<std::shared_ptr<const DomainAnalysisData>(const experiment::AnalysisRecord&, const Analysis&)>`. The fingerprint statement is unchanged: it hashes heads, refs and the version string, and the flux, production and chronology refs are already among `refs->refs`.

- [x] **Step 0: Record the fingerprint fixture** from the Phase 1 binary: `build/dev/tests/processing/pychron_processing_tests --gtest_filter='StoreSource.*Fingerprint*'` (add a one-line print of the hex to that test in Task 4 if none prints) → `tests/arar/fixtures/fingerprint_before_move.txt`.
- [x] **Step 1: Write the failing tests**: `TEST(StoreLoader, LoadsFluxProductionChronology)` (today's assertions, over `extension`); `TEST(StoreSource, FingerprintUnchangedByTheMoveAndChangedByJ)` (equals the fixture under `reduction_version = "arar-1"`; a new flux revision changes it); `TEST(RecordLoader, AppliesReferencesByIdentifierAndIrradiation)`; `TEST(ArArReduceUnit, WithoutTheLoaderNothingReduces)` (store opened without the loader: every item's error is the message above).
- [x] **Step 2: FAIL; implement.** Then `grep -n 'reduction::\|arar' libs/processing/include/pychron/processing/model.hpp libs/processing/CMakeLists.txt` returns nothing.
- [x] **Step 3: `ctest --preset dev-ui`, expect PASS.**
- [x] **Step 4: Commit** `refactor(processing): Analysis carries its domain data through an extension the domain loads`

Landed differently: the store loader returns a `Result` and also receives the neutral `Analysis`; `RecordExtensionLoader` is a `{refresh, load}` pair so the references file is read again when it changes; `ReductionContext` was removed entirely; `ArArAnalysisData` also carries `constants` and `fixed_k3739`; the loaders live in their own targets, `pychron::arar_store` and `pychron::arar_records`.

### Task 11: Preset migration, rules; Phase 2 close

**Files:**
- Modify: `libs/processing/src/units.cpp:171-202` (`pipeline_from_toml`: a `reduce` node carrying a key that the registry's `arar_reduce` schema owns and `reduce`'s does not moves it to the pipeline's `arar_reduce` node and warns `'<id>': option '<key>' now belongs to arar_reduce`), `apps/pychron-ui/src/figure_window.cpp` (a saved preset loads through the same path), root `AGENTS.md` (component table row `libs/arar`, `tests/arar`, `apps/pychron-ui/src/arar` → `libs/arar/AGENTS.md`; the C++ conventions rule from spec §10; the age-cache paragraph at `:207-216` names the marker row and `derive()`), `README.md:16-26` (`libs/arar` row), `tools/spec_router/spec_router/units.toml`
- Test: `tests/processing/test_units.cpp`: `TEST(Pipeline, ReducePresetKeysMigrateToArArReduce)` (TOML with `[reduce] constants = "legacy"` over `make_arar_domain()->units()`: loads, one warning naming `constants`, the `arar_reduce` node's `constants == "legacy"`; the same TOML over `core("")` loads with the key dropped and the existing unknown-key warning); `tests/ui/test_data_windows.cpp` `figure_options_presets_and_export` extended with a pre-split preset file under the test's preset root.

- [x] **Step 1: Tests FAIL; implement; PASS.**
- [x] **Step 2: `python3 tools/quality_check.py`; `python3 tools/compiler_sweep.py --build-dir build/dev-ui`.**
- [x] **Step 3: `cmake --build --preset dev-ui && ctest --preset dev-ui`.** Also one ASan/UBSan build and run of `tests/arar` and `tests/processing` (`-DPYCHRON_SANITIZE=address,undefined`) if the `dev` preset does not set it (check `CMakePresets.json`).
- [x] **Step 4: Spec check**: §3.1 table against `git ls-files libs/arar libs/reduction libs/processing`; fix the spec where a file landed elsewhere.
- [ ] **Step 5: Commit and land** `refactor(arar): preset keys migrate to arar_reduce; rules` then push `refactor/arar-library:develop`. (Committed; the push is left to the owner.)

Landed differently: the migration rule is neutral (an option a node does not own, that the one node taking its output owns, moves there with the warning) and lives in `pipeline_from_toml` only. A figure window's preset holds the figure node's options alone and no screen saves a pipeline, so `figure_window.cpp` is unchanged and there is no UI test. The tests are in `tests/arar/test_preset_migration.cpp` (the Ar-Ar registry) and `tests/processing/test_units.cpp` `Pipeline.ReducePresetKeysStayOverCore`; over core the key is kept unread in `extra`, not dropped with a warning, because that is what unknown keys already do.

---

## Phase 3: `feat/noble-gas-domain`

### Task 12: Persistence: derived bounds and rows by name

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/store.hpp:486-495,519-521,549-551` (`DerivedBound{name,min,max}`; `BrowseFilter::derived_bounds`; `BrowseRow::derived` as `std::vector<std::pair<std::string, DerivedRow>>` or a map; `IStore::derived_fingerprints(ids, marker_name)`), `libs/persistence/src/sql/browse.hpp:20-58` (select and joins bind `?` for the name; range join per bound), `libs/persistence/src/sql/statements.hpp:207-213`, `libs/persistence/src/browse.cpp:115-139,169-170,213-275`, `libs/persistence/src/refs.cpp`
- Test: `tests/persistence/test_browse.cpp:61-91,236-310`, `tests/persistence/test_schema.cpp:232-247,331-410`

**Interfaces:**
- Produces: `BrowseFilter::derived_bounds` (vector of `DerivedBound`), `BrowseFilter::derived_names` (the names to select for each row; empty → no join at all, page included), `BrowseRow::derived`; `derived_fingerprints(std::span<const std::string> ids, std::string_view marker)`.
- Rules kept (root `AGENTS.md:207-216`): count and facet join only with a bound; page joins by primary key when `derived_names` is non-empty, with the name bound (`dv.name || '' = ?`); range join uses `derived_value_name_ix`.

- [ ] **Step 1: Rewrite the helpers at `test_browse.cpp:61-91` to take a name, keep every existing test green under `"age"`, add**: `TEST(BrowseTest, BoundsOnAnotherName)` (rows cached under `he4_moles`, bound on it filters, bound on `age` matches nothing); `TEST(BrowseTest, NoNamesNoJoin)` (empty `derived_names`: page SQL does not contain `derived_value`); plan tests in `test_schema.cpp` with the name bound keep their `USING INDEX` expectations.
- [ ] **Step 2: FAIL; implement; PASS on SQLite and with `PYCHRON_TEST_PG_URL`.** Commit `feat(persistence): browse joins derived_value by name`

### Task 13: Processing source types and the derive pass

**Files:**
- Modify: `libs/processing/include/pychron/processing/source.hpp:27-52,126-156` (`BrowseQuery::derived_bounds`, `derived_names`; `AnalysisSummary::derived`; `MemorySource::set_derived`), `libs/processing/src/source.cpp:115-117`, `libs/processing/adapters/store/src/store_source.cpp:281-282,758-780` (`to_store_filter`, `summary_from_row`, `fingerprints` with the marker), `libs/processing/adapters/store/include/pychron/processing/derive.hpp:40-58` and `src/derive.cpp:33-108` (`derive_values(IDeriveSource&, const Domain&, const DeriveOptions&, keep_going)`; returns at once with zero counts when `domain.derived()` is empty; runs `reduction_chain()` through a `Runner` at default options and writes `domain.derive()`), `apps/elctl/src/import_run.cpp:123`, `db.cpp:70`
- Test: `tests/processing/test_store_source.cpp` (derive cases), `tests/processing/test_domain.cpp`

- [ ] **Step 1: Tests**: `TEST(Derive, NobleGasWritesNothing)` (store with two analyses, `derive_values` under `noble_gas`: counts all zero, `derived_value` empty); `TEST(Derive, ArArWritesTheMarkerAndValues)` (today's `derive_ages` assertions through the domain); `TEST(MemorySource, DerivedBoundsFilterByName)`.
- [ ] **Step 2: FAIL; implement; PASS.** Commit `feat(processing): derived values by name; derive pass through the domain`

### Task 14: Browser columns and filters from `derived()`

**Files:**
- Modify: `apps/pychron-ui/src/analysis_table_model.hpp:28-60`, `analysis_table_model.cpp:138-146,180-182,227-237` (fixed columns then one per `DerivedQuantity`; `set_derived(std::vector<DerivedQuantity>)`; `format_value` replaces `format_age`), `data_browser_window.cpp:92,141-153,291-293,393-410,487-490,602,668,745,751` (filter pairs built per `filterable` quantity; `query()` fills `derived_bounds` and `derived_names`; status texts say the quantity's label), `data_workspace.cpp` (`settings_` key for a stale bound ignored when the name is not listed)
- Test: `tests/ui/test_data_windows.cpp:668-720` (`browser_shows_the_cached_age` → by name), new `browser_under_noble_gas_has_no_derived_column_or_filter`, `browser_ignores_a_saved_bound_for_an_unlisted_name` (QSettings `data_browser/derived/age_min` set, domain `noble_gas`: `query().derived_bounds` empty), `browser_toolbar_offers_every_figure_recall_and_export` unchanged.

- [ ] **Step 1: Tests FAIL; implement; PASS.** Commit `feat(ui): browser columns and filters are the domain's derived quantities`

### Task 15: Stages CSV exporter and `elctl export` under `noble_gas`

**Files:**
- Create: `libs/processing/include/pychron/processing/stages_csv.hpp`, `src/stages_csv.cpp` (`Result<void> write_stages_csv(const std::filesystem::path&, const std::vector<std::vector<std::shared_ptr<const ReducedAnalysis>>>&)`: header `uuid,runid,isotope,detector,stage,value,error,units`; one row per analysis per isotope per core stage; RFC 4180 as `report.cpp`)
- Modify: `libs/processing/src/domain.cpp` (`noble_gas` exporter `{"stages", "Export", "Export: write every core stage of the selected analyses as CSV", "CSV (*.csv)", write}`), `apps/elctl/src/export.cpp:137-199` (`--format` chooses among `domain.exporters()` ids; default the first; unknown id → `fatal` listing them)
- Test: `tests/processing/test_stages_csv.cpp` (header, row count = analyses × isotopes × 7 stages, a quoted detector name), `apps/elctl/tests/test_export_cmd.cpp` (`--domain noble_gas` writes the stages CSV; `--format schaen` under `noble_gas` fails listing `stages`)

- [ ] **Step 1: Tests FAIL; implement; PASS.** Commit `feat(processing): stages CSV export for the noble gas domain`

### Task 16: Install, wizard, doctor, e2e per domain; Phase 3 close

**Files:**
- Modify: `profiles/data-reduction/profile.toml:4` (summary without "Ar/Ar"), `apps/pychron-ui/src/brand.cpp:361`, `profiles/README.md` (`data-reduction` row), `tests/integration/test_install_e2e.cpp` (parameterised over `{"arar","noble_gas"}`: install, open the data window, under `noble_gas` the toolbar has four figure buttons and no Age column), `docs/user/06-data-analysis.md:143,260` (Age column section says "in an Ar/Ar install"), `docs/user/07-configuration-reference.md` (`domain` values and what each shows)
- [ ] **Step 1: e2e test FAIL; adjust; PASS.**
- [ ] **Step 2: `python3 tools/quality_check.py`; sweep; `cmake --build --preset dev-ui && ctest --preset dev-ui`.**
- [ ] **Step 3: Commit and land** `feat(setup): noble gas data-reduction installs` then push `feat/noble-gas-domain:develop`.

---

## Phase 4: `refactor/domain-ui`

### Task 17: `DomainUi` and the Ar-Ar registration

**Files:**
- Create: `apps/pychron-ui/src/domain_ui.hpp`, `domain_ui.cpp` (`class DomainUi { virtual std::string_view id() const; virtual void add_toolbar_actions(DataBrowserWindow&); virtual void add_menus(QMainWindow& owner, const std::string& store_url, DataWorkspace&, PresetStore&); virtual void add_preferences_pages(PreferencesDialog&); }`, `DomainUiRegistry`, `make_noble_gas_ui()` doing nothing), `apps/pychron-ui/src/arar/arar_ui.cpp` (`make_arar_ui()`: Publish toolbar action and `set_publish_handler`, `FitActions`, `PublishActions`, `EarthbankPreferencesPage`)
- Move (`git mv`): `earthbank_*`, `publish_actions`, `publish_window`, `publish_dialog`, `fit_actions`, `flux_window`, `flux_*_model`, `flux_model_support`, `release_dialog` → `apps/pychron-ui/src/arar/`; `apps/pychron-ui/CMakeLists.txt:102-107,201-206,233-288` paths
- Modify: `data_browser_window.cpp:255-257` (Publish action removed from the core toolbar; `add_toolbar_action(QAction*)` public), `preferences_dialog.cpp:201-202,304-305` (EarthBank page added by the registration), `main.cpp:289-301,675-691` (`choose_domain_ui(registry, id).add_*` in place of the direct constructions), `data_main_window.cpp`, `main_window.cpp`
- Test: `tests/ui/test_data_windows.cpp:1077-1123,1640-1666` (Publish present under `arar`, absent under `noble_gas`), `tests/ui/test_flux_window.cpp`, `test_earthbank_preferences.cpp`, `test_publish_dialog.cpp` (construct through `make_arar_ui()` where they constructed `FitActions`/pages directly), `tests/ui/CMakeLists.txt:10-37` (paths)

- [ ] **Step 1: Tests FAIL; implement; PASS.** Commit `refactor(ui): EarthBank, publish and flux behind the Ar-Ar DomainUi`

### Task 18: Docs and Phase 4 close

- [ ] **Step 1: Docs**: `docs/user/06-data-analysis.md` Publishing (`:940`) and `docs/earthbank.md`, `docs/flux.md`, `docs/export.md` open with "Ar/Ar installs only"; `docs/user/01-getting-started.md` Preferences table EarthBank row says the same; root `AGENTS.md` component rows for `apps/pychron-ui/src/arar` (→ `libs/arar/AGENTS.md`) and `libs/earthbank` row's paths.
- [ ] **Step 2: `python3 tools/quality_check.py`; sweep; `cmake --build --preset dev-ui && ctest --preset dev-ui`.**
- [ ] **Step 3: Spec check** against the tree (every path in §3.1, §4, §5.6 exists); fix the spec where not.
- [ ] **Step 4: Commit and land** `docs: domain seam user guide and rules` then push `refactor/domain-ui:develop`.

## Open questions

None.
