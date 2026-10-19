# The release directory format

A *release* is an immutable, citable directory of a selection of a lab's
reduced 40Ar/39Ar data, made by `elctl publish build` (or **View > Publish…**
in pychron-ui) from the store: GeoParquet tables, an OGC API - Records
catalogue, the Schaen et al. (2021) report, and a manifest that lists every
file with its hash and the revisions the release pins. It is hostable on any
static web server and verifiable against the store it came from. The design
is `docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md`;
this page is the reader's reference to what is in the directory.

## Layout

```
<name>-v<version>/
  manifest.json                   every file with its SHA-256; the pinned revisions; how it was built
  catalog.json                    OGC API - Records collection (a crawlable catalogue)
  records/<sample uuid>.json      one GeoJSON record per sample
  records/release.json            the release itself as a record
  data/samples.parquet            GeoParquet (see below); absent when the build had no Arrow
  data/positions.parquet          Parquet
  data/analyses.parquet           GeoParquet
  data/isotopes.parquet           Parquet
  data/interpreted_ages.parquet   GeoParquet
  tables/release.csv, release.json          the Schaen report of every analysis of the release
  tables/<sample>/report.csv, report.json   the same report, per sample
  figures/<sample>/<kind>.svg, <kind>.pdf   ideogram, spectrum, isochron; only with --with-figures
  constants.json                  the decay constants and ratios the ages used
  LICENSE.txt                     the licence text (CC-BY-4.0, CC-BY-SA-4.0 and CC0-1.0 ship with pychron)
  CITATION.cff                    how to cite the release
  README.md                       one paragraph: what this is
```

`<sample>` is the sample's name with every character outside `A-Z a-z 0-9 . _ -`
replaced by `_` (an empty name, dots only, a Windows device name or a
trailing dot become `sample`, `name_`); when two samples of the release have
the same safe name (case ignored), or the name had to be invented, each gets
`-<last 8 hex of its uuid>`. The paths in `manifest.json` are the
truth; the names are for people.

Every file the build writes is marked as a user file, so macOS Gatekeeper
opens it.

## Reading it

Serve the directory over HTTP (`python3 -m http.server` in it) and point a
Parquet reader at `data/*.parquet`: DuckDB (`SELECT * FROM
'http://host/r-v1/data/analyses.parquet'`), GeoPandas, QGIS. A file open from
disk works with every Parquet reader too. The CSV under `tables/` opens in a
spreadsheet. The records under `records/` are plain GeoJSON.

## Verifying it

```bash
elctl publish verify <dir>                 every file against manifest.json
elctl publish verify <dir> --db <url>      and against a rebuild of the same release from the store
```

A rebuild from the same store, software version and options gives the same
row hashes for every table (`tables[].rows_sha256`) and the same bytes for
every non-Parquet file. Parquet bytes are not promised across Arrow
versions: the row hashes are what verify compares.

## The tables

Common conventions:

- Uuids are 36-character lowercase text. Times are `timestamp[us, UTC]`.
  Numbers are `double`. A value the store does not have is null, never a
  sentinel; a non-finite number is null.
- Every `<x>_err` column is the uncertainty of `<x>` at the sigma level of
  the release's options (`manifest.build.options.sigma`, 2 by default), as
  the Schaen report writes it. Intensities are in fA, ages in Ma, J
  dimensionless.
- Row order is fixed: samples by name then uuid; positions by irradiation,
  level, position (identifiers without an irradiation last); analyses by
  timestamp then uuid; isotopes by analysis then isotope; interpreted ages by
  sample name then name.
- A table with a location (`samples`, `analyses`, `interpreted_ages`) is
  GeoParquet 1.1: file metadata `geo` (`primary_column` `geometry`, WKB
  encoding, CRS OGC:CRS84 as PROJJSON, a `covering` naming the `bbox`
  struct), a `geometry` column of WKB points (longitude, latitude) and a
  `bbox` struct column (`xmin`, `ymin`, `xmax`, `ymax`). Without a location
  both are null.

### `samples.parquet`

One row per sample of the release (the samples of its analyses; a sample
under a live embargo is absent from a public build).

| Column | Type | What |
|---|---|---|
| `sample_uuid` | uuid | the catalog's sample row |
| `name` | text | sample name |
| `igsn` | text | IGSN, when the sample has one |
| `project`, `project_uuid` | text, uuid | the sample's project |
| `principal_investigator` | text | the project's PI, as displayed |
| `material`, `grainsize` | text | the material row |
| `geometry`, `bbox` | WKB, struct | the sample's location |
| `elevation_m` | double | elevation, metres |
| `location`, `unit`, `lithology`, `lithology_class`, `lithology_type`, `lithology_group` | text | the sample row |
| `approximate_age_ma` | double | the sample row's approximate age |
| `note` | text | the sample row's note |
| `n_analyses`, `n_ages`, `n_interpreted_ages` | int32 | counted in the release: its analyses, those with an age, its interpreted ages |
| `first_analysis_utc`, `last_analysis_utc` | timestamp | of its analyses in the release |
| `record_path` | text | `records/<uuid>.json` |

### `positions.parquet`

One row per identifier (labnumber) in the release: the Schaen report's
irradiation table.

| Column | Type | What |
|---|---|---|
| `identifier`, `identifier_uuid` | text, uuid | the labnumber |
| `sample_uuid` | uuid | its sample |
| `irradiation`, `level`, `position` | text, text, int32 | where it was irradiated; null when it was not |
| `reactor` | text | the production's reactor |
| `irradiation_start_utc`, `irradiation_duration_h` | timestamp, double | the chronology's first dose and total hours |
| `holder`, `packet` | text | the level's holder and the position's packet |
| `weight_mg` | double | the position's weight |
| `monitor_name`, `monitor_material`, `monitor_age_ma`, `monitor_age_ma_err` | text, text, double, double | the fluence monitor of the flux record |
| `j`, `j_err`, `j_position_err` | double | J, its uncertainty, the position error |
| `lambda_k_total`, `lambda_k_total_err` | double | a lambda_K override of the flux record |
| `p_40_39_k`, `p_38_39_k`, `p_37_39_k`, `p_39_37_ca`, `p_38_37_ca`, `p_36_37_ca`, `p_36_38_cl` | double | the production ratios (40Ar/39Ar)K, (38Ar/39Ar)K, (37Ar/39Ar)K, (39Ar/37Ar)Ca, (38Ar/37Ar)Ca, (36Ar/37Ar)Ca, (36Ar/38Ar)Cl |
| `p_40_39_k_err`, `p_38_39_k_err`, `p_37_39_k_err`, `p_39_37_ca_err`, `p_38_37_ca_err`, `p_36_37_ca_err`, `p_36_38_cl_err` | double | their uncertainties |
| `ca_k`, `ca_k_err`, `cl_k`, `cl_k_err` | double | Ca/K and Cl/K of the production |
| `flux_revision_uuid`, `production_revision_uuid`, `chronology_revision_uuid` | uuid | the reference revisions the release pins for the identifier |

### `analyses.parquet`

One row per analysis of the release: the Schaen report's analyses table
plus the analysis's identity and the revisions pinned for it.

| Column | Type | What |
|---|---|---|
| `analysis_uuid`, `runid`, `identifier` | text | the analysis, its run id (`<identifier>-<aliquot>[<step>]`), its labnumber |
| `aliquot`, `increment` | int32 | aliquot; step index (-1: no step) |
| `step` | text | the step letters (`A`, `B`, ... `AA`) |
| `sample_uuid`, `sample`, `project`, `material` | uuid, text | its sample |
| `analysis_type`, `experiment_type` | text | `unknown`, `air`, ... |
| `timestamp_utc` | timestamp | when it was measured |
| `mass_spectrometer`, `extract_device`, `analyst`, `laboratory` | text | who and what measured it |
| `extract_value`, `extract_units`, `extract_duration_s`, `cleanup_duration_s`, `weight_mg`, `beam_diameter`, `pattern` | double, text, ... | the extraction |
| `irradiation`, `level`, `position` | text | where it was irradiated |
| `tag` | text | the head tag at the release (`ok`, `omit`, `outlier`, ...) |
| `included` | bool | whether it enters the group ages |
| `group_id` | text | the report's group (aliquot or identifier, per the options) |
| `comment` | text | the annotation |
| `j`, `j_err` | double | J used for its age |
| `ar40_fa`, `ar39_fa`, `ar38_fa`, `ar37_fa`, `ar36_fa` | double | corrected intensities: baseline, blank, detector factors and discrimination, and decay for 37Ar and 39Ar |
| `ar40_fa_err`, `ar39_fa_err`, `ar38_fa_err`, `ar37_fa_err`, `ar36_fa_err` | double | their uncertainties |
| `ar40_blank_fa`, `ar39_blank_fa`, `ar38_blank_fa`, `ar37_blank_fa`, `ar36_blank_fa` | double | the blank subtracted from each |
| `ar40_blank_fa_err`, `ar39_blank_fa_err`, `ar38_blank_fa_err`, `ar37_blank_fa_err`, `ar36_blank_fa_err` | double | their uncertainties |
| `f`, `f_err` | double | 40Ar*/39ArK |
| `rad40_pct` | double | 40Ar* in percent |
| `k39_fa`, `k39_pct_group`, `k39_cum_pct` | double | 39ArK, as a percent of the group, cumulative |
| `kca`, `kca_err`, `kcl`, `kcl_err` | double | K/Ca, K/Cl |
| `iso_39_40`, `iso_39_40_err`, `iso_36_40`, `iso_36_40_err`, `iso_corr` | double | inverse isochron coordinates and their correlation |
| `age_ma`, `age_err_analytical_ma`, `age_err_with_j_ma` | double | the age; its analytical uncertainty; with the J uncertainty added |
| `age_note` | text | why there is no age, when there is none |
| `record_sha256` | text | SHA-256 (hex) of the acquisition record |
| `rev_signals`, `rev_intercepts`, `rev_baselines`, `rev_blanks`, `rev_icfactors`, `rev_tags`, `rev_annotation` | uuid | the revision of each kind the release pins |
| `geometry`, `bbox` | WKB, struct | the sample's location |

### `isotopes.parquet`

One row per (analysis, isotope).

| Column | Type | What |
|---|---|---|
| `analysis_uuid`, `isotope`, `detector`, `units` | text | the isotope (`Ar40`, ...), its detector, `fA` |
| `intercept`, `intercept_err`, `fit`, `error_type`, `n`, `fn`, `reviewed` | double, text, int32, bool | the intercept fit: value, error, fit kind, error type, points, filtered points, reviewed |
| `baseline`, `baseline_err`, `baseline_fit` | double, text | the baseline |
| `blank`, `blank_err`, `blank_fit` | double, text | the blank and where it came from |
| `ic_factor`, `ic_factor_err` | double | the detector intercalibration factor |
| `discrimination` | bool | whether a discrimination correction applied (reserved; null in this version) |
| `decay_corrected` | bool | true for 37Ar and 39Ar |
| `corrected`, `corrected_err` | double | the decay-corrected intensity (`ar<n>_fa` of the analyses table) |
| `blob_sha256`, `n_points` | text, int32 | the raw signal series in the store (its content hash) and its length; not in the release |

### `interpreted_ages.parquet`

One row per interpreted age of the release, at its pinned revision.

| Column | Type | What |
|---|---|---|
| `ia_uuid`, `name` | uuid, text | the interpreted age |
| `sample_uuid`, `identifier` | uuid, text | whose it is (of its first member) |
| `age_kind` | text | `plateau`, `weighted_mean`, `isochron`, `integrated` or `other` |
| `age_ma`, `age_err_ma` | double | the age |
| `mswd`, `n` | double, int32 | MSWD and the number of analyses |
| `kca`, `kca_err` | double | K/Ca |
| `plateau_k39_pct` | double | percent of 39ArK in the plateau (reserved; null in this version) |
| `trapped_40_36`, `trapped_40_36_err` | double | the isochron's trapped ratio (reserved; null in this version) |
| `members` | JSON | `[{"analysis_uuid", "plateau_step", "tag"}]` |
| `revision_uuid` | uuid | the pinned revision |
| `doc` | JSON | the full interpreted-age document as stored |
| `geometry`, `bbox` | WKB, struct | the sample's location |

## Figures

`elctl publish build --with-figures` adds, per sample:

- `ideogram.svg` and `.pdf`: the ideogram of the sample's analyses.
- `spectrum.svg` and `.pdf`: the age spectrum of a step-heated aliquot. A
  sample with more than one has `spectrum-<identifier>-<aliquot>.svg` and
  `.pdf` per aliquot (`spectrum-66012-01.svg`), the name made safe as
  `<sample>` is, with `-2`, `-3` when two would be the same ignoring case.
- `isochron.svg` and `.pdf`: the inverse isochron, when at least three of the
  sample's analyses are included.

A figure that cannot be drawn (a sample with no ages, say) is left out and
the build says so. The figures use the figure editors' defaults, with the
release's `sigma` for the error bars and its `plateau`, `plateau_steps` and
`plateau_gas` for the spectrum.

The SVG is text, sized in points, with fonts named (Helvetica, Arial,
sans-serif) and not embedded. The PDF is one page of PDF 1.4, uncompressed,
in the standard fonts (Helvetica; Symbol for Greek letters; a character
neither has is `?`), with no document id and the release's `created_utc` as
its creation date. Both are drawn from the same layout, so they show the same
thing. The same release built twice gives the same bytes for every figure,
so `verify` compares them like any other file; `verify --db` rebuilds with
figures when the directory has them.

## `manifest.json`

```json
{
  "format": "pychron-release/1",
  "release": {"uuid", "name", "version", "title", "description", "state", "visibility",
              "licence", "authors": [{"name", "orcid", "affiliation"}], "publisher", "doi",
              "supersedes": {"uuid", "doi"}, "created_utc", "published_utc", "embargo_until_utc"},
  "build": {"built_utc", "software", "reduction_version", "constants_preset", "options": {...},
            "store": {"kind", "schema_version", "store_id"}, "private_build",
            "omitted_embargoed": {"samples", "analyses"}},
  "pins": {"bookmark_uuid",
           "analyses": [{"uuid", "runid", "record_sha256", "heads": {"<kind>": "<revision>"},
                         "refs": {"<ref object>": "<revision>"}}],
           "interpreted_ages": [{"uuid", "revision"}],
           "samples": ["<uuid>"]},
  "tables": [{"path", "rows", "rows_sha256", "columns": [...]}],
  "files": [{"path", "bytes", "sha256", "media_type"}]
}
```

- `files` lists every file but `manifest.json`, by path.
- `rows_sha256` is SHA-256 over the table's rows in canonical text: cells
  joined by `\x1f`, rows by `\x1e`; a double as its shortest round-trip
  text, a time as ISO-8601 UTC with microseconds, a bool `true`/`false`,
  null empty, JSON as it is, WKB as lower-case hex. Independent of the
  Parquet encoder.
- `pins` is the release's identity: two manifests with equal `pins`,
  `build.reduction_version` and `build.options` describe the same data.
- `private_build` true means embargoed rows are in the directory; such a
  build is for the lab's own use and is never hosted as public.
- `build.options` holds `sigma`, `constants`, `decay_error`, `plateau`,
  `plateau_steps`, `plateau_gas`, `group_by`, `laboratory`, `notes`: the
  reduction and report options of `elctl export`.

## `catalog.json` and `records/`

OGC API - Records - Part 1: Core 1.0, as a crawlable catalogue: `catalog.json`
is the collection (`type: Collection`, `itemType: record`, title,
description, licence, spatial and temporal extent over the samples that have
a location, and links: `item` to every record, `enclosure` to every Parquet
table that was written, `describedby` to the manifest, `cite-as` to the DOI
when there is one). Each record is a GeoJSON Feature whose `properties` hold
the record core (`type`, `title`, `description`, `keywords`, `language`,
`license`, `created`, `updated`, `externalIds`, `contacts`, `formats`,
`themes`) and, prefixed `pychron:`, the sample's uuid and counts and its
interpreted ages. Links inside a record are relative to the record's own
location (`../catalog.json`, `../tables/<sample>/report.csv`).

## Reproducing a release

A release pins revisions, not values: `elctl publish build --release <r>`
rebuilds from the pins alone. `manifest.build.store` says which store
(`kind`, its schema version, an id derived from its url) and
`manifest.build.software` which pychron-cpp built it. A rebuild with another
`reduction_version` may give other values; `verify --db` then says so and
compares nothing else.
