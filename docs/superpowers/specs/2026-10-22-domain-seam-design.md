# Domain seam: Ar-Ar behind an interface, noble-gas core

Date: 2026-10-22
Status: Draft (design agreed in conversation 2026-10-22, sections 1-4; nothing built)
Owner: Jake Ross
Depends on: `2026-10-18-noble-gas-abundance-reduction-design.md` (generic gas
stages; §6 here says how the two compose), `2026-10-02-data-browsing-visualization-design.md`
(units, pipeline, figures), `2026-10-09-age-cache-filter-design.md`
(`derived_value` rules), `2026-10-03-installation-wizard-design.md` (profiles),
`libs/persistence/AGENTS.md`.

## 1. Goal

Owner (2026-10-22): "a lot of features are highly ar-ar specific, for instance
ideogram and spectrum. they are part of the main data window which is useful
across all domains. what can be done to make the ui more configurable and
customizable to the domain." Target second domain: generic noble gas
(diffusion, cosmogenic He/Ne, abundances), no age.

So: core of pychron knows intensities, fits, correction stages up to IC,
time series, isotope evolutions, blank and IC reference fits, recall of
those, CSV of stages. Everything with an age, a J, a decay or interference
correction, an isochron, Schaen report, EarthBank, flux: Ar-Ar's, behind a
`Domain` interface. Second domain = one library, no UI edit. "No domain" is
itself a domain (`noble_gas`): what core gives alone.

## 2. What exists (2026-10-22, `origin/develop` dac4824e)

No domain concept in tree. Ar-Ar roots by layer:

| Layer | Where | How |
|---|---|---|
| UI chrome | `data_browser_window.cpp:218-227` toolbar (time_series, ideogram, spectrum, inverse_isochron, spectrum_isochron, isotope_evolution_fit, blank_fit, icfactor_fit, Recall, Export, Publish); `figure_window.cpp:51-55` kind → default group key; `data_workspace.cpp:187-212` kind → window class; Export tip "40Ar/39Ar data report after Schaen et al. (2021)" | string literals |
| Units | `units_builtin.cpp:451` `UnitRegistry::builtin()`: one static list | registry exists, only Ar-Ar fills it |
| Reduction | `libs/reduction`: `arar.hpp`, `arar_reduction.hpp`, `arar_types.hpp`, `flux.hpp` Ar-only; `ufloat`, `fits`, `stats` neutral. `pychron::processing` links `pychron::reduction` PUBLIC | whole library is the domain |
| Reduced model | `reduced.hpp`: `Stage` through `DecayCorrected`, `InterferenceCorrected`; `ReducedAnalysis::arar`, `constants`, `j`, `lambda_k_total`; `ReductionSettings::preset` (`ConstantsPreset`) | Ar-Ar fields in core type |
| Analysis model | `model.hpp:14` includes `arar_types.hpp`; `Analysis::flux`, `production`, `chronology`, `reactor`; `irradiation`, `level`, `position` strings | Ar-Ar inputs loaded by every source adapter |
| Quantities | `quantity.hpp`: `Named::Age`, `Kca`, ...; `Term::stage` is `Stage`; `group_stats` default quantity `"age"` | grammar names Ar-Ar values |
| Browser | `AnalysisTableModel::Age` column; age from/to filter; `AnalysisQuery::age_min/age_max`; `derived_value` name `'age'` written by derive pass (`import_run.cpp:112`, `elctl db derive`, `StoreSource`) via `derived_age()` | one hard-coded derived quantity |
| Recall | `recall_window.cpp`: Summary with ages, Isotopes with all nine stages, Error budget tab; `RecallModel` | mixed neutral and Ar-Ar |
| Export, publish | `report.cpp` (Schaen), `libs/publish` (keywords "40Ar/39Ar"), `libs/earthbank`, `earthbank_publish_dialog`, EarthBank preferences page; flux window (`fit_actions.cpp:61`) | Ar-Ar-owned, wired into core windows |
| Install | `profiles/data-reduction/profile.toml` summary "Ar/Ar"; no key chooses a domain | nothing selects |
| Store | kinds `blanks`, `icfactors`, `intercepts`, ... neutral names; `interpreted_age`, `cosmogenic` Ar-Ar | schema fine as is |

Noble-gas abundance spec (2026-10-18, agreed, unbuilt) adds generic gas
stages to neutral `libs/reduction` (`noble.hpp`), `Stage::MonitorCorrected`,
`ReducedAnalysis::gases`, `make_signal(label)`, and keeps Ar's pipeline
inside `ReducedAnalysis`. This spec moves that Ar pipeline out; §6 orders
the two.

## 3. Boundaries

### 3.1 Libraries

| Today | After |
|---|---|
| `libs/reduction`: `ufloat`, `fits`, `stats`; noble spec's `noble.hpp`, `noble_types.hpp` | stay (neutral numerics and gas stages) |
| `libs/reduction/arar_types.hpp`: `MeasuredSignal`, `IsotopeSignal`, `make_signal(label)` (noble §3.3), `baseline_corrected`, `non_detector_corrected`, `corrected_intensity`, deadtime E5 | stay, in new `libs/reduction/.../signal.hpp` |
| `libs/reduction/arar_types.hpp` rest (`ArgonIsotope`, `index`, `make_signal(ArgonIsotope)`, `ReductionInput`, `ReductionConstants`, `ConstantsPreset`, `ProductionRatios`, `Dose`, `Flux`, `make_j`), `arar_reduction.hpp` (`ArArResult`, `reduce`, decay), `flux.hpp` | → `libs/arar` (new; links `reduction`, `processing`) |
| `libs/reduction/arar.hpp`, `arar.cpp` (`compute_arar`, live conditionals), `src/arar_kernels.hpp` | stay: `libs/experiment` links them and sits below `processing` in dependency order (top `CMakeLists.txt:31`). Kernels become `include/pychron/reduction/detail/arar_kernels.hpp` so `libs/arar` reads them too. Whatever else `arar.cpp` needs stays with it. |
| `libs/processing`: `arar_figures`, `arar_groups`, `group_ages`, `report` (Schaen), `derived` (`derived_age`), `flux_fit`, `flux_view`; units ideogram, spectrum, inverse_isochron, spectrum_isochron | → `libs/arar` |
| `libs/processing` rest: `UnitRegistry`, `Pipeline`, `Runner`, select, reduce, filter, group, edits, group_stats, `time_series`, `isotope_evolution_fit`, `reference_fit`, `scene*`, `options`, `source`, `recall`, `quantity`, `model`, `reduced` | stay; names no isotope, no age, no J |
| `libs/processing` link to `pychron::reduction` PUBLIC | keeps it: `reduction` is neutral after the split |
| `libs/processing/adapters/store`, `adapters/records` | stay; load neutral `Analysis`; call the extension loader (§3.3) |
| new `libs/arar/adapters/store`, `libs/arar/adapters/records` | Ar-Ar loaders: flux, production, chronology from store rows / record files |
| `libs/publish`, `libs/earthbank`, `libs/earthbank_qt` | unchanged inside; link `pychron::arar` (`earthbank/src/mapping.cpp:1281` reduces); wired by Ar-Ar's `DomainUi` only (§5.6) |

`libs/arar` builds two targets so the move can land in steps without a
link cycle: `pychron::arar_reduction` (types, `reduce()`, decay, flux
math; links `reduction` only) and `pychron::arar` (units, figures, report,
group ages, `Domain`, loaders; links `processing` and `arar_reduction`).
Apps (`pychron-ui`, `elctl`) link `libs/arar`; `libs/processing` never does.
CMake enforces: `processing` lists no `arar` target. `arar` goes in
`PYCHRON_LIBS` (top `CMakeLists.txt:31`) after `processing`, before
`earthbank`; `tests/arar` follows from the same list.

### 3.2 Reduced model

`Stage` in core ends at `IcCorrected` (with noble spec's `MonitorCorrected`
before it). `ReducedAnalysis` loses `arar`, `constants`, `j`,
`lambda_k_total`; `ReductionSettings` loses `preset`, `include_decay_error`,
`use_irradiation_endtime`. `ReducedAnalysis` keeps its `IsotopeSignal`s
(the variables the stages were built from: `UFloat` correlation is by
process-unique variable id, `ufloat.hpp:30`, so a domain that re-minted
signals would lose correlation with the neutral stages) and gains

```cpp
struct DomainReduced {  // libs/processing reduced.hpp
  virtual ~DomainReduced() = default;
  virtual std::string_view domain() const = 0;
  // A stage of this domain for `label` ("decay_corrected"); nullopt: not one of its.
  virtual std::optional<reduction::UFloat> stage(std::string_view label, std::string_view stage) const = 0;
  // A named value ("age", "kca"); nullopt: not one of its.
  virtual std::optional<reduction::UFloat> named(std::string_view name) const = 0;
  virtual std::vector<std::string> stages() const = 0;  // names, in order after IcCorrected
  virtual std::vector<std::string> names() const = 0;
  virtual std::string_view error() const = 0;  // why no values (reduction_error today)
};
// The domain's reduction version string, part of every input fingerprint
// (`StoreSource` passes it where it passes `kReductionVersion` today) and of
// `reduction_tag`: `arar` keeps "arar-1", so cached rows survive the move;
// `noble_gas` is the noble spec's `kNobleReductionVersion`.
// Domain::reduction_version()
struct ReducedAnalysis {
  AnalysisPtr analysis;
  std::vector<reduction::IsotopeSignal> signals;  // same order as analysis->isotopes
  std::vector<IsotopeStages> isotopes;            // core stages only
  std::vector<GasResult> gases;                   // noble spec §13
  std::shared_ptr<const DomainReduced> domain;    // null under noble_gas
  std::string reduction_error;
};
```

`arar_reduce` unit (`libs/arar`): Dataset in, Dataset out; for each item
builds `ReductionInput` from `signals` plus the Ar-Ar extension of the
analysis (§3.3), runs `reduction::reduce()`, attaches `ArArReduced :
DomainReduced` holding `ArArResult`, `constants`, `j`, `lambda_k_total`,
the two Ar-Ar stages per isotope. Options: what `ReductionSettings` lost.
Values bit-identical to today: same signals, same `reduce()`. The cache
write that `ReduceUnit` makes today (`units_builtin.cpp:164-169`, through
`RunContext::source->derived()`) moves into `arar_reduce`: the unit that
makes the derived values writes them; core `reduce` writes nothing.
`reduction_tag` is `reduction_version() + "/" + options` as today.

### 3.3 Analysis model

`model.hpp` drops `#include "pychron/reduction/arar_types.hpp"` and
`Analysis::flux`, `production`, `chronology`, `reactor`. Keeps
`irradiation`, `level`, `position`, `load`: store columns, browser facets,
neutral strings (rename out of scope, §9). Gains

```cpp
struct DomainAnalysisData { virtual ~DomainAnalysisData() = default; virtual std::string_view domain() const = 0; };
std::shared_ptr<const DomainAnalysisData> extension;  // Analysis; null under noble_gas
```

Loader hook per adapter, Qt-free, persistence-free in the interface:
`StoreSource::set_extension_loader(StoreExtensionLoader)`,
`RecordDirectorySource::set_extension_loader(RecordExtensionLoader)`, each
a `std::function` over that adapter's own row or record type. `libs/arar/
adapters/store` gives `arar::store_extension_loader()` (reads flux,
production, chronology as `StoreSource` does today, same queries, same
`input_fingerprint` coverage: the fingerprint is computed over what the
loader returns too, else the age cache would not notice a J change). Apps
wire the loader for the active domain at startup; `noble_gas` wires none.

### 3.4 Quantities

`quantity.hpp` `Named` keeps neutral members only: `Timestamp`, `Aliquot`,
`StepIndex`, `ExtractValue`, `ExtractDuration`, `CleanupDuration`,
`Weight`. `Age`, `AgeWithJ`, `AgeWithPosition`, `F`, `KCa`, `CaK`, `KCl`,
`ClK`, `RadiogenicYield`, `Rad40`, `K39`, `J` go to `arar`'s `quantities()`
and `ArArReduced::named()`. A term that is not
an isotope-with-core-stage and not a neutral name is a domain term:
`Quantity::eval` asks `ReducedAnalysis::domain->named(name)` or
`->stage(label, stage)`; absent → no value, as an unknown name today.
`needs_reduction()` true for any domain term. `available_quantities()` =
core's + `Domain::quantities()` (§4). `group_stats` default quantity: the
domain's `default_quantity()` (`arar`: `age`; `noble_gas`: `He4` ratio to
normalising, first of its list); schema built when the domain builds its
registry.

### 3.5 Selection

No single main config exists: a data-reduction install has only its
`site.toml` entry (`SiteInstall`, `libs/setup` `site.hpp:33`). So the key
is the install's:

```toml
[[installs]]
name = "lab"
kind = "data_reduction"
domain = "arar"   # or "noble_gas"
```

`SiteInstall::domain` (`std::string`, default `"arar"`); `load_site` reads
it, `save_site` writes it. Absent → `"arar"`: every install today is Ar-Ar;
nothing changes for them. A profile's `profile.toml` top table gains
`domain = "arar"` (default when absent), copied to the install entry;
`data-reduction` asks (`choice` question id `domain`, choices
`["arar", "noble_gas"]`, labels "Ar/Ar geochronology", "Noble gas
(abundances, no age)", default `arar`) and the answer wins over the top
key. `doctor` checks the name is registered. `pychron-ui` reads it from the
chosen install (`main.cpp:410-426`); a run with no install (`--files`,
`--examples`) is `arar`. `elctl` resolves it with the install
(`resolve_install`); commands that take `--db` without an install take
`--domain <id>` (default `arar`). Unknown name → startup error listing
`DomainRegistry::ids()`. One binary holds every domain: compile-time
exclusion out of scope.

## 4. The `Domain` interface

Two halves, same id: `processing::Domain` (Qt-free, `libs/processing`) and
`ui::DomainUi` (Qt, `apps/pychron-ui`).

```cpp
// libs/processing/include/pychron/processing/domain.hpp
namespace pychron::processing {

struct FigureEntry {
  std::string kind;   // a unit kind in units()
  std::string label;  // toolbar / menu / palette text
  std::string icon;   // data_icon name; unknown name draws the generic figure glyph
  std::string tip;
  std::string group_key = "none";  // FigureWindow's default: "none" | "identifier" | "aliquot" | ...
  enum class Window { Figure, ReferenceFit, IsotopeEvolution } window = Window::Figure;
  bool separator_before = false;
};

// source.hpp: replaces DerivedAge {uuid, input_fingerprint, age, age_w_j, kca}.
struct DerivedValue { std::string name; std::optional<reduction::UFloat> value; std::string units; };
struct DerivedValues { std::string uuid, input_fingerprint; std::vector<DerivedValue> values; };
// IDerivedSink::put(std::vector<DerivedValues>); StoreSource writes one derived_value row per value,
// a valueless row when `value` is absent (today's "no age" row).

// What the derive pass writes to derived_value and the browser may show.
struct DerivedQuantity {
  std::string name;   // derived_value.name ("age")
  std::string label;  // column header ("Age (Ma)")
  std::string units;
  bool filterable = true;  // browser's from/to filter
};

struct Exporter {
  std::string id, label, tip;
  std::string filters;  // file dialog filter ("CSV report (*.csv);;JSON report (*.json)")
  // Groups as export_report groups them today (by aliquot when any is a heating step, else by identifier).
  std::function<Result<void>(const std::filesystem::path&,
                             const std::vector<std::vector<std::shared_ptr<const ReducedAnalysis>>>&)> write;
};

class Domain {
 public:
  virtual ~Domain() = default;
  virtual std::string_view id() const = 0;     // "noble_gas", "arar"
  virtual std::string_view title() const = 0;
  // Core builtins plus this domain's units; built once.
  virtual const UnitRegistry& units() const = 0;
  // Unit kinds after "select", in order, before filter/group ("reduce" | "reduce", "arar_reduce").
  virtual std::vector<std::string> reduction_chain() const = 0;
  virtual std::vector<FigureEntry> figures() const = 0;  // toolbar order
  virtual std::vector<std::string> quantities() const = 0;  // names and stages Quantity may use beyond core's
  virtual std::string default_quantity() const = 0;
  // Browser column and filter; empty → no column, no filter, no cache join.
  // First entry is the marker row: written for every derived analysis, valueless when there is
  // no value, and the name fingerprint lookups use (`derived_fingerprints`, today 'age').
  virtual std::vector<DerivedQuantity> derived() const = 0;
  // Nullopt when not cacheable (no input fingerprint); else one DerivedValues per derived().
  virtual std::optional<DerivedValues> derive(const ReducedAnalysis&) const = 0;
  virtual std::string_view reduction_version() const = 0;  // §3.2
  // Recall window: sections after the neutral ones, and the error budget (empty: no tab).
  virtual std::vector<RecallSection> recall_sections(const ReducedAnalysis&) const = 0;
  virtual std::vector<RecallErrorComponent> error_budget(const ReducedAnalysis&) const = 0;
  virtual std::vector<Exporter> exporters() const = 0;
};

class DomainRegistry {  // filled by the app at startup: apps link libs/arar, processing never does
 public:
  void add(std::unique_ptr<Domain>);  // replaces one of the same id
  const Domain* find(std::string_view id) const;
  std::vector<std::string> ids() const;
};

std::unique_ptr<Domain> make_noble_gas_domain();  // core only
}  // namespace pychron::processing

// libs/arar/include/pychron/arar/domain.hpp
namespace pychron::arar {
std::unique_ptr<processing::Domain> make_domain();
}
```

`noble_gas`: units = `UnitRegistry::builtin()` (after §3.1 that is select,
reduce, filter, group, edits, group_stats, time_series, blank_fit,
icfactor_fit, isotope_evolution_fit, and the noble spec's fit targets as
they land); `reduction_chain` = `{"reduce"}`; figures = time series,
isotope evolutions, blanks, IC factors; quantities = noble spec §9 names;
derived = none; recall sections = none; error budget = none; exporters =
stages CSV (one row per analysis per isotope, every core stage, value and
error; new, small).

`arar`: units = builtin + `arar_reduce`, ideogram, spectrum,
inverse_isochron, spectrum_isochron, flux units; `reduction_chain` =
`{"reduce", "arar_reduce"}`; figures = today's toolbar, same order, same
labels, same tips, group keys from `figure_window.cpp:51-55`; quantities =
`age`, `kca`, `kcl`, `rad40`, ..., stages `decay_corrected`,
`interference_corrected`; derived = `age` ("Age (Ma)", filterable, marker), `age_w_j`, `kca` (not
filterable, not shown; written today, kept); recall sections = today's Summary rows
after the five neutral ones (computed values, corrected ratios); error
budget = `ArArResult::age_error_components`; exporters = Schaen CSV/JSON.

`ui::DomainUi`, registry filled in `main.cpp` beside the other:

- extra toolbar actions after Export (`arar`: Publish to EarthBank, opens
  `EarthbankPublishDialog`),
- extra windows and menu entries (`arar`: Flux window; `fit_actions.cpp`
  moves under it),
- extra Preferences pages (`arar`: EarthBank page),
- `noble_gas`: none.

Plumbing: `DataWorkspace`, `ProcessingBridge`, `FigureWindow`,
`ReferenceFitWindow`, `IsotopeEvolutionWindow`, `RecallWindow`, `elctl`
take `const Domain&` where they take `UnitRegistry::builtin()` or hard-code
kinds today. Browser toolbar, View menu, command palette: one action per
`FigureEntry`, in order, separators as flagged. `data_workspace.cpp`
dispatches on `FigureEntry::window`; `FigureWindow::default_group_key`
gone. Pipelines built as select → `reduction_chain()` → filter → group →
edits → figure.

Presets (`PresetStore`): per kind, unchanged. A saved preset for node
`reduce` carrying keys that `arar_reduce`'s schema owns after the split
(`preset`, `include_decay_error`, `use_irradiation_endtime`) is read as
`arar_reduce`'s, once, with a `pipeline_from_toml` warning naming the key;
written back under the new node on next save.

Not in the interface: options editor (schema-driven), scene drawing
(kind-free), persistence, scripting (instrument side, domain-free).

## 5. Where the domain shows

### 5.1 Browser

Fixed columns stay: Run ID, Type, Sample, Identifier, Spec., Date, Extract,
Tag, Project, Irradiation, Uuid. Columns after Extract from
`domain.derived()`: `arar` → "Age (Ma)"; `noble_gas` → none. Filter panel:
one from/to pair per `filterable` derived quantity; none → no form.
`BrowseQuery::age_min/age_max` (`source.hpp:35`) →
`derived_bounds: std::vector<DerivedBound{name, min, max}>`;
`AnalysisSummary::age/age_err` (`source.hpp:51`) →
`derived: std::map<std::string, Value>` for the names the domain lists;
`MemorySource::set_age` → `set_derived(uuid, name, value, err)`.
Persistence mirrors: `BrowseFilter::derived_bounds`, `BrowseRow::derived`
(`store.hpp:519-551`); `kBrowseSelect`, `kBrowseAgeJoin`,
`kBrowseAgeRangeJoin` (`sql/browse.hpp:20-58`) and `derived_fingerprints`
(`sql/statements.hpp:207`) take the name as a bound parameter; age cache spec rules keep their shape: join `derived_value` only
when a bound is set; page joins by primary key with the name bound
(`dv.name || '' = ?`); never a hard-coded `'age'`.

### 5.2 Derive pass

`derive_ages` (`adapters/store` `derive.cpp:33`, called by
`import_run.cpp:123` and `db.cpp:70`) becomes `derive_values(source,
domain, options, keep_going)`: runs `reduction_chain()` at default options
and writes `domain.derive()`'s result through `write_derived_now`. With
`domain.derived()` empty the pass returns at once with zero counts, so
`noble_gas` writes nothing; the three cache rules (fingerprint from load,
one fingerprint per analysis, join rules) hold unchanged. The in-run write
(§3.2) and `RecallWindow::display` (`recall_window.cpp:305`) go through the
same `derive()`.
`derived_age()` moves to `libs/arar` as `arar`'s `derive()`; `DerivedAge`
(`source.hpp:90`) becomes `std::vector<DerivedValue>` and the source's cache
write takes that. `derived_age()`'s "nullopt when settings are not the
default ones" becomes: the derive pass runs `reduction_chain()` at default
options only, as it does today.

### 5.3 Recall

Neutral tabs: Summary (run id, sample, type, date, spectrometer, then
`domain.recall_sections()`), Isotopes (core stages; `Stage` combo lists
core stages), Evolutions, Identity, Extraction, Spectrometer, History.
Error budget tab only when `error_budget()` non-empty. Ar-Ar's two extra
stages appear in Ar-Ar's Summary sections as corrected-ratio rows, not in
the Isotopes stage table: `RecallIsotope::stages` stays keyed by core
`Stage`.

### 5.4 Figures and export

Toolbar/menu/palette per §4. Export: one action per `Exporter`; file dialog
uses its `filters`. Icons: `data_icons.cpp` keeps its drawings by name;
unknown name → generic figure glyph.

### 5.5 elctl

`elctl export`, `elctl flux`, `elctl db derive`, `elctl import` take the
domain (§3.5: from the install, else `--domain`); `flux` under a domain
without it fails with
`elctl flux: needs domain arar (this install: noble_gas)`. `elctl import`
derive pass per §5.2.

### 5.6 Ar-Ar UI pieces

EarthBank (toolbar action, dialog, worker, preferences page, no-network
notice), flux window and `fit_actions.cpp`, publish actions: registered by
`arar`'s `DomainUi`; files stay under `apps/pychron-ui/src`, moved into
`apps/pychron-ui/src/arar/`. `libs/publish` release model: reached from
Ar-Ar's publish actions only.

### 5.7 Setup

Per §3.5. Setup wizard's data-reduction summary text loses "Ar/Ar".
`profiles/README.md` row for `data-reduction`: "a data folder, a local or
server database, and the domain".

## 6. Composition with the noble-gas abundance spec

Both touch `reduced.hpp`, `Stage`, `make_signal`. Order:

1. Noble spec item 1 (`make_signal(label)`, tag fix, `Stage::MonitorCorrected`,
   `IsotopeSignal::interference`, N3-N4 parity guard): first. It creates
   the neutral `make_signal` §3.1 keeps and touches nothing this spec
   moves.
2. This spec §7 steps 1 and 2 (seam, file move): next, before noble items
   2-11 add code to files step 2 moves.
3. Noble items 2-11 and this spec steps 3-4: independent after that.
   `GasResult` lands in neutral `ReducedAnalysis` (§3.2) as the noble spec
   says; its figures (three-isotope, Arrhenius, ratio isochron; noble
   §2.2) are `noble_gas`'s `figures()` when their spec comes.

Noble spec §13 "`compute_arar` unchanged, live path nothing": unchanged
here too (`libs/experiment`, conditionals: instrument side, out of scope).

## 7. Order of work

Four branches, each on `develop` with Ar-Ar behaviour unchanged (same
figures, same report bytes, same cache rows) until step 3 adds the second
domain. Spec and code change together.

1. `feat(processing): domain seam`. `Domain`, `DomainRegistry`,
   `make_noble_gas_domain()`, an `ArArDomain` inside `libs/processing` for
   now answering with what is hard-coded today (toolbar list, `age`,
   Schaen, recall summary). UI, `elctl`, the install's `domain` key read
   it. No file moves. Toolbar, menus, palette, dispatch data-driven; recall
   window fills its Summary and Error budget from `recall_sections()` and
   `error_budget()`. `reduction_chain` = `{"reduce"}` still.
2. `refactor(arar): own library`. §3.1 moves; §3.2 `ReducedAnalysis` split
   and `arar_reduce`; §3.3 `Analysis::extension` and loaders; §3.4
   quantities; preset key migration; `RecallModel` loses its Ar-Ar rows
   (the domain computes them) and the recall window runs the chain.
   `tests/arar` holds the moved tests (golden vectors unchanged, compared
   as today). Largest step, in six commits (plan).
3. `feat(ui): noble gas domain`. §5.1 browser, §5.2 derive, stages CSV
   exporter, `data-reduction` profile question, `doctor` check,
   `elctl flux` refusal.
4. `refactor(ui): domain ui`. `DomainUi` registry; §5.6.

## 8. Testing

Every existing test keeps passing through 1, 2, 4. New:

- `tests/processing/test_domain.cpp`: registry add/find/replace; `noble_gas`
  runs every core unit end to end on an argon-free fixture (He3, He4, Ne20,
  Ne21, Ne22; a blank; an IC factor), derives nothing, recalls with no
  error budget; `Quantity` of a domain term under `noble_gas` has no value
  and no crash.
- `tests/arar`: moved tests; `arar_reduce` after `reduce` gives the golden
  `ArArResult`s; a ratio of a core stage to an Ar-Ar stage carries the
  shared variables (error equals today's to 1e-15).
- `tests/ui`: toolbar actions equal `figures()` in order; no age column or
  filter under `noble_gas`; Ar-Ar toolbar unchanged (pinned list of labels
  and kinds); a figure of each `Window` opens from its entry.
- `tests/persistence`: count/facet/page with a derived bound of another
  name; no join without a bound (existing test, name parameter).
- `tests/setup`: `data-reduction` with noble gas renders, loads, doctor
  passes; `test_install_e2e` once per domain.
- `elctl`: `flux` under `noble_gas` fails with the message in §5.5;
  `export` under `noble_gas` writes the stages CSV.
- Preset migration: a preset file with `reduce.preset = "..."` loads with
  one warning and runs `arar_reduce` with it.

## 9. Out of scope

Compile-time exclusion of Ar-Ar (`-DPYCHRON_ARAR=OFF`); loadable plugins
(shared libraries, Python); renaming Irradiation, level, position; a real He
or Ne age or diffusion reduction (`noble_gas` is core only until the noble
spec's items land); instrument side (plans, spectrometer, extraction line,
conditionals, `compute_arar`); MassSpec; `libs/publish` beyond §5.6;
EarthBank for any domain but Ar-Ar; translation of figure and exporter labels
(they come from the domain as plain strings; no translations exist).

## 10. Rules (added to root `AGENTS.md` when step 2 lands)

- `libs/processing`, `libs/reduction` outside `libs/arar`, `apps/pychron-ui`
  outside `src/arar/`, `apps/elctl` outside `flux`: name no isotope, no
  age, no J. What does lives in `libs/arar` or Ar-Ar's `DomainUi`.
- A new figure, exporter, derived quantity or recall section is a
  `Domain` entry, never a literal in a window.
- `UnitRegistry::builtin()` holds core units only; a domain's registry is
  `Domain::units()`.
- Table row: `libs/arar`, `tests/arar`, `apps/pychron-ui/src/arar` → read
  `libs/arar/AGENTS.md` (made in step 2 from `libs/processing/AGENTS.md`'s
  report and flux rules, which move with the code).

## Open questions

None.
