# EarthBank Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish 40Ar/39Ar results from pychron-cpp to EarthBank (AusGeochem): upload over its REST API, export of the vendor's template workbooks, and a pre-flight check of controlled terms, from `elctl` and from the data browser.

**Architecture:** A Qt-free `libs/earthbank` turns selected analyses into one typed `Submission` per sample; pre-flight, the workbook exporter and the uploader all consume that value. The network is behind `IHttpClient` (a Qt Network adapter in `libs/earthbank_qt`), passwords behind `ISecretStore` (native OS stores). Every automated test runs against an in-process `FakeEarthBank`; nothing in CI reaches the service.

**Tech Stack:** C++20, nlohmann-json, toml++, miniz and pugixml (new), Qt6 Network (adapter only), Security.framework / wincred / libsecret, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-07-earthbank-design.md`. Read it first; section numbers below refer to it.

**Reference being migrated:** `UMelbournePychron/pychron`, branch `dev/dc2025`, directory `pychron/ausgeochem/`. Fetch a file to a scratch directory (never into the repository) with:

```bash
gh api "repos/UMelbournePychron/pychron/contents/pychron/ausgeochem/<file>?ref=dev/dc2025" -H "Accept: application/vnd.github.raw" > <scratch>/<file>
```

`README.md`, `UPLOAD_GUIDE.md`, `earthbank_service.py`, `xlsx_exporter.py`, `data_package_dialog.py`, `tasks/node.py`, `tasks/preferences.py`, `tests/*.py`, and the two workbooks under `templates/` (fetch those as binary: `-H "Accept: application/vnd.github.raw"` and redirect to a `.xlsx` file).

## Global Constraints

- Branch `feat/earthbank`, cut from `develop`.
- Conventional Commits with the component as scope (`feat(earthbank): ...`, `feat(elctl): ...`, `feat(ui): ...`). End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build and test: `cmake --build build/dev -j` then `ctest --test-dir build/dev --output-on-failure -R <regex>`; UI tree `build/dev-ui` with `QT_QPA_PLATFORM=offscreen`. CI does not run on work branches: run the task's tests before each commit, and both full suites before the last commit of Tasks 5, 11 and 14.
- Never skip or disable a failing test. A failure on one compiler only is a real bug.
- **No automated test, example, script or default configuration contacts EarthBank.** The only code that does is behind explicit user action or the opt-in live test of Task 14. The production host `https://ausgeochem.auscope.org.au` appears as a default in exactly one place (`profiles.hpp`).
- **No password, token or real person's name, login or package id is committed**, in code, fixtures, logs or test output. A token never appears in a log line or an error message; a request is logged as method, path and status.
- `libs/earthbank` is Qt-free and has no network code: it depends on `pychron::processing`, `pychron::reduction`, `pychron::core`, nlohmann-json, toml++, miniz, pugixml. Qt appears only in `libs/earthbank_qt` (privately) and the UI.
- Field names of EarthBank records are the service's, verbatim, including its misspellings (`apparentAgeUncertainityTypeName`, `stratographicUnitName`).
- Ages are in Ma; uncertainties 1 sigma, absolute; intensities in fA.
- A file written for the user goes through `pychron::mark_as_user_file`.
- Each header a file uses is included by that file. Warnings are errors.
- The UI's network work runs on a worker thread that is not a clock participant (AGENTS.md "Time": a wait on the outside world).
- An unknown field in `profiles.toml` or an overrides file is a `Config` error naming the file and key.

## Review Focus

1. A request that times out after it was sent: the server may or may not have written. The journal must not guess; the user is told exactly which record is in doubt and how to continue. (Task 10 tests.)
2. Names with spaces, non-ASCII letters, `&`, `<`, quotes, `%`, `+`: in a URL query, in JSON, in workbook XML, in a journal file name. (Tasks 4, 7, 10 tests.)
3. A sample whose analyses disagree: different constants, a mix of step-heating and fusion, two materials, half a location. Refused with the two values named, or handled by a stated rule; never silently the first analysis's. (Task 2 tests.)
4. A headless machine with no secret store, and a wrong or expired password mid-run: a clear message, exit code 2, nothing written, no prompt that blocks a script. (Tasks 8, 9, 11 tests.)
5. A selection of several thousand analyses across many samples: progress is reported, a cancel stops between records and leaves a journal a re-run can finish, and lookups are cached so a term is asked for once. (Tasks 7, 10, 13 tests.)

## File Structure

| File | Responsibility |
|---|---|
| `libs/earthbank/include/pychron/earthbank/model.hpp`, `src/model.cpp` | record structs, `Submission`, `to_json` |
| `.../vocab.hpp`, `src/vocab.cpp` | static tables and the pure functions over them |
| `.../mapping.hpp`, `src/mapping.cpp` | `build_submissions` |
| `.../http.hpp` | `IHttpClient`, `HttpRequest`, `HttpResponse`, URL encoding |
| `.../session.hpp`, `src/session.cpp` | authenticate, account, packages, error sentences |
| `.../lookup.hpp`, `src/lookup.cpp` | `LookupResolver` |
| `.../preflight.hpp`, `src/preflight.cpp` | `preflight` |
| `.../uploader.hpp`, `src/uploader.cpp`, `.../journal.hpp`, `src/journal.cpp` | upload order, journal |
| `.../xlsx.hpp`, `src/xlsx.cpp`, `src/ooxml.hpp`, `src/ooxml.cpp` | template fill; zip and sheet XML helpers |
| `.../profiles.hpp`, `src/profiles.cpp`, `.../secret_store.hpp`, `src/secret_store_{mac.mm,win.cpp,linux.cpp,memory.cpp}` | profiles, passwords |
| `libs/earthbank/templates/*.xlsx` | the two vendor workbooks |
| `libs/earthbank_qt/include/pychron/earthbank/qt_http_client.hpp`, `src/qt_http_client.cpp` | HTTPS |
| `apps/elctl/src/earthbank.cpp`, `earthbank.hpp`, `earthbank_stub.cpp`, `selection.hpp`, `selection.cpp` | the command; the selection flags shared with `export` |
| `apps/pychron-ui/src/earthbank_preferences_page.*`, `earthbank_publish_dialog.*`, `earthbank_worker.*` | UI |
| `tests/earthbank/fake_earthbank.hpp/.cpp`, `fixtures/*.json`, `test_*.cpp` | tests |
| `docs/earthbank.md` | user guide and live-test runbook |

---

### Task 1: Records and vocabulary

**Files:**
- Create: `libs/earthbank/CMakeLists.txt`, `libs/earthbank/include/pychron/earthbank/model.hpp`, `vocab.hpp`, `libs/earthbank/src/model.cpp`, `vocab.cpp`, `tests/earthbank/CMakeLists.txt`, `tests/earthbank/test_model.cpp`, `test_vocab.cpp`
- Modify: the top-level `CMakeLists.txt` lists of libraries and test suites (add `earthbank` after `processing`)

**Interfaces:**
- Produces, namespace `pychron::earthbank`:
  ```cpp
  // One optional per service field, named as the service names it.
  struct Sample; struct Location; struct DataPoint; struct Aliquot; struct Measurement; struct AgeCalc; struct AgeSummary;
  struct Submission { Sample sample; std::optional<Location> location; DataPoint data_point;
                      std::vector<Aliquot> aliquots; std::vector<Measurement> measurements;
                      AgeCalc age_calc; AgeSummary age_summary; std::vector<std::string> notes; };
  nlohmann::json to_json(const Sample&);   // one overload per record: only set fields, service names as keys
  std::string content_hash(const Submission&);   // SHA-256 (libs/core sha256) of a canonical JSON of the records, notes excluded

  // vocab.hpp
  struct LookupSpec { std::string_view name_field, id_field, endpoint; };   // endpoint empty: no vocabulary
  std::span<const LookupSpec> lookup_specs();                               // legacy LOOKUP_ENDPOINTS
  std::optional<int> uncertainty_type_id(std::string_view name);           // legacy UNCERTAINTY_TYPE_VOCAB
  std::string ar_method(bool any_step, std::optional<double> max_extract_value);   // spec 4.3
  std::string decay_constant_name(double lambda_total_per_year);           // "" when none matches to 1e-4 relative
  std::string air_ratio_name(double atm4036);
  std::string normalize_citation(std::string_view override_text, bool decay);   // legacy _normalize_*
  std::string flux_monitor_query(std::string_view name, std::optional<double> age_ma);
  ```
- The record structs' fields are exactly legacy's `SAMPLE_FIELDS`, `LOCATION_FIELDS`, `DATA_POINT_FIELDS`, `ALIQUOT_FIELDS`, `MEASUREMENT_FIELDS`, `AGE_CALC_FIELDS`, `AGE_SUMMARY_FIELDS` (`earthbank_service.py` lines 143-355): `std::optional<double>` for numbers, `std::optional<std::string>` for text and for every `*Name`, `std::optional<std::int64_t>` for every `*Id` and `id`, `std::optional<bool>` for `preferredAge`. Generate them from one X-macro list per record so the struct, `to_json` and the workbook's field access cannot drift.
- Constants: `Steiger and Jager, 1977` 5.543e-10, `Min et al. 2000` 5.463e-10, `Renne et al. 2011` 5.531e-10 (per year, total); `Lee et al. 2006` 298.56, `Nier 1950` 295.5. Before pinning, compute `lambda_b + lambda_e` and `atm4036` of each `reduction::constants_preset()` in a test and record in the commit body which name each preset maps to; if a preset maps to none, say so there (do not add a name the service may not have).

- [ ] **Step 1: Write the failing tests:**
  - `Model.ToJsonHasOnlySetFieldsUnderServiceNames`: a `Measurement` with `ar40Corr = 1.5`, `apparentAgeUncertainityTypeName = "SE"`: JSON is exactly `{"apparentAgeUncertainityTypeName":"SE","ar40Corr":1.5}`.
  - `Model.EveryLegacyFieldIsPresent`: for each record, the set of keys `to_json` can emit (all fields set) equals a list written out in the test from the legacy tuples.
  - `Model.ContentHashIgnoresNotesAndFieldOrder`, `Model.ContentHashChangesWithAValue`.
  - `Vocab.ArMethod`: no step gives `Total fusion - laser`; steps with max 50 `Step-heating - laser`; 300 `Step-heating - furnace`; 51 and 299 and no value `Step-heating - undefined`.
  - `Vocab.UncertaintyType`: `SE`, `SEM`, `SD`, `MSEM`, `1 sigma`, `1s`, `1` give 1; `2 sigma`, `2s`, `2` give 2; `3 sigma` and `""` give nothing; case and punctuation ignored.
  - `Vocab.DecayConstantAndAirRatioByValue`: each value above and a value 2e-4 relative away (no match).
  - `Vocab.PresetsMapToNames`: each `ConstantsPreset` through `constants_preset()` gives the names the commit body records.
  - `Vocab.NormalizeCitation`: `Min (2008)`, `Min et al. 2008`, `Steiger and Jager (1977)`, `Renne et al. (2011)`, `Nier (1950)`, `Lee (2006)` map as legacy's tables; an unknown string passes through.
  - `Vocab.LookupSpecsCoverEveryNameField`: every `*Name` field of every record has a spec or is in the static uncertainty table.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Model\.|Vocab\.'`. Expected: build fails, no such library.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): the service's records and vocabulary tables`.

---

### Task 2: From analyses to submissions

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/mapping.hpp`, `libs/earthbank/src/mapping.cpp`, `tests/earthbank/test_mapping.cpp`, `tests/earthbank/analyses.hpp` (fixture builders)

**Interfaces:**
- Consumes: Task 1; `processing::reduce_analysis`, `processing/arar_groups.hpp` (`integrated_age`, `isochron_points`, `isochron_age`), the plateau and weighted-mean code the report uses (`libs/processing/src/report.cpp`: reuse its functions; where one is file-local, move it to a `processing` header rather than copying it).
- Produces: `PreferredAge`, `SampleOptions`, `PublishOptions`, `build_submissions` exactly as spec 4.1.

**Rules beyond the spec's tables (decided here):**
- Order: samples by name; within a sample, analyses by timestamp; aliquots in first-appearance order.
- Disagreement within a sample (Review Focus 3): two materials, two lithologies, two decay-constant or air-ratio values, or a location on some analyses that differs: `Config` error naming the sample, the field and the two values. A sample with both step-heating aliquots and fusion analyses: `arMethodName` from the whole set (it has steps), the preferred-age rule as step-heating, and a note. Latitude without longitude or the reverse: no `Location`, and a note.
- An analysis with no age (`ReducedAnalysis::arar` absent or without ages): left out of the measurements, counted in a note. A sample with none left: error.
- `datapoint_name` default: `"<sample> <material>"`, or `"<sample>"` with no material.
- `cumulative39ArK`: percent, per aliquot, in step order; absent for fusion analyses.
- Preferred age as spec 4.4. `ageCalcTypeName`: `Plateau`, `Weighted Mean`, `Inverse Isochron`, `Total Gas`. `nAnalysisGrouped`: the analyses the reported age used. `mswd` and `agepvalue` from that computation; absent for `Total Gas`.

- [ ] **Step 1: Write the failing tests** (fixtures built in `analyses.hpp` from `tests/processing/fixtures.hpp`'s helpers: a 10-step sample `S-STEP` with a plateau over steps C-H, and a 6-analysis fusion sample `S-FUS`, with known isotope intercepts, J, constants):
  - `Mapping.OneSubmissionPerSampleInNameOrder`.
  - `Mapping.SampleAndLocation`: fields per spec 4.2; lat without lon gives no location and a note.
  - `Mapping.DataPoint`: `analysisDate` is the earliest UTC date; `analysisUnits` `fA`; `arMethodName` for each fixture; `mineralName`, `lithologyName`; uncertainty names from the options' error kind.
  - `Mapping.MeasurementValues`: for one analysis of each fixture, every numeric field equals the value computed in the test from `reduce_analysis` on the same analysis (stage and `ArArResult` member named in the assertion), to 1e-12 relative; `stepNumber`; `analysisTime` format; laser power against furnace temperature by units.
  - `Mapping.PlateauStepsAreFlagged`: `plateauStepName == "Yes"` on exactly steps C-H when the plateau is the preferred age; on none when the preferred age is the integrated age.
  - `Mapping.PreferredAgeRule`: step sample with a plateau gives `Plateau`; the same with criteria no plateau meets gives `Total Gas` and a note; fusion gives `Weighted Mean`; an explicit option wins; each age equals the function the report uses, to 1e-12.
  - `Mapping.AgeCalcNamesComeFromTheConstantsUsed`, `Mapping.OverridesAreNormalized`.
  - `Mapping.DisagreementIsRefusedNamingBothValues`: four cases above. (Review Focus 3)
  - `Mapping.AnalysesWithoutAgesAreLeftOutAndCounted`, `Mapping.ASampleWithNoAgeIsRefused`.
  - `Mapping.LeftEmptyFieldsAreAbsent`: the fields of spec 4.5 are not in any record's JSON.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R Mapping`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same, then `-R 'Report|Processing'` (functions moved out of `report.cpp` must leave the report unchanged). Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): analyses become one submission per sample`.

---

### Task 3: Workbook parts: zip and sheet XML

**Files:**
- Modify: `cmake/PychronDependencies.cmake`, `vcpkg.json` (miniz, pugixml: `find_package` first, `FetchContent` fallback, as the existing entries)
- Create: `libs/earthbank/src/ooxml.hpp`, `ooxml.cpp`, `tests/earthbank/test_ooxml.cpp`, `libs/earthbank/templates/ArArDataPoint_Template2026.xlsx`, `Sample_Template_v2025-04-16.xlsx` (fetched from the fork as binary; record their SHA-256 in the commit body)

**Interfaces:**
- Produces (internal to the library):
  ```cpp
  struct Cell { std::variant<std::monostate, double, std::string> value; };
  class Workbook {
   public:
    static Result<Workbook> open(const std::filesystem::path& template_file);
    std::vector<std::string> sheet_names() const;                       // workbook order
    Result<std::vector<std::string>> row_values(std::string_view sheet, int row) const;   // strings resolved through sharedStrings
    Result<void> append_rows(std::string_view sheet, int first_row, const std::vector<std::vector<Cell>>& rows);
    Result<void> save(const std::filesystem::path& out) const;          // every untouched part copied byte for byte
  };
  ```
- `append_rows` writes `<row r="N">` elements with `<c r="A5" t="inlineStr"><is><t xml:space="preserve">text</t></is></c>` or `<c r="A5"><v>1.5</v></c>`; numbers as shortest round-trip decimal; an empty cell is omitted. It refuses if the sheet already has a row at or after `first_row` with content. It updates the sheet's `<dimension>` if present. Text is XML-escaped; characters XML 1.0 forbids are refused with the cell named.

- [ ] **Step 1: Write the failing tests:**
  - `Ooxml.OpensBothTemplatesAndNamesTheirSheets`: the Ar-Ar template has the five sheets legacy names (`ArArDataPoints`, `ArArMeasurement`, `ArArAliquot`, `ArArAgeSummary`, `ArArAgeCalc`; confirm against the file and pin what it has) plus `Datapoint Props`; the Sample template has `Samples` and `Sample Props`.
  - `Ooxml.Row4IsTheApiFieldRow`: for `ArArMeasurement`, row 4 contains `ar40Corr`, `apparentAge`, `stepNumber`.
  - `Ooxml.SaveWithNoChangeKeepsEveryPartByteIdentical`: each zip entry's bytes equal the template's.
  - `Ooxml.AppendedRowsReadBack`: numbers and strings, including `Café & <Co> "x"` and a leading space; read back with `row_values`. (Review Focus 2)
  - `Ooxml.OnlyTheTouchedSheetChanges`: after appending to one sheet, every other part is byte-identical and rows 1-4 of the touched sheet are unchanged.
  - `Ooxml.RefusesARowThatIsNotEmpty`, `Ooxml.RefusesAControlCharacter`, `Ooxml.RefusesAnUnknownSheet`.
  - `Ooxml.TheResultIsAWellFormedZip`: reopen with `Workbook::open`; central directory entry count equals the template's.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R Ooxml`. Expected: build fails.
- [ ] **Step 3: Implement.** miniz for the archive; pugixml for `xl/workbook.xml`, `xl/_rels/workbook.xml.rels`, `xl/sharedStrings.xml` and the sheet being appended to.
- [ ] **Step 4: Run** the same; then by hand open one output in a spreadsheet program and confirm it opens without a repair prompt (record in the commit body which program).
- [ ] **Step 5: Commit** `feat(earthbank): rows appended to a workbook template`.

---

### Task 4: Workbook export

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/xlsx.hpp`, `libs/earthbank/src/xlsx.cpp`, `tests/earthbank/test_xlsx.cpp`
- Modify: `libs/earthbank/CMakeLists.txt` (install the templates; a compile definition for their build-tree path in tests), `cmake/PychronInstall.cmake` (templates under the application's data directory)

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:
  ```cpp
  struct XlsxOptions { std::filesystem::path template_dir;   // holds the two templates
                       bool per_sample = false; bool samples = true; };
  struct XlsxResult { std::vector<std::filesystem::path> files; std::vector<std::string> notes; };   // notes: fields a template lacks
  Result<XlsxResult> export_xlsx(const std::vector<Submission>& submissions, const std::filesystem::path& out_dir,
                                 const XlsxOptions& options);
  std::filesystem::path default_template_dir();   // beside the executable's data directory, else the build tree's
  ```
- A column takes the record's field named by the column's `apiField`, else that name plus `Name`, with legacy's `SHEET_OVERRIDES` (`xlsx_exporter.py` lines 30-70) as a table. Rows are linked by `datapointName` (the data point's name) in every Ar-Ar sheet that has that column. File names: `ArArDataPoint_<UTC date>.xlsx`, `Samples_<UTC date>.xlsx`, or `ArArDataPoint_<sample>.xlsx` per sample with the sample name made safe for a file name (letters, digits, `-`, `_`; anything else `_`; a collision gets `-2`). Each file goes through `mark_as_user_file`.

- [ ] **Step 1: Write the failing tests** (submissions from Task 2's fixtures):
  - `Xlsx.EveryRecordLandsInItsSheetUnderItsColumn`: read back each sheet; for each record and each set field, the cell under the column whose row-4 name maps to that field holds the value.
  - `Xlsx.HeaderRowsAreTheTemplates`: rows 1-4 of every sheet equal the template's.
  - `Xlsx.RowsAreLinkedByDatapointName`.
  - `Xlsx.OneWorkbookOrOnePerSample`, `Xlsx.SampleWorkbookIsOptional`.
  - `Xlsx.AFieldTheTemplateLacksIsNoted`: with a template whose row 4 has one column removed (made in the test with `Workbook`).
  - `Xlsx.SheetOverrides`: `arArMethod` takes `arMethodName`; `sampleName` takes `name`; `Tag` stays empty.
  - `Xlsx.UnsafeSampleNamesMakeSafeDistinctFileNames`: `A/B`, `A B`, `A:B` give three files. (Review Focus 2)
  - `Xlsx.AMissingTemplateIsAnErrorNamingTheFile`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R Xlsx`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Xlsx|Ooxml|Mapping'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): submissions written into the vendor's workbooks`.

---

### Task 5: `elctl earthbank export-xlsx`

**Files:**
- Create: `apps/elctl/src/selection.hpp`, `selection.cpp`, `apps/elctl/src/earthbank.hpp`, `earthbank.cpp`, `earthbank_stub.cpp`, `apps/elctl/tests/test_earthbank_commands.cpp`
- Modify: `apps/elctl/src/export.cpp` (use `selection`), `apps/elctl/src/cli.cpp` (dispatch `earthbank`, usage text), `apps/elctl/CMakeLists.txt` (the stub split as for `export`; link `pychron::earthbank`), `docs/user/08-elctl-reference.md`

**Interfaces:**
- Produces:
  ```cpp
  // selection.hpp: the flags `export` already has, parsed once for both commands.
  struct Selection { /* samples, identifiers, projects, irradiations, types, uuids, from, to, include_invalid, limit */ };
  // Consumes its flags from `args` (removing them) and returns the rest untouched.
  Result<Selection> take_selection(std::vector<std::string>& args);
  Result<std::vector<processing::AnalysisPtr>> load_selection(const Selection&, const std::string& db_url, /* reduction settings */);
  int earthbank_command(const std::vector<std::string>& args, Io io);
  ```
- `elctl earthbank export-xlsx --db <url> [selection] --out <dir> [--per-sample] [--no-samples] [--templates <dir>] [--overrides <file.toml>] [reduction and plateau flags as export]`. Prints one line per sample (analyses, preferred age and why, notes), then the files written. Exit 0 wrote files; 1 nothing matched; 2 usage or error.
- Overrides file:
  ```toml
  decay_constant = "Min et al. 2000"
  air_ratio = "Lee et al. 2006"
  [samples."S-STEP"]
  interpretation = "Eruption age"
  preferred_age = "plateau"        # plateau | weighted_mean | isochron | integrated
  flux_monitor = "Fish Canyon Tuff sanidine Kuiper et al. 2008"
  datapoint_name = "S-STEP sanidine"
  ```
  A sample named there that the selection lacks is an error naming it.
- Without persistence: the stub says the command needs the store and exits 2 (as `export_stub.cpp`).

- [ ] **Step 1: Write the failing tests** (the store fixture `apps/elctl/tests` already uses for `export`):
  - `ElctlExport.StillWorksThroughTheSharedSelection`: the existing export tests pass unchanged.
  - `ElctlEarthbank.ExportXlsxWritesBothWorkbooks`: files exist, read back with `Workbook`, one measurement row per analysis.
  - `ElctlEarthbank.PerSampleAndNoSamples`.
  - `ElctlEarthbank.OverridesFileIsApplied`, `ElctlEarthbank.AnUnknownKeyOrSampleInOverridesIsRefused`.
  - `ElctlEarthbank.NothingMatchedExitsOne`, `ElctlEarthbank.UsageErrorsExitTwo` (no `--out`, unknown flag, unknown subcommand).
  - `ElctlEarthbank.ADisagreeingSampleIsReportedAndTheOthersAreNotWritten`: exit 2, no file left behind.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'ElctlEarthbank|ElctlExport'`. Expected: build fails.
- [ ] **Step 3: Implement.** Move the selection parsing out of `export.cpp` without changing its behaviour or messages.
- [ ] **Step 4: Run** the same, then both full suites. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): earthbank export-xlsx writes the AusGeochem workbooks`. This is the first usable point: a lab can publish through the website.

---

### Task 6: `IHttpClient` and the fake service

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/http.hpp`, `libs/earthbank/src/http.cpp`, `tests/earthbank/fake_earthbank.hpp`, `fake_earthbank.cpp`, `tests/earthbank/fixtures/*.json`, `tests/earthbank/test_fake_earthbank.cpp`, `tests/earthbank/fixtures/README.md`

**Interfaces:**
- Produces: `HttpRequest`, `HttpResponse`, `IHttpClient` as spec 5.5; `std::string url_encode(std::string_view)` (RFC 3986 unreserved kept, everything else percent-encoded, space as `%20`); `std::string with_query(std::string_view url, std::initializer_list<std::pair<std::string_view, std::string>> params)`.
- `FakeEarthBank : IHttpClient`, in memory:
  - `POST /api/authenticate` `{username, password}` gives `{"id_token": "<opaque>"}` for the configured user, 401 otherwise; `expire_tokens()` makes the next authenticated call 401 once.
  - `GET /api/account` (bearer) gives the account.
  - `GET` on each lookup endpoint of `lookup_specs()` with `name.equals`, `name.contains`, `page`, `size`; some endpoints need a bearer (`/api/core/l-*`), as legacy found.
  - `GET|POST|DELETE /api/management/data-packages`; a POST with a flat body creates a package whose name is ignored (the trap legacy documents); the creator can write to what they create and to packages marked writable.
  - `GET /api/core/sample-with-locations?name.equals=&dataPackageId.equals=`, `POST` the wrapper.
  - `POST /api/arar/ArArDataPoint` (wrapper required: a body without `dataPointDTO` is `500 {"title":"Package is not writable by user."}`), `ArArAliquot`, `ArArMeasurement`, `ArArAgeCalc`, `ArArAgeSummary`. An id for a `*Id` field that is not in that field's vocabulary is `400` with a constraint message naming the field. A write to a package the user cannot write is the 500 above.
  - Test controls: `fail_next(method, path_prefix, status, body)`, `drop_next(method, path_prefix)` (the request is applied on the server and the client gets a timeout error: Review Focus 1), `requests()` (the log), `store()` (what exists).
- Fixtures: `lookups/<endpoint>.json` (a few dozen entries each, including the verbose flux-monitor names legacy quotes), `account.json`, `packages.json`. They are written by hand from legacy's code, README and probe scripts, with invented people and ids, and `fixtures/README.md` says so and gives the commands to replace them with captures (owner step below).

**Owner step (a person with an EarthBank login; not required for the task to pass):** run legacy's `tests/packages_probe.py`, `tests/arar_post_probe.py` and `tests/integration_test.py --smoke` on the fork, save the JSON each prints, replace every real name, login and id, and drop the files over the hand-written fixtures. If a shape differs from what the fake does, that is a finding to fix in the fake before Task 9.

- [ ] **Step 1: Write the failing tests:** one per behaviour in the list above (`FakeEarthBank.Authenticate`, `.LookupEquals`, `.LookupContainsPaged`, `.LookupNeedsABearerOnCoreEndpoints`, `.PackageFlatBodyIgnoresTheName`, `.SampleSearchIsScopedToThePackage`, `.DataPointNeedsTheWrapper`, `.WrongVocabularyIdIsAConstraintError`, `.UnwritablePackageIsRefused`, `.DropNextAppliesAndTimesOut`), and `Http.UrlEncode` (space, `&`, `+`, `%`, `é`, `/`).
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'FakeEarthBank|Http\.'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same. Expected: PASS.
- [ ] **Step 5: Commit** `test(earthbank): an in-process EarthBank for tests`.

---

### Task 7: Session, lookup and pre-flight

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/session.hpp`, `lookup.hpp`, `preflight.hpp`, `libs/earthbank/src/session.cpp`, `lookup.cpp`, `preflight.cpp`, `tests/earthbank/test_session.cpp`, `test_lookup.cpp`, `test_preflight.cpp`

**Interfaces:**
- Consumes: Tasks 1, 6.
- Produces:
  ```cpp
  struct Account { std::int64_t id = 0; std::string login, name; };
  struct DataPackage { std::int64_t id = 0; std::string name, institution_name, workflow_state; std::int64_t institution_id = 0; };
  class Session {
   public:
    Session(IHttpClient& http, std::string base_url);
    Result<void> sign_in(std::string_view username, std::string_view password);   // keeps both for one re-authentication
    bool signed_in() const;
    Result<Account> account();
    Result<std::vector<DataPackage>> packages(std::optional<std::int64_t> institution_id);
    Result<DataPackage> create_package(std::string_view name, std::optional<std::int64_t> institution_id);
    // Adds the bearer when signed in; on a 401 signs in again once and repeats.
    Result<HttpResponse> send(HttpRequest request, bool needs_login);
    // The plain sentence for a failed call, spec 5.4; `record` is what was being written.
    static std::string sentence(int status, std::string_view body, std::string_view path, std::string_view record);
  };
  enum class LookupOutcome { Found, Unknown, CouldNotAsk };
  struct LookupResult { LookupOutcome outcome; std::int64_t id = 0; std::string matched_name; };
  class LookupResolver { public: explicit LookupResolver(Session&);
                         LookupResult resolve(std::string_view name_field, std::string_view value); };
  struct Miss { std::string sample, record, field, value, endpoint; LookupOutcome outcome; };
  std::vector<Miss> preflight(const std::vector<Submission>&, LookupResolver&);
  ```
- Lookup strategies and their order as spec 5.1. A resolver asks the service at most once per (endpoint, value) and loads a whole vocabulary at most once. Query values go through `with_query` (Review Focus 2). `create_package` posts the nested-and-mirrored body of spec 5.4.
- A token is never put in an error or a log line.

- [ ] **Step 1: Write the failing tests:**
  - `Session.SignInThenAccount`, `Session.WrongPasswordIsTheLoginSentence`, `Session.ReauthenticatesOnceOnA401` (with `expire_tokens()`), `Session.ASecond401IsAnError`, `Session.PackagesByInstitution`, `Session.CreatePackageKeepsItsName` (fails against the fake if the body is flat), `Session.NoTokenInAnyErrorOrLog`.
  - `Session.Sentences`: each row of legacy's table in `UPLOAD_GUIDE.md` from the status and body the fake produces; a constraint message names the field.
  - `Lookup.CachedExactIsCaseInsensitive`, `Lookup.EqualsThenCapitalizedThenContains`, `Lookup.ContainsNeedsAnExactMatchAmongTheResults`, `Lookup.FluxMonitorFuzzyByAuthorAndYear` (the verbose entry legacy quotes), `Lookup.UncertaintyTypeComesFromTheStaticTable` (no request made), `Lookup.CouldNotAskWithoutALogin`, `Lookup.AsksOncePerValue` (count the fake's requests), `Lookup.ValuesAreEncoded` (`Café & Co`).
  - `Preflight.ReportsEveryUnknownTermWithItsRecordAndEndpoint`, `Preflight.CouldNotAskIsSeparateFromUnknown`, `Preflight.WritesNothing` (the fake's store is unchanged and only GETs were made), `Preflight.ManySamplesAskForEachTermOnce`. (Review Focus 5)
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Session\.|Lookup\.|Preflight\.'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): sign-in, vocabulary lookup and a pre-flight check`.

---

### Task 8: Profiles and passwords

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/profiles.hpp`, `secret_store.hpp`, `libs/earthbank/src/profiles.cpp`, `secret_store_memory.cpp`, `secret_store_mac.mm`, `secret_store_win.cpp`, `secret_store_linux.cpp`, `tests/earthbank/test_profiles.cpp`, `test_secret_store.cpp`
- Modify: `libs/earthbank/CMakeLists.txt` (per-platform source; `-framework Security` on macOS; `pkg_check_modules(libsecret-1)` optional on Linux; `PYCHRON_EARTHBANK_SECRET_STORE` definition naming the backend), `packaging/macos/entitlements.plist` only if the hardened runtime needs one for Keychain access (it does not for the login keychain; confirm and say so in the commit body)

**Interfaces:**
- Produces:
  ```cpp
  inline constexpr std::string_view kDefaultBaseUrl = "https://ausgeochem.auscope.org.au";
  struct Profile { std::string name, base_url{kDefaultBaseUrl}, username;
                   std::optional<std::int64_t> institution_id, data_package_id; };
  struct Profiles { std::string active; std::vector<Profile> profiles;
                    const Profile* find(std::string_view name) const; const Profile* active_profile() const; };
  Result<Profiles> load_profiles(const std::filesystem::path& file);     // a missing file is an empty Profiles
  Result<void> save_profiles(const Profiles&, const std::filesystem::path& file);   // atomic: write beside, rename
  std::filesystem::path default_profiles_file();                          // <config>/earthbank/profiles.toml
  Result<Profiles> import_legacy_ini(const std::filesystem::path& ini);  // legacy pychron.ausgeochem.ini, non-secret fields

  class ISecretStore { public: virtual ~ISecretStore() = default;
    virtual std::string backend() const = 0;                              // "keychain", "credential-manager", "libsecret", "memory"
    virtual bool persistent() const = 0;
    virtual Result<std::optional<std::string>> get(std::string_view account) = 0;
    virtual Result<void> set(std::string_view account, std::string_view password) = 0;
    virtual Result<void> erase(std::string_view account) = 0; };
  std::unique_ptr<ISecretStore> make_secret_store(std::string_view service = "pychron.earthbank");
  std::string secret_account(const Profile&);                             // "<profile>@<host of base_url>"
  ```
- `default_profiles_file()`: `$XDG_CONFIG_HOME` or `~/.config` on Linux, `~/Library/Application Support` on macOS, `%APPDATA%` on Windows, then `pychron/earthbank/profiles.toml`; the UI passes its own `AppConfigLocation` instead so both agree (check what `QStandardPaths::AppConfigLocation` resolves to for this application on each platform and make the two the same directory; state the paths in the commit body).
- `base_url` must be `https://` (or `http://localhost` / `127.0.0.1` for tests); anything else is a `Config` error. A profile name is letters, digits, `-`, `_`.
- `make_secret_store` returns the platform store when it was built in and is usable at run time, else the memory store. The memory store is per process.

- [ ] **Step 1: Write the failing tests:**
  - `Profiles.RoundTrip`, `Profiles.AMissingFileIsEmpty`, `Profiles.NoPasswordKeyIsEverWrittenOrAccepted` (a `password` key in the file is a `Config` error naming it), `Profiles.UnknownKeyIsRefused`, `Profiles.BaseUrlMustBeHttps`, `Profiles.ActiveMustNameAProfile`, `Profiles.SaveIsAtomic` (an existing file is intact if the write fails: make the directory read-only for the temporary file).
  - `Profiles.ImportLegacyIni`: a sample ini written in the test from legacy's `tasks/preferences.py` format.
  - `SecretStore.MemoryRoundTrip`.
  - `SecretStore.PlatformRoundTrip`: with service `pychron.earthbank.test.<random>`: set, get, overwrite, erase, get gives nothing; skipped (with the reason printed) where `make_secret_store` returns the memory store or the environment has no session (`CI` set). It must leave nothing behind on failure (erase in a guard).
  - `SecretStore.AccountNameIsProfileAtHost`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Profiles\.|SecretStore\.'`. Expected: build fails.
- [ ] **Step 3: Implement.** macOS: `SecItemAdd` / `SecItemCopyMatching` / `SecItemUpdate` / `SecItemDelete`, generic password. Windows: `CredWriteW` / `CredReadW` / `CredDeleteW`, `CRED_TYPE_GENERIC`. Linux: `secret_password_store_sync` and friends with a simple schema.
- [ ] **Step 4: Run** the same on this machine (the platform test runs on a developer Mac). Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): profiles in a file, passwords in the system's store`.

---

### Task 9: HTTPS and the account commands

**Files:**
- Create: `libs/earthbank_qt/CMakeLists.txt`, `libs/earthbank_qt/include/pychron/earthbank/qt_http_client.hpp`, `libs/earthbank_qt/src/qt_http_client.cpp`, `tests/earthbank_qt/CMakeLists.txt`, `tests/earthbank_qt/test_qt_http_client.cpp`
- Modify: `cmake/PychronDependencies.cmake` (`find_package(Qt6 QUIET COMPONENTS Core Network)`; `PYCHRON_EARTHBANK_NETWORK_ENABLED`), top-level `CMakeLists.txt` lists, `apps/elctl/src/earthbank.cpp`, `apps/elctl/CMakeLists.txt`, `apps/elctl/tests/test_earthbank_commands.cpp`, `docs/user/08-elctl-reference.md`

**Interfaces:**
- Produces: `class QtHttpClient final : public IHttpClient` (public header Qt-free: pimpl); `std::unique_ptr<IHttpClient> make_http_client();` declared in `libs/earthbank` `http.hpp` and defined in `earthbank_qt` (or, without it, a definition in `elctl`'s stub path that returns null).
- Behaviour: blocking; HTTPS verified against the system store; a redirect to another host is not followed; timeout as the request says; `User-Agent: pychron-cpp/<version>`; an empty reply body is allowed; a transport failure is `ErrorKind::Io` with Qt's description and no URL query in the message.
- `elctl earthbank login [--profile <name>] [--username <u>] [--base-url <url>] [--institution <id>]`: asks for the password without echo (or reads `EARTHBANK_PASSWORD`; with no terminal and no variable: exit 2 saying so), signs in, prints the account's name, stores the password, saves the profile and makes it active. `account`, `packages [--create <name>] [--institution <id>]` (`--create` sets the profile's `data_package_id` to the new one), `preflight --db <url> [selection] [options]` (prints misses grouped by sample; exit 0 none, 1 misses), `import-profiles <ini>`.
- For tests `earthbank_command` takes an optional `IHttpClient*` and `ISecretStore*` through a small struct (`EarthbankDeps`), null meaning the real ones; the tests pass `FakeEarthBank` and the memory store. `HOME`-relative paths are overridable with `--profiles <file>` on every subcommand.
- Without Qt Network: `login`, `account`, `packages`, `preflight`, `upload` print `this build has no network support (Qt6 Network was not found)` and exit 2.

- [ ] **Step 1: Write the failing tests:**
  - `QtHttpClient.GetPostAndHeaders`, `.StatusAndBodyOfAnErrorReply`, `.Timeout`, `.DoesNotFollowARedirectToAnotherHost`: against a `QTcpServer` on `127.0.0.1` started in the test (plain HTTP on localhost; the test thread runs no event loop of its own beyond what the client needs).
  - `ElctlEarthbank.LoginStoresThePasswordAndTheProfile` (fake, memory store; password from `EARTHBANK_PASSWORD`), `.LoginWithAWrongPasswordStoresNothing`, `.LoginWithoutATerminalOrVariableExitsTwo` (Review Focus 4), `.AccountPrintsTheName`, `.PackagesListsAndCreates`, `.PreflightListsMissesAndExitsOne`, `.PreflightWithoutALoginSaysWhatItCouldNotAsk`, `.ImportProfiles`, `.NoPasswordOrTokenInAnyOutput`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'QtHttpClient|ElctlEarthbank'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same, and build once with `-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON` in a scratch tree to confirm the no-network path compiles and its message test passes. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): earthbank login, account, packages and preflight`.

---

### Task 10: Upload with a journal

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/journal.hpp`, `uploader.hpp`, `libs/earthbank/src/journal.cpp`, `uploader.cpp`, `tests/earthbank/test_journal.cpp`, `test_uploader.cpp`
- Modify: `docs/superpowers/specs/2026-10-07-earthbank-design.md` (section 5.3: the "in doubt" rule below)

**Interfaces:**
- Consumes: Tasks 1, 6, 7.
- Produces:
  ```cpp
  struct Journal {                       // one per (host, package, sample)
    std::string hash; std::optional<std::int64_t> sample_id, data_point_id, age_calc_id, age_summary_id;
    std::map<std::string, std::int64_t> aliquots, measurements;   // by aliquot name, by run id
    std::optional<std::string> in_doubt;                          // the record whose POST had no answer
  };
  std::filesystem::path journal_file(const std::filesystem::path& dir, std::string_view host, std::int64_t package, std::string_view sample);
  Result<std::optional<Journal>> load_journal(const std::filesystem::path&);
  Result<void> save_journal(const Journal&, const std::filesystem::path&);   // atomic

  struct UploadOptions { std::int64_t data_package_id = 0; bool allow_unresolved = false; bool new_datapoint = false;
                         std::optional<std::string> resolve_in_doubt;   // "written:<id>" or "not-written"
                         std::filesystem::path journal_dir;
                         std::function<void(const UploadEvent&)> on_event;      // progress
                         std::function<bool()> cancelled; };
  struct UploadEvent { std::string sample, record, detail; std::size_t done = 0, total = 0; };
  enum class SampleStatus { Complete, Partial, Stopped, InDoubt, Refused };
  struct SampleReport { std::string sample; SampleStatus status; std::optional<std::int64_t> data_point_id;
                        std::vector<std::string> written, missing, messages; };
  struct UploadReport { std::vector<SampleReport> samples; std::vector<Miss> misses; };
  Result<UploadReport> upload(const std::vector<Submission>&, Session&, LookupResolver&, const UploadOptions&);
  ```
- Order, wrappers and the package-scoped sample search as spec 5.3. The journal file name encodes the sample name (percent-encoding of anything outside letters, digits, `-`, `_`, `.`): Review Focus 2.
- **In doubt (Review Focus 1; amends spec 5.3).** Before each POST the journal's `in_doubt` is set to that record and saved; on an answer it is cleared with the id recorded. A transport failure or timeout on a POST leaves it set: the sample's status is `InDoubt`, nothing further is sent for that sample, and the message names the record and says to look for it on EarthBank and re-run with `resolve_in_doubt` (`written:<id>` or `not-written`). A re-run that finds `in_doubt` set and no resolution stops for that sample with the same message. For the sample record only, the uploader resolves the doubt itself by the package-scoped search.
- Pre-flight runs first; with misses and without `allow_unresolved` nothing is written and the report carries the misses. With it, unresolved `*Name` fields are dropped from the outgoing record and listed in `messages`.
- A journal with a different hash: `Refused`, the message says the data changed since the upload began and names `new_datapoint`; with `new_datapoint` the old journal is renamed `<name>.<UTC time>.superseded.json` and a fresh upload starts.
- `cancelled()` is asked between records; a cancel gives `Stopped` with the journal as it stands.
- Failure before the data point exists: `Stopped`. After: remaining sub-records are still attempted and the status is `Partial` with `missing` listed.

- [ ] **Step 1: Write the failing tests** (fake service; submissions from Task 2's fixtures; a temporary journal directory):
  - `Journal.RoundTripAndAtomicSave`, `Journal.FileNameEncodesTheSample` (`A/B`, `Café`, `..`).
  - `Uploader.WritesEveryRecordInOrder`: the fake's request log is authenticate-free and in the spec's order; the store holds one sample, one data point, the aliquots, one measurement per analysis, one age calc, one age summary, each carrying the data point's id.
  - `Uploader.DataPointGoesInTheWrapper`, `Uploader.SampleSearchIsScopedToThePackage` (a same-named sample in another package is not reused), `Uploader.ReusesAnExistingSampleInThePackage`.
  - `Uploader.NamesAreResolvedToIdsAndNamesAreNotSent`.
  - `Uploader.StopsBeforeAnyWriteOnMisses`, `Uploader.AllowUnresolvedDropsThoseFields`.
  - `Uploader.AnUnwritablePackageStopsWithTheSentence`.
  - `Uploader.FailureAtEachCallThenRerunPostsOnlyTheRemainder`: parameterised over every POST index; after the re-run the store equals a clean upload's and no record exists twice.
  - `Uploader.ATimeoutAfterSendIsInDoubtAndNothingMoreIsSent` (`drop_next`), `Uploader.RerunWithoutAResolutionStops`, `Uploader.ResolvedAsWrittenContinuesWithThatId`, `Uploader.ResolvedAsNotWrittenPostsAgain`, `Uploader.ASampleInDoubtIsResolvedBySearch`. (Review Focus 1)
  - `Uploader.AChangedSubmissionIsRefused`, `Uploader.NewDatapointStartsAfresh`.
  - `Uploader.CancelStopsBetweenRecordsAndARerunFinishes`, `Uploader.ReportsProgress` (done/total monotone, total equals the number of POSTs planned). (Review Focus 5)
  - `Uploader.TokenExpiryMidRunIsInvisible` (`expire_tokens()` between records).
  - `Uploader.TwoSamplesOneFailsTheOtherCompletes`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Journal\.|Uploader\.'`. Expected: build fails.
- [ ] **Step 3: Implement**; amend the spec's 5.3 with the in-doubt rule.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R earthbank --repeat until-fail:3`, and the earthbank tests once in the sanitizer tree. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): upload in order, resumable from a journal`.

---

### Task 11: `elctl earthbank upload`

**Files:**
- Modify: `apps/elctl/src/earthbank.cpp`, `apps/elctl/tests/test_earthbank_commands.cpp`, `docs/user/08-elctl-reference.md`

**Interfaces:**
- `elctl earthbank upload --db <url> [selection] [--profile <name>] [--package <id>] [--overrides <file>] [--allow-unresolved] [--new-datapoint] [--in-doubt written:<id>|not-written] [--journal <dir>] --yes [options]`.
- Before anything is sent it prints: the account's name and login, the package's name and id, per sample the records to be written and the preferred age, and the misses if any. Without `--yes` it stops there with exit 2 and the line `nothing was sent; add --yes to upload`. With misses and no `--allow-unresolved`: exit 1, nothing sent.
- Progress: one line per record written. Result: per sample its status and the data point id; for `Partial`, `InDoubt`, `Refused` the message and the exact command line to continue.
- Exit: 0 every sample `Complete`; 1 nothing matched, or misses; 2 usage, sign-in failure, or any sample not `Complete`.
- The password comes from the secret store, else `EARTHBANK_PASSWORD`, else a prompt on a terminal, else exit 2.

- [ ] **Step 1: Write the failing tests** (fake, memory store, the store fixture):
  - `ElctlEarthbank.UploadWithoutYesSendsNothingAndSaysWhatItWould`.
  - `ElctlEarthbank.UploadWritesAndPrintsTheIds`.
  - `ElctlEarthbank.UploadStopsOnMissesWithExitOne`, `.AllowUnresolvedProceeds`.
  - `ElctlEarthbank.UploadWithNoPackageSaysHowToSetOne`.
  - `ElctlEarthbank.APartialUploadPrintsTheCommandToFinish` and running that command finishes it.
  - `ElctlEarthbank.InDoubtPrintsTheTwoWaysToContinue`.
  - `ElctlEarthbank.NoStoredPasswordAndNoTerminalExitsTwoHavingSentNothing`, `.AWrongStoredPasswordSaysToLoginAgain`. (Review Focus 4)
  - `ElctlEarthbank.NoPasswordOrTokenInAnyOutput` over every upload test's captured output.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R ElctlEarthbank`. Expected: FAIL (unknown subcommand).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same, then both full suites. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): earthbank upload`. Parity with legacy on the command line.

---

### Task 12: Preferences page

**Files:**
- Create: `apps/pychron-ui/src/earthbank_worker.hpp`, `earthbank_worker.cpp`, `earthbank_preferences_page.hpp`, `earthbank_preferences_page.cpp`, `earthbank_package_dialog.hpp`, `earthbank_package_dialog.cpp`, `tests/ui/test_earthbank_preferences.cpp`
- Modify: `apps/pychron-ui/src/preferences_dialog.cpp` (add the page), `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt`

**Interfaces:**
- `EarthbankWorker`: a `QObject` on its own `QThread` owning an `IHttpClient`, a `Session` and an `ISecretStore`; slots `test(Profile, password)`, `listPackages(Profile, password)`, `createPackage(Profile, password, name)`, and later `run(...)` (Task 13); signals with results and plain-sentence errors. Constructed with factories so tests pass `FakeEarthBank` and the memory store. Requests are queued and run one at a time.
- Page: a table of profiles (name, base URL, username, institution id, data package id, active); Add, Remove, Test, Select / Create Package; the password is typed into a field that is never read back into the table and is written to the secret store on Test success or on Apply; a line shows the store in use (`Passwords are kept in: keychain`, or `not stored: this session only`).
- Package dialog: lists the institution's packages (name, id, state), a filter box, a name field with Create; OK gives the id.
- Applying writes `profiles.toml` through `save_profiles`; the UI and `elctl` read the same file.

- [ ] **Step 1: Write the failing tests** (Qt Test, offscreen, fake service):
  - `addingAProfileAndApplyingWritesTheFile`, `thePasswordIsNotInTheFileAndIsInTheStore`, `testShowsTheAccountsName`, `testWithAWrongPasswordShowsTheSentenceAndStoresNothing`, `selectingAPackageSetsItsId`, `creatingAPackageSelectsIt`, `removingAProfileErasesItsPassword`, `theUiStaysResponsiveWhileARequestIsOutstanding` (the fake delays its reply; the event loop processes a timer meanwhile).
- [ ] **Step 2: Run** `QT_QPA_PLATFORM=offscreen ctest --test-dir build/dev-ui -R earthbank_preferences`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same, then the UI suite. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): EarthBank profiles in Preferences`.

---

### Task 13: Publish dialog

**Files:**
- Create: `apps/pychron-ui/src/earthbank_publish_dialog.hpp`, `earthbank_publish_dialog.cpp`, `tests/ui/test_earthbank_publish.cpp`
- Modify: `apps/pychron-ui/src/data_browser_window.cpp/.hpp` (an action **Publish to EarthBank...** beside the report export, enabled with a selection), `apps/pychron-ui/src/processing_bridge.*` if the browser needs a way to hand over the selected analyses reduced, `earthbank_worker.*` (`run`), `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt`

**Interfaces:**
- The dialog is given the selected analyses and builds submissions with the browser's current reduction settings.
- Controls: Action (Upload / Export xlsx / Pre-flight); Profile and Package with Select / Create Package for this run; a table, one row per sample: analyses, Interpretation (editable, with the vocabulary's entries offered once loaded), Preferred age (a choice showing each candidate's age and error, the rule's pick marked), Flux monitor (editable, the found one shown), notes; Decay constant and Air ratio names; for Export: folder, one workbook per sample, include samples; for Upload: Allow unresolved terms.
- Run: Pre-flight shows misses in the table rows they belong to. Export writes and offers to show the folder. Upload first shows the confirmation (account, package, record counts) and proceeds on Yes; progress per record; Cancel stops between records; the result pane lists per sample what was written with ids, what was not and why, and for `InDoubt` the two choices as buttons (`It is there, id: [ ]` / `It is not there`) which re-run with the resolution.
- The last interpretation, preferred age and flux monitor per sample name are remembered in the UI's settings.
- A disagreeing sample (Task 2) is shown as a row with the reason and is excluded from the run; the others proceed.

- [ ] **Step 1: Write the failing tests** (Qt Test, offscreen, fake service, the UI fixtures' store):
  - `theActionIsEnabledOnlyWithASelection`, `oneRowPerSampleWithCandidatesAndTheRulesPick`, `preflightMarksTheRowsWithMisses`, `exportWritesTheWorkbooks`, `uploadAsksFirstAndWritesOnYes`, `uploadWritesNothingOnNo`, `cancelStopsAndARerunFinishes`, `anInDoubtRecordOffersBothResolutions`, `aDisagreeingSampleIsShownAndLeftOut`, `choicesAreRememberedPerSample`, `noPasswordOrTokenAppearsInTheResultPane`, `manySamplesShowProgress` (300 samples built in the test: the progress signal arrives more than once and the dialog stays responsive: Review Focus 5).
- [ ] **Step 2: Run** `QT_QPA_PLATFORM=offscreen ctest --test-dir build/dev-ui -R earthbank_publish`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same, then the UI suite. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): publish the selected analyses to EarthBank`.

---

### Task 14: Guide, rules, live check

**Files:**
- Create: `docs/earthbank.md`, `tests/earthbank/test_live.cpp`
- Modify: `AGENTS.md`, `README.md` and `docs/user/README.md` (list the guide), `docs/user/08-elctl-reference.md`, `docs/superpowers/specs/2026-10-07-earthbank-design.md`

- [ ] **Step 1: Write `docs/earthbank.md`** for a lab user, in the style of `docs/export.md`, from legacy's `UPLOAD_GUIDE.md` and what was built: what EarthBank and a data package are; what you need (account, write access, institution id); one-time setup in Preferences and with `elctl earthbank login`; publishing from the data browser, step by step; the same with `elctl`; pre-flight; export to workbooks and importing them on the website; what is sent for each sample and where each value comes from (the table of spec 4.2, in the reader's words); what is left empty and why; choosing the preferred age; constants and monitor names; when something goes wrong (the sentences, what each means, what to do, including "in doubt"); resuming; where the profile file and the journal live; that passwords are in the system's store. Then a maintainer's section: the live-test runbook (below), replacing the fake's fixtures with captures, swapping in a newer vendor template.
- [ ] **Step 2: The live test** `tests/earthbank/test_live.cpp`, built only when Qt Network is, and skipped unless `PYCHRON_TEST_EARTHBANK_URL`, `_USER`, `_PASSWORD`, `_INSTITUTION` are all set: creates a package `pychron-test-<UTC date-time>`, uploads the two fixture samples, reads the data point and its measurements back and compares counts and three values, runs the upload again (nothing new is written), then deletes the package in a guard that runs on failure too. It prints the package's name and id first, so a person can remove it by hand if the process dies. It is excluded from the default `ctest` run by a label (`LABELS live`) and `ctest -LE live` is what AGENTS.md documents; confirm the default run in CI configuration does not pick it up without the variables (it skips), and say which.
- [ ] **Step 3: Add to `AGENTS.md`** under "Build and test", one bullet: `libs/earthbank` is Qt-free and has no network code (`IHttpClient`; `libs/earthbank_qt` is the adapter, built when Qt6 Network is found); one `Submission` per sample feeds pre-flight, workbooks and upload; record field names are the service's verbatim; no test reaches EarthBank (`FakeEarthBank`), the live test is opt-in by environment and never in CI; no password or token in files, logs or output; the uploader journals before and after each write and never guesses the outcome of an unanswered one; the guide is `docs/earthbank.md`.
- [ ] **Step 4: Bring the spec in line** with what was built, section by section; `Status: Implemented`; move what remains of section 11 to a "Not yet" section.
- [ ] **Step 5: Verify.** Both trees built, both full suites; the sanitizer tree's full suite; `ctest --test-dir build/dev -R 'earthbank|Earthbank|Ooxml|Xlsx|Mapping|Vocab|Model\.' --repeat until-fail:5`; `grep -rnI "ausgeochem.auscope.org.au" libs apps tests | grep -v "profiles.hpp\|docs"` prints nothing; `git grep -nI -i "id_token\|bearer " -- tests/earthbank/fixtures` shows only the fake's opaque placeholders.
- [ ] **Step 6: One live run, by the owner.** With real credentials: `elctl earthbank login`, `packages --create`, `preflight` on a real sample (correct `vocab.cpp` where a name differs and add the case to its test), `export-xlsx` and an import of the workbook through the website into the test package, `upload` of the same sample into a second test package, a comparison of the two on EarthBank, then deletion of both packages. Record the outcome in `docs/earthbank.md`'s maintainer section (date, what differed, what was fixed).
- [ ] **Step 7: Commit** `docs: publishing to EarthBank, for users and maintainers`.
- [ ] **Step 8: Land, when the owner says so.** Rebase on `origin/develop`, rerun Step 5, merge into `develop`. No pull request.

## Decisions the owner must make outside the code

These do not block Tasks 1 to 13:

1. Ask AuScope / Lithodat (`support@lithodat.com`) whether the two template workbooks may ship with pychron's installers. Until answered they are in the repository and the build tree; if refused, remove them from the install step and have `default_template_dir()` fall back to `<config>/earthbank/templates`, which the guide tells the user to fill once.
2. Read the API licence linked from legacy's README for any condition on clients (identification, rate limits).
3. Run the capture step of Task 6 and the live run of Task 14 Step 6.
