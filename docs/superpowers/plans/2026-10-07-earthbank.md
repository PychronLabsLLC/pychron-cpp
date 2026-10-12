# EarthBank Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish 40Ar/39Ar results from pychron-cpp to EarthBank (AusGeochem): REST upload, export vendor template workbooks, pre-flight check of controlled terms; from `elctl` and data browser.

**Architecture:** Qt-free `libs/earthbank` turns selected analyses into one typed `Submission` per sample; pre-flight, workbook exporter, uploader consume it. Network behind `IHttpClient` (Qt Network adapter in `libs/earthbank_qt`), passwords behind `ISecretStore` (native OS stores). All automated tests run against in-process `FakeEarthBank`; CI never reaches service.

**Tech Stack:** C++20, nlohmann-json, toml++, miniz and pugixml (new), Qt6 Network (adapter only), Security.framework / wincred / libsecret, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-07-earthbank-design.md`. Read first; section numbers below refer to it.

**Reference being migrated:** `UMelbournePychron/pychron`, branch `dev/dc2025`, directory `pychron/ausgeochem/`. Fetch file to scratch directory (never into repository):

```bash
gh api "repos/UMelbournePychron/pychron/contents/pychron/ausgeochem/<file>?ref=dev/dc2025" -H "Accept: application/vnd.github.raw" > <scratch>/<file>
```

Files: `README.md`, `UPLOAD_GUIDE.md`, `earthbank_service.py`, `xlsx_exporter.py`, `data_package_dialog.py`, `tasks/node.py`, `tasks/preferences.py`, `tests/*.py`, two workbooks under `templates/` (fetch binary: `-H "Accept: application/vnd.github.raw"`, redirect to `.xlsx` file).

## Global Constraints

- Branch `feat/earthbank`, cut from `develop`.
- Conventional Commits, component scope (`feat(earthbank): ...`, `feat(elctl): ...`, `feat(ui): ...`). End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build/test: `cmake --build build/dev -j` then `ctest --test-dir build/dev --output-on-failure -R <regex>`; UI tree `build/dev-ui` with `QT_QPA_PLATFORM=offscreen`. CI skips work branches: run task's tests before each commit, both full suites before last commit of Tasks 5, 11, 14.
- Never skip or disable failing test. Failure on one compiler only = real bug.
- **No automated test, example, script or default configuration contacts EarthBank.** Only code that does: behind explicit user action, or opt-in live test of Task 14. Production host `https://ausgeochem.auscope.org.au` appears as default in exactly one place (`profiles.hpp`).
- **No password, token or real person's name, login or package id committed**: code, fixtures, logs, test output. Token never in log line or error message; request logged as method, path, status.
- `libs/earthbank` Qt-free, no network code: depends on `pychron::processing`, `pychron::reduction`, `pychron::core`, nlohmann-json, toml++, miniz, pugixml. Qt only in `libs/earthbank_qt` (privately) and UI.
- Record field names = service's, verbatim, misspellings included (`apparentAgeUncertainityTypeName`, `stratographicUnitName`).
- Ages Ma; uncertainties 1 sigma, absolute; intensities fA.
- File written for user goes through `pychron::mark_as_user_file`.
- Each file includes each header it uses. Warnings are errors.
- UI network work runs on worker thread, not clock participant (AGENTS.md "Time": wait on outside world).
- Unknown field in `profiles.toml` or overrides file: `Config` error naming file and key.

## Review Focus

1. Request times out after sent: server may or may not have written. Journal must not guess; user told exactly which record in doubt and how to continue. (Task 10 tests.)
2. Names with spaces, non-ASCII letters, `&`, `<`, quotes, `%`, `+`: in URL query, JSON, workbook XML, journal file name. (Tasks 4, 7, 10 tests.)
3. Sample whose analyses disagree: different constants, mix of step-heating and fusion, two materials, half a location. Refused naming the two values, or handled by stated rule; never silently first analysis's. (Task 2 tests.)
4. Headless machine, no secret store; wrong or expired password mid-run: clear message, exit code 2, nothing written, no prompt blocking a script. (Tasks 8, 9, 11 tests.)
5. Selection of several thousand analyses across many samples: progress reported; cancel stops between records, leaves journal a re-run can finish; lookups cached, term asked once. (Tasks 7, 10, 13 tests.)

## File Structure

| File | Responsibility |
|---|---|
| `libs/earthbank/include/pychron/earthbank/model.hpp`, `src/model.cpp` | record structs, `Submission`, `to_json` |
| `.../vocab.hpp`, `src/vocab.cpp` | static tables, pure functions over them |
| `.../mapping.hpp`, `src/mapping.cpp` | `build_submissions` |
| `.../http.hpp` | `IHttpClient`, `HttpRequest`, `HttpResponse`, URL encoding |
| `.../session.hpp`, `src/session.cpp` | authenticate, account, packages, error sentences |
| `.../lookup.hpp`, `src/lookup.cpp` | `LookupResolver` |
| `.../preflight.hpp`, `src/preflight.cpp` | `preflight` |
| `.../uploader.hpp`, `src/uploader.cpp`, `.../journal.hpp`, `src/journal.cpp` | upload order, journal |
| `.../xlsx.hpp`, `src/xlsx.cpp`, `src/ooxml.hpp`, `src/ooxml.cpp` | template fill; zip and sheet XML helpers |
| `.../profiles.hpp`, `src/profiles.cpp`, `.../secret_store.hpp`, `src/secret_store_{mac.mm,win.cpp,linux.cpp,memory.cpp}` | profiles, passwords |
| `libs/earthbank/templates/*.xlsx` | two vendor workbooks |
| `libs/earthbank_qt/include/pychron/earthbank/qt_http_client.hpp`, `src/qt_http_client.cpp` | HTTPS |
| `apps/elctl/src/earthbank.cpp`, `earthbank.hpp`, `earthbank_stub.cpp`, `selection.hpp`, `selection.cpp` | command; selection flags shared with `export` |
| `apps/pychron-ui/src/earthbank_preferences_page.*`, `earthbank_publish_dialog.*`, `earthbank_worker.*` | UI |
| `tests/earthbank/fake_earthbank.hpp/.cpp`, `fixtures/*.json`, `test_*.cpp` | tests |
| `docs/earthbank.md` | user guide, live-test runbook |

---

### Task 1: Records and vocabulary

**Files:**
- Create: `libs/earthbank/CMakeLists.txt`, `libs/earthbank/include/pychron/earthbank/model.hpp`, `vocab.hpp`, `libs/earthbank/src/model.cpp`, `vocab.cpp`, `tests/earthbank/CMakeLists.txt`, `tests/earthbank/test_model.cpp`, `test_vocab.cpp`
- Modify: top-level `CMakeLists.txt` library and test lists (add `earthbank` after `processing`)

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
- Fields = exactly legacy's `SAMPLE_FIELDS`, `LOCATION_FIELDS`, `DATA_POINT_FIELDS`, `ALIQUOT_FIELDS`, `MEASUREMENT_FIELDS`, `AGE_CALC_FIELDS`, `AGE_SUMMARY_FIELDS` (`earthbank_service.py` lines 143-355): `std::optional<double>` numbers, `std::optional<std::string>` text and every `*Name`, `std::optional<std::int64_t>` every `*Id` and `id`, `std::optional<bool>` `preferredAge`. One X-macro list per record generates struct, `to_json`, workbook field access.
- Constants: `Steiger and Jager, 1977` 5.543e-10, `Min et al. 2000` 5.463e-10, `Renne et al. 2011` 5.531e-10 (per year, total); `Lee et al. 2006` 298.56, `Nier 1950` 295.5. Before pinning, compute `lambda_b + lambda_e` and `atm4036` of each `reduction::constants_preset()` in a test; commit body records name each preset maps to; none → say so (add no name service may lack).

- [ ] **Step 1: Write the failing tests:**
  - `Model.ToJsonHasOnlySetFieldsUnderServiceNames`.
  - `Model.EveryLegacyFieldIsPresent`.
  - `Model.ContentHashIgnoresNotesAndFieldOrder`, `Model.ContentHashChangesWithAValue`.
  - `Vocab.ArMethod`.
  - `Vocab.UncertaintyType`.
  - `Vocab.DecayConstantAndAirRatioByValue`.
  - `Vocab.PresetsMapToNames`.
  - `Vocab.NormalizeCitation`.
  - `Vocab.LookupSpecsCoverEveryNameField`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Model\.|Vocab\.'`. Expected: build fails, no such library.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): the service's records and vocabulary tables`.

---

### Task 2: From analyses to submissions

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/mapping.hpp`, `libs/earthbank/src/mapping.cpp`, `tests/earthbank/test_mapping.cpp`, `tests/earthbank/analyses.hpp` (fixture builders)

**Interfaces:**
- Consumes: Task 1; `processing::reduce_analysis`, `processing/arar_groups.hpp` (`integrated_age`, `isochron_points`, `isochron_age`), plateau and weighted-mean code report uses (`libs/processing/src/report.cpp`: reuse; file-local → move to `processing` header, no copy).
- Produces: `PreferredAge`, `SampleOptions`, `PublishOptions`, `build_submissions` exactly as spec 4.1.

**Rules beyond spec tables:**
- Order: samples by name; analyses by timestamp; aliquots first-appearance.
- Disagreement within sample (Review Focus 3): two materials, two lithologies, two decay-constant or air-ratio values, or location on some analyses differing → `Config` error naming sample, field, two values. Both step-heating aliquots and fusion analyses: `arMethodName` from whole set (has steps), preferred-age rule as step-heating, plus note. Latitude without longitude or reverse: no `Location`, plus note.
- Analysis with no age (`ReducedAnalysis::arar` absent or without ages): left out of measurements, counted in note. Sample with none left: error.
- `datapoint_name` default `"<sample> <material>"`, or `"<sample>"` with no material.
- `cumulative39ArK`: percent, per aliquot, step order; absent for fusion.
- Preferred age per spec 4.4. `ageCalcTypeName`: `Plateau`, `Weighted Mean`, `Inverse Isochron`, `Total Gas`. `nAnalysisGrouped`: analyses reported age used. `mswd`, `agepvalue` from that computation; absent for `Total Gas`.

- [ ] **Step 1: Write the failing tests** (fixtures in `analyses.hpp` from `tests/processing/fixtures.hpp` helpers: 10-step sample `S-STEP` with plateau over steps C-H, 6-analysis fusion sample `S-FUS`, known intercepts, J, constants):
  - `Mapping.OneSubmissionPerSampleInNameOrder`.
  - `Mapping.SampleAndLocation`.
  - `Mapping.DataPoint`.
  - `Mapping.MeasurementValues`.
  - `Mapping.PlateauStepsAreFlagged`.
  - `Mapping.PreferredAgeRule`.
  - `Mapping.AgeCalcNamesComeFromTheConstantsUsed`, `Mapping.OverridesAreNormalized`.
  - `Mapping.DisagreementIsRefusedNamingBothValues`. (Review Focus 3)
  - `Mapping.AnalysesWithoutAgesAreLeftOutAndCounted`, `Mapping.ASampleWithNoAgeIsRefused`.
  - `Mapping.LeftEmptyFieldsAreAbsent`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R Mapping`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same, then `-R 'Report|Processing'` (functions moved out of `report.cpp` must leave report unchanged). Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): analyses become one submission per sample`.

---

### Task 3: Workbook parts: zip and sheet XML

**Files:**
- Modify: `cmake/PychronDependencies.cmake`, `vcpkg.json` (miniz, pugixml: `find_package` first, `FetchContent` fallback, as existing entries)
- Create: `libs/earthbank/src/ooxml.hpp`, `ooxml.cpp`, `tests/earthbank/test_ooxml.cpp`, `libs/earthbank/templates/ArArDataPoint_Template2026.xlsx`, `Sample_Template_v2025-04-16.xlsx` (fetched from fork as binary; SHA-256 in commit body)

**Interfaces:**
- Produces (library-internal):
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
- `append_rows` writes `<row r="N">` elements with `<c r="A5" t="inlineStr"><is><t xml:space="preserve">text</t></is></c>` or `<c r="A5"><v>1.5</v></c>`; numbers shortest round-trip decimal; empty cell omitted. Refuses if sheet has row at or after `first_row` with content. Updates `<dimension>` if present. Text XML-escaped; characters XML 1.0 forbids refused, cell named.

- [ ] **Step 1: Write the failing tests:**
  - `Ooxml.OpensBothTemplatesAndNamesTheirSheets`.
  - `Ooxml.Row4IsTheApiFieldRow`: `ArArMeasurement` row 4 contains `ar40Corr`, `apparentAge`, `stepNumber`.
  - `Ooxml.SaveWithNoChangeKeepsEveryPartByteIdentical`.
  - `Ooxml.AppendedRowsReadBack`. (Review Focus 2)
  - `Ooxml.OnlyTheTouchedSheetChanges`.
  - `Ooxml.RefusesARowThatIsNotEmpty`, `Ooxml.RefusesAControlCharacter`, `Ooxml.RefusesAnUnknownSheet`.
  - `Ooxml.TheResultIsAWellFormedZip`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R Ooxml`. Expected: build fails.
- [ ] **Step 3: Implement.** miniz for archive; pugixml for `xl/workbook.xml`, `xl/_rels/workbook.xml.rels`, `xl/sharedStrings.xml`, appended sheet.
- [ ] **Step 4: Run** same; then open one output by hand in spreadsheet program, confirm no repair prompt (program in commit body).
- [ ] **Step 5: Commit** `feat(earthbank): rows appended to a workbook template`.

---

### Task 4: Workbook export

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/xlsx.hpp`, `libs/earthbank/src/xlsx.cpp`, `tests/earthbank/test_xlsx.cpp`
- Modify: `libs/earthbank/CMakeLists.txt` (install templates; compile definition for build-tree path in tests), `cmake/PychronInstall.cmake` (templates under application's data directory)

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
- Column takes record field named by column's `apiField`, else that name plus `Name`, with legacy's `SHEET_OVERRIDES` (`xlsx_exporter.py` lines 30-70) as table. Rows linked by `datapointName` (data point's name) in every Ar-Ar sheet with that column. File names: `ArArDataPoint_<UTC date>.xlsx`, `Samples_<UTC date>.xlsx`, or `ArArDataPoint_<sample>.xlsx` per sample, name file-safe (letters, digits, `-`, `_`; else `_`; collision → `-2`). Each file through `mark_as_user_file`.

- [ ] **Step 1: Write the failing tests** (submissions from Task 2 fixtures):
  - `Xlsx.EveryRecordLandsInItsSheetUnderItsColumn`.
  - `Xlsx.HeaderRowsAreTheTemplates`.
  - `Xlsx.RowsAreLinkedByDatapointName`.
  - `Xlsx.OneWorkbookOrOnePerSample`, `Xlsx.SampleWorkbookIsOptional`.
  - `Xlsx.AFieldTheTemplateLacksIsNoted`.
  - `Xlsx.SheetOverrides`.
  - `Xlsx.UnsafeSampleNamesMakeSafeDistinctFileNames`. (Review Focus 2)
  - `Xlsx.AMissingTemplateIsAnErrorNamingTheFile`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R Xlsx`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'Xlsx|Ooxml|Mapping'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): submissions written into the vendor's workbooks`.

---

### Task 5: `elctl earthbank export-xlsx`

**Files:**
- Create: `apps/elctl/src/selection.hpp`, `selection.cpp`, `apps/elctl/src/earthbank.hpp`, `earthbank.cpp`, `earthbank_stub.cpp`, `apps/elctl/tests/test_earthbank_commands.cpp`
- Modify: `apps/elctl/src/export.cpp` (use `selection`), `apps/elctl/src/cli.cpp` (dispatch `earthbank`, usage text), `apps/elctl/CMakeLists.txt` (stub split as `export`; link `pychron::earthbank`), `docs/user/08-elctl-reference.md`

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
- `elctl earthbank export-xlsx --db <url> [selection] --out <dir> [--per-sample] [--no-samples] [--templates <dir>] [--overrides <file.toml>] [reduction and plateau flags as export]`. Prints one line per sample (analyses, preferred age and why, notes), then files written. Exit 0 wrote files; 1 nothing matched; 2 usage or error.
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
  Sample named there but absent from selection: error naming it.
- Without persistence: stub says command needs store, exits 2 (as `export_stub.cpp`).

- [ ] **Step 1: Write the failing tests** (store fixture `apps/elctl/tests` uses for `export`):
  - `ElctlExport.StillWorksThroughTheSharedSelection`.
  - `ElctlEarthbank.ExportXlsxWritesBothWorkbooks`.
  - `ElctlEarthbank.PerSampleAndNoSamples`.
  - `ElctlEarthbank.OverridesFileIsApplied`, `ElctlEarthbank.AnUnknownKeyOrSampleInOverridesIsRefused`.
  - `ElctlEarthbank.NothingMatchedExitsOne`, `ElctlEarthbank.UsageErrorsExitTwo` (no `--out`, unknown flag, unknown subcommand).
  - `ElctlEarthbank.ADisagreeingSampleIsReportedAndTheOthersAreNotWritten`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'ElctlEarthbank|ElctlExport'`. Expected: build fails.
- [ ] **Step 3: Implement.** Move selection parsing out of `export.cpp`; behaviour, messages unchanged.
- [ ] **Step 4: Run** same, then both full suites. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): earthbank export-xlsx writes the AusGeochem workbooks`. First usable point: lab can publish through website.

---

### Task 6: `IHttpClient` and the fake service

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/http.hpp`, `libs/earthbank/src/http.cpp`, `tests/earthbank/fake_earthbank.hpp`, `fake_earthbank.cpp`, `tests/earthbank/fixtures/*.json`, `tests/earthbank/test_fake_earthbank.cpp`, `tests/earthbank/fixtures/README.md`

**Interfaces:**
- Produces: `HttpRequest`, `HttpResponse`, `IHttpClient` as spec 5.5; `std::string url_encode(std::string_view)` (RFC 3986 unreserved kept, rest percent-encoded, space `%20`); `std::string with_query(std::string_view url, std::initializer_list<std::pair<std::string_view, std::string>> params)`.
- `FakeEarthBank : IHttpClient`, in memory:
  - `POST /api/authenticate` `{username, password}` → `{"id_token": "<opaque>"}` for configured user, else 401; `expire_tokens()` makes next authenticated call 401 once.
  - `GET /api/account` (bearer) → account.
  - `GET` each lookup endpoint of `lookup_specs()` with `name.equals`, `name.contains`, `page`, `size`; some need bearer (`/api/core/l-*`), as legacy found.
  - `GET|POST|DELETE /api/management/data-packages`; POST with flat body creates package, name ignored (trap legacy documents); creator can write to what they create and to packages marked writable.
  - `GET /api/core/sample-with-locations?name.equals=&dataPackageId.equals=`, `POST` the wrapper.
  - `POST /api/arar/ArArDataPoint` (wrapper required: body without `dataPointDTO` → `500 {"title":"Package is not writable by user."}`), `ArArAliquot`, `ArArMeasurement`, `ArArAgeCalc`, `ArArAgeSummary`. Id for `*Id` field not in that field's vocabulary → `400` with constraint message naming field. Write to package user cannot write → the 500 above.
  - Test controls: `fail_next(method, path_prefix, status, body)`, `drop_next(method, path_prefix)` (request applied on server, client gets timeout error: Review Focus 1), `requests()` (log), `store()` (what exists).
- Fixtures: `lookups/<endpoint>.json` (few dozen entries each, incl. verbose flux-monitor names legacy quotes), `account.json`, `packages.json`. Hand-written from legacy code, README, probe scripts, invented people and ids; `fixtures/README.md` says so, gives commands to replace with captures (owner step below).

**Owner step (person with EarthBank login; not required for task to pass).** Reads only, on fork:
- run legacy `tests/packages_probe.py` with `EARTHBANK_KNOWN_PACKAGES=<own package id>` (without variable it asks for two ids hard-coded in it): account, one package, first row of list;
- run `tests/integration_test.py --smoke` (anonymous; prints entry count per lookup, not JSON): which lists answer without login;
- save plain `GET <endpoint>?size=2000` of each list;
- **do not run** `tests/arar_post_probe.py`, `tests/integration_test.py` without `--smoke`, or `tests/live_upload_test.py`: each writes to production database, leaves a record;
- error bodies captured during by-hand live run in throwaway package (Task 14), not here.

Replace every real name, login, id in saved files; drop over hand-written fixtures. Shape differs from fake → finding; fix fake before Task 9.

- [ ] **Step 1: Write the failing tests:** one per behaviour above (`FakeEarthBank.Authenticate`, `.LookupEquals`, `.LookupContainsPaged`, `.LookupNeedsABearerOnCoreEndpoints`, `.PackageFlatBodyIgnoresTheName`, `.SampleSearchIsScopedToThePackage`, `.DataPointNeedsTheWrapper`, `.WrongVocabularyIdIsAConstraintError`, `.UnwritablePackageIsRefused`, `.DropNextAppliesAndTimesOut`), and `Http.UrlEncode` (space, `&`, `+`, `%`, `é`, `/`).
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'FakeEarthBank|Http\.'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
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
- Lookup strategies, order per spec 5.1. Resolver asks service at most once per (endpoint, value), loads whole vocabulary at most once. Query values via `with_query` (Review Focus 2). `create_package` posts nested-and-mirrored body of spec 5.4. Token never in error or log line.

- [ ] **Step 1: Write the failing tests:**
  - `Session.SignInThenAccount`, `Session.WrongPasswordIsTheLoginSentence`, `Session.ReauthenticatesOnceOnA401` (with `expire_tokens()`), `Session.ASecond401IsAnError`, `Session.PackagesByInstitution`, `Session.CreatePackageKeepsItsName` (fails against fake if body flat), `Session.NoTokenInAnyErrorOrLog`.
  - `Session.Sentences`.
  - `Lookup.CachedExactIsCaseInsensitive`, `Lookup.EqualsThenCapitalizedThenContains`, `Lookup.ContainsNeedsAnExactMatchAmongTheResults`, `Lookup.FluxMonitorFuzzyByAuthorAndYear` (verbose entry legacy quotes), `Lookup.UncertaintyTypeComesFromTheStaticTable` (no request), `Lookup.CouldNotAskWithoutALogin`, `Lookup.AsksOncePerValue` (count fake's requests), `Lookup.ValuesAreEncoded` (`Café & Co`).
  - `Preflight.ReportsEveryUnknownTermWithItsRecordAndEndpoint`, `Preflight.CouldNotAskIsSeparateFromUnknown`, `Preflight.WritesNothing` (fake's store unchanged, only GETs), `Preflight.ManySamplesAskForEachTermOnce`. (Review Focus 5)
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Session\.|Lookup\.|Preflight\.'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): sign-in, vocabulary lookup and a pre-flight check`.

---

### Task 8: Profiles and passwords

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/profiles.hpp`, `secret_store.hpp`, `libs/earthbank/src/profiles.cpp`, `secret_store_memory.cpp`, `secret_store_mac.mm`, `secret_store_win.cpp`, `secret_store_linux.cpp`, `tests/earthbank/test_profiles.cpp`, `test_secret_store.cpp`
- Modify: `libs/earthbank/CMakeLists.txt` (per-platform source; `-framework Security` on macOS; optional `pkg_check_modules(libsecret-1)` on Linux; `PYCHRON_EARTHBANK_SECRET_STORE` definition naming backend), `packaging/macos/entitlements.plist` only if hardened runtime needs one for Keychain access (not for login keychain; confirm, say so in commit body)

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
- `default_profiles_file()`: `$XDG_CONFIG_HOME` or `~/.config` on Linux, `~/Library/Application Support` on macOS, `%APPDATA%` on Windows, then `pychron/earthbank/profiles.toml`; UI passes its own `AppConfigLocation` so both agree (check what `QStandardPaths::AppConfigLocation` resolves to per platform, make same directory; state paths in commit body).
- `base_url` must be `https://` (or `http://localhost` / `127.0.0.1` for tests), else `Config` error. Profile name: letters, digits, `-`, `_`.
- `make_secret_store` returns platform store when built in and usable at run time, else memory store (per process).

- [ ] **Step 1: Write the failing tests:**
  - `Profiles.RoundTrip`, `Profiles.AMissingFileIsEmpty`, `Profiles.NoPasswordKeyIsEverWrittenOrAccepted` (`password` key in file → `Config` error naming it), `Profiles.UnknownKeyIsRefused`, `Profiles.BaseUrlMustBeHttps`, `Profiles.ActiveMustNameAProfile`, `Profiles.SaveIsAtomic` (existing file intact if write fails.
  - `Profiles.ImportLegacyIni`.
  - `SecretStore.MemoryRoundTrip`.
  - `SecretStore.PlatformRoundTrip`.
  - `SecretStore.AccountNameIsProfileAtHost`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Profiles\.|SecretStore\.'`. Expected: build fails.
- [ ] **Step 3: Implement.** macOS: `SecItemAdd` / `SecItemCopyMatching` / `SecItemUpdate` / `SecItemDelete`, generic password. Windows: `CredWriteW` / `CredReadW` / `CredDeleteW`, `CRED_TYPE_GENERIC`. Linux: `secret_password_store_sync` and friends, simple schema.
- [ ] **Step 4: Run** same on this machine (platform test runs on developer Mac). Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): profiles in a file, passwords in the system's store`.

---

### Task 9: HTTPS and the account commands

**Files:**
- Create: `libs/earthbank_qt/CMakeLists.txt`, `libs/earthbank_qt/include/pychron/earthbank/qt_http_client.hpp`, `libs/earthbank_qt/src/qt_http_client.cpp`, `tests/earthbank_qt/CMakeLists.txt`, `tests/earthbank_qt/test_qt_http_client.cpp`
- Modify: `cmake/PychronDependencies.cmake` (`find_package(Qt6 QUIET COMPONENTS Core Network)`; `PYCHRON_EARTHBANK_NETWORK_ENABLED`), top-level `CMakeLists.txt` lists, `apps/elctl/src/earthbank.cpp`, `apps/elctl/CMakeLists.txt`, `apps/elctl/tests/test_earthbank_commands.cpp`, `docs/user/08-elctl-reference.md`

**Interfaces:**
- Produces: `class QtHttpClient final : public IHttpClient` (public header Qt-free: pimpl); `std::unique_ptr<IHttpClient> make_http_client();` declared in `libs/earthbank` `http.hpp`, defined in `earthbank_qt` (or, without it, definition in `elctl`'s stub path returning null).
- Behaviour: blocking; HTTPS verified against system store; redirect to another host not followed; timeout as request says; `User-Agent: pychron-cpp/<version>`; empty reply body allowed; transport failure = `ErrorKind::Io` with Qt's description, no URL query in message.
- `elctl earthbank login [--profile <name>] [--username <u>] [--base-url <url>] [--institution <id>]`: asks password without echo (or reads `EARTHBANK_PASSWORD`; no terminal and no variable: exit 2 saying so), signs in, prints account's name, stores password, saves profile, makes it active. `account`, `packages [--create <name>] [--institution <id>]` (`--create` sets profile's `data_package_id` to new one), `preflight --db <url> [selection] [options]` (prints misses grouped by sample; exit 0 none, 1 misses), `import-profiles <ini>`.
- For tests `earthbank_command` takes optional `IHttpClient*` and `ISecretStore*` via small struct (`EarthbankDeps`), null = real ones; tests pass `FakeEarthBank` and memory store. `HOME`-relative paths overridable with `--profiles <file>` on every subcommand.
- Without Qt Network: `login`, `account`, `packages`, `preflight`, `upload` print `this build has no network support (Qt6 Network was not found)` and exit 2.

- [ ] **Step 1: Write the failing tests:**
  - `QtHttpClient.GetPostAndHeaders`, `.StatusAndBodyOfAnErrorReply`, `.Timeout`, `.DoesNotFollowARedirectToAnotherHost`: against `QTcpServer` on `127.0.0.1` started in test (plain HTTP on localhost; test thread runs no event loop of its own beyond what client needs).
  - `ElctlEarthbank.LoginStoresThePasswordAndTheProfile` (fake, memory store; password from `EARTHBANK_PASSWORD`), `.LoginWithAWrongPasswordStoresNothing`, `.LoginWithoutATerminalOrVariableExitsTwo` (Review Focus 4), `.AccountPrintsTheName`, `.PackagesListsAndCreates`, `.PreflightListsMissesAndExitsOne`, `.PreflightWithoutALoginSaysWhatItCouldNotAsk`, `.ImportProfiles`, `.NoPasswordOrTokenInAnyOutput`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'QtHttpClient|ElctlEarthbank'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same; build once with `-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON` in scratch tree: confirm no-network path compiles, its message test passes. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): earthbank login, account, packages and preflight`.

---

### Task 10: Upload with a journal

**Files:**
- Create: `libs/earthbank/include/pychron/earthbank/journal.hpp`, `uploader.hpp`, `libs/earthbank/src/journal.cpp`, `uploader.cpp`, `tests/earthbank/test_journal.cpp`, `test_uploader.cpp`
- Modify: `docs/superpowers/specs/2026-10-07-earthbank-design.md` (section 5.3: "in doubt" rule below)

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
- Order, wrappers, package-scoped sample search per spec 5.3. Journal file name encodes sample name (percent-encoding of anything outside letters, digits, `-`, `_`, `.`): Review Focus 2.
- **In doubt (Review Focus 1; amends spec 5.3).** Before each POST journal's `in_doubt` set to that record, saved; on answer cleared, id recorded. Transport failure or timeout on POST leaves it set: sample status `InDoubt`, nothing further sent for that sample, message names the record, says to look for it on EarthBank and re-run with `resolve_in_doubt` (`written:<id>` or `not-written`). Re-run finding `in_doubt` set and no resolution stops for that sample with same message. For sample record only, uploader resolves doubt itself by package-scoped search.
- Pre-flight runs first; misses without `allow_unresolved`: nothing written, report carries misses. With it, unresolved `*Name` fields dropped from outgoing record, listed in `messages`.
- Journal with different hash: `Refused`, message says data changed since upload began, names `new_datapoint`; with `new_datapoint` old journal renamed `<name>.<UTC time>.superseded.json`, fresh upload starts.
- `cancelled()` asked between records; cancel → `Stopped` with journal as it stands.
- Failure before data point exists: `Stopped`. After: remaining sub-records still attempted, status `Partial` with `missing` listed.

- [ ] **Step 1: Write the failing tests** (fake service; submissions from Task 2 fixtures; temporary journal directory):
  - `Journal.RoundTripAndAtomicSave`, `Journal.FileNameEncodesTheSample` (`A/B`, `Café`, `..`).
  - `Uploader.WritesEveryRecordInOrder`: fake's request log authenticate-free, in spec's order; store holds one sample, one data point, the aliquots, one measurement per analysis, one age calc, one age summary, each carrying data point's id.
  - `Uploader.DataPointGoesInTheWrapper`, `Uploader.SampleSearchIsScopedToThePackage` (same-named sample in another package not reused), `Uploader.ReusesAnExistingSampleInThePackage`.
  - `Uploader.NamesAreResolvedToIdsAndNamesAreNotSent`.
  - `Uploader.StopsBeforeAnyWriteOnMisses`, `Uploader.AllowUnresolvedDropsThoseFields`.
  - `Uploader.AnUnwritablePackageStopsWithTheSentence`.
  - `Uploader.FailureAtEachCallThenRerunPostsOnlyTheRemainder`: parameterised over every POST index; after re-run store equals clean upload's, no record twice.
  - `Uploader.ATimeoutAfterSendIsInDoubtAndNothingMoreIsSent` (`drop_next`), `Uploader.RerunWithoutAResolutionStops`, `Uploader.ResolvedAsWrittenContinuesWithThatId`, `Uploader.ResolvedAsNotWrittenPostsAgain`, `Uploader.ASampleInDoubtIsResolvedBySearch`. (Review Focus 1)
  - `Uploader.AChangedSubmissionIsRefused`, `Uploader.NewDatapointStartsAfresh`.
  - `Uploader.CancelStopsBetweenRecordsAndARerunFinishes`, `Uploader.ReportsProgress` (done/total monotone, total = number of POSTs planned). (Review Focus 5)
  - `Uploader.TokenExpiryMidRunIsInvisible` (`expire_tokens()` between records).
  - `Uploader.TwoSamplesOneFailsTheOtherCompletes`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Journal\.|Uploader\.'`. Expected: build fails.
- [ ] **Step 3: Implement**; amend spec's 5.3 with in-doubt rule.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R earthbank --repeat until-fail:3`, and earthbank tests once in sanitizer tree. Expected: PASS.
- [ ] **Step 5: Commit** `feat(earthbank): upload in order, resumable from a journal`.

---

### Task 11: `elctl earthbank upload`

**Files:**
- Modify: `apps/elctl/src/earthbank.cpp`, `apps/elctl/tests/test_earthbank_commands.cpp`, `docs/user/08-elctl-reference.md`

**Interfaces:**
- `elctl earthbank upload --db <url> [selection] [--profile <name>] [--package <id>] [--overrides <file>] [--allow-unresolved] [--new-datapoint] [--in-doubt written:<id>|not-written] [--journal <dir>] --yes [options]`.
- Before anything sent, prints: account's name and login, package's name and id, per sample records to write and preferred age, misses if any. Without `--yes` stops there, exit 2, line `nothing was sent; add --yes to upload`. Misses and no `--allow-unresolved`: exit 1, nothing sent.
- Progress: one line per record written. Result: per sample status and data point id; for `Partial`, `InDoubt`, `Refused` the message and exact command line to continue.
- Exit: 0 every sample `Complete`; 1 nothing matched, or misses; 2 usage, sign-in failure, or any sample not `Complete`.
- Password from secret store, else `EARTHBANK_PASSWORD`, else prompt on terminal, else exit 2.

- [ ] **Step 1: Write the failing tests** (fake, memory store, store fixture):
  - `ElctlEarthbank.UploadWithoutYesSendsNothingAndSaysWhatItWould`.
  - `ElctlEarthbank.UploadWritesAndPrintsTheIds`.
  - `ElctlEarthbank.UploadStopsOnMissesWithExitOne`, `.AllowUnresolvedProceeds`.
  - `ElctlEarthbank.UploadWithNoPackageSaysHowToSetOne`.
  - `ElctlEarthbank.APartialUploadPrintsTheCommandToFinish`, and running that command finishes it.
  - `ElctlEarthbank.InDoubtPrintsTheTwoWaysToContinue`.
  - `ElctlEarthbank.NoStoredPasswordAndNoTerminalExitsTwoHavingSentNothing`, `.AWrongStoredPasswordSaysToLoginAgain`. (Review Focus 4)
  - `ElctlEarthbank.NoPasswordOrTokenInAnyOutput` over every upload test's captured output.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R ElctlEarthbank`. Expected: FAIL (unknown subcommand).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same, then both full suites. Expected: PASS.
- [ ] **Step 5: Commit** `feat(elctl): earthbank upload`. Parity with legacy on command line.

---

### Task 12: Preferences page

**Files:**
- Create: `apps/pychron-ui/src/earthbank_worker.hpp`, `earthbank_worker.cpp`, `earthbank_preferences_page.hpp`, `earthbank_preferences_page.cpp`, `earthbank_package_dialog.hpp`, `earthbank_package_dialog.cpp`, `tests/ui/test_earthbank_preferences.cpp`
- Modify: `apps/pychron-ui/src/preferences_dialog.cpp` (add page), `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt`

**Interfaces:**
- `EarthbankWorker`: `QObject` on own `QThread` owning `IHttpClient`, `Session`, `ISecretStore`; slots `test(Profile, password)`, `listPackages(Profile, password)`, `createPackage(Profile, password, name)`, later `run(...)` (Task 13); signals with results and plain-sentence errors. Constructed with factories so tests pass `FakeEarthBank` and memory store. Requests queued, run one at a time.
- Page: table of profiles (name, base URL, username, institution id, data package id, active); Add, Remove, Test, Select / Create Package; password typed into field never read back into table, written to secret store on Test success or on Apply; line shows store in use (`Passwords are kept in: keychain`, or `not stored: this session only`).
- Package dialog: institution's packages (name, id, state), filter box, name field with Create; OK gives id.
- Applying writes `profiles.toml` via `save_profiles`; UI and `elctl` read same file.

- [ ] **Step 1: Write the failing tests** (Qt Test, offscreen, fake service):
  - `addingAProfileAndApplyingWritesTheFile`, `thePasswordIsNotInTheFileAndIsInTheStore`, `testShowsTheAccountsName`, `testWithAWrongPasswordShowsTheSentenceAndStoresNothing`, `selectingAPackageSetsItsId`, `creatingAPackageSelectsIt`, `removingAProfileErasesItsPassword`, `theUiStaysResponsiveWhileARequestIsOutstanding` (fake delays reply; event loop processes a timer meanwhile).
- [ ] **Step 2: Run** `QT_QPA_PLATFORM=offscreen ctest --test-dir build/dev-ui -R earthbank_preferences`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same, then UI suite. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): EarthBank profiles in Preferences`.

---

### Task 13: Publish dialog

**Files:**
- Create: `apps/pychron-ui/src/earthbank_publish_dialog.hpp`, `earthbank_publish_dialog.cpp`, `tests/ui/test_earthbank_publish.cpp`
- Modify: `apps/pychron-ui/src/data_browser_window.cpp/.hpp` (action **Publish to EarthBank...** beside report export, enabled with a selection), `apps/pychron-ui/src/processing_bridge.*` if browser needs way to hand over selected analyses reduced, `earthbank_worker.*` (`run`), `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt`

**Interfaces:**
- Dialog given selected analyses, builds submissions with browser's current reduction settings.
- Controls: Action (Upload / Export xlsx / Pre-flight); Profile and Package with Select / Create Package for this run; table, one row per sample: analyses, Interpretation (editable, vocabulary entries offered once loaded), Preferred age (choice showing each candidate's age and error, rule's pick marked), Flux monitor (editable, found one shown), notes; Decay constant and Air ratio names; Export: folder, one workbook per sample, include samples; Upload: Allow unresolved terms.
- Run: Pre-flight shows misses in table rows they belong to. Export writes, offers to show folder. Upload first shows confirmation (account, package, record counts), proceeds on Yes; progress per record; Cancel stops between records; result pane lists per sample what was written with ids, what was not and why, and for `InDoubt` the two choices as buttons (`It is there, id: [ ]` / `It is not there`) which re-run with the resolution.
- Last interpretation, preferred age, flux monitor per sample name remembered in UI's settings.
- Disagreeing sample (Task 2) shown as row with reason, excluded from run; others proceed.

- [ ] **Step 1: Write the failing tests** (Qt Test, offscreen, fake service, UI fixtures' store):
  - `theActionIsEnabledOnlyWithASelection`, `oneRowPerSampleWithCandidatesAndTheRulesPick`, `preflightMarksTheRowsWithMisses`, `exportWritesTheWorkbooks`, `uploadAsksFirstAndWritesOnYes`, `uploadWritesNothingOnNo`, `cancelStopsAndARerunFinishes`, `anInDoubtRecordOffersBothResolutions`, `aDisagreeingSampleIsShownAndLeftOut`, `choicesAreRememberedPerSample`, `noPasswordOrTokenAppearsInTheResultPane`, `manySamplesShowProgress` (300 samples built in test: progress signal arrives more than once, dialog stays responsive: Review Focus 5).
- [ ] **Step 2: Run** `QT_QPA_PLATFORM=offscreen ctest --test-dir build/dev-ui -R earthbank_publish`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** same, then UI suite. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): publish the selected analyses to EarthBank`.

---

### Task 14: Guide, rules, live check

**Files:**
- Create: `docs/earthbank.md`, `tests/earthbank/test_live.cpp`
- Modify: `AGENTS.md`, `README.md` and `docs/user/README.md` (list guide), `docs/user/08-elctl-reference.md`, `docs/superpowers/specs/2026-10-07-earthbank-design.md`

- [ ] **Step 1: Write `docs/earthbank.md`** for lab user, style of `docs/export.md`, from legacy `UPLOAD_GUIDE.md` and what was built: what EarthBank and data package are; needs (account, write access, institution id); one-time setup in Preferences and with `elctl earthbank login`; publishing from data browser, step by step; same with `elctl`; pre-flight; export to workbooks, importing on website; what is sent per sample, where each value comes from (spec 4.2 table, reader's words); what is left empty and why; choosing preferred age; constants and monitor names; when something goes wrong (sentences, meaning, what to do, incl. "in doubt"); resuming; where profile file and journal live; passwords in system's store. Then maintainer section: live-test runbook (below), replacing fake's fixtures with captures, swapping in newer vendor template.
- [ ] **Step 2: The live test** `tests/earthbank/test_live.cpp`, built only when Qt Network is, skipped unless `PYCHRON_TEST_EARTHBANK_URL`, `_USER`, `_PASSWORD`, `_INSTITUTION` all set: creates package `pychron-test-<UTC date-time>`, uploads the two fixture samples, reads data point and its measurements back, compares counts and three values, runs upload again (nothing new written), then deletes package in guard that runs on failure too. Prints package's name and id first, so person can remove by hand if process dies. Excluded from default `ctest` run by label (`LABELS live`); `ctest -LE live` is what AGENTS.md documents; confirm default run in CI configuration does not pick it up without variables (it skips), say which.
- [ ] **Step 3: Add to `AGENTS.md`** under "Build and test", one bullet: `libs/earthbank` is Qt-free and has no network code (`IHttpClient`; `libs/earthbank_qt` is the adapter, built when Qt6 Network is found); one `Submission` per sample feeds pre-flight, workbooks and upload; record field names are service's verbatim; no test reaches EarthBank (`FakeEarthBank`), live test opt-in by environment, never in CI; no password or token in files, logs or output; uploader journals before and after each write, never guesses outcome of unanswered one; guide is `docs/earthbank.md`.
- [ ] **Step 4: Bring the spec in line** with what was built, section by section; `Status: Implemented`; move what remains of section 11 to a "Not yet" section.
- [ ] **Step 5: Verify.** Both trees built, both full suites; sanitizer tree's full suite; `ctest --test-dir build/dev -R 'earthbank|Earthbank|Ooxml|Xlsx|Mapping|Vocab|Model\.' --repeat until-fail:5`; `grep -rnI "ausgeochem.auscope.org.au" libs apps tests | grep -v "profiles.hpp\|docs"` prints nothing; `git grep -nI -i "id_token\|bearer " -- tests/earthbank/fixtures` shows only fake's opaque placeholders.
- [ ] **Step 6: One live run, by the owner.** Real credentials: `elctl earthbank login`, `packages --create`, `preflight` on real sample (correct `vocab.cpp` where name differs, add case to its test), `export-xlsx` and import of workbook through website into test package, `upload` of same sample into second test package, compare the two on EarthBank, then delete both packages. Record outcome in `docs/earthbank.md`'s maintainer section (date, what differed, what was fixed).
- [ ] **Step 7: Commit** `docs: publishing to EarthBank, for users and maintainers`.
- [ ] **Step 8: Land, when the owner says so.** Rebase on `origin/develop`, rerun Step 5, merge into `develop`. No pull request.

## Decisions the owner must make outside the code

Do not block Tasks 1 to 13:

1. Settled on 2026-10-12: the owner decided that the two template workbooks ship with pychron's installers. They are installed by `cmake/PychronInstall.cmake`, and the fallback to a folder the user fills once was not needed.
2. Read API licence linked from legacy's README for any condition on clients (identification, rate limits).
3. Run capture step of Task 6 and live run of Task 14 Step 6.
