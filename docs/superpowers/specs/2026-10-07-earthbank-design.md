# EarthBank: publishing Ar-Ar data from pychron-cpp

Date: 2026-10-07
Status: Draft
Owner: Jake Ross
Source being migrated: `UMelbournePychron/pychron`, branch `dev/dc2025`,
`pychron/ausgeochem/` (about 5300 lines of Python; last commit e541a4ffa,
2026-07-10). It exists only on that fork. Its `README.md` and
`UPLOAD_GUIDE.md` are the reference for the service's behaviour; this spec
quotes what it relies on, marked **[legacy]**. Everything else is decided
here.
Builds on: `2026-10-02-data-browsing-visualization-design.md` (the analysis
model, sources, the data browser; it listed AusGeochem as out of scope),
`2026-10-02-arar-reduction-design.md` (reduction, group ages), the
publication report (`libs/processing` `report.hpp`, `elctl export`).

## 1. Intent

EarthBank is AuScope's data platform for geochemistry (the AusGeochem /
LithoSurfer service at `https://ausgeochem.auscope.org.au`). A lab publishes
40Ar/39Ar results there as, per sample: a sample record with its location, an
Ar-Ar data point, one row per aliquot, one per measurement (analysis), the
constants and monitor the ages rest on, and a summary age.

Legacy pychron does this from a pipeline node, in three modes that share one
payload builder **[legacy]**:

| Mode | What happens | Needs an account |
|---|---|---|
| Upload | POSTs the records to the REST API | yes, with write access to a data package |
| Export xlsx | writes AusGeochem's template workbooks; the user imports them on the website | no |
| Pre-flight | builds the same records and checks every controlled term against the service's vocabularies | no for most vocabularies |

This spec brings all three to pychron-cpp, from the data browser and from
`elctl`.

Not in scope: populating `funding`, `literature` and the two `Props` sheets
(legacy leaves them empty too); grain size and aliquot weight, which
pychron-cpp's `Analysis` does not carry (section 4.5); disciplines other than
Ar-Ar; editing or deleting records already on EarthBank.

## 2. Constraints

- **No sandbox [legacy].** The vendor publishes no test host. A write goes to
  production. Design and tests follow from this: nothing in CI talks to
  EarthBank, a write is preceded by a check that it can succeed, and a
  failed upload can be resumed without writing anything twice.
- **Write permission is not visible [legacy].** A data package's record does
  not say whether the caller may write to it. The server refuses at the first
  write with `500 "Package is not writable by user."`.
- pychron-cpp today has no HTTP client, no xlsx writer and no secret store.
  Its dependencies are asio, toml++, spdlog, nlohmann-json and GoogleTest;
  Qt appears only in the UI and, privately, in `libs/persistence`.
- Qt does not appear in a `libs/` public header (AGENTS.md).
- A file written for the user goes through `mark_as_user_file`.

## 3. Shape

```
processing::Analysis, reduce_analysis, group ages (plateau, mean, isochron, integrated)
        |
libs/earthbank                      Qt-free; builds on every platform
  model      typed records: Sample, Location, DataPoint, Aliquot, Measurement, AgeCalc, AgeSummary
  mapping    analyses + PublishOptions -> Submission (one per sample); pure
  vocab      static tables: method, decay constant, air ratio, uncertainty type
  lookup     name -> id over IHttpClient, with a session cache
  preflight  Submission -> unresolved terms; no write
  uploader   Submission -> EarthBank, in order, with a journal
  xlsx       Submission -> the vendor's template workbooks
  profiles   profiles.toml (no secrets); ISecretStore
  http       IHttpClient
        |                              |
libs/earthbank_qt (Qt6 Network)    secret stores: macOS Keychain, Windows Credential Manager, libsecret
  QtHttpClient
        |
apps/elctl: `elctl earthbank ...`      apps/pychron-ui: Preferences page, Publish dialog
```

`Submission` is the hinge. It is built once from the selected analyses and is
what pre-flight checks, what the workbook is filled from and what is
uploaded. The three modes cannot disagree about a value.

| Unit | Does | Depends on |
|---|---|---|
| `model`, `vocab` | types and constant tables | standard library, nlohmann-json |
| `mapping` | builds a `Submission` | `processing`, `reduction` |
| `lookup`, `preflight`, `uploader` | talk to the service | `IHttpClient`, `model` |
| `xlsx` | fills templates | miniz, pugixml, `model` |
| `profiles` | reads and writes `profiles.toml`, asks `ISecretStore` | toml++ |
| `QtHttpClient` | HTTPS | Qt6 Network; built only when it is found |
| secret stores | keep one password per profile | the platform's API |

New third-party code: **miniz** (zip, MIT) and **pugixml** (XML, MIT), both
small and header-or-single-source, fetched like the other dependencies.

## 4. From analyses to a submission

### 4.1 Inputs

```cpp
namespace pychron::earthbank {

enum class PreferredAge { Plateau, WeightedMean, Isochron, Integrated };

struct SampleOptions {              // per sample; all optional
  std::string interpretation;       // EarthBank LArArInterpretation name; empty: "Unknown"
  std::optional<PreferredAge> preferred_age;   // empty: section 4.4's rule
  std::string flux_monitor;         // overrides the monitor found on the analyses
  std::string datapoint_name;       // empty: "<sample> <material>"
};

struct PublishOptions {
  processing::ReductionSettings reduction;      // as the report
  processing::ReportOptions ages;               // plateau criteria, error kind, York method
  std::string decay_constant;       // EarthBank name; empty: from the constants preset (4.3)
  std::string air_ratio;            // likewise
  std::map<std::string, SampleOptions> samples; // by sample name
};

Result<std::vector<Submission>> build_submissions(const std::vector<processing::AnalysisPtr>& analyses,
                                                  const PublishOptions& options);
}
```

Analyses are grouped by `Analysis::sample`; one `Submission` per sample, as
**[legacy]** ("mixed-sample selections produce one ArArDataPoint per sample").
Analyses tagged invalid are left out unless the caller included them, as in
`elctl export`. A sample with no analysis that reduces to an age is refused
with the reason.

### 4.2 Records and where each value comes from

`Submission { Sample sample; Location location; DataPoint data_point;
std::vector<Aliquot> aliquots; std::vector<Measurement> measurements;
AgeCalc age_calc; AgeSummary age_summary; std::vector<std::string> notes; }`.

Each record is a struct of optional fields named as the service names them
(`ar40Corr`, `apparentAgeUncertainty`, ...); the full lists are legacy's
`DATA_POINT_FIELDS`, `MEASUREMENT_FIELDS`, `ALIQUOT_FIELDS`,
`AGE_SUMMARY_FIELDS`, `AGE_CALC_FIELDS`, `SAMPLE_FIELDS`, `LOCATION_FIELDS`
**[legacy]** and are copied into `model.hpp` verbatim. A controlled term is
kept as its name (`mineralName`); its id is resolved later (section 5).

| Record | Field(s) | From |
|---|---|---|
| Sample | `name`, `sampleID` | `Analysis::sample` |
| | `materialName` | `Analysis::material` |
| | `referenceElevation`, `igsn` | `SampleInfo` |
| | `stratographicUnitName` | `SampleInfo::unit` |
| | `description` | `"Uploaded from pychron-cpp <version>"` unless the sample has a note |
| Location | `lat`, `lon` | `SampleInfo`; both or neither |
| DataPoint | `analysisDate` | earliest analysis, UTC `YYYY-MM-DD` |
| | `analysisUnits` | `"fA"` |
| | `arMethodName` | section 4.3 |
| | `mineralName` | `Analysis::material` |
| | `lithologyName` | `SampleInfo::lithology` |
| | uncertainty type and unit names | the error kind of the ages (`ReportOptions::mean_error`), `"abs."` |
| Aliquot | `aliquotName` | `<identifier>-<aliquot, two digits>` |
| Measurement | `ar{36..40}Corr` and uncertainty | `Stage::InterferenceCorrected` |
| | `ar{36..40}ExcInt` and uncertainty | `Stage::DecayCorrected` |
| | `ar{36..40}blank` and uncertainty | `Stage::Blank` |
| | `ratio{36,37,38,40}Arx39ArCorr`, `ratio40Arx39ArK`, `caK`, `kCa`, `clK`, `radiogenicArgon`, `ar39KReleased`, `cumulative39ArK` | `reduction::ArArResult` |
| | `jValue`, `apparentAge` and uncertainties | `ReducedAnalysis::j`, `ArArResult` |
| | `stepNumber` | `Analysis::increment + 1`; absent with no step |
| | `analysisTime` | `Analysis::timestamp`, `YYYY-MM-DDTHH:MM:SSZ` |
| | `laserPower` + units, or `furnaceTemperature` | `ExtractionInfo` value and units: units containing `C` are a temperature **[legacy]** |
| | `plateauStepName` = `"Yes"` | the step is in the plateau the age summary reports |
| AgeCalc | `decayConstantName`, `airRatioName`, `fluxMonitorName` | section 4.3 |
| AgeSummary | `age`, `ageUncertainty`, `mswd`, `agepvalue`, `nAnalysisGrouped`, `ageCalcTypeName` | section 4.4 |
| | `dataInterpretationToolName` | `"Pychron"` **[legacy]** |
| | `interpretationName` | `SampleOptions::interpretation`, else `"Unknown"` |
| | `preferredAge` | `true` |

Ages are in Ma whatever the lab's display unit. Uncertainties are 1 sigma,
absolute.

### 4.3 Vocabulary mapping

In `vocab.hpp`, as data, each with a test:

- **Method [legacy].** No analysis with a step: `Total fusion - laser`. With
  steps: the largest extraction value at most 50 gives `Step-heating - laser`,
  at least 300 gives `Step-heating - furnace`, otherwise
  `Step-heating - undefined`.
- **Decay constant and air ratio.** pychron-cpp's constants carry values, not
  citations (`reduction::ReductionConstants`; an analysis's may come from a
  preset or from its reference record), so the name is found from the value
  the reduction used: `atm4036` of 298.56 is `Lee et al. 2006`, 295.5 is
  `Nier 1950`; the total 40K decay constant (`lambda_b + lambda_e`) of
  5.543e-10 /a is `Steiger and Jager, 1977`, 5.463e-10 is `Min et al. 2000`,
  5.531e-10 is `Renne et al. 2011`; each matched to one part in 10^4. A
  value that matches none leaves the name empty, which pre-flight reports,
  and `PublishOptions` supplies it. Analyses of one sample that disagree are
  refused with the two values. Legacy's citation normaliser (`Min (2008)` to
  `Min et al. 2000`, and so on) is kept for the override strings
  **[legacy]**.
- **Uncertainty type [legacy].** `SE`, `SEM`, `SD`, `MSEM`, `1 sigma` are id 1;
  `2 sigma` is id 2. Resolved from this table and never from
  `/api/core/l-error-types`, whose ids violate the data point's foreign key.
- **Flux monitor.** `"<MonitorInfo::name> <age> Ma"`, resolved by the fuzzy
  rule of section 5.1; `SampleOptions::flux_monitor` overrides.

The table's numbers are checked against `constants_preset()` in the mapping
task (the `Default` preset's `atm4036` is 298.56, the legacy presets' 295.5);
the names are checked against the live vocabularies in stage 3.

### 4.4 The summary age

pychron-cpp stores no interpreted age; it computes the candidates on demand
(`arar_groups.hpp`). The submission reports one, the preferred age:

- `SampleOptions::preferred_age` when given;
- otherwise, for a step-heating sample, the plateau if one exists under
  `PublishOptions::ages.plateau`, else the integrated age; for a fusion
  sample, the weighted mean.

`ageCalcTypeName` is `Plateau`, `Weighted Mean`, `Inverse Isochron` or
`Total Gas` (the names are checked by pre-flight like any other term). The
choice and the reason are written to `Submission::notes` and shown to the
user before anything is sent.

### 4.5 What is left empty

| Field | Why |
|---|---|
| `grainDiameterMin/Max` | `Analysis` has no grain size |
| `aliquotSize`, `aliquotUnits` | no weight on the analysis |
| `beamSize` | not recorded |
| `kContent` | not computed |
| `fundingName`, `literatureName`, `Datapoint Props`, `Sample Props` | as legacy |
| `archiveName`, `sampleKindName`, `sampleMethodName`, collection dates, rock unit ages | not in the catalog |

Each is a field of the record already; filling one later is a change to the
mapping and its table row, nothing else.

## 5. Talking to the service

### 5.1 Lookup

`LookupResolver(IHttpClient&, base_url, token)` turns a term into its id. The
endpoint for each `*Name` field is legacy's `LOOKUP_ENDPOINTS` table
**[legacy]**, copied into `vocab.hpp`. Strategies, in order **[legacy]**:

1. the session's cache, case-insensitive;
2. `?name.equals=<name>`, then the same capitalised, then
   `?name.contains=<name>&size=200` filtered to a case-insensitive exact match
   (this is how the 50 000-row `materials` table is searched without loading
   it);
3. for flux monitors only: load the vocabulary once and take the first entry
   whose name or reference contains both the author and the year parsed from
   the query.

A term resolves to an id, to "unknown" or to "could not ask" (the endpoint
needs a login the caller does not have). The three are distinct results.

### 5.2 Pre-flight

`preflight(submission, resolver) -> std::vector<Miss>`, a `Miss` being
`{record, field, value, endpoint, reason}`. It resolves every controlled term
of every record and writes nothing. "Could not ask" is reported separately
from "unknown" so that a run without a login does not show false misses
**[legacy]**.

### 5.3 Upload

Per submission, in order **[legacy]**:

```
data package id (the run's, else the profile's); none: stop
find the sample by name within that package; reuse it, or create it with its location
create the data point: one POST of { dataPointDTO: {dataStructure "ARARDATAPOINT",
    dataPackageId, sampleId, name}, extendingDataPointDTO: {the Ar-Ar fields} }
create each aliquot, each measurement, the age calculation, the age summary
```

Three rules that differ from legacy, all because there is no sandbox:

- **Check first.** Upload runs pre-flight and stops before the first write if
  any term is unknown, listing them. `allow_unresolved` drops those fields and
  proceeds, which is what legacy always did.
- **Journal.** After each successful POST the id returned is appended to
  `<config>/earthbank/journal/<host>/<package id>/<sample>.json`
  (`sampleId`, `arArDataPointId`, the aliquot names and measurement run ids
  done, `ageCalc`, `ageSummary`, and a hash of the submission). A run that
  finds a journal for the same sample, package and hash posts only what is
  missing. A different hash (the data changed) is refused with the reason and
  a way to start afresh (`--new-datapoint`), since the service offers no
  update. Legacy re-posted every sub-record on a retry.
- **Say who and where.** Before the first write the caller is given the
  account's login, the package's name and the count of records, and must
  confirm (`--yes` on the command line, a prompt in the UI).

A failure before the data point exists stops the submission. A failure after
it is recorded and the remaining sub-records are still attempted; the result
lists what is missing and a re-run completes it **[legacy]**.

The sample search is always scoped to the package
(`dataPackageId.equals=<id>`): reusing a same-named sample from another
package links the new data point across packages **[legacy]**.

### 5.4 Session, packages, errors

- `POST /api/authenticate` with username and password gives a JWT; it is sent
  as a bearer token. One re-authentication on a 401, then the error.
- `GET /api/account` is the connection test and names the user.
- `GET /api/management/data-packages?institutionId.equals=<id>&sort=name,ASC`
  lists packages; `POST` the same path creates one. The body nests the fields
  under `dataPackageDTO` and mirrors them at the top level: a flat body is
  accepted and silently ignores the name **[legacy]**. The creator becomes
  the package's supervisor, so a new package is always writable.
- Every failure the user can act on has a plain sentence; the status and body
  go to the log. The sentences are legacy's table in `UPLOAD_GUIDE.md`
  **[legacy]**: no write access to the package; no package selected; login
  rejected; wrong address; a value the service does not recognise (naming the
  field from the constraint in the body); partly uploaded; unreachable.

### 5.5 `IHttpClient`

```cpp
struct HttpRequest  { std::string method, url; std::vector<std::pair<std::string, std::string>> headers; std::string body;
                      std::chrono::milliseconds timeout{30000}; };
struct HttpResponse { int status = 0; std::string body; };
class IHttpClient { public: virtual ~IHttpClient() = default;
                    virtual Result<HttpResponse> send(const HttpRequest& request) = 0; };
```

Blocking. `QtHttpClient` runs a `QNetworkAccessManager` with a local event
loop on the calling thread, follows no redirects to another host, and uses
the system's certificate store. The UI calls the library from a worker
thread; that thread waits on the outside world and is not a clock
participant.

## 6. Workbooks

The two vendor templates, `ArArDataPoint_Template2026.xlsx` and
`Sample_Template_v2025-04-16.xlsx`, ship with the application (they are on
the fork; their licence to redistribute is confirmed with AuScope before the
first release, section 11). The exporter never builds a workbook from
nothing: it copies the template, and for each sheet reads row 4 (the
`apiField` row), appends data rows from row 5 **[legacy]**, and writes the
copy.

- A column's value is the record's field of the same name, or that name plus
  `Name`, with legacy's per-sheet exceptions (`SHEET_OVERRIDES`) **[legacy]**.
- Rows are linked by `datapointName`, not by id; the website resolves them on
  import **[legacy]**.
- Cells are written as inline strings or numbers, so the workbook's shared
  string table is not rewritten.
- The four header rows of every sheet, and every part of the file the
  exporter does not touch, are byte-for-byte the template's.
- One Ar-Ar workbook for all samples or one per sample; the Sample workbook
  optionally.
- A newer vendor template is a file swap, provided row 4 still names the
  fields. A field the template has and the records do not is left empty; a
  field the records have and the template lacks is listed in the result.

## 7. Profiles and passwords

`<config>/earthbank/profiles.toml`:

```toml
active = "umelb"

[profiles.umelb]
base_url = "https://ausgeochem.auscope.org.au"
username = "a.user"
institution_id = 193203        # University of Melbourne [legacy]
data_package_id = 12345
```

No password is ever written there. `ISecretStore { get, set, erase }` keeps
it under service `pychron.earthbank`, account `<profile>@<host>`: Keychain on
macOS, Credential Manager on Windows, libsecret on Linux when it is present
at build time. With no store the password lives for the session only:
`elctl` reads `EARTHBANK_PASSWORD` or asks without echo; the UI asks.
Legacy's encrypted-file fallback is not carried over: its key sat beside the
file.

`elctl earthbank import-profiles <pychron.ausgeochem.ini>` copies legacy's
profiles (the non-secret fields).

## 8. Surfaces

### 8.1 `elctl earthbank`

```
elctl earthbank login [--profile <name>]            store the password after a successful test
elctl earthbank account                             who am I
elctl earthbank packages [--create <name>]          list the institution's packages, or make one
elctl earthbank preflight   --db <url> [selection] [options]
elctl earthbank export-xlsx --db <url> [selection] --out <dir> [--per-sample] [--no-samples] [options]
elctl earthbank upload      --db <url> [selection] [--package <id>] [--allow-unresolved] [--new-datapoint] --yes [options]
elctl earthbank import-profiles <ini>
```

Selection is `elctl export`'s (`--sample`, `--identifier`, `--project`,
`--irradiation`, `--from`, `--to`, `--include-invalid`). Options are its
reduction and plateau flags plus `--overrides <file.toml>` (per-sample
interpretation, preferred age, flux monitor, data point name; and the decay
constant and air ratio names). Exit codes as `export`: 0 done, 1 nothing
matched or there were misses, 2 usage or a fatal error. Built without Qt
Network, the network commands say so and exit 2; `export-xlsx` works.

### 8.2 pychron-ui

- **Preferences, EarthBank page.** The profiles as a table; Add, Remove,
  Test (signs in, shows the account's name); Select / Create Package (lists
  the institution's packages, or creates one named by the user).
- **Data browser: Publish to EarthBank...** on the selected analyses. A
  dialog with: the action (upload, export xlsx, pre-flight); the profile and
  package, with Select / Create Package for this run only; one row per sample
  (analyses, interpretation, preferred age with the computed candidates
  shown, flux monitor); the constants' names. Run shows progress per record
  and ends with a result that can be copied: what was written, with ids;
  what was not, with the reason; what to do.
- The dialog remembers the last choices per sample name in the UI's settings,
  not on EarthBank.

## 9. Tests

Nothing in CI reaches EarthBank.

- **`FakeEarthBank`**, an `IHttpClient` in `tests/earthbank/`: authenticate
  and account; the lookup endpoints with `name.equals`, `name.contains`,
  `size`, `page`; packages (list, create, the flat-body trap); the sample
  search with its package scope; the five Ar-Ar POSTs. It reproduces the
  rejections legacy met: a data point without `dataPointDTO` is `500
  "Package is not writable by user."`; an uncertainty id from the wrong table
  is a constraint violation; a write to a package the user cannot write is
  refused. Its response shapes come from fixtures captured once from the live
  service with legacy's probe scripts (`tests/packages_probe.py`,
  `tests/arar_post_probe.py`), with names and ids of real people and
  packages replaced, committed as JSON.
- **Mapping.** A fixture of two samples (one step-heating with a plateau, one
  fusion) gives records compared field by field with what legacy builds for
  the same analyses, captured once; every difference is either fixed or
  listed in this spec's section 4.5.
- **Vocabulary.** Each table: every row, and the nearest miss.
- **Lookup.** Each strategy; the order; "could not ask" against "unknown".
- **Upload.** The order of calls; the wrapper's shape; the package scope of
  the sample search; stop on misses; `allow_unresolved`; a failure injected
  at each call in turn, then a re-run that posts only the remainder and
  nothing twice; a changed submission refused.
- **Workbooks.** The output is read back: header rows identical to the
  template's; each record's values in the columns row 4 names; the file
  opens (zip central directory and XML well-formed); the untouched parts
  byte-identical.
- **Profiles and secrets.** Round trip of `profiles.toml`; no password in it;
  each store against the real platform API in a test that is skipped where
  the platform is not the store's, using a throwaway service name.
- **`elctl`.** Each command against the fake, including without Qt Network.
- **Live**, by hand, never in CI: with `PYCHRON_TEST_EARTHBANK_URL`,
  `_USER`, `_PASSWORD`, `_INSTITUTION` set, a test creates a package named
  `pychron-test-<UTC date and time>`, uploads the fixture, reads the records
  back, and deletes the package. A runbook in `docs/earthbank.md` says when
  to run it (before a release that touches this library, and after the
  vendor announces an API change).

## 10. Order of work

1. `model`, `vocab`, `mapping`, with the golden fixture.
2. `xlsx` and `elctl earthbank export-xlsx`. The first point at which a lab
   can publish (through the website).
3. `IHttpClient`, `FakeEarthBank`, `lookup`, `preflight`,
   `elctl earthbank preflight`.
4. `profiles`, the secret stores, `QtHttpClient`,
   `elctl earthbank login | account | packages | import-profiles`.
5. `uploader` with the journal, `elctl earthbank upload`. Parity with legacy
   on the command line.
6. The UI: Preferences page, Publish dialog.
7. `docs/earthbank.md` (user guide, from legacy's `UPLOAD_GUIDE.md` and what
   was built), the AGENTS.md bullet, the live-test runbook, one live run.

## 11. Open points to settle before release, none blocking the work

- Redistribution of the two template workbooks with the installers: ask
  AuScope / Lithodat (`support@lithodat.com`). If refused, the exporter asks
  the user for the template files once and keeps them in the config
  directory; the code path is the same.
- The API licence (linked from legacy's README) and whether an application
  identifier or user agent is expected.
- The names EarthBank uses for the four age types and for the decay constants
  and air ratios: confirmed by pre-flight against the live vocabularies during
  stage 3, and corrected in `vocab.hpp` if they differ.
