# Sample and irradiation (package) entry

Date: 2026-10-04
Status: Proposed
Owner: Jake Ross
Depends on: `2026-10-01-dvc-schema-design.md` (catalog tables 3.1-3.3,
reference data 6, identifier reservation 8.6, catalog audit D6, roles 11.2),
`2026-10-03-legacy-ingestion-design.md` (what the importer already puts in
the catalog), `2026-10-02-data-browsing-visualization-design.md` (the store
worker-thread pattern of `StoreSource`).
Scope: entering and editing principal investigators, projects, materials and
samples; entering irradiations ("packages"), their chronology, levels,
productions and positions; assigning samples to positions; generating
identifiers (labnumbers). Store additions, a Qt-free `libs/entry`, `elctl
entry` commands and two `pychron-ui` windows.
Out of scope: flux fitting and monitor ages (J is shown, never edited here),
loads and load positions (loading task), sample prep and IR tables (DVC spec
3.9), IGSN registration, the git publisher (D5 of the DVC spec).

Legacy citations are `file:line` relative to the `pychron/` package of the
legacy Python repo (read-only).

## 1. Goal

A lab whose catalog lives in the DVC store can enter new samples and a new
irradiation without the legacy Python application, and every identifier it
runs afterwards resolves to the right sample.

Success: from an empty SQLite store, `elctl entry samples import
samples.csv` creates 40 samples with their PIs, projects and materials;
Entry > Irradiations creates `NM-301` with a chronology and levels A-C on a
24-hole holder; samples are assigned to positions; Generate Identifiers
numbers them continuing from the highest identifier already in the store;
and the data browser filtered by irradiation `NM-301` lists those samples
once their analyses arrive.

## 2. What legacy does (summary of the survey)

"Package" is the legacy UI label for an irradiation. In `simple` mode the
package editors skip the chronology and production
(`entry/labnumber_entry.py:1012-1029`, `entry/editors/irradiation_editor.py:86-148`,
`entry/editors/package_level_editor.py:103-118`). "Packet" is a per-position
text column (`dvc/dvc_orm.py:436`). There is no locking or permission check
anywhere in `entry/`.

| Area | Legacy behaviour | Problems to avoid |
|---|---|---|
| Sample task | PI, project, material and sample staged as in-memory specs, saved together (`entry/tasks/sample/sample_entry.py:569-669`); fuzzy duplicate prompt (`:948-960`); CSV import with header aliases, row validation and a create/skip/error preview (`entry/tasks/sample/importer.py:33-465`); edit form that renames and moves samples (`entry/tasks/sample/sample_edit_view.py:243-282`) | Fields from the detail dialog silently dropped on add (`sample_entry.py:927-944`); material lookup ignores grainsize (`:822-831`); `todump` returns nothing (`:184-191`); project regex never enforced (`:90-91`); `get_project(name)` without PI picks the first match (`dvc/dvc_database.py:2014-2030`) |
| Irradiation | name auto-increment by prefix (`labnumber_entry.py:792-870`), no spaces, unique; chronology rows of power, start and end (`entry/editors/chronology.py`); reactor defaults (`entry/editors/irradiation_editor.py:203-213`) | chronology and reactor from the Add dialog discarded (`irradiation_editor.py:145-148`); `dump_chronology` converts the start twice and never the end (`dvc/meta_repo.py:68-82`) |
| Levels | next letter, same z and tray as the last level (`package_level_editor.py:161-178`); name, production and tray required (`entry/editors/level_editor.py:230-237`) | rename changes the DB but not the meta file (`level_editor.py:290-305`); level notes lost (`labnumber_entry.py:891-893`, `dvc/meta_object.py:245-252`); tray shrink "orphan" branch deletes nothing (`level_editor.py:354-413`) |
| Positions | rows built from the holder's holes, DB rows matched by index (`labnumber_entry.py:634-642`, `:941-1010`); assign the selected sample to selected rows (`entry/tasks/labnumber/task.py:488-511`); packet auto-increment (`task.py:450-486`) | save **deletes** every row without a sample, identifier and all (`labnumber_entry.py:705-707`, `dvc/dvc.py:1069-1076`); an identifier clash aborts mid-loop with nothing committed (`:727-736`) |
| Identifiers | monitor and unknown streams, `offset` and `level_offset`, overwrite prompt (`entry/identifier_generator.py:242-356`) | level order is ORM relationship order (`dvc/dvc_orm.py:422-423`); preview numbers differ from the real run (`identifier_generator.py:185-197`); "last identifier" is `ORDER BY abs(identifier)` on text (`dvc/dvc_database.py:2084-2092`); no lock, so two users can hand out the same numbers |
| Productions | nine ratios with errors, edited in the level editor, saved per irradiation, assigned per level, reactor defaults in `reactors.json` (`entry/editors/production.py:49-184`, `level_editor.py:445-590`) | |
| Holders | text files `shape,radius[,has_hole_number]` then `x,y[,r]` or `id,x,y` (`dvc/meta_object.py:260-299`) | TrayMaker writes files the parser cannot read (`entry/tray_maker.py:112-118`, `dvc/meta_repo.py:413-417`) |
| Export | level PDF with canvas and table (`entry/irradiation_pdf_writer.py:111-289`), MassSpec/XML/YAML/XLS exporters | XLS import and the irradiation table writer are broken or debug-only (`entry/xls_irradiation_source.py:22`, `entry/irradiation_xls_writer.py:55-95`) |

## 3. Decisions

| # | Decision |
|---|---|
| E1 | **The store is the only place entry writes.** No meta-repo files, no git. The publisher (DVC spec D5) mirrors irradiations to the meta repo when it lands. Section 11 Q1 covers labs that still run legacy acquisition. |
| E2 | **Catalog edits use optimistic concurrency on field values.** Catalog rows are not revisioned (D6) and have no head to compare. An edit carries the values it was made against; the update matches on them (`IS NOT DISTINCT FROM`). If another client changed any of those fields first, the whole save is refused and the stale rows are returned with their current values. There is no silent last-writer-wins path. |
| E3 | **A save is one transaction.** A level sheet, a sample import or an identifier generation either lands entirely or not at all, with one `change_log` entry (kind `catalog`) and a field diff per row in `change_entity.detail` (D6). |
| E4 | **Reference data stays revisioned.** Chronology, productions, the level's production assignment, the level's z and holder geometry are `ref_object` revisions with CAS on the head (DVC spec 6), staged through the existing unit of work. A level save that changes both catalog rows and references commits them in one transaction (section 5.3). |
| E5 | **What has been analyzed is protected.** An identifier with analyses, a load position or a lease keeps its text and its position forever; it cannot be deleted, overwritten or moved. Clearing a position's sample never deletes its identifier. Changing the sample of an analyzed position is allowed, because legacy needs it to fix entry mistakes, but only with an explicit flag that the UI asks for, naming the number of analyses affected. |
| E6 | **Identifiers come from `identifier_counter`** (DVC spec 8.6), with a CAS on the counter value the preview was made from. Preview and commit run the same pure planner, so the preview is what gets written. A counter that moved since the preview refuses the commit and the UI re-previews. Entry requires the server; nothing is allocated offline. |
| E7 | **Deterministic order.** Levels are numbered in name order and positions in position order. Nothing depends on row order from the database. |
| E8 | **Qt-free core.** Validation, CSV parsing, the identifier planner, name increments and the level-sheet edit model live in a new `libs/entry` (depends on `persistence` and `core`; holder text parsing from `dvc`). The UI and `elctl` are thin. |
| E9 | **No XLS.** Bulk input is CSV or TSV, from a file or pasted from a spreadsheet. Export is CSV and a PDF level sheet. |
| E10 | **A level's z lives only in its `level_geometry` reference**, as the importer already does (`libs/dvc/src/meta_adapter.cpp:258-263`). The `level.z` column is left NULL by entry. |
| E11 | **Renames are cheap until the first analysis.** An irradiation or level can be renamed while no identifier in it has an analysis. The rename rewrites the `ref_object.key` of every reference scoped to it in the same transaction. After the first analysis a rename is refused. |
| E12 | **Lab entry settings are shared.** The identifier streams, irradiation prefix, monitor sample and other options (section 7) are one `document` reference, `pychron/entry_settings.json`, so every client numbers the same way and changes are audited. |

## 4. Data model use

No new tables. Entry writes these existing rows:

| Thing | Rows | Natural key |
|---|---|---|
| PI | `principal_investigator` | (`last_name`, `first_initial`) |
| Project | `project` | (`name`, `pi_uuid`) |
| Material | `material` | (`name`, `grainsize`) |
| Sample | `sample` (+ `updated_utc`) | (`name`, `project_uuid`, `material_uuid`) |
| Irradiation | `irradiation` | `name` |
| Chronology | `ref_object` `chronology` `<irrad>` + `chronology_dose` revisions | |
| Production | `ref_object` `production` `<irrad>/<name>` + `production_meta`, `production_value` | |
| Level | `level` (`holder_ref_uuid`, `note`) | (`irradiation_uuid`, `name`) |
| Level z | `ref_object` `level_geometry` `<irrad>/<level>` + `level_z_value` | |
| Level production | `ref_object` `level_production` `<irrad>/<level>` + `level_production_value` | |
| Position | `irradiation_position` (`sample_uuid`, `weight`, `packet`, `note`) | (`level_uuid`, `position`) |
| Identifier | `identifier` (`kind = unknown`, `position_uuid`) | `identifier` |
| Holder | `ref_object` `irradiation_holder` `<name>` + `holder_meta`, `holder_hole` | |
| Reactor defaults | `ref_object` `document` `reactors.json` | |
| Settings | `ref_object` `document` `pychron/entry_settings.json` | |

Position `n` is the holder's `n`-th hole, `holder_hole.ordinal = n - 1`
(legacy matches by index, `labnumber_entry.py:941-1010`). For a holder with
hole numbers the grid shows `hole_id` beside the position. A position row exists only once something is
entered for it. A row whose position is beyond the holder's hole count, after
a holder change, is shown as an orphan and can be moved or cleared, never
dropped silently.

The sample of an analysis is its identifier's position's sample, else its
identifier's own sample (browse design 9.2). Entry sets `identifier.sample_uuid`
only for identifiers without a position, which entry does not create.

## 5. Store additions (`libs/persistence`)

### 5.1 Reads

New header `pychron/persistence/catalog.hpp`, std-only like the others, and
these `IStore` methods:

```cpp
struct SampleRow {
  Uuid uuid;
  std::string name, project, principal_investigator, material, grainsize;
  Uuid project_uuid, material_uuid;
  std::optional<Uuid> pi_uuid;
  SampleFields fields;               // note, igsn, lat, lon, elevation, ... (the SampleSpec optionals)
  UtcTime updated;
  int n_positions = 0, n_analyses = 0;
};
struct SampleQuery {
  std::string text;                  // case-insensitive substring of the name
  std::optional<Uuid> pi, project, material;
  int limit = 500;
};
struct IrradiationRow { Uuid uuid; std::string name; UtcTime created; int n_levels = 0;
                        int n_positions = 0; int n_analyzed = 0; bool has_chronology = false; };
struct LevelRow { Uuid uuid; std::string name; std::optional<Uuid> holder; std::optional<std::string> holder_name;
                  std::optional<std::string> note; };
struct PositionRow {
  Uuid uuid; int position = 0;
  std::optional<Uuid> sample;  std::string sample_name, project, principal_investigator, material, grainsize;
  std::optional<double> weight; std::optional<std::string> packet, note;
  std::optional<Uuid> identifier_uuid; std::optional<std::string> identifier;
  int n_analyses = 0; bool in_load = false;
  std::optional<double> j, j_err;    // head of the position's flux_position reference, read-only
};
struct LevelSheet {
  LevelRow level;
  std::vector<PositionRow> positions;              // by position
  std::map<RefType, std::pair<Uuid, Uuid>> heads;  // level_geometry, level_production: ref_object, head revision
  std::optional<LevelZValue> z;
  std::optional<LevelProductionValue> production;
};

Result<std::vector<PrincipalInvestigatorRow>> principal_investigators();
Result<std::vector<ProjectRow>> projects(std::optional<Uuid> pi);
Result<std::vector<MaterialRow>> materials();
Result<std::vector<SampleRow>> samples(const SampleQuery&);
Result<std::vector<IrradiationRow>> irradiations();        // newest first
Result<std::vector<LevelRow>> levels(Uuid irradiation);    // by name
Result<std::optional<LevelSheet>> level_sheet(Uuid level);
Result<std::optional<std::int64_t>> identifier_counter(const std::string& scope);
Result<std::int64_t> max_numeric_identifier(std::int64_t floor, std::optional<std::int64_t> ceiling);
```

Counts come from joins in one statement each (no N+1). All SQL in
`src/sql/catalog.hpp` (P6).

### 5.2 Catalog edits (E2, E3)

```cpp
using CatalogValue = std::variant<std::monostate, std::string, double, std::int64_t, bool, Uuid>;
using CatalogFields = std::map<std::string, CatalogValue>;   // column name -> value

struct CatalogInsert { CatalogTable table; Uuid uuid; CatalogFields values; };
struct CatalogUpdate { CatalogTable table; Uuid uuid; CatalogFields expected, values; };
struct CatalogDelete { CatalogTable table; Uuid uuid; CatalogFields expected; };
using CatalogEdit = std::variant<CatalogInsert, CatalogUpdate, CatalogDelete>;

struct CatalogEditBatch {
  std::vector<CatalogEdit> edits;          // applied in order; later edits may name earlier inserts
  bool allow_analyzed_sample_change = false;  // E5
  std::string message;                     // goes to change_log detail
};
struct StaleRow { CatalogTable table; Uuid uuid; CatalogFields expected, actual; };  // actual empty: row gone
struct Refusal { CatalogTable table; Uuid uuid; std::string rule; std::string what; };
struct CatalogApplied { ChangeSeq seq = 0; };
using CatalogOutcome = std::variant<CatalogApplied, std::vector<StaleRow>, std::vector<Refusal>>;

Result<CatalogOutcome> apply_catalog_edits(Uuid client, const CatalogEditBatch&);
```

- Editable columns are a fixed per-table allowlist in `src/sql/catalog.hpp`.
  A column outside it, a wrong value type or an unknown table is an `Error`
  (`Protocol`), not a refusal.
- Tables: `principal_investigator`, `project`, `material`, `sample`,
  `irradiation`, `level`, `irradiation_position`, `identifier`.
- `CatalogUpdate` runs `UPDATE ... SET <values> WHERE uuid = :u AND <each
  expected column> IS NOT DISTINCT FROM :e` (SQLite: `IS`). Zero rows
  affected is a `StaleRow` with the row's current values. Fields not in
  `expected` are not compared, so two users editing different columns of one
  sample do not conflict.
- Any stale row or refusal rolls back the batch; all of them are reported,
  not only the first.
- Unique violations on insert or update (a second sample with the same name,
  project and material; a position number taken) are `Refusal{rule =
  "unique"}`, using the existing constraint classification
  (`is_refused_catalog_fill`'s codes).
- Rules checked in the transaction, as `Refusal`s (E5, E11):
  - `analyzed_identifier`: update or delete of an `identifier` that has an
    analysis, a `load_position` or an `aliquot_lease` row.
  - `analyzed_sample_change`: update of `irradiation_position.sample_uuid`
    where the position's identifier has analyses, unless
    `allow_analyzed_sample_change`.
  - `position_has_identifier`: delete of a position that has an identifier.
  - `in_use`: delete of a PI, project, material or sample that rows still
    reference (checked explicitly for a message that names them, before the
    foreign key would fire).
  - `analyzed_rename`: rename of an irradiation or level that has an
    analyzed identifier.
- A rename of an irradiation or level also rewrites `ref_object.key` for
  every reference scoped to it (`irradiation_uuid` or `level_uuid`), in the
  same transaction.
- `sample.updated_utc` is set to the write time on every sample update.
- Each edited row gets a `change_entity` row with `detail = {"op", "before",
  "after"}` holding only the changed columns. One `change_log` row of kind
  `catalog` per batch.

The existing ensure-style `add_*` calls stay as they are; the importer and
acquisition keep using them.

### 5.3 Catalog edits with reference revisions

A level save may change catalog rows (position samples, level note) and
references (z, production assignment) together. `apply_catalog_edits` gets an
overload that takes an `IUnitOfWork&` holding staged revisions, and commits
both in one transaction: catalog edits first, then the section 5.4 CAS head
moves of the DVC spec. A head conflict rolls everything back and returns the
`Conflict`s as a fourth outcome alternative:

```cpp
using CatalogOutcome = std::variant<CatalogApplied, std::vector<StaleRow>, std::vector<Refusal>,
                                    std::vector<Conflict>>;
Result<CatalogOutcome> apply_catalog_edits(const Actor&, const CatalogEditBatch&, IUnitOfWork& refs,
                                           ChangesetKind kind, std::string message);
```

### 5.4 Identifier allocation (E6)

```cpp
struct IdentifierAssignment { Uuid position; std::int64_t number; std::optional<Uuid> replaces; };
struct IdentifierAllocation {
  std::string scope;                        // "unknown", "monitor" (section 7)
  std::int64_t floor = 1;                   // the stream's first number
  std::optional<std::int64_t> ceiling;      // exclusive; the next stream's floor
  std::int64_t expected_last = 0;           // the counter value the plan was made from
  std::vector<IdentifierAssignment> assignments;
};
struct AllocationStale { std::int64_t actual_last = 0; };
using AllocationOutcome = std::variant<CatalogApplied, AllocationStale, std::vector<Refusal>>;
Result<AllocationOutcome> allocate_identifiers(Uuid client, const std::vector<IdentifierAllocation>&);
```

In one transaction, per allocation in scope order:

1. Lock the counter row (`SELECT ... FOR UPDATE`; the SQLite transaction is
   already `BEGIN IMMEDIATE`). When the row is absent, seed it: `last_value =
   max(floor - 1, max numeric identifier in [floor, ceiling))`, where numeric
   means the text is all ASCII digits without a leading zero, compared as an
   integer. This replaces legacy `ORDER BY abs(identifier)`.
2. `last_value != expected_last`: `AllocationStale`, roll back.
3. For each assignment: a number outside `[floor, ceiling)`, a number at or
   below `expected_last`, or a text already in `identifier` is a refusal. A
   `replaces` identifier must be the position's current one and must pass
   the `analyzed_identifier` rule; it is updated in place (same uuid, new
   text), so nothing else that names it changes. Otherwise a new `identifier`
   row is inserted.
4. `last_value = max(number)` of the allocation.

The importer does not touch `identifier_counter`, so after an import the
first allocation seeds from the imported identifiers. A test pins that.

### 5.5 Migration

`0003_entry.sql`: an index for sample search, `CREATE INDEX sample_name_lower_ix
ON sample (lower(name))`, and the same on `project`. Additive (11.4).
Regenerate the SQLite file with `tools/ddl_sqlite.py`.

## 6. `libs/entry` (Qt-free)

Built only with persistence, like `libs/ingest`. Links `pychron::persistence`
and `pychron::core`, and `pychron::dvc` privately for `parse_holder` and
`parse_chronology`, which move from `libs/dvc/src/meta_layout.hpp` to a public
`pychron/dvc/meta_files.hpp`.

| Unit | Responsibility |
|---|---|
| `names.hpp` | validators and generators: PI (`^[A-Z][A-Za-z'\-]+(, ?[A-Z])?$` or an allowed lab name, as `sample_entry.py:60-83`, "Last, F" parsed into `last_name`, `first_initial`); project (`^[A-Za-z][-\w]*$`, now enforced); irradiation (no whitespace, the prefix increment of `labnumber_entry.py:792-870`: `NM-001`, `NM001`, `NM-ABC-001`, zero-padded to the existing width, at least 3); level next letter (`A..Z`, then `AA`); packet (`^[A-Za-z]*\d+$`) and packet sequences (`P7` -> `P8`). Every name is trimmed; internal runs of spaces in sample names are kept. |
| `sample_fields.hpp` | numeric and range checks: lat in [-90, 90] and lon in [-180, 180], both or neither; UTM easting, northing and zone (`\d{1,2}[C-X]`) converted to WGS84 lat/lon (own Transverse Mercator code, no proj dependency) and only when lat/lon are empty, as `importer.py:393-451`. |
| `csv.hpp` | RFC 4180 reader with delimiter sniffing over `, ; \t |` (`importer.py:308-313`), BOM and CRLF handled. TSV pasted from a spreadsheet goes through the same reader. |
| `sample_import.hpp` | header aliases (`importer.py:36-69`, e.g. `latitude`/`lat`, `pi`/`principal_investigator`), a column mapping the user can override, and a plan: each row is `create`, `exists` (same natural key, all given fields equal), `update` (same natural key, given fields differ; off unless asked), or `error` with every message for the row. Rows duplicated within the file are errors. The plan becomes one `CatalogEditBatch` (PIs, projects and materials first). `template()` writes the header-only CSV. |
| `sample_search.hpp` | the near-duplicate check before a new sample: same name ignoring case, spaces, `-` and `_`, in any project. It warns and never blocks (`sample_entry.py:948-960`). |
| `level_sheet.hpp` | `LevelSheetEdit`: an editable copy of a `LevelSheet` with the holder's holes. Operations: assign sample to positions, clear fields of positions (choose which, `entry/tasks/labnumber/task.py:54-113`), set weight, note and packet, fill packet sequence, move a position's contents to another hole (only if it has no analyzed identifier), set level note, z, holder, production. `dirty()`, `validate()` (a row with an identifier needs a sample; packets match; positions inside the holder unless orphaned) and `to_batch()` that emits only changed fields with their loaded values as `expected`. |
| `identifier_plan.hpp` | the pure planner (section 8). |
| `irradiation_edit.hpp` | new irradiation: name, chronology doses, reactor and production defaults, levels; validation (doses ordered, `end > start`, power > 0); emits one batch plus a unit of work for the chronology and productions. Duration helper and estimated J (`hours x j_multiplier`, `labnumber_entry.py:1163-1177`), display only. |
| `holder_import.hpp` | reads a legacy holder text file through `parse_holder` and stages an `irradiation_holder` revision. Hole ids must be unique and positions numbered `1..n`. |
| `settings.hpp` | `EntrySettings` (section 7) read from and written to the `pychron/entry_settings.json` document reference, with defaults when absent. |
| `export.hpp` | level and irradiation CSV export (the columns of section 9.2). |

## 7. Settings

The document (comments here are explanations, not part of the file):

```json
{
  "irradiation_prefix": "NM-",
  "mode": "argon",                      // "argon" | "package"; package hides chronology and productions
  "pi_names_allowed": ["NMGRL"],
  "monitor": {"sample": "FC-2", "material": "sanidine"},
  "irradiation_project_prefix": "Irradiation-",
  "create_irradiation_project": true,   // project <prefix><irrad> with the monitor sample, legacy :1091-1127
  "identifiers": {
    "consecutive": false,               // one stream for monitors and unknowns
    "streams": {"monitor": {"floor": 1}, "unknown": {"floor": 50000}},
    "offset": 5,
    "level_offset": 1
  },
  "j_multiplier": 1e-4,
  "null_identifier_rows": "allow"       // legacy allow_multiple_null_identifiers; "packet" requires packets
}
```

The defaults above are placeholders. Section 11 Q2 asks the owner for the
NMGRL values. The stream floors are lab data and must be set before the first
generation. Until they are, Generate Identifiers is disabled with that reason.

## 8. Identifier planner

Input: the irradiation's levels, sorted by name (E7), each with its
positions sorted by position; the settings; per stream the counter value
`last` (from `identifier_counter`, or the seed of 5.4 step 1 computed by a
read when the row is absent); `overwrite`.

```
for each stream s (monitor, then unknown; one stream "unknown" when consecutive):
  n = last[s] + offset - 1         # the first number is last + offset (legacy start + offset)
  for each level L in name order:
    i = 0
    for each position p of L in position order:
      skip if p has no sample
      skip if p is not in s        # monitor: sample name and material equal the settings' monitor
      skip if p has an identifier and (not overwrite or it is analyzed)
      i += 1; assign n + i
    n += i + level_offset - 1      # level_offset 1: no gap between levels
```

This is `identifier_generator.py:289-356` with the level order fixed, analyzed
identifiers never overwritten, and `offset` and `level_offset` clamped to at
least 1 as legacy does. With `level_offset = 1` and a level with no new
positions, nothing is added for that level. The planner returns the
assignments and, per stream, `expected_last` and the resulting last.

Before planning, the UI runs the legacy "human error" checks
(`labnumber_entry.py:464-534`) as warnings: a level with no monitor
position, and a monitor sample outside project
`<irradiation_project_prefix><irradiation>`.

Preview shows every level, not only the current one (fixing
`identifier_generator.py:185-197`). Commit sends the same plan to
`allocate_identifiers`. On `AllocationStale` the UI re-plans from the new
counter and shows the new preview. It never commits a plan the user has not
seen.

## 9. User interface (`apps/pychron-ui`)

An **Entry** menu, present when the app has a store (`PYCHRON_UI_HAS_STORE`
and a `--db` or a data install), in both `MainWindow` and `DataMainWindow`:
Samples…, Irradiations…, Import Samples…, Holders…, Entry Settings….
All store calls go through an `EntryBridge` that owns one worker thread with
its own store (as `StoreSource` does). It registers the client with role
`reduction` (11.2 lets that role write catalog rows) and returns results to
the GUI thread through queued signals. The windows never block the GUI
thread.

### 9.1 Samples window

- Left: filters for PI, project and material, plus search text.
- Centre: sample table (Name, Project, PI, Material, Grainsize, Lat, Lon,
  Elevation, Unit, Lithology, Location, Storage, IGSN, Note, Positions,
  Analyses). Cells are editable. Edited cells are tinted until saved.
- Bottom: detail form for the selected sample with every field, the UTM
  entry mode, and the irradiation positions the sample is in.
- "New sample" row at the top of the table. PI, project and material are
  combo boxes that accept a new value. A new value is created on save,
  with a "will be created" marker, so no separate add buttons are needed. A
  new project needs a PI. The near-duplicate check runs when the name is
  committed to the cell.
- Save applies every edit as one batch. Stale rows come back highlighted with
  the other client's values in a tooltip, plus a Reload button. Revert drops
  local edits.
- Delete is offered only for rows with no positions and no identifiers.
- Paste (Ctrl+V) of a TSV block into the new-sample row runs the import
  preview.

### 9.2 Import Samples dialog

File or clipboard, column mapping table, preview with a filter
(All/Create/Exists/Update/Error) and per-row messages, "Export errors" to CSV,
"Write template", and Import, which sends one batch. Same core as
`elctl entry samples import`.

### 9.3 Irradiations window

- Left: tree of irradiations (newest first) and their levels, with counts.
  New Irradiation… and New Level… buttons.
- Centre: positions grid for the selected level. Rows are holder holes and
  orphans. Columns: analyzed marker (count in tooltip), Position, Packet,
  Identifier, Sample, Project, PI, Material, Grainsize, Weight, J, ±J, Note.
  Multi-select. Weight, packet and note are editable in the cell. Identifier
  is read-only.
- Right dock, tabbed:
  - Sample picker: the Samples filters with a list. "Assign to selected"
    writes the sample to the selected rows. Rows with an analyzed identifier
    ask once, naming the analysis count (E5).
  - Level: holder (combo of `irradiation_holder` refs), z, production
    (combo of the irradiation's productions, with Edit…), note.
  - Chronology (hidden in package mode): dose table with power, start and
    end in the lab's local time, stored as UTC (P4); duration helper; total
    hours and estimated J.
  - Holder view: the holes drawn from the holder geometry, filled holes
    coloured by project, selection synced with the grid (click and
    rubber-band).
- Toolbar: Save, Revert, Generate Identifiers…, Clear Fields…, Fill Packets…,
  Import Positions… (CSV: level, position, sample, project, PI, material,
  grainsize, weight, packet, note), Export CSV, Save PDF.
- Switching level or irradiation with unsaved edits asks Save, Discard or
  Cancel (`labnumber_entry.py:1179-1189`).
- Stale rows after a save are shown as in the samples window.

New Irradiation dialog: name (pre-filled by the prefix increment), the
chronology table, a reactor from `reactors.json` (whose production is
copied into the irradiation as `<irrad>/<reactor>`), and the levels to
create (count, first letter, holder, z). In argon mode a reactor is
required. Unlike legacy, everything entered in this dialog is written in one
transaction.

New Level dialog: next letter, last level's holder, z and production
pre-filled (`package_level_editor.py:161-178`).

Production editor: the nine ratios (`K4039`, `K3839`, `K3739`, `Ca3937`,
`Ca3837`, `Ca3637`, `Cl3638`, `Ca_K`, `Cl_K`) with errors, reactor and note.
Saving writes a new revision of that production. Copy from reactor default.
"Set as reactor default" writes a new revision of `reactors.json`.

Generate Identifiers dialog: stream settings shown read-only with a link to
Entry Settings, overwrite checkbox, the warnings of section 8, and a preview
table of every level (position, sample, current identifier, new identifier).
Commit, or re-preview when stale.

### 9.4 Holders dialog

A list of `irradiation_holder` refs with a geometry preview. Import from a
legacy `.txt` file. No interactive tray maker in this spec (section 12).

### 9.5 PDF

`Save PDF` renders the legacy level sheet (`irradiation_pdf_writer.py:111-289`)
with `QPdfWriter`: a summary page (irradiation, chronology, levels with
holder and projects), then per level the holder drawing and a table
(checkbox, Pos., Identifier, Sample, Material, Project, PI, Note) with a row
for every hole.

## 10. `elctl entry`

Headless and scriptable, built with persistence like `elctl import`. Every
writing command takes `--db <url>`, `--user` and `--dry-run`, and prints
what it would write or what it wrote.

```
elctl entry samples import <file.csv> [--update-existing] [--errors <out.csv>]
elctl entry samples template <out.csv>
elctl entry samples list [--pi ..] [--project ..] [--material ..] [--text ..]
elctl entry irradiation add <name> [--chronology <file>] [--reactor <name>] [--levels A-C --holder <name> --z <z>]
elctl entry irradiation show <name> [--level <L>] [--csv]
elctl entry positions import <irradiation> <file.csv>
elctl entry identifiers generate <irradiation> [--overwrite]   # --dry-run prints the plan
elctl entry holders import <file.txt> [--name <name>]
elctl entry settings show|set <key> <value>
```

Exit code 0 on success, 1 on error, 2 when a save is stale or refused
(nothing written). Refusals and stale rows are listed one per line.

## 11. Open questions

1. **Legacy acquisition during migration (E1).** Python pychron reads
   identifiers from the legacy MySQL database and level files from the meta
   repo. An irradiation entered here is invisible to it until the publisher
   exists, and the publisher writes only the meta repo, never MySQL.
   Recommendation: entry here is for labs whose acquisition already uses the
   store. Labs still on Python acquisition keep entering in legacy and
   re-import (`elctl import run`). Entry Settings shows a banner when the
   store has a `legacy_db` import source.
2. **NMGRL identifier streams.** What are the monitor and unknown floors
   (and is consecutive numbering used), and what are the defaults for
   `offset` and `level_offset`?
3. **Changing the sample of an analyzed position (E5).** Keep it, behind the
   confirmation, or make it admin-only?
4. **Irradiation mode per lab or per irradiation?** Legacy has it per lab.
   Some labs irradiate both Ar/Ar packages and other packages.

## 12. Not in this spec

Flux and monitor editing, J transfer, loads, sample prep, IGSN, the tray
maker, MassSpec/XML/YAML/XLS export, and the status report of unanalyzed
identifiers (a browse filter will cover it).

## 13. Testing

- Store (`tests/persistence/test_catalog_edit.cpp`, SQLite always,
  PostgreSQL with `PYCHRON_TEST_PG_URL`):
  - Every allowlisted column round-trips.
  - Stale detection is per field, so two writers on different fields both
    succeed.
  - A batch with one stale row writes nothing and reports every stale row.
  - Every refusal rule has a test.
  - A rename rewrites ref keys.
  - Audit rows hold before and after.
  - Two threads racing to update one field: exactly one applied.
- Allocation (`tests/persistence/test_identifier_allocation.cpp`):
  - The seed ignores non-numeric and leading-zero identifiers and respects
    the ceiling.
  - CAS: two allocators racing get one applied and one stale.
  - `replaces` an analyzed identifier is refused.
  - After an import, the first allocation continues above the imported
    maximum.
- `tests/entry/`:
  - Name rules and increments, table-driven with the legacy examples.
  - UTM conversion against published reference points.
  - CSV dialects.
  - Import plan for each row state.
  - Level sheet edit to batch: only changed fields, expected values as
    loaded.
  - The planner: the legacy cases (offset, level_offset, monitors first,
    overwrite, skip analyzed) and the guarantee that the preview equals the
    commit. Property test: random sheets give unique, increasing numbers
    within each stream, and re-planning a fully numbered irradiation with
    overwrite off assigns nothing.
- `apps/elctl/tests`: `entry samples import --dry-run` output and an end-to-end
  import on SQLite.
- `tests/ui` (headless): samples table edit and save; stale highlighting;
  irradiation window assign, save and generate on a SQLite store; the unsaved
  edits prompt.
