# Data publishing and hosted sharing: design

Date: 2026-10-18
Status: Proposed (owner decisions of 2026-10-18 folded in: sections 6.3,
7, 9.4, 9.7, 9.8, 10.1, 10.2)
Deciders: Jake Ross
Depends on: `2026-10-01-dvc-schema-design.md` (store, revisions, outbox D3,
change feed D6, offline export D7), `2026-10-03-legacy-ingestion-design.md`,
`2026-10-07-earthbank-design.md`, `2026-10-02-data-browsing-visualization-design.md`
(Scene, report), `docs/export.md`, `docs/earthbank.md`.
Scope: how a lab's reduced data leaves the store as a citable release, how a
release is hosted and found, how the lab's store reaches an owner-run cloud
store, and which identifiers a release and its samples get.
Style: compressed (root `AGENTS.md`). Nothing technical dropped.

## 1. Problem and direction

Data trapped on lab computers. Sparrow dead; needed each lab to host Docker
stack. EarthBank, Geochron: aggregators, upload friction, no embargo model.
Owner (2026-10-18) wants OGC API approach, simple web viewer, and cloud
hosting of lab data by owner for labs that cannot host.

Direction: two tiers, one format.

- **Static tier (first).** A *release* is an immutable directory: GeoParquet
  tables, OGC API - Records catalog JSON, Schaen 2021 tables, figure PDFs,
  manifest of hashes. Hostable on any static host (lab web server, S3,
  GitHub Pages, owner's bucket). Small static viewer beside it.
- **Live tier.** Owner-run multi-tenant PostgreSQL, fed by lab's outbox and
  uploader. pygeoapi over read-only views gives OGC API - Features and
  Records. Private by default. Cross-lab Records index over what labs made
  public.

Value: embargo per project, sample or analysis; DOI per release (DataCite);
IGSN per sample (SESAR); preservation and region choice; one cross-lab
index.

Truth: lab's local store (SQLite on the acquisition PC) is acquisition truth.
Cloud store is shared truth for reduction and publishing. Release directory is
a frozen read of either store.

## 2. Terms

| Term | Meaning |
|---|---|
| Store | `libs/persistence` database, SQLite or PostgreSQL, shared schema |
| Release | Row in `release` plus pinned revision set; builds one release directory |
| Release directory | Files of section 5; what is hosted, cited, downloaded |
| Pin | (subject, kind, revision) frozen by a release: same shape as `bookmark_entry` |
| Embargo | Row in `embargo` hiding a project, sample or analysis from every public surface until a date or until lifted |
| Visibility | `release.visibility`: `private` or `public` |
| Hosted tier | Owner-run PostgreSQL plus pygeoapi plus static buckets |
| Tenant | One lab in hosted tier: own database, own role, own region |

## 3. Non-goals and out of scope

- Analysis in the viewer: no re-reduction, no fits, no figure options. Viewer
  shows what the release holds.
- A portal: no accounts for readers, no comments, no submissions, no search
  across labs beyond the Records index.
- Sparrow compatibility: no Sparrow API, schema or importer.
- Git write-back: `publish_target` (DVC spec section 10) untouched; git is a
  one-way mirror and this spec does not add a second one. Release directory
  is not a git repository.
- Two-way sync of a release: a release never edits the store. Corrections are
  a new release (section 4.6).
- EarthBank as primary target: stays a downstream export (section 11).
- Qt in `libs/`: everything below `apps/` is Qt-free, as today.
- Legacy pychron (Python) output formats.

## 4. Release model in the store

### 4.1 Rules

- R1 Nothing is public by default. A fresh store, a fresh release, a fresh
  tenant: nothing visible outside the lab until a release is `published` with
  `visibility = 'public'` and nothing in it is under embargo. Confirmed by
  the owner 2026-10-18: not even a released sample's metadata (name,
  location, material) is listed in the index before that.
- R2 A release is immutable once `published`: its pins, members and
  manifest never change. A change is a new release that names the old one.
- R3 A release pins revisions, not values: every value in the directory is a
  pure function of (pins, reduction version, options) (DVC invariant I4).
  Rebuilding from pins reproduces the directory (section 5.8).
- R4 Embargo wins over release: an embargoed sample is left out of a public
  build and of every hosted view even when a published release names it.
  A private build (lab's own copy) includes it and says so in manifest.
- R5 One migration for all of this (`0007_release.sql`; `0006` is
  `pi_identity`), additive only:
  new tables, nullable columns, indexes. Old builds keep opening the store.
- R6 Store never calls the network. Registration (DOI, IGSN) is done by
  `elctl` or the cloud service and written back as a catalog edit.

### 4.2 Tables (migration `0007_release.sql`)

```sql
-- A release: metadata plus a pinned revision set. The pins are a bookmark
-- (bookmark_entry is append-only and FK-checked already); the bookmark sits
-- on an analysis_group holding the release's analyses.
CREATE TABLE release (uuid uuid PRIMARY KEY,
  name text NOT NULL,                          -- slug, directory name: 'nmgrl-2026-10-fc2'
  title text NOT NULL, description text,
  version int NOT NULL DEFAULT 1,              -- bumps per name: 'v2 of nmgrl-2026-10-fc2'
  supersedes_uuid uuid REFERENCES release,     -- the release this one replaces
  group_uuid uuid NOT NULL REFERENCES analysis_group,
  bookmark_uuid uuid NOT NULL REFERENCES bookmark,
  state text NOT NULL DEFAULT 'draft' CHECK (state IN ('draft','published','withdrawn')),
  visibility text NOT NULL DEFAULT 'private' CHECK (visibility IN ('private','public')),
  licence text NOT NULL,                       -- SPDX id: 'CC-BY-4.0'
  authors jsonb NOT NULL,                      -- [{name, orcid?, affiliation?}], DataCite creators
  publisher text,                              -- DataCite publisher; default the lab's name
  embargo_until_utc timestamptz,               -- release-wide embargo; per-row embargo is table embargo
  doi text,                                    -- '10.xxxx/yyyy' once registered (section 10)
  doi_state text CHECK (doi_state IN ('draft','registered','findable')),
  options jsonb NOT NULL,                      -- ReportOptions + figure options + constants preset, as built
  reduction_version text NOT NULL,
  software_version text NOT NULL,              -- 'pychron-cpp/0.5.0+g1a2b3c4'
  manifest_sha256 bytea,                       -- of manifest.json of the published build
  created_by_user_uuid uuid NOT NULL REFERENCES app_user, client_uuid uuid NOT NULL REFERENCES client,
  created_utc timestamptz NOT NULL, published_utc timestamptz, withdrawn_utc timestamptz,
  withdrawn_reason text, UNIQUE (name, version));  -- one name, several versions
CREATE INDEX release_state_ix ON release (state, visibility);

-- Samples of a release (derived from members' identifiers at creation; frozen).
CREATE TABLE release_sample (release_uuid uuid REFERENCES release, sample_uuid uuid REFERENCES sample,
  PRIMARY KEY (release_uuid, sample_uuid));
CREATE INDEX release_sample_sample_ix ON release_sample (sample_uuid);

-- Interpreted ages of a release, pinned at a revision.
CREATE TABLE release_interpreted_age (release_uuid uuid REFERENCES release,
  ia_uuid uuid REFERENCES interpreted_age, revision_uuid uuid NOT NULL REFERENCES revision,
  PRIMARY KEY (release_uuid, ia_uuid));

-- Embargo: hides a scope from public builds and hosted views. Lifted by date
-- or by hand (lifted_utc). Mutable operational table, audited by change_log
-- like a catalog row (D6).
CREATE TABLE embargo (uuid uuid PRIMARY KEY,
  scope_type text NOT NULL CHECK (scope_type IN ('project','sample','analysis')),
  scope_uuid uuid NOT NULL, until_utc timestamptz, reason text,
  set_by_user_uuid uuid NOT NULL REFERENCES app_user, created_utc timestamptz NOT NULL,
  lifted_utc timestamptz, lifted_by_user_uuid uuid REFERENCES app_user);
CREATE INDEX embargo_scope_ix ON embargo (scope_type, scope_uuid) WHERE lifted_utc IS NULL;

-- Registered external identifiers: what was minted where, for which entity.
-- sample.igsn and release.doi are the catalog's copies; this is the receipt.
CREATE TABLE external_id (uuid uuid PRIMARY KEY,
  entity_type text NOT NULL CHECK (entity_type IN ('release','sample')), entity_uuid uuid NOT NULL,
  scheme text NOT NULL CHECK (scheme IN ('doi','igsn')), value text NOT NULL,
  registrar text NOT NULL,                     -- 'datacite', 'datacite-test', 'sesar', 'sesar-sandbox'
  state text NOT NULL CHECK (state IN ('draft','registered','findable','in_doubt')),
  request_sha256 bytea NOT NULL,               -- of the payload sent
  response jsonb,                              -- what the registrar answered (no token)
  registered_utc timestamptz NOT NULL, UNIQUE (scheme, value));
CREATE INDEX external_id_entity_ix ON external_id (entity_type, entity_uuid);
```

- `embargo.until_utc` null with `lifted_utc` null: indefinite, lifted by hand
  only. No constraint ties the two columns; the rule is in the access layer
  (an embargo is live when `lifted_utc IS NULL AND (until_utc IS NULL OR
  until_utc > now)`).
- No cap on an embargo in the software: the lab decides its length
  (owner's decision). The Publish page shows the end date and warns only
  when none is set (indefinite).
- Embargo on a `project` covers its samples and their analyses; on a `sample`
  its analyses; on an `analysis` that row. `is_embargoed(analysis, at)` is
  one query over the three scopes, written as `UNION`, never `OR` across
  tables (`libs/persistence/AGENTS.md`).
- Triggers: `release_sample`, `release_interpreted_age` are never updated
  and are deleted only while their release is a `draft` (`delete_release`).
  `release` is deleted only while a `draft`, and updated only in `state`,
  `visibility`, `doi`, `doi_state`, `manifest_sha256`, `published_utc`,
  `withdrawn_utc`, `withdrawn_reason` (`release_guard()`; the SQLite
  triggers are written out with `-- @sqlite exec`).
  `embargo` and `external_id` are mutable operational tables.
- `SchemaTest.MigrateIsIdempotentAndRecordsChecksums` lists `0007 release`;
  `SchemaIndexes` gets plan tests for `embargo_scope_ix`,
  `release_sample_sample_ix` and the `UNION` embargo query.
- Commit of the migration carries `BREAKING CHANGE: run elctl db migrate`.
- Roles (DVC 11.2): `pychron_reduce` gains INSERT on `release*`, `embargo`,
  `external_id` and UPDATE on `release`, `embargo`, `external_id`. New
  role `pychron_api` (hosted tier, section 9): SELECT on `pub.*` views only.

### 4.3 Members and pins

Creating a release (section 6) in one unit of work:

1. Select analyses (`elctl export` selection: `--sample`, `--identifier`,
   `--project`, `--irradiation`, `--type`, `--uuid`, `--from`, `--to`;
   default type `unknown`; `invalid` left out unless `--include-invalid`)
   and tag filter `--tag <name>` (repeatable; default every head tag but
   `invalid`). "Released by tag" means membership is the analyses whose
   head `tags` revision has one of the named tags at creation time; the
   tag revision is pinned with the rest, so a later retag does not move the
   release.
2. Resolve every member's reference revisions (`IStore::resolve_refs`,
   `RefPolicy` head) and its heads for all kinds.
3. `create_group(name = release name, analyses)`;
   `create_bookmark({group, message: 'release <name> v<n>'})`: one
   `bookmark_entry` per (member, kind) and per resolved ref object
   (`value`), and per interpreted age (`interpreted_age`). Bookmark entries
   for ref objects are what makes the J, production and chronology of the
   release reproducible.
4. Insert `release`, `release_sample` (distinct samples of members'
   identifiers), `release_interpreted_age` (IAs whose members are all in
   the release, at their heads; `--ia <name>` adds or `--no-ia` omits).
5. `change_log` entry kind `changeset` with `change_entity` rows
   (`release`, `release_sample`); `state = 'draft'`.

Loading a member "as of" the release: `IStore::load_analysis_at(analysis,
bookmark)` (DVC plan D2 completion item "loading an analysis as of a bookmark
without restoring it", not yet built): heads replaced by bookmark entries,
refs resolved from bookmark entries of type `value`. Build (section 5) reads
only through it.

### 4.4 States

```
draft ──publish──> published ──withdraw──> withdrawn
  │                     │
  └─delete (draft only) └─ supersede: new release row, supersedes_uuid = this
```

- `draft`: rebuilt freely; members and pins already fixed (a different
  selection is a new draft). Directory may be built and inspected, never
  hosted as public.
- `published`: pins frozen (R2); `manifest_sha256` set to the build that was
  published; DOI may be minted (section 10). `visibility` may go
  `private -> public` any time; `public -> private` only by `withdraw`.
- `withdrawn`: stays in the store, listed with reason; hosted views drop it;
  DOI, if findable, gets its landing page's "withdrawn" notice (DataCite:
  findable DOIs are never deleted; metadata updated with `withdrawn_reason`
  in `descriptions`).
- Delete: `draft` only; drops `release*` rows, keeps group and bookmark.

### 4.5 Visibility rule, one function

`release_visible(release, at)` = `state = 'published' AND visibility =
'public' AND (embargo_until_utc IS NULL OR embargo_until_utc <= at)`.
`sample_visible(sample, at)` = exists visible release naming it AND not
embargoed at `at`. `analysis_visible` likewise, plus its sample visible.
Written once in `libs/persistence/src/sql/publish.hpp`; the hosted views
(section 9.2) and the public build (section 5) use the same text, through the
`Dialect` switch.

### 4.6 Corrections

A wrong value after publication: new release with `supersedes_uuid`,
`version + 1`, same `name`; DOI of the new one carries
`relatedIdentifiers: IsNewVersionOf <old doi>`, old one `IsPreviousVersionOf`.
Old directory stays hosted (citations keep resolving). Index lists the newest
version of a name and links the chain.

## 5. Release directory format

### 5.1 Layout

```
<name>-v<version>/
  manifest.json                      section 5.7
  catalog.json                       OGC API - Records collection (crawlable catalogue), 5.4
  records/<sample uuid>.json         one record per sample
  records/release.json               record of the release itself
  data/samples.parquet               5.3
  data/positions.parquet
  data/analyses.parquet
  data/isotopes.parquet
  data/interpreted_ages.parquet
  tables/release.csv  tables/release.json          Schaen report, every member (5.5)
  tables/<sample>/report.csv  report.json           Schaen report per sample
  figures/<sample>/<kind>.pdf  <kind>.svg           5.6; kind: ideogram, spectrum, isochron
  constants.json                     the constants table of the report, as JSON
  LICENSE.txt                        the licence text
  CITATION.cff                       title, authors, DOI (when any), version, date
  README.md                          one paragraph: what this is, how to cite, how to read it
  viewer/                            section 7, only with --with-viewer
```

Names: `<sample>` is the sample name made file-safe (`[^A-Za-z0-9._-]` ->
`_`; collision -> suffix `-<last 8 hex of uuid>`; a v7's first 8 are a shared
timestamp). Paths in manifest are the
truth; names are for people.

Every file written by `elctl publish` or the UI goes through
`pychron::mark_as_user_file` after it is closed (root `AGENTS.md`):
quarantined application would otherwise quarantine its own release.

### 5.2 GeoParquet conventions

- GeoParquet 1.1.0 ([spec](https://geoparquet.org/releases/v1.1.0)):
  file key-value metadata key `geo`, `version: "1.1.0"`,
  `primary_column: "geometry"`, column `geometry` encoded WKB, `crs` PROJJSON
  of OGC:CRS84 (the default; written explicitly), `geometry_types:
  ["Point"]`, `bbox` of the file, `covering.bbox` naming the `bbox` struct
  column (`xmin`, `ymin`, `xmax`, `ymax`) that every geometry-bearing table
  carries. Not GeoParquet 2.0 (native GEOMETRY type): readers in the field
  (DuckDB-WASM, GeoPandas, QGIS) are at 1.1 in 2026; pygeoapi 0.24 reads 1.1
  ([releases](https://github.com/geopython/pygeoapi/releases)).
- A table without a location (`isotopes`) is plain Parquet, no `geo` key.
- Row order fixed: samples by name then uuid; analyses by timestamp then
  uuid; isotopes by analysis order then isotope name; positions by
  irradiation, level, position; interpreted ages by sample then name.
  Deterministic rows make the row hash (5.7) stable.
- Types: uuids `string` (36-char canonical), times `timestamp[us, UTC]`,
  numbers `double`, flags `bool`, lists `list<...>`, structs `struct`.
  Nulls where the store has nothing; never a sentinel.
- Compression ZSTD, one row group per table (release tables are small: a
  decade of one lab is about 10^5 analyses, 10^6 isotope rows, tens of MB).
- Writer: Apache Arrow C++ with Parquet (Homebrew `apache-arrow`, Ubuntu
  `libarrow-dev libparquet-dev`, or vcpkg's `publish` feature: `arrow[parquet]`,
  port 26.0.0; core pulls boost-filesystem, thrift, zstd, snappy, re2 and
  others: a heavy dependency). Behind `PYCHRON_PUBLISH=AUTO|ON|OFF` as
  OpenCV is behind `PYCHRON_VISION_OPENCV`; without it `libs/publish`
  builds and `elctl publish` writes everything but `data/*.parquet`, and
  says so. Arrow headers appear only in `libs/publish/src/parquet_writer.cpp`.
  Alternative kept open: an in-house PLAIN-encoded uncompressed writer
  (Thrift compact metadata, ~600 lines) if Arrow's build cost on the three
  CI compilers turns out too high; the `IParquetWriter` seam is the same.
- Byte-identical Parquet across Arrow versions is not promised; row-level
  hashes (5.7) are what verify carries. Within one pychron-cpp build the
  bytes are deterministic (no timestamps, no random ids in the file).

### 5.3 Tables and columns

Column sources: `processing::Analysis`, `reduction::ArArResult`,
`processing::SampleInfo`, the Schaen report (`report.hpp`) and the store
rows. Names snake_case, units in the name where a unit exists.

**`samples.parquet`** (GeoParquet; one row per `release_sample`):

| Column | Type | Source |
|---|---|---|
| `sample_uuid` | string | `sample.uuid` |
| `name` | string | `sample.name` |
| `igsn` | string? | `sample.igsn` |
| `project`, `project_uuid` | string | `project` |
| `principal_investigator` | string? | `principal_investigator.display_name` |
| `material`, `grainsize` | string | `material` |
| `geometry` | WKB point? | `sample.geom` (lon, lat; CRS84) |
| `bbox` | struct{xmin,ymin,xmax,ymax}? | from `geometry` |
| `elevation_m` | double? | `sample.elevation` |
| `location`, `unit`, `lithology`, `lithology_class`, `lithology_type`, `lithology_group` | string? | sample row |
| `approximate_age_ma` | double? | `sample.approximate_age` |
| `note` | string? | `sample.note` |
| `n_analyses`, `n_ages`, `n_interpreted_ages` | int32 | counted in the release |
| `first_analysis_utc`, `last_analysis_utc` | timestamp? | members |
| `record_path` | string | `records/<uuid>.json` |

**`positions.parquet`** (one row per identifier in the release; the Schaen
`irradiation` table): `identifier`, `identifier_uuid`, `sample_uuid`,
`irradiation`, `level`, `position` (int32), `reactor`, `irradiation_start_utc`,
`irradiation_duration_h`, `holder`, `packet`, `weight_mg`, `monitor_name`,
`monitor_material`, `monitor_age_ma`, `monitor_age_err_ma`, `j`, `j_err`,
`j_position_err`, `lambda_k_total`, `lambda_k_total_err`, production ratios
`p_40_39_k`, `p_38_39_k`, `p_37_39_k`, `p_39_37_ca`, `p_38_37_ca`,
`p_36_37_ca`, `p_36_38_cl`, each with `_err`, `ca_k`, `cl_k`,
`flux_revision_uuid`, `production_revision_uuid`, `chronology_revision_uuid`.

**`analyses.parquet`** (GeoParquet, geometry = the sample's point; one row per
member; the Schaen `analyses` table plus identity and pins):

| Column | Type | Source |
|---|---|---|
| `analysis_uuid`, `runid`, `identifier`, `aliquot` (int32), `increment` (int32; -1 none), `step` (string?) | | `analysis` row |
| `sample_uuid`, `sample`, `project`, `material` | string | catalog |
| `analysis_type`, `experiment_type`, `timestamp_utc`, `mass_spectrometer`, `extract_device`, `analyst`, `laboratory` | | `analysis` row |
| `extract_value`, `extract_units`, `extract_duration_s`, `cleanup_duration_s`, `weight_mg`, `beam_diameter`, `pattern` | | `analysis` row |
| `irradiation`, `level`, `position` | | identifier's position |
| `tag`, `included` (bool), `group_id` (string: identifier or aliquot per grouping), `comment` | | pinned `tags`, `annotation`; grouping of the build |
| `j`, `j_err` | double | pinned flux |
| `ar40_fa` .. `ar36_fa` with `_err`, and `ar40_blank_fa` .. `ar36_blank_fa` with `_err` | double? | corrected intensities and blanks as `docs/export.md` defines them (baseline, blank, IC, discrimination, decay for 37 and 39) |
| `f`, `f_err` | double? | 40Ar*/39ArK |
| `rad40_pct`, `k39_fa`, `k39_pct_group`, `k39_cum_pct` | double? | |
| `kca`, `kca_err`, `kcl`, `kcl_err` | double? | |
| `iso_39_40`, `iso_39_40_err`, `iso_36_40`, `iso_36_40_err`, `iso_corr` | double? | inverse isochron coordinates and correlation |
| `age_ma`, `age_err_analytical_ma`, `age_err_with_j_ma`, `age_note` | | report |
| `record_sha256` | string (hex) | `analysis.record_sha256` |
| `rev_signals`, `rev_intercepts`, `rev_baselines`, `rev_blanks`, `rev_icfactors`, `rev_tags`, `rev_annotation` | string? | pinned revision uuids |
| `geometry`, `bbox` | | sample's |

**`isotopes.parquet`** (plain Parquet; one row per `(analysis, isotope)`):
`analysis_uuid`, `isotope`, `detector`, `units` (`fA`), `intercept`,
`intercept_err`, `fit`, `error_type`, `n`, `fn`, `reviewed`,
`baseline`, `baseline_err`, `baseline_fit`, `blank`, `blank_err`,
`blank_fit`, `ic_factor`, `ic_factor_err`, `discrimination` (bool),
`decay_corrected` (bool), `corrected`, `corrected_err`, `blob_sha256`
(string?; the raw series' content hash in the store, for anyone with store
access; raw points are not in the release, section 5.9), `n_points`.

**`interpreted_ages.parquet`** (GeoParquet, sample's point; one row per
`release_interpreted_age`): `ia_uuid`, `name`, `sample_uuid`, `identifier`,
`age_kind` (`plateau`, `weighted_mean`, `isochron`, `integrated`,
`other`), `age_ma`, `age_err_ma`, `mswd`, `n`, `kca`, `kca_err`,
`plateau_k39_pct`, `trapped_40_36`, `trapped_40_36_err`, `members`
(list<struct{analysis_uuid string, plateau_step bool?, tag string?}>),
`revision_uuid`, `doc` (string: `ia_value.doc` JSON verbatim), `geometry`,
`bbox`.

Every column of every table is listed in `docs/release_format.md` with its
unit and source (written in phase 1 with the code); that page is the reader's
reference and this spec is the design.

### 5.4 OGC API - Records catalog (crawlable)

OGC API - Records - Part 1: Core 1.0, approved 2025-04-07
([20-004r1](https://docs.ogc.org/is/20-004r1/20-004r1.html)): a crawlable
catalogue is files at fixed URLs, a collection object linking each record
with `rel: item`. No server needed.

`catalog.json`: `{"id": "<release name>-v<n>", "type": "Collection",
"itemType": "record", "title", "description", "extent": {"spatial": {"bbox":
[[w,s,e,n]], "crs": "http://www.opengis.net/def/crs/OGC/1.3/CRS84"},
"temporal": {"interval": [[first, last]]}}, "license": "<SPDX>",
"created", "updated", "keywords": ["40Ar/39Ar", "geochronology", ...],
"conformsTo": ["http://www.opengis.net/spec/ogcapi-records-1/1.0/req/record-core"],
"links": [{"rel": "self"}, {"rel": "root"}, {"rel": "license"},
{"rel": "describedby", "href": "manifest.json"}, {"rel": "cite-as", "href":
"https://doi.org/<doi>"} when any, {"rel": "enclosure", "type":
"application/vnd.apache.parquet", "href": "data/<table>.parquet"} per table,
{"rel": "item", "type": "application/geo+json", "href":
"records/<uuid>.json", "title": "<sample>"} per sample, {"rel": "item",
"href": "records/release.json"}]}`.

`records/<sample uuid>.json`: GeoJSON Feature, `id = sample uuid`,
`geometry` = point or null, `time = {"interval": [first, last]}`,
`properties`: `type: "dataset"`, `title: "<sample> (<material>)"`,
`description` (project, PI, n analyses, ages summary), `keywords`
(material, lithology, unit, irradiations), `language: "en"`, `license`,
`created`, `updated`, `externalIds: [{"scheme": "igsn", "value": ...}]`
when the sample has one, `contacts` (PI as `name`, `organization`),
`formats: ["GeoParquet", "CSV", "PDF"]`, `themes: [{"scheme": "https://...
pychron.../themes", "concepts": [{"id": "40Ar/39Ar"}]}]`,
`pychron:sample_uuid`, `pychron:n_analyses`, `pychron:ages` (list of
{kind, age_ma, err_ma, n}); `links`: `collection` (catalog.json),
`enclosure` for `tables/<sample>/report.csv` and each figure, `related`
to `https://doi.org/<igsn doi>` when registered.

`records/release.json`: the release as a record (`type: "dataset"`, `title`,
`description`, `externalIds` DOI, `contacts` authors, `license`,
`links`: every enclosure, `predecessor-version` when superseding).

Field names beyond record-core are prefixed `pychron:`; record-core names
are the standard's.

### 5.5 Schaen tables

`processing::make_report` (`report.hpp`) over the release's `Dataset` with
`ReportOptions` from `release.options`; `report_csv` and `report_json`
unchanged. `tables/release.*` is one report of every member; `tables/<sample>/`
one per sample with the same options. Grouping as `docs/export.md`
(aliquot when any step, else identifier; `options.group_by` overrides).
Report metadata gains rows `release`, `release version`, `doi` (when any),
`licence`; nothing else in the report changes.

### 5.6 Figures

Per sample (`libs/publish` `figures.hpp`, `sample_figures`): ideogram of its
ages; spectrum per step-heated aliquot (items with increment >= 0, by
identifier and aliquot), file `spectrum.{pdf,svg}` when one, else
`spectrum-<identifier>-<aliquot two digits>.{pdf,svg}` made file-safe;
inverse isochron only when >= 3 included analyses. Built with
`build_ideogram`, `build_spectrum`, `build_isochron` (`arar_figures.hpp`)
from same `Dataset` as report. Options: schema defaults, plus
`error_bar_nsigma` = `release.options.sigma` and spectrum `plateau.method`,
`plateau.nsteps`, `plateau.gas_fraction` from `plateau`, `plateau_steps`,
`plateau_gas`; no per-release figure options yet. A figure whose builder
fails is left out with warning `<sample>: no <kind>: <why>`; build goes on.
Time series not a release figure (per instrument, not per sample).

`Scene` is Qt-free; rendering in UI is `SceneView` (Qt, QCustomPlot). Qt-free
writers in `libs/processing`:

- `scene_layout.hpp`: what `SceneView` computed itself, moved down: axis
  ranges (`graph_x_range`, `panel_y_range`: data extent, 8% pad, log rule,
  axis min/max per side), used by `SceneView::reset_view` too. Plus what
  QCustomPlot did for view, writers only: ticks (linear 1-2-5, log decades,
  time, category; powers of ten via `from_chars`, not libm), Helvetica and
  Helvetica-Bold AFM widths, margins, panel geometry
  (`layout_scene(const Scene&, const FigureSize&)`; cell 432 x 324 pt per
  graph).
- `scene_draw.hpp`: `DrawList draw_scene(const Scene&, const FigureSize&)`:
  paths and texts in points, painting order as `SceneView`. Both writers
  serialize it, so SVG and PDF place same primitives. Insets not drawn
  (`SceneView` draws none). Non-finite values, values <= 0 on log axis left
  out. Markers and ellipses Bezier/line paths using `sqrt` only.
  `format_coord`: fixed, 2 decimals, `std::to_chars`.
- `scene_svg.hpp`: `std::string scene_svg(const Scene&, const FigureSize& = {})`.
  Text, deterministic, sized in pt, fonts named not embedded,
  `xml:space="preserve"`.
- `scene_pdf.hpp`: `std::string scene_pdf(const Scene&, const PdfInfo&,
  const FigureSize& = {})` returns bytes; `Result<void> save_scene_pdf(scene,
  info, path, size)` writes and marks user file. `PdfInfo{created_unix,
  title}`. PDF 1.4: one page, vector paths, standard 14 fonts (Helvetica,
  Helvetica-Bold WinAnsi; Symbol for Greek; other characters `?`), one
  ExtGState per alpha, `CreationDate` = release `created_utc` (clamped
  1970..9999), no `/ID`, no compression, xref table correct.
  UI keeps `QPdfWriter` for interactive export.
- Parity test (`tests/ui/test_scene_view.cpp`): same Scene in `SceneView` and
  `draw_scene`: equal axis ranges, point, step box, ellipse counts and corner
  texts. Not bytes, not ticks (QCustomPlot's own).
- Same Scene twice gives same bytes: both in manifest with sha. Committed
  fixtures `tests/processing/fixtures/scene/figure.{svg,pdf}` compared byte
  for byte, so every CI compiler checks bytes one wrote (stored-format rule).
- `verify --db` rebuilds with figures when manifest lists `figures/`
  (`has_figures`). Records' `formats` gain `SVG`, `PDF` when figures present.

### 5.7 Manifest

`manifest.json`:

```json
{
  "format": "pychron-release/1",
  "release": {"uuid", "name", "version", "title", "description", "state", "visibility",
              "licence", "authors": [...], "publisher", "doi", "supersedes": {"uuid", "doi"},
              "created_utc", "published_utc", "embargo_until_utc"},
  "build": {"built_utc", "software": "pychron-cpp/<version.txt>" (plus "+g<short sha>" only when the build defines PYCHRON_GIT_SHA; it does not yet),
            "reduction_version", "constants_preset", "options": {...},
            "store": {"kind": "sqlite|postgresql", "schema_version": 6, "store_id"},
            "private_build": false, "omitted_embargoed": {"samples": 0, "analyses": 0}},
  "pins": {"bookmark_uuid",
           "analyses": [{"uuid", "runid", "record_sha256",
                         "heads": {"signals": "<rev>", "intercepts": "<rev>", "baselines": "<rev>",
                                   "blanks": "<rev>", "icfactors": "<rev>", "tags": "<rev>",
                                   "annotation": "<rev>|null"},
                         "refs": {"<ref_object uuid>": "<rev>"}}],
           "interpreted_ages": [{"uuid", "revision"}],
           "samples": ["<uuid>"]},
  "tables": [{"path": "data/analyses.parquet", "rows": 1234, "rows_sha256": "<hex>",
              "columns": ["analysis_uuid", ...]}],
  "files": [{"path", "bytes", "sha256", "media_type"}]
}
```

- `files` lists every file but `manifest.json` itself, sorted by path.
- `rows_sha256`: SHA-256 over the table's rows in canonical text (each row:
  cells joined by `\x1f`, rows by `\x1e`; doubles as shortest round-trip
  text, times ISO-8601 UTC microseconds, nulls empty, lists and structs as
  canonical JSON). Independent of the Parquet encoder.
- `pins` is the release's identity: two manifests with equal `pins`,
  `build.reduction_version` and `build.options` describe the same data.
- `release.manifest_sha256` in the store = SHA-256 of the published
  `manifest.json` bytes.

### 5.8 Reproducible from the store

`elctl publish build --release <uuid>` rebuilds from pins only (4.3): same
store, same software version, same options give the same `rows_sha256` for
every table and the same bytes for every non-Parquet file. `elctl publish
verify <dir> --db <url>`: reads manifest, rebuilds to a scratch directory,
compares `rows_sha256` per table and sha per file, reports differences;
without `--db` checks only that files match `files[]`. Test: build, verify,
corrupt a cell, verify fails naming the table.

Across software versions: same pins, different `reduction_version` may give
different values; `verify` then says `reduction_version differs` and
compares nothing else. The published build is the record; that is why the
directory is hosted, not regenerated.

### 5.9 What is not in a release

Raw signal points (`signal_blob`), peak centers, monitor checks, snapshots,
videos, scripts, spectrometer snapshots. Reason: size (blobs are most of a
store) and they are not what a reader of ages needs. `isotopes.blob_sha256`
names them for anyone who gets store access or a later "raw" add-on
(`--with-raw` writes `raw/<sha>.bin` in the store's codec plus
`raw/index.parquet`; out of phase 1, format reserved).

## 6. Producing a release

### 6.1 `libs/publish`

New library `libs/publish` (Qt-free; depends on `persistence`, `processing`,
`reduction`, `core`; Arrow optional, section 5.2):

```
include/pychron/publish/
  release.hpp        ReleaseSpec (title, authors, licence, selection, tags, options),
                     create_release(IStore&, Actor, ReleaseSpec) -> Result<Uuid>,
                     publish_release / withdraw_release / set_visibility
  build.hpp          BuildOptions (out dir, with_viewer, private_build, with_figures),
                     build_release(IStore&, Uuid release, BuildOptions, IProgress&) -> Result<Manifest>
  manifest.hpp       Manifest, read/write, verify(dir) / verify(dir, IStore&)
  records.hpp        catalog.json and records/*.json from the build's rows
  tables.hpp         row model of the five tables; row canonical text; rows_sha256
  parquet.hpp        IParquetWriter; make_parquet_writer() null when built without Arrow
  embargo.hpp        set / lift / list; is_embargoed(IStore&, scope, at)
  registrar.hpp      IRegistrar (DOI, IGSN) over IHttpClient (section 10)
  earthbank.hpp      release -> earthbank::Submission bridge (section 11)
  figures.hpp        sample_figures: ideogram, spectra, isochron of a sample as SVG and PDF (5.6)
src/                 parquet_writer.cpp (Arrow only here); figures use processing's writers
```

Build reads the store with `load_analysis_at` (4.3), one statement per
member list where the store has one (`WHERE analysis_uuid IN (...)`), never
a per-row subquery (`libs/persistence/AGENTS.md`). A release of 10^4
analyses builds in the time `elctl export` of the same takes plus Parquet
write; measured in phase 1 at 24, 400 and 2000 analyses and written in the
commit (store timing rule).

`IProgress` reports per stage (select, reduce, tables, figures, parquet,
records, manifest); the UI shows it on the processing thread.

### 6.2 `elctl publish`

```
elctl publish create  --db <url> [selection] [--tag <name>]... [--ia <name>|--no-ia]
                      --name <slug> --title <text> --author "<name>[|orcid|affiliation]"...
                      --licence <spdx> [--description <text>] [--publisher <name>]
                      [--embargo-until YYYY-MM-DD] [--supersedes <name|uuid>] [reduction options]
                      -> prints release uuid, member counts, embargoed counts
elctl publish build   --db <url> --release <name|uuid> --out <dir> [--with-viewer] [--with-figures]
                      [--private]                   private: include embargoed rows, manifest says so
elctl publish verify  <dir> [--db <url>]
elctl publish publish --db <url> --release <name|uuid> --manifest <dir>/manifest.json [--public]
elctl publish withdraw --db <url> --release <name|uuid> --reason <text>
elctl publish list    --db <url> [--state s] ;  elctl publish show --db <url> --release <r>
elctl publish doi     --db <url> --release <r> --out <dir> [--registrar datacite|datacite-test] [--findable] --yes
elctl publish igsn    --db <url> [--release <r> | --sample <name>...] [--registrar sesar|sesar-sandbox] --yes
elctl embargo set     --db <url> (--project P | --sample S | --analysis U)... [--until YYYY-MM-DD] [--reason t]
elctl embargo lift    --db <url> --id <uuid> | (--project|--sample|--analysis ...)
elctl embargo list    --db <url> [--live]
elctl publish upload  --db <url> --release <r> --dir <dir> --to <profile>      (section 9.5)
```

Reduction options = `elctl export`'s (`--sigma`, `--constants`,
`--decay-error`, `--plateau*`, `--group-by`, `--lab`, `--note`). Exit codes
as `export`: 0 done, 1 nothing matched or verify found a difference, 2 usage
or fatal. `--yes` required by anything that talks to a registrar; without it
the command prints what it would send and exits 2. Default registrar is the
test one (`datacite-test`, `sesar-sandbox`): production is named explicitly.

`publish` (the state change) requires a built manifest whose `pins` equal
the store's; it records `manifest_sha256` and `published_utc`. `--public`
sets visibility; default stays `private` (R1).

### 6.3 UI: Publish page

`pychron-ui`, Data workspace, new page **Publish** (View > Publish). Qt code
in `apps/pychron-ui/src/publish_page.*` over a `PublishBridge` calling
`libs/publish` on the processing thread (as `ExportBridge` does for
`elctl export`'s report; nothing of `libs/publish` touches Qt).

- Left: releases table (name, version, state, visibility, members, DOI,
  created). Buttons: New release..., Build..., Verify, Publish, Withdraw,
  Set public.
- New release dialog: selection from the Data browser's current filter or
  selection (same path as **Export**), tags, title, authors (table with
  ORCID), licence (combo of SPDX ids; preselected `CC-BY-4.0`, one constant
  `kDefaultLicence` in `libs/publish/release.hpp`; owner undecided and may
  change it before the page ships), description,
  embargo date, supersedes. Shows member count and count under embargo
  before OK.
- Build: directory chooser; options with-viewer, with-figures, private;
  progress bar per stage; status line names the directory and whether
  Parquet was written (built without Arrow: says so).
- Embargo dock on the Publish page: live embargoes, set and lift, scope
  picker (project, sample, analysis from the browser's selection).
- Registrations: DOI... and IGSN... buttons open the confirmation dialog of
  section 10 (what will be sent, to which registrar; production in red).
  Network work on a worker thread in real time, as `EarthbankWorker`.
- EarthBank: button **Publish release to EarthBank...** opens the existing
  EarthBank Publish dialog loaded with the release's analyses at their
  pins (section 11).

Not a Qt `QWizard`; a page like Flux window. Naming: the EarthBank dialog
keeps its name; the page is "Publish" and its items say "release".

## 7. Static viewer

Deliberately small. One directory `viewer/` copied beside the data when
`--with-viewer`, or hosted once by the owner at a fixed URL and pointed at
any release with `?release=<url>`. No build step at the lab: files ship in
the installer (`share/pychron/viewer/`), built in this repository under
`viewer/` (TypeScript, one bundle; `npm` only for the maintainer; CI job
builds it and checks the bundle hash into the installer step).

Scope, all of it:

1. Header: title, authors, licence, DOI (link), version chain
   (supersedes), download buttons (each Parquet, Schaen CSV, manifest).
2. Map: MapLibre GL with OpenStreetMap raster tiles (owner's decision;
   attribution shown as the OSMF tile usage policy requires, a `User-Agent`
   naming the viewer, no bulk prefetch; none when offline), one point per
   sample from `samples.parquet`, click opens the sample.
3. Sample list: name, material, project, n, ages summary; filter box over
   name, project, material; sort.
4. Sample page: metadata, IGSN link, ages (interpreted ages table), analysis
   table (`analyses.parquet` rows of the sample: runid, step, extract,
   included, 40Ar*/39ArK, %40Ar*, K/Ca, age ± both errors), figures (SVG
   inline, PDF link), Schaen CSV link.
5. Analysis row expands to its `isotopes.parquet` rows.

Data access: DuckDB-WASM (`@duckdb/duckdb-wasm`, pinned) with `read_parquet`
over HTTP range requests; one connection; queries parameterised; the
`spatial` extension not loaded (points come as lon/lat from `bbox`, no
spatial SQL). Host must allow range requests and CORS `GET`, `HEAD`
([DuckDB WASM known issues](https://duckdb.org/docs/current/clients/wasm/known_issues),
[troubleshooting](https://duckdb.org/docs/preview/clients/wasm/troubleshoot)).
A `file://` open does not work (no range, no CORS); README says to serve
the directory (`python3 -m http.server`).

Not in the viewer: reduction, fits, figure options, editing, auth, search
across releases (that is the index, 9.6), tiles, 3D, offline bundle of
tiles. Bundle target under 6 MB with DuckDB-WASM, loaded lazily on first
query.

Tests: Playwright smoke in CI's UI job against a fixture release built by
`elctl publish` from the test store (open, map has n points, sample page
shows the fixture's plateau age to the printed precision).

## 8. Sync path: lab to cloud

### 8.1 Topology

```
acquisition PC                      cloud (owner)                     other desktops
+-------------------+   uploader   +----------------------------+    direct PG
| local store       | -----------> | tenant PostgreSQL (shared  | <-- reduction clients
| SQLite (truth for |   outbox     |   schema, migrated)        |     (pychron-ui --db postgresql://...)
| acquisition)      | <----------- |   pub.* views (section 9)  |
| outbox.sqlite     |  change feed +----------------------------+
+-------------------+                       |
                                    pygeoapi per tenant, static buckets, index
```

Lab without cloud: everything in this spec works against the local store;
sync is absent; releases are built locally and hosted wherever the lab puts
them.

### 8.2 What of D3 to D7 is needed

| Stage | Needed | Why |
|---|---|---|
| D3 outbox, `OutboxPersister`, uploader, leases | **Yes** (outbox and uploader; leases only if a second acquisition program shares the tenant) | Acquisition must never block on the WAN (I12). Outbox as DVC 8.1 to 8.4; uploader as 8.3 against the tenant database over TLS. |
| D4 git importer | No | Legacy import is a one-time `elctl import` into whichever store. |
| D5 git publisher | No | Release directory replaces git as the publication artifact. `publish_*` tables stay unused. |
| D6 `ChangeFeed` | **Yes** | Acquisition PC's `catalog_cache` (offline queue building) and the UI's live panes follow the tenant's `change_log` by cursor (DVC 9.2), `LISTEN` as hint only (9.3). |
| D7 writable offline export | Later | Field laptops. Rules of DVC 8.7 unchanged; not needed for sharing. |

Order: D3 then D6 (phase 4, section 13).

### 8.3 Rules

- S1 Local SQLite store on the acquisition PC is written first, always
  (`StorePersister` inline, as today), then the same `RunIngest` (the
  `record_ingest.hpp` conversion the DVC note names) is spooled as outbox
  items (`analysis`, `blob` per sha, `catalog` for load and user ensure).
  Idempotency keys as DVC 8.2 (`Uuid::v5(record uuid, label)` today; kept).
- S2 Uploader applies items to the tenant through `IStore::ingest` of a
  PostgreSQL store opened with the lab's `pychron_acq` role over TLS
  (`sslmode=verify-full`, CA pinned in the profile). Retry and poison as
  DVC 8.3; status as 8.4; `elctl persist status|retry|show|export` as 8.4.
- S3 Reduction on the acquisition PC's local store after a run is allowed
  and lands as `changeset` outbox items (DVC 8.7 item 1) with expected heads;
  CAS on the tenant; conflict -> `sync_conflict` row and local `conflict`
  state with refresh-and-reapply or discard (8.7 items 4, 5). Same code
  path as D7's offline edits, so D7 later is the export file, not new sync
  logic.
- S4 Catalog edits (samples, projects, identifiers, embargoes, releases) are
  made on the tenant when it is reachable and refused locally when not
  (DVC 8.7 item 2: no CAS anchor). The acquisition PC's `catalog_cache`
  mirrors them by change feed. Exception: `ensure_user`, `add_load` and
  the identifier a queue needs are spooled as `catalog` items (DVC 3.8).
- S5 What stays local: `outbox.sqlite`, `<data>/spool`, logs, videos and
  snapshots (`analysis_artifact.url` points at the lab's file share; not
  uploaded), spectrometer configuration files, the local store itself.
  Blobs are uploaded (re-reduction needs them).
- S6 Both stores run the same migration set; `schema_compat.min_writer_version`
  on the tenant refuses an acquisition build behind it (DVC 11.4);
  `payload_schema_version` accepted current and previous.
- S7 Release creation, build and registration run against the tenant store
  when the lab has one (the shared truth) and against the local store
  otherwise. A release built from the local store of a lab that also has a
  tenant is private by construction (`build.store.store_id` differs from the
  tenant's) and `publish upload` refuses it.
- S8 Clock: outbox and uploader are off the simulated path (real time, DVC
  8.3 backoff on `SteadyClock`); `--sim` runs never upload (uploader not
  started).

### 8.4 Tests

Fault-injection suite of DVC action item 6 (crash between spool and ack,
duplicate ack, poison, dependency order) on SQLite outbox plus a PostgreSQL
tenant when `PYCHRON_TEST_PG_URL` is set; `ChangeFeed` cursor test (I6) and
a `catalog_cache` refresh test; TSan run of uploader (adds a thread).

## 9. Hosted tier

### 9.1 Tenancy

- One PostgreSQL database per lab (`pychron_<lab>`), same shared schema and
  migrations as any store; `elctl db migrate` run by the owner's deploy job.
  One cluster per region (section 9.7). Extensions: PostGIS (migration
  0004).
- Roles per tenant: `pychron_acq_<lab>`, `pychron_reduce_<lab>`,
  `pychron_view_<lab>` (DVC 11.2) for the lab's clients; `pychron_api_<lab>`
  for pygeoapi: SELECT on schema `pub` only, `search_path = pub`, no access
  to the base tables. Credentials issued per client by the owner's admin
  tool (`pychron-cloud`, a separate repository: deploy, tenants, keys,
  index; not in this tree).
- Backups: nightly base backup plus WAL to object storage in the tenant's
  region; 30 days. Releases (static directories) are additionally copied to
  a second region: preservation.

### 9.2 Read-only views, `pub_*`

Created by migration `0007` (`-- @sqlite skip`; views are PostgreSQL-only;
SQLite has no hosted tier). They live in the store's own schema, named
`pub_<name>` (a separate `pub` schema would be shared across the throwaway
test schemas and across tenants of one cluster); the spec writes them
`pub.<name>` below for short. Every view applies the visibility rule of 4.5
with `at = now()`. No base table is ever granted.

| View | Row | Columns |
|---|---|---|
| `pub.sample` | a visible sample | `samples.parquet` columns (5.3) plus `geom` (PostGIS point), `releases` (jsonb: [{name, version, doi}]), `igsn_doi` (`https://doi.org/<igsn>`), `updated_utc` |
| `pub.analysis` | a visible analysis at its newest published pins | `analyses.parquet` columns plus `geom`, `release_name`, `release_version` |
| `pub.interpreted_age` | a visible IA at its published revision | `interpreted_ages.parquet` columns plus `geom` |
| `pub.release` | a visible release | `release` row less internal uuids, `n_samples`, `n_analyses`, `catalog_url`, `doi_url`, `geom` (convex hull of its samples) |
| `pub.record` | one Records row per visible sample and per visible release | `id`, `type`, `title`, `description`, `geom`, `time_start_utc`, `time_end_utc`, `keywords` (text[]), `license`, `created_utc`, `updated_utc`, `external_ids` (jsonb), `links` (jsonb), `search` (`tsvector`, GENERATED from title, description, keywords) |

Values in `pub.analysis` are the published build's: a table
`pub_analysis_value` (`release_uuid`, `analysis_uuid`, the numeric columns),
written by `publish upload` (9.5) from the release's `analyses.parquet` rows,
not computed in SQL (the store cannot reduce). `pub.analysis` joins it to
the catalog. Same for `pub_interpreted_age_value`. These two tables are in
the lab's database, `pychron_publish`-writable, append-only per release.

Plan tests (`SchemaIndexes`, PostgreSQL only): `pub.sample` by bbox uses
`sample_geom_gix`; `pub.record` `search @@ websearch_to_tsquery(...)` uses
`pub_record_search_gin` (an index on the materialised search column, so
`pub.record` is a materialised view refreshed by `publish upload` and
`embargo` changes: `REFRESH MATERIALIZED VIEW CONCURRENTLY`).

### 9.3 pygeoapi

pygeoapi 0.24.0 (2026-07-28;
[releases](https://github.com/geopython/pygeoapi/releases)), one process
per tenant, config generated by `pychron-cloud` from a template.
PostgreSQL feature provider
([docs](https://docs.pygeoapi.io/en/latest/publishing/ogcapi-features.html)):
`search_path: [pub]`, `table: sample`, `id_field: sample_uuid`,
`geom_field: geom`, `time_field: last_analysis_utc`, `count: true`,
connection pool keys; CQL2 filters, `sortby`, `properties`, `bbox`,
`datetime` come with it. Transactions off.

Collections per tenant (OGC API - Features):

| Collection | View | Items |
|---|---|---|
| `samples` | `pub.sample` | one per visible sample |
| `analyses` | `pub.analysis` | one per visible analysis (geometry = sample's) |
| `interpreted-ages` | `pub.interpreted_age` | one per visible IA |
| `releases` | `pub.release` | one per visible release |

OGC API - Records: pygeoapi 0.24 ships record providers
ElasticsearchCatalogue, TinyDBCatalogue, CSWFacade only
([docs](https://docs.pygeoapi.io/en/latest/publishing/ogcapi-records.html));
none over PostgreSQL. Decision: a small pygeoapi provider plugin
`pychron_records` in `pychron-cloud` (Python; subclass of the PostgreSQL
provider; `type: record`; implements `q` as `search @@
websearch_to_tsquery('english', :q)` over `pub.record`, `bbox`, `datetime`,
`sortby`, `properties`; no transactions). Collection `records` per tenant.
Until that plugin exists, `pychron-cloud index` writes the tenant's visible
`pub.record` rows to a TinyDB file and configures `TinyDBCatalogue` (fine
to about 10^4 records; the plugin replaces it). Either way, the public URL
shape is the standard's.

Formats: GeoJSON, HTML (pygeoapi's), CSV where the provider gives it; the
release's Parquet is linked from each record (`enclosure`), not served by
pygeoapi.

### 9.4 Auth for private data

pygeoapi has no access control
([security](https://docs.pygeoapi.io/en/latest/security.html)). Decision:
the reverse proxy (Caddy) in front of every tenant process enforces it;
pygeoapi listens on localhost only.

- URL shape (domain `pychronlabs.com`, owner's decision; releases on a
  separate host from the API):
  `https://<region>.api.pychronlabs.com/<lab>/` -> tenant's pygeoapi;
  `https://<region>.api.pychronlabs.com/index/` -> the cross-lab index (9.6);
  `https://releases.pychronlabs.com/<lab>/<name>-v<n>/` -> static release
  directories (object storage behind the same proxy, immutable cache
  headers, CORS `GET`, `HEAD`, range).
- Private by default: a tenant's collections and its releases need
  `Authorization: Bearer <token>`; tokens issued per lab (and per reader the
  lab invites) by `pychron-cloud keys`, stored hashed, scoped to a tenant
  and to `read`. Proxy checks the hash and forwards; no token reaches
  pygeoapi.
- Public: the lab sets `visibility = public` on a release; the views then
  show its rows to everyone, and the proxy allows unauthenticated `GET` on
  that tenant's collections. Row-level privacy is the view's (4.5), not the
  proxy's: a private release's rows are never in `pub.*` at all, so a
  public tenant endpoint cannot leak them. A tenant with nothing public
  answers 401 to everything.
- Static releases: `private` -> object key under `private/<lab>/...`,
  served only with a token (proxy); `public` -> `public/<lab>/...`, served
  to all. `withdraw` moves nothing; it marks (a `withdrawn.json` beside the
  manifest, viewer shows the notice).
- fastgeoapi (OIDC + OPA) is the upgrade path if labs want per-user login;
  not in v1.

### 9.5 `publish upload`

`elctl publish upload --release <r> --dir <dir> --to <profile>`: profile in
`profiles.toml` (`libs/earthbank/profiles.hpp` pattern: host, lab, no
secret; token in `ISecretStore`). Steps: verify the directory (5.8);
PUT every file to the release bucket under `<visibility>/<lab>/<name>-v<n>/`
(S3-compatible API over `IHttpClient`; one journal as the EarthBank uploader
keeps: an unanswered PUT is in doubt, re-checked by `HEAD` and sha, never
blindly re-sent); then write `pub_analysis_value` and
`pub_interpreted_age_value` rows from the directory's Parquet, refresh
`pub.record`. A lab without a tenant hosts the directory itself: `upload`
then stops after the PUT step (`--static-only`).

### 9.6 Cross-lab Records index

`https://<region>.api.pychronlabs.com/index/`: one pygeoapi, collection
`records`, over a database `pychron_index` fed by `pychron-cloud index`
(hourly): `INSERT ... ON CONFLICT` of every tenant's `pub.record` rows whose
release is `public`, with `lab` and `region` columns added; never a private
row (the query runs as `pychron_api_<lab>`, which sees `pub` only, and `pub`
holds only visible rows). Records carry `links` to the lab's own
collections and to the release directory. Regions federate by the same
job pulling the other regions' index rows (public rows only): one global
index, three copies.

### 9.7 Region choice

Tenant chooses region at creation: `us`, `eu`, `au` (confirmed by the
owner; where the labs are). Database, backups, private releases and the tenant's pygeoapi
stay in that region. Public releases are copied to every region's
`releases` bucket (preservation and locality). The index is global (9.6).
A tenant moves region by backup and restore; `store_id` unchanged, releases
re-copied; tokens reissued.

### 9.8 Decided later: pricing

Pricing tiers of the hosted tier (storage, private readers, regions,
preservation copies, DOI minting, what a free tier holds) are the owner's
commercial decision, not yet made. Nothing in this design depends on them:
every limit is a per-tenant setting in `pychron-cloud` (quota bytes,
reader token count, regions allowed), read at provisioning, absent in the
store and in `libs/publish`. Phases 1 to 5 proceed without them.

## 10. Registrations

### 10.1 DOI per release (DataCite)

- DataCite REST API ([create](https://support.datacite.org/docs/api-create-dois),
  [findable](https://support.datacite.org/docs/how-do-i-make-a-findable-doi-with-the-rest-api)):
  `POST https://api.datacite.org/dois` (test: `https://api.test.datacite.org/dois`),
  `Content-Type: application/vnd.api+json`, HTTP basic with repository id
  and password. Without `event` a Draft; `"event": "publish"` makes it
  Findable; a Draft becomes Findable by a later state update. Findable DOIs
  cannot be deleted. Required for Findable: `url`, `doi` or `prefix`,
  `creators`, `titles`, `publisher`, `publicationYear`,
  `types.resourceTypeGeneral`. Metadata Schema 4.6 (2024-12-05;
  [versions](https://schema.datacite.org/versions.html)); the JSON
  attributes follow it.
- Payload from the release: `prefix` (owner's or lab's, below),
  `url` = the release's landing page (`.../<lab>/<name>-v<n>/viewer/` or
  the lab's own URL), `creators` from `authors` (name, `nameIdentifiers`
  ORCID, `affiliation`), `titles`, `publisher`, `publicationYear`,
  `types: {resourceTypeGeneral: "Dataset", resourceType: "40Ar/39Ar
  geochronology release"}`, `descriptions` (release description plus
  "pychron-cpp release <name> v<n>, <n> analyses, <n> samples"),
  `subjects` (keywords), `rightsList` (SPDX licence with `rightsUri`),
  `dates` (`Created`, `Available` = embargo end when any), `geoLocations`
  (bbox of samples as `geoLocationBox`, or one `geoLocationPoint` per
  sample up to 100), `relatedIdentifiers`: each sample's IGSN
  (`relatedIdentifierType: IGSN`, `relationType: HasPart`), the superseded
  DOI (`IsNewVersionOf`), the lab's instrument when it has a PIDINST
  (`IsCompiledBy`, later), `version: "<n>"`, `formats`, `sizes`,
  `language: en`, `schemaVersion: http://datacite.org/schema/kernel-4`.
- Flow in `elctl publish doi` and the Publish page: release must be
  `published`; a Draft is minted first (`doi_state = draft`), its DOI
  written into `release.doi` and the directory rebuilt (CITATION.cff,
  manifest, records carry it) and re-uploaded; then with `--findable`
  the state update. `external_id` row per request; `in_doubt` when no
  answer (never re-sent blindly; `GET /dois/<doi>` settles it). Secrets via
  `ISecretStore`, never a file or a log (EarthBank rules apply verbatim).
- Two routes, both offered (owner's decision): the owner's prefix under
  pychronlabs' DataCite membership, run from the hosted tier (owner's
  repository account in `pychron-cloud`, on `publish upload --public` when
  the release asks for a DOI); or the lab's own institutional repository
  account and prefix, run from `elctl publish doi` with the lab's
  credentials in its `ISecretStore`. `publisher` follows the prefix used:
  the owner's route sets `publisher = "Pychron Labs"` with the lab named
  in `contributors` (`HostingInstitution`), the lab's route sets its
  institution. The tenant's profile names its route (`doi_route =
  owner|lab`); a lab without a tenant has only the lab route. Same payload
  code (`libs/publish/registrar.hpp`); the cloud side calls `elctl publish
  doi` in a container with the tenant's store.

### 10.2 IGSN per sample (SESAR)

- IGSNs are DataCite DOIs since 2023-01 (SESAR prefix `10.58052`;
  [SESAR](https://www.geosamples.org/?p=4290)); SESAR is an allocating
  agent and a DataCite Registered Service Provider (2025-06). Registration:
  `POST https://app.geosamples.org/webservices/upload.php` (sandbox host
  for tests), JWT auth, XML body in SESAR sample schema 3.0 or 4.0
  (4.0: [schema on Zenodo](https://zenodo.org/records/3875531));
  developer docs at [geosamples.github.io/sesar-doc](https://geosamples.github.io/sesar-doc/web_services/sample_registration.html).
  Web-service access since SESAR v8.0 (2021-07) needs a request form per
  account; a `user_code` per account prefixes every IGSN it mints.
- Payload per sample (schema 4.0 elements): `user_code`, `sample_type`
  (`Individual Sample`), `name` (sample name), `material` (`Rock` or
  `Mineral` from the material table, vocabulary map in
  `libs/publish/igsn_vocab.cpp`, tested as EarthBank's `vocab.cpp` is),
  `classification` (lithology, mapped), `latitude`, `longitude`,
  `elevation`, `elevation_unit: meters`, `primary_location_name`
  (`location`), `geological_unit` (`unit`), `collector` (PI),
  `collection_method`, `description` (note), `current_archive`
  (`storage_location`), `comment` ("registered by pychron-cpp <version>").
  Missing values left out; a sample without a location is refused by the
  pre-flight (SESAR requires none, but an IGSN without a place is worth
  little; `--allow-no-location` overrides).
- Pre-flight before any write (as EarthBank): samples that already carry
  `igsn` are skipped; vocabulary misses listed; what will be sent printed;
  `--yes` required; default sandbox.
- Response: IGSN per sample; written as a catalog edit to `sample.igsn`
  (audited by `change_log`, D6) plus an `external_id` row. No answer:
  `in_doubt` row, and SESAR's `getSampleProfile`-style lookup by name and
  user code settles it before anything is re-sent. One request per sample,
  never retried in a client.
- Account: each lab its own SESAR account and user code (owner's decision);
  its IGSNs carry its user code. The lab requests web-service access from
  SESAR itself; JWT credentials live in the lab's `ISecretStore` (or the
  tenant's secret store for runs from the hosted tier, where the same
  `elctl publish igsn` runs in the tenant's container). The owner mints no
  IGSN on a lab's behalf.
- `sample.igsn` already feeds `SampleInfo::igsn`, the Schaen irradiation
  table, EarthBank's `Samples.igsn` and now `records/*.json`
  `externalIds`, `samples.parquet` and the DOI's `relatedIdentifiers`.

### 10.3 Where identifiers land

| Identifier | Catalog column | Receipt | In the release |
|---|---|---|---|
| Release DOI | `release.doi`, `release.doi_state` | `external_id` (`release`, `doi`) | manifest, CITATION.cff, catalog.json `cite-as`, records/release.json, report metadata, viewer header |
| Sample IGSN | `sample.igsn` | `external_id` (`sample`, `igsn`) | `samples.parquet`, records `externalIds`, Schaen irradiation table, EarthBank sample |

## 11. EarthBank as downstream export

Unchanged: `libs/earthbank` maps `processing` analyses to one `Submission`
per sample (`mapping.hpp`); pre-flight, workbooks, upload as built; all its
rules (no test reaches production, journal, no retries) stand.

Added, `libs/publish/earthbank.hpp`: `release_dataset(IStore&, Uuid
release) -> Result<processing::Dataset>` loads the release's members at
their pins (4.3). `elctl earthbank preflight|export-xlsx|upload --release
<name|uuid>` take it in place of the selection flags (selection flags and
`--release` together: usage error). The Publish page button (6.3) opens the
EarthBank Publish dialog with that dataset. Mapping gains, from the
release: `literature` / data point reference = the release DOI when any
(mapping.hpp lists it among what was left unset for want of a source);
`Samples.igsn` from `sample.igsn` as today. Nothing else in the submission
changes; `content_hash(Submission)` therefore changes only for releases
with a DOI, and the journal's "changed data" rule applies.

Reason to keep it downstream: EarthBank has no embargo or versioning; a
release is the citable unit and EarthBank one more place it is listed.

## 12. Tests

| Area | Test |
|---|---|
| Migration | `SchemaTest` lists `0006 release`; both engines; append-only triggers on `release_sample`, `release_interpreted_age`; `release` guard allows only its listed columns |
| Embargo | project embargo hides its samples' analyses; lifted by date and by hand; `UNION` plan test, no `SCAN` |
| Release | create pins every head and ref; a retag after creation does not change the build; publish freezes; delete only draft; supersede chain |
| Build | fixture store (tests/persistence five-rows fixture plus IR1010 import fixture) builds; every file in manifest with matching sha; `rows_sha256` stable across two builds; private build includes embargoed rows and says so; public build omits them |
| Parquet | each table reads back with Arrow (test only) with the listed columns and types; `geo` metadata validates against the GeoParquet 1.1 JSON schema (vendored); built without Arrow: no `data/` and exit message |
| Records | `catalog.json` and each record checked structurally (required fields of 20-004r1 present, values of the fixed ones exact); JSON-schema validation when a validator is a dependency; every `item` link resolves to a file in the directory |
| Figures | `SceneView` and `draw_scene` of one Scene: same ranges, points, steps, ellipses, corner texts (UI parity test); SVG and PDF byte-stable across two runs and equal to committed fixtures; PDF parses (xref offsets and stream lengths, checked by a small reader in the test); a release with figures verifies against a rebuild |
| Verify | corrupt a cell -> names the table; delete a file -> names it; other `reduction_version` -> says so, compares nothing |
| Viewer | Playwright smoke in `dev-ui` job (7) |
| Sync | DVC fault-injection suite; `ChangeFeed` cursor; TSan on uploader |
| Hosted | `pub.*` views on PostgreSQL (`PYCHRON_TEST_PG_URL`): private release invisible; public visible; embargo removes a row; `pychron_api` cannot read a base table; plan tests for bbox and `search` |
| Registrars | `FakeDataCite`, `FakeSesar` (as `FakeEarthBank`): payload shape, draft then findable, in-doubt on no answer, nothing sent without `--yes`, default is the test host; live tests skipped unless `PYCHRON_TEST_DATACITE_*` / `PYCHRON_TEST_SESAR_*` set, never in CI |
| EarthBank | `--release` gives the same `Submission` as the selection it was made from, plus the DOI; `content_hash` otherwise equal |

## 13. Order of work (phases)

Static tier first. Each phase ends merged on `develop` with its tests.

1. **Release model.** Done 2026-10-19 (plan
   `docs/superpowers/plans/2026-10-18-data-publishing-phase-1.md`). Migration
   `0007`, `release*`, `embargo`, `external_id`, `pub_*` views (PostgreSQL),
   `load_analysis_at`, `libs/publish` core (`release.hpp`, `embargo.hpp`,
   `tables.hpp`, `manifest.hpp`, `records.hpp`, `build.hpp`, `parquet.hpp`),
   `elctl publish create|build|verify|publish|withdraw|list|show|delete`,
   `elctl embargo`. Build writes Schaen tables, records, constants,
   CITATION, README, manifest; Parquet behind `PYCHRON_PUBLISH` with Arrow
   (Homebrew, apt or the vcpkg `publish` feature); `docs/release_format.md`.
2. **Figures.** Done 2026-10-18 (plan
   `docs/superpowers/plans/2026-10-18-data-publishing-phase-2.md`).
   `scene_layout.hpp` (ranges shared with `SceneView`), `scene_draw.hpp`,
   `scene_svg`, `scene_pdf`, `libs/publish` `figures.hpp`,
   `--with-figures`, verify with figures.
3. **Viewer.** `viewer/` in-repo, installer ships it, `--with-viewer`,
   Playwright smoke.
4. **Publish page** in `pychron-ui` (6.3) with embargo dock.
5. **Sync.** D3 outbox, `OutboxPersister`, uploader, `elctl persist`;
   D6 `ChangeFeed`; `catalog_cache`.
6. **Hosted tier.** `pychron-cloud` repository: tenant provisioning,
   Caddy, pygeoapi config template, `publish upload`, index job,
   `pychron_records` plugin (TinyDB first), regions.
7. **Registrations.** `registrar.hpp`, DataCite (test first), SESAR
   (sandbox first), Publish page dialogs, `elctl publish doi|igsn`.
8. **EarthBank from a release.**
9. Later: D7 offline export; `--with-raw`; fastgeoapi per-user login.

Phase 1 alone gives a lab a citable, verifiable directory it can host
anywhere; nothing below it is needed for that.

## 14. Open questions

None.
