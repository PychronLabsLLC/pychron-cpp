# Cached ages and the browser's age filter: design

Date: 2026-10-09. Status: draft, awaiting review. Not implemented.

Second of two specs. First:
`2026-10-09-data-browser-search-display-design.md` (browser's date range,
lists, colours, time breaks). This one fills the derived-value cache of
`2026-10-01-dvc-schema-design.md` section 4.3, which has an API and a test
and no writer, and puts an age filter and an Age column on it.

## 1. Problem

- Browser cannot find analyses by age. Age exists only after reduction
  (`processing::reduce_analysis`), one analysis at a time, after a load.
- `derived_value` was designed for this (schema spec 4.3, invariant I14):
  rows keyed by analysis and input fingerprint, served only while the
  fingerprint is current. `IStore::input_fingerprint`, `put_derived`,
  `get_derived`, `prune_derived` exist. Nothing but
  `tests/persistence/test_references.cpp` calls them.

## 2. Goal

1. Every analysis that can have an age has one cached, with what it was
   computed from, soon after the analysis or its inputs change.
2. Browser: Age column; filter "age between low and high" applied in SQL,
   paged like every other filter.

Developer's choices (2026-10-09): age comes from the store, filtered
store-side; cache written by a backfill command, by every reduction the
application makes, at import, and at acquisition.

## 3. Findings that shape the design

- No one place reduces with a store in hand. `reduce_analysis` is pure.
  `ReduceUnit` and `RecallWindow` see `IAnalysisSource`, which has no cache
  methods. `StoreSource` owns the connections (pool of 2, tasks posted to
  it).
- `libs/ingest`, `libs/dvc` do not link `reduction` or `processing`, and
  must not: importer rules (AGENTS.md) want adapters and writer to decide
  from the walk alone.
- Executor does not write to the store at all. A finished run goes to
  `FilePersister` (JSON under records root); `persister.hpp` says a database
  persister comes later. So "at acquisition" has nothing to hook today
  (section 4.6).
- Fingerprint is SHA-256 over text built in C++ from heads and resolved
  references. SQL cannot tell a current row from a stale one.
  `input_fingerprint` costs about 4 statements plus payload reads per
  analysis; `StoreSource::load` already reads the same heads and resolves
  the same references.
- `derived_value` has PK `(analysis_uuid, fingerprint, name)` and index
  `derived_value_name_ix (name, value)`. Both are what the filter needs.
  **No migration.**

## 4. Design

### 4.1 What is cached

Per analysis, one fingerprint, these rows (`DerivedRow`), units text `Ma`
for ages:

| name | value | error |
|---|---|---|
| `age` | `AgeSet::age` nominal | its 1 sigma (analytical, J without error) |
| `age_w_j` | `AgeSet::age_w_j_err` nominal | its 1 sigma |
| `kca` | `ArArResult::kca` nominal | its 1 sigma |

- Analysis that reduces to no age (blank, air, no flux, `1 + JF <= 0`,
  reduction error): row `age` with `value` and `error` NULL. Means "current,
  and there is no age": backfill does not come back to it, filter never
  matches it.
- Ages always stored in Ma, whatever `ReductionConstants::age_units` a lab
  displays in. Filter and column speak Ma.
- Only a reduction with default `ReductionSettings` is cached. Fingerprint
  covers heads, references and `kReductionVersion`, not a figure's options;
  an age computed under other settings is that figure's, not the
  analysis's.

### 4.2 One fingerprint per analysis

Rule: after any write, `derived_value` holds rows of at most one
fingerprint for an analysis. Join from `analysis` to its `age` row is then
one row or none, by PK prefix.

- `put_derived` changes: in its transaction it first deletes rows of the
  analysis whose fingerprint differs from the one given, then inserts
  (`insert_or_ignore`, as now). Cache semantics of schema spec 4.3 allow it
  ("stale rows are garbage and may be deleted").
- New `IStore::put_derived_many(std::span<const DerivedPut>)`,
  `DerivedPut { Uuid analysis; Sha256Digest fingerprint; std::vector<DerivedRow> rows; }`,
  same rule, one transaction for all. Reduction version passed once.
- `prune_derived` kept; `DerivedValuesAreServedOnlyForCurrentInputs`
  updated for the new counts (a second `put` under a new fingerprint leaves
  nothing to prune).
- `tests/ingest/forwarding_store.hpp` forwards the new method.

### 4.3 Fingerprint taken at load

Race to avoid: inputs change between load and write, and old values get
filed under the new fingerprint.

- `detail::input_fingerprint` split: pure
  `fingerprint_of(heads, resolved_refs, reduction_version)` and the present
  function that reads then calls it.
- `StoreSource::load` computes it from the heads and references it has
  already read (no extra statement) and puts it on the model:
  `processing::Analysis::input_fingerprint` (`std::string`, hex; empty for
  sources that have none).
- Every cache write uses the fingerprint the analysis was loaded with.

### 4.4 The sink

`libs/processing` `source.hpp`:

    struct DerivedAge {               // what one reduction gives the cache
      std::string uuid, input_fingerprint;
      std::optional<UFloat> age, age_w_j, kca;   // age absent: "no age"
    };
    class IDerivedSink {
     public:
      virtual ~IDerivedSink() = default;
      // Queues the rows; returns at once. Failure is logged by the sink.
      virtual void put(std::vector<DerivedAge> ages) = 0;
    };
    // IAnalysisSource:
    virtual IDerivedSink* derived() noexcept { return nullptr; }

    // derived.hpp: nullopt when not cacheable (no fingerprint, or settings
    // other than default).
    std::optional<DerivedAge> derived_age(const ReducedAnalysis&, const ReductionSettings&);

- `StoreSource` implements the sink: posts one task to its pool (no future
  waited on) that calls `put_derived_many`. A write that fails is logged
  and dropped: it is a cache, next reduction or backfill fills it.
- `MemorySource`, record source: no sink.
- Sink is on real time and on StoreSource's own threads, like the rest of
  StoreSource; no simulated path waits on it.

### 4.5 Writers

1. **Every reduction the application makes.** `ReduceUnit::execute`: after
   reducing its analyses, one `put` with all that are cacheable.
   `RecallWindow::display`: one `put` for its analysis. Export goes through
   `ReduceUnit`. `flux_store.cpp` `reduce_monitor` reduces without flux on
   purpose: never cached.
   - Not written when nothing changed: `StoreSource::load` cannot know
     without a statement, so the sink's task asks first, one statement for
     the batch (`SELECT analysis_uuid, fingerprint FROM derived_value WHERE name = 'age' AND analysis_uuid IN (...)`),
     and writes only analyses whose cached fingerprint differs. Opening a
     figure of 2000 current analyses costs that one read and no write.
2. **Backfill.** `elctl db derive --db <url> [--all] [--limit N] [--quiet]`.
   `libs/processing/adapters/store` `derive.hpp`:

       struct DeriveOptions { bool all = false; std::optional<int> limit; };
       struct DeriveStats { int seen, current, written, no_age, failed; };
       Result<DeriveStats> derive_ages(StoreSource&, const DeriveOptions&,
                                       const std::function<bool(const DeriveStats&)>& keep_going);

   - Walks analyses newest first by keyset `(timestamp_utc, uuid)`, pages of
     200 through `browse`.
   - Per page: cached fingerprints in one statement; per analysis
     `input_fingerprint` (about 4 statements); only an analysis whose two
     differ (or `--all`) is loaded, reduced, and put. One
     `put_derived_many` per page.
   - Resumable by construction: a second run skips what is current. Safe to
     interrupt. One at a time per database is not required: two runs write
     the same rows.
   - An analysis that fails to load is counted `failed`, reported by run
     id, and does not stop the walk.
   - Exit status 0; 1 when any failed. Opens store with `migrate = false`.
   - `elctl db` gains the subcommand in `db.cpp`; `db_stub.cpp` unchanged.
3. **Import.** `elctl import run` calls `derive_ages` after the walk ends
   (not per batch), unless `--no-derive`. In `apps/elctl`, not `libs/ingest`:
   importer stays free of reduction, and the tests that compare stores cut
   at every batch (`same_at_every_cut`, `OneHistoryOneResult`) never see
   `derived_value` written. Flux and production imported later in the same
   walk are in place by then.
4. **Reference edits.** Saving a flux fit, blanks, IC factors or a
   production changes fingerprints of many analyses at once. Nothing is
   recomputed at save. Their cached ages stay, stale, until those analyses
   are next reduced or `elctl db derive` runs. Flux window's and fit
   windows' save messages gain: `cached ages of <n> analyses are now out of
   date; run elctl db derive` only if `n` can be had from what the save
   already knows (flux: positions of the level); else no message. User
   guide states the rule.

### 4.6 Acquisition

Developer asked for a write when the executor saves an analysis. Executor
has no store writer (section 3). This spec therefore does not touch
`libs/experiment`. Requirement recorded for the database persister's spec:
after it commits an analysis it calls the same function the importer does
for that one analysis (`derive_ages` for one uuid), outside the run's
critical path, and a failure there never fails a save.

Until then a new analysis reaches the store by import, and writer 3 covers
it.

### 4.7 Browse

Store (`libs/persistence`):

- `BrowseFilter` gains `std::optional<double> age_min, age_max` (Ma,
  inclusive). `BrowseRow` gains `std::optional<double> age, age_err`.
- `sql/browse.hpp`:
  - `kBrowseAgeJoin` =
    `LEFT JOIN derived_value dv ON dv.analysis_uuid = a.uuid AND dv.name = 'age' `
  - `kBrowseSelect` gains `dv.value AS age, dv.error AS age_err`.
  - `kBrowseAgeMin` = `dv.value >= ?`, `kBrowseAgeMax` = `dv.value <= ?`.
- Page statement always has the join (one PK probe per row of a page).
  Count and facet statements have it only when an age bound is set: they
  run over every matching analysis and must not pay a probe per row for a
  filter that is off.
- `load_analysis_detail` uses `kBrowseSelect`: gets the join too.
- Standard SQL; same text on both engines.

Processing: `BrowseQuery::age_min`, `age_max`; `AnalysisSummary::age`,
`age_err`; `matches()` applies the bounds to the summary's age (absent age
never matches a bound). `MemorySource` gets `set_age(uuid, age, err)` for
tests and previews. `to_store_filter` and the row mapping carry the four
fields.

Browser (`apps/pychron-ui`), on top of first spec:

- Table column `Age (Ma)`: `value ± error`, 4 significant figures of the
  error's scale; empty when none. Shown by default, after Extract.
- Filter column, under the date controls: `[x] Age from [ ] Ma`,
  `[x] to [ ] Ma`, same pattern as the date range (checkbox per end,
  `editingFinished`, low above high = no query and a status message).
- Column header tooltip and user guide: age is the **cached** one, computed
  when the analysis was last reduced or by `elctl db derive`; after a
  change of flux, blanks or IC factors it is out of date until then; a
  figure always recomputes.
- With an age bound set, status line: `<shown> of <total> analyses with a
  cached age in range`.

## 5. Rules that must hold

- A cache write uses the fingerprint its analysis was loaded with, never
  one computed at write time.
- At most one fingerprint per analysis in `derived_value` after any write.
- No cache write fails or delays a load, a figure, a recall, an import
  batch or a save. Errors are logged.
- `libs/ingest`, `libs/dvc`, `libs/experiment` gain no dependency.
- Count and facet statements join `derived_value` only when an age bound
  is set.
- Cached age is never used in a calculation. Figures, tables, export and
  interpreted ages reduce.

## 6. Tests

- `tests/persistence/test_references.cpp`: `put_derived` replaces another
  fingerprint's rows; `put_derived_many` one transaction, same rule;
  `fingerprint_of` equals `input_fingerprint` for the same analysis.
- `tests/persistence/test_browse.cpp`: age bounds (each end, both, NULL age
  not matched, analysis without row not matched); rows carry age; paging
  and total under an age bound; facets under an age bound.
- `tests/persistence/test_schema.cpp` `SchemaIndexes`, plans on SQLite:
  page statement reaches `derived_value` by its primary key, no `SCAN
  derived_value`; count statement without an age bound does not mention
  `derived_value`; count with one uses `derived_value_name_ix` or the
  primary key and scans neither table whole.
- `tests/processing/test_store_source.cpp`: load carries the fingerprint;
  `ReduceUnit` over a store source fills the cache; second run writes
  nothing; a flux change then a reduction replaces the row; non-default
  settings write nothing; summary carries the age.
- `tests/processing/test_derive.cpp`: fills an empty cache; second run all
  `current`; after a flux change only that level's analyses are written;
  blank gets a NULL age and is `current` next time; `--limit`; a failing
  load counted and passed over.
- `apps/elctl/tests`: `db derive` output and status; `import run` derives,
  `--no-derive` does not.
- `tests/ui/test_data_windows.cpp`: Age column text; age range sets
  `age_min`/`age_max`; low above high.
- Measurement, in commit messages (AGENTS.md, "How to know"), on a copy of
  a lab's store: `StoreLoadTiming` at 24, 400, 2000 before and after (the
  load now computes a fingerprint; the unit now reads cached fingerprints);
  browse page, count and facet times with and without an age bound;
  `elctl db derive` over the whole store, first run and second.
- Persistence tests run on PostgreSQL too before this lands
  (`PYCHRON_TEST_PG_URL`): new join, new filter.

## 7. Out of scope

- Database persister for the executor (4.6).
- Marking a cached age as stale in the browser: needs the fingerprint per
  row shown, about 4 statements each.
- Recomputing dependants when a reference is saved.
- Filters on other derived values (`kca`, radiogenic yield). Rows for `kca`
  are written so that a later filter needs no backfill.
- Ages in units other than Ma in column or filter.
- Interpreted (sample) ages: `ia_value`, a user's decision, not a cache.
