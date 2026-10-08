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

Not in scope: populating `funding` and `literature` (legacy leaves them
empty too without the user's input) and the two `Props` sheets (legacy
filled those automatically with `pychron.*` metadata; this work leaves them
empty, sections 4.5 and 4.6); grain size, which pychron-cpp's `Analysis`
does not carry (section 4.5); disciplines other than
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

enum class AgeKind { Plateau, WeightedMean, Isochron, Integrated };   // vocab.hpp
using PreferredAge = AgeKind;

struct SampleOptions {              // per sample; all optional
  std::string interpretation;       // EarthBank LArArInterpretation name; empty: "Unknown"
  std::optional<PreferredAge> preferred_age;   // empty: section 4.4's rule
  std::string flux_monitor;         // overrides the monitor found on the analyses
  std::string datapoint_name;       // empty: "<sample> <material>", or "<sample>"
  std::string irradiation;          // the irradiation batch as EarthBank names it; empty: the analyses'
};

struct PublishOptions {
  processing::ReductionSettings reduction;      // as the report
  processing::ReportOptions ages;               // plateau criteria, error kind, York method
  bool age_includes_j_error = true; // the reported age's uncertainty with J (4.4)
  std::string decay_constant;       // EarthBank name or a citation; empty: from the value used (4.3)
  std::string air_ratio;            // likewise
  std::string setup;                // the lab's analytical set-up as EarthBank names it; no default
  std::map<std::string, SampleOptions> samples; // by sample name
  std::string software = "pychron-cpp";         // name and version, for the two descriptions
};

Result<std::vector<Submission>> build_submissions(const std::vector<processing::AnalysisPtr>& analyses,
                                                  const PublishOptions& options);
}
```

Analyses are grouped by `Analysis::sample`; one `Submission` per sample, as
**[legacy]** ("mixed-sample selections produce one ArArDataPoint per sample"),
in order of sample name. Analyses tagged invalid are left out by the caller,
as in `elctl export`: every analysis given is used.

An analysis is published when it has an age and measured all five argon
isotopes: 40Ar, 39Ar, 38Ar, 37Ar and 36Ar. `reduce_analysis` puts a zero in
the place of an isotope that was not measured and goes on, and for three of
the five it gives an age all the same: without 36Ar as if there were no
atmospheric argon, without 37Ar with no Ca correction of 36Ar and 39Ar (for
a Ca-rich phase the age is wrong by percent), without 38Ar with no chlorine
correction of 36Ar. Nothing on a published record would say so, so an
analysis lacking any of the five is left out whole; none is published with
some of its fields unset instead. What is left out is counted in
`Submission::notes` (`1 analysis of 6 left out, not having all five argon
isotopes measured: 20002-03 (Ar37 not measured)`; analyses without an age
have their own note, with the reduction's reason), and a sample with nothing
left is refused, naming each analysis and what it lacks.

`age_includes_j_error` is a decision for the owner of the published data:
the service has one uncertainty per age and no field that says what is in
it. It defaults to `true` (the report's "with J" column); either way the age
summary's description says which (4.4).

`setup` and `irradiation` name two things that must already exist on the
service: the laboratory's analytical set-up and the irradiation batch. Every
data point refers to one of each, and the workbook's columns for them
(`arArSetUp`, `irradiationBatchID`) are marked Required. Nothing on an
analysis says what EarthBank calls the set-up, so `setup` has no default and
is the same for every sample of a build. The irradiation is
`SampleOptions::irradiation` when given, else the one irradiation the
sample's published analyses name (`Analysis::irradiation`; an analysis that
names none is passed over). A sample can span irradiations and the record
has room for one: analyses that name different ones are not refused; the
name is left empty and a note gives two of them with their run ids. A
sample none of whose analyses names an irradiation gets a note too, and
every submission one when `setup` is empty; each says which setting names
the thing. Both names are taken as given: whether the service knows them is for pre-flight
(section 5) or, in a workbook, for the importer.

`software` is filled by the apps with the name and version; it goes into
`Sample.description` (`Uploaded from <software>`) and into the age summary's
description (`reduced with <software>`). Left empty it reads `pychron-cpp`
in both.

### 4.2 Records and where each value comes from

`Submission { std::string datapoint_name; std::string setup_name; std::string
irradiation_name; Sample sample; std::optional<Location> location; DataPoint
data_point; std::vector<Aliquot> aliquots; std::vector<Measurement>
measurements; AgeCalc age_calc; AgeSummary age_summary;
std::vector<std::string> notes; }`. `setup_name` and `irradiation_name` (4.1)
are not fields of the Ar-Ar record, which has only their ids: an upload
sends them beside the record (`arArSetUpName`, `arArIrradiationName`
**[legacy]**), the workbook has a column for each. They are part of the
submission's content hash; the notes are not. `datapoint_name` is the key
that ties a workbook's rows together and the name of the umbrella record:
`SampleOptions::datapoint_name`, else `<sample> <material>`, else `<sample>`;
two samples with the same one are refused.

Each record is a struct of optional fields named as the service names them
(`ar40Corr`, `apparentAgeUncertainty`, ...); the full lists are legacy's
`DATA_POINT_FIELDS`, `MEASUREMENT_FIELDS`, `ALIQUOT_FIELDS`,
`AGE_SUMMARY_FIELDS`, `AGE_CALC_FIELDS`, `SAMPLE_FIELDS`, `LOCATION_FIELDS`
**[legacy]** and are copied into `model.hpp` verbatim. A controlled term is
kept as its name (`mineralName`); its id is resolved later (section 5).

What a field means is taken from the service's own definitions: the `Field
Definitions` sheet of its Ar-Ar template
(`ArArDataPoint_Template2026.xlsx`). Where legacy's value and that definition
disagree the definition wins; every such case is in 4.6.

Below, "corrected" is `reduction::ArArResult::corrected` (blank,
discrimination, detector factor, abundance sensitivity, and the decay of
39Ar and 37Ar: `Stage::DecayCorrected`), and `f` is `ArArResult::f`.

| Record | Field(s) | From |
|---|---|---|
| Sample | `name`, `sampleID` | `Analysis::sample` |
| | `materialName` | `Analysis::material` |
| | `referenceElevation`, `igsn` | `SampleInfo` |
| | `stratographicUnitName` | `SampleInfo::unit` |
| | `description` | `"Uploaded from <PublishOptions::software>"`; `"Uploaded from pychron-cpp"` when that is empty |
| Location | `lat`, `lon` | `SampleInfo`: the latitude and longitude one analysis carries together. The analyses with such a pair must carry the same one; a lone coordinate that is not the pair's is refused; halves on different analyses are no location, with a note |
| DataPoint | `analysisDate` | earliest published analysis, UTC `YYYY-MM-DD`; the timestamp is cut to its second, not rounded |
| | `analysisUnits` | `"fA"` |
| | `arMethodName` | section 4.3 |
| | `mineralName` | `Analysis::material` |
| | `lithologyName` | `SampleInfo::lithology` |
| | `analyticalUncertaintyTypeName`, `apparentAgeUncertaintyTypeName` | `"1 sigma"`: they describe the measurements' own values |
| | the three uncertainty unit names | `"abs."` |
| Aliquot | `aliquotName` | `<identifier>-<aliquot, two digits>`, one per loaded piece |
| | `aliquotSize`, `aliquotUnits` | `ExtractionInfo::weight` where known and above zero, `"mg"`; two weights for one aliquot are refused |
| Measurement | `ar40Corr`, `ar39Corr`, `ar37Corr` and uncertainty | `Stage::InterferenceCorrected`: 40Ar less the K-derived 40Ar (= 40Ar* + trapped 40Ar), 39ArK, 37Ar decay corrected |
| | `ar36Corr` and uncertainty | `Stage::InterferenceCorrected` (`f.atmospheric.atm36`): 36Ar less the Ca- and Cl-derived 36Ar, and less the cosmogenic 36Ar when the constants carry cosmogenic ratios (`ReductionConstants::cosmogenic`; the split then replaces the atmospheric 36Ar by its non-cosmogenic part). It is the atmospheric (trapped) 36Ar |
| | `ar38Corr` and uncertainty | corrected 38Ar less `f.interference.k38` and `f.interference.ca38`, as `UFloat`s of the one reduction. The atmospheric and chlorine-derived 38Ar stay (and the cosmogenic 38Ar, when there is such a split): 4.6 has the question this raises beside `ar36Corr` |
| | `ar40ExcInt`, `ar39ExcInt`, `ar38ExcInt`, `ar36ExcInt` and uncertainty | `Stage::DecayCorrected` (only 39Ar has a decay in it) |
| | `ar37ExcInt` and uncertainty | `Stage::DecayCorrected` with the decay factor (`ArArResult::decay.df37`, an exact number) divided out: what the other four have, without decay |
| | `ar{36..40}blank` and uncertainty | `Stage::Blank` |
| | `ratio{36,37,38,40}Arx39ArCorr` and uncertainty | the `Corr` value above over `ar39Corr`, divided as `UFloat`s so the uncertainty is the correlated one |
| | `ratio40Arx39ArK` and uncertainty | `f.f` |
| | `radiogenicArgon` | `100 * f.rad40 / ar40Corr`, percent (the column is headed `%40Ar*`); no uncertainty field exists |
| | `ar39KReleased`, `cumulative39ArK` | percent: the step's `f.interference.k39` over the sum over its aliquot's published steps, alone and summed in step order. Absent on a fusion, and for an aliquot one of whose steps is not published (a letter missing from A, B, C, ..., or an analysis left out), with a note naming the steps |
| | `caK`, `kCa` and uncertainty | `ArArResult::cak`, `kca` |
| | `clK` and uncertainty | `ArArResult::clk` |
| | `jValue` and uncertainty | `ReducedAnalysis::j` |
| | `apparentAge` and uncertainty | `ArArResult::ages->age` (no J uncertainty; with the decay constant's when the reduction carries it, 4.4), scaled to Ma |
| | `jvalueUncertaintyTypeName`, `apparentAgeUncertainityTypeName` | `"1 sigma"` |
| | `stepNumber` | `Analysis::increment + 1`; absent with no step |
| | `analysisTime` | `Analysis::timestamp`, UTC `YYYY-MM-DDTHH:MM:SSZ`, cut to the second |
| | `laserPower` + units, or `furnaceTemperature` | `ExtractionInfo` value and units: units containing `C` are a temperature **[legacy]** |
| | `plateauStepName`, `weightedMeanStepName`, `inverseIsoStepName` | the one of the kind of age reported is `"Yes"` on the analyses that age used and `"No"` on the sample's others; the other two are empty, and all three under a total gas age |
| Submission | `setup_name` | `PublishOptions::setup` |
| | `irradiation_name` | `SampleOptions::irradiation`, else the irradiation the published analyses name (4.1) |
| AgeCalc | `decayConstantName`, `airRatioName`, `fluxMonitorName` | section 4.3 |
| AgeSummary | `age`, `ageUncertainty`, `mswd`, `agepvalue`, `nAnalysisGrouped`, `ageCalcTypeName` | section 4.4 |
| | `ageUncertaintyTypeName`, `calculatedAgeUncertaintyTypeName` | `"1 sigma"` |
| | `calculatedAgeUncertaintyTypeUnitsName` | `"abs."` |
| | `description` | always set: the sentence of section 4.4 |
| | `dataInterpretationToolName` | `"Pychron"` **[legacy]** |
| | `interpretationName` | `SampleOptions::interpretation`, else `"Unknown"` |
| | `preferredAge` | `true` |

Ages are in Ma whatever the lab's display unit. Uncertainties are 1 sigma,
absolute. A number that is not finite is not sent and is named in the notes.

Analyses of one sample that disagree about a fact of the sample (material,
lithology, stratigraphic unit, IGSN, location, elevation, flux monitor,
decay constant, atmospheric 40Ar/36Ar, age unit, whether the decay
constant's uncertainty is in the ages, the weight of an aliquot) are refused
with both values and both run ids; an analysis that says nothing about a
fact is passed over.

### 4.3 Vocabulary mapping

In `vocab.hpp`, as data, each with a test. The names are the service's own,
read from the `Lookup Tables` sheet of the Ar-Ar template.

- **Method [legacy].** No analysis with a step: `Total fusion - laser`. With
  steps: the largest extraction value at most 50 gives `Step-heating - laser`,
  at least 300 gives `Step-heating - furnace`, otherwise
  `Step-heating - undefined`.
- **Decay constant and air ratio.** pychron-cpp's constants carry values, not
  citations (`reduction::ReductionConstants`; an analysis's may come from a
  preset or from its reference record), so the name is found from the value
  the reduction used, each matched to one part in 10^4.
  - The total 40K decay constant is the flux's `lambda_k_total` when it is
    set and not exactly 0 +- 0, else `lambda_b + lambda_e`: 5.543e-10 /a is
    `Steiger and Jager, 1977`, 5.463e-10 is `Min et al. 2000`, 5.531e-10 is
    `Renne et al. 2011`. All three of `constants_preset()`'s presets
    (`Default`, `Legacy`, `LegacyPreferences`) carry 5.543e-10 and so map to
    `Steiger and Jager, 1977`. The sheet lists a fourth, `Carter et al.
    2025`, which no value here maps to; it can be given as an override.
  - `atm4036` by value: 298.56 is `Lee et al. 2006` (the `Default` preset),
    295.5 is `Nier 1950` (the two legacy presets).

  A value that matches none leaves the name empty, with a note; pre-flight
  reports it, and `PublishOptions` supplies it. Analyses of one sample that
  disagree are refused with the two values. Legacy's citation normaliser
  (`Min (2008)` to `Min et al. 2000`, and so on) is kept for the override
  strings **[legacy]**.
- **Uncertainty type [legacy].** `SE`, `SEM`, `SD`, `MSEM`, `1 sigma` are id 1;
  `2 sigma` is id 2. Resolved from this table and never from
  `/api/core/l-error-types`, whose ids violate the data point's foreign key.
  The mapping itself only ever writes `1 sigma`.
- **Flux monitor.** `"<MonitorInfo::name> <age> Ma"`, resolved by the fuzzy
  rule of section 5.1 on upload and by the rule of section 6 in a workbook;
  `SampleOptions::flux_monitor` overrides.
- **Fixed terms.** `fA`, `abs.`, `1 sigma`, `Yes`, `No` (the sheet's `Yes/No`
  list also has `N/A`, which is never written: a column that does not apply
  is left empty), `mg`, `Pychron`, `Unknown`.

The names are checked against the live vocabularies in stage 3.

### 4.4 The summary age

pychron-cpp stores no interpreted age; it computes the candidates on demand
(`group_ages.hpp`, `arar_groups.hpp`), the same calls the publication report
makes. The submission reports one, the preferred age:

- `SampleOptions::preferred_age` when given. One that cannot be computed is
  refused, never replaced by another;
- otherwise, for a step-heating sample, the plateau if one exists under
  `PublishOptions::ages.plateau`, else the integrated age; for a fusion
  sample, the weighted mean.

`ageCalcTypeName` is `Plateau Age`, `Weighted Mean Age`, `Inverse Isochron
Age` or `Total Gas Age` (the sheet's names, checked by pre-flight like any
other term). `mswd` and `agepvalue` are left out when fewer than two analyses
entered, and for a total gas age. The choice and the reason are written to
`Submission::notes` and shown to the user before anything is sent.

**Several step-heated aliquots.** Such a sample has no default: without a
`preferred_age` it is refused, with a message that names the aliquots and
says to set the sample's `preferred_age` setting to `weighted_mean`,
`integrated` or `isochron`, or to publish the aliquots as separate selections. With one of
those three chosen the age pools the aliquots (the mean of all their steps'
ages, the gas of all their steps, or one inverse isochron fitted to all
their steps), and the note of the choice (`chosen in the options; the age
pools 2 step-heated aliquots (20001-01, 20001-02)`) and the description
(`..., pooling 2 step-heated aliquots ...`) say so. An isochron is pooled
only when asked for, never by default; the J rule below still holds for it.
A plateau is of one heating experiment and stays refused for such a sample,
with the same advice.

**J.** A published age is made with one J or says how it is not.

- An integrated or isochron age is one pass through the age equation. Over
  analyses whose J differ (value or uncertainty, compared exactly) it is
  refused, naming the sample, the kind and two of the J with their run ids;
  it is never made with the first analysis's J. This holds for the rule's
  own fall back to the integrated age.
- A plateau and a weighted mean are means of ages each made with its own
  analysis's J. With `age_includes_j_error` the relative J uncertainty is
  added in quadrature (`with_external_error`), and it is that of the analyses
  the mean is of, not of the sample's other analyses. When those analyses do
  not share one J, the term is the largest relative J uncertainty among them
  (the J uncertainty of their mean cannot exceed it, however the positions
  are weighted or correlated), and the description and a note say so.

**What the uncertainty is.** `ageUncertainty` is one sigma: with the J
uncertainty when `age_includes_j_error` (the default), without it otherwise.
The record's uncertainty type is only a sigma level, so
`AgeSummary.description` is always set and states the rest, in one sentence
that is also in the notes:

`<age type> of <n> of <N> analyses[, pooling <k> step-heated aliquots][ (<facts>)];
uncertainty 1 sigma (<what>), <J>[, decay constant uncertainty included];
reduced with <software>.`

- `n` the analyses the age used, `N` the analyses published;
- facts: for a plateau, `steps C to H, 87.3% of the 39ArK`; for an isochron,
  `trapped 40Ar/36Ar 298.2 +/- 1.4` (one over the fit's 36Ar/40Ar intercept);
- what: `the mean's msem` (or `sem`, `sd`: `ReportOptions::mean_error` as
  pychron-cpp names it) for a plateau and a weighted mean; `propagated
  through the age equation` for a total gas age; `propagated through the
  regression and the age equation` for an isochron;
- J: `J uncertainty included` (followed, for a mean over analyses of several
  J, by `(the largest relative J uncertainty among its analyses, <p>% of
  <run id>)`), or `analytical uncertainty only`;
- the decay constant: when the reduction carries its uncertainty into the
  ages (`ReductionSettings::include_decay_error`, or an analysis's own
  constants with `include_decay_error`), it is in every apparent age and in
  all four kinds of summary age, with J or without, and the sentence adds
  `decay constant uncertainty included`. The J part without J then reads `J
  uncertainty not included`: no description says `analytical uncertainty
  only` of an uncertainty that holds the decay constant's. Analyses of one
  sample that disagree on it are refused (4.2).

### 4.5 What is left empty

| Field | Why |
|---|---|
| `grainDiameterMin/Max` | `Analysis` has no grain size |
| `beamSize`, `beamSizeUnits` | **open question for the owner.** `ExtractionInfo::beam_diameter` is recorded without a unit: legacy labels it `microns`, a queue may give mm. A size a thousand times off is worse than none, so it is not sent until the unit is settled |
| `kContent`, `kContentUncertainty` | not computed (the definition: "relevant for K-Ar age only") |
| `ratio40Arx36ArTrapped` | a field of the measurement, and the only trapped ratio computed here is a group's isochron intercept, which goes into the age summary's description |
| `analysisScaleName` | nothing records whether an aliquot is one grain or many |
| `commentAnalyte`, `measurementComment`, `aliquotComment`, AgeCalc `comment`, `locationName`, Location `description` | no counterpart that is the thing the field asks for |
| `fundingName`, `literatureName` | as legacy leaves them without the user's input |
| the sheets `Datapoint Props` and `Sample Props` | free name-and-value rows. Legacy filled both automatically with `pychron.*` metadata (`build_datapoint_props`, `build_sample_props`, `earthbank_service.py:1164-1242`): on the data point the first analysis's experiment and analysis type, irradiation, level and position, extract device, monitor name, material and age, the three citations of the constants, the number of analyses, the preferred age's kind and error kind, MSWD and p-value; on the sample the project, principal investigator, repository, material, lithology, grain size and identifier. This mapping writes neither sheet: nothing in a `Submission` holds such rows. What of it has a field of its own is sent there (4.2); the rest is not sent (4.6) |
| `archiveName`, `sampleKindName`, `sampleMethodName`, collection dates, rock unit ages, relative elevations, `locationKindName`, `latLonPrecision` | not in the catalog |
| every `id` and `*Id` | resolved later (section 5) |

Each is a field of the record already; filling one later is a change to the
mapping and its table row, nothing else.

### 4.6 Departures from legacy, for the owner to confirm

Each row is a field where this mapping sends something other than what the
legacy integration sent. "Legacy" is `earthbank_service.py`'s
`build_*_payload`; what its accessors (`get_interference_corrected_value`
and the like) compute is not in the reference and is given as the mapping
task's report found it in upstream pychron, marked *(report)*. The
definition is quoted from the `Field Definitions` sheet of
`ArArDataPoint_Template2026.xlsx`, column B, with its cell. Data other labs
uploaded with legacy will not be comparable in these columns.

| Field | Legacy sent | This mapping sends | The service's definition | Cell |
|---|---|---|---|---|
| `ar38Corr` and uncertainty | `iso.get_interference_corrected_value()` of 38Ar: the corrected 38Ar with no interference removed *(report)* | corrected 38Ar less the K- and Ca-derived 38Ar; atmospheric and chlorine-derived 38Ar stay | "38Ar corrected for mass discrimination and interferences" | B42 |
| `ratio38Arx39ArCorr` and uncertainty | `get_ratio("Ar38/Ar39")`: detector-corrected intensities, neither decay nor interference corrected *(report)* | `ar38Corr` / `ar39Corr` | "38Ar/39Ar corrected for mass discrimination and interferences" | B90 |
| `ratio36Arx39ArCorr`, `ratio37Arx39ArCorr`, `ratio40Arx39ArCorr` and uncertainties | `get_ratio("ArNN/Ar39")`, as above *(report)* | the `Corr` value over `ar39Corr` (39ArK), uncertainty correlated | "36Ar/39Ar corrected for mass discrimination and interferences" (and the same of 37Ar, 40Ar) | B86, B88, B92 |
| `radiogenicArgon` | `analysis.radiogenic_yield`: `100 * 40Ar* / all the 40Ar` *(report)* | `100 * 40Ar* / ar40Corr`, where `ar40Corr` = 40Ar* + trapped 40Ar (the K-derived 40Ar is not in the denominator) | "Radiogenic 40Ar (40Ar*) expressed as a proportion of total 40Arxcorr (40Arxcorr – 40Artrapped). By convention calculated assuming trapped argon is of atmospheric composition."; B38: "40Ar corrected for mass discrimination and interferences (=40Ar* + 40Artrapped)."; the column is headed "Radiogenic argon proportion (%40Ar*)" | B74 (B38) |
| `ar40ExcInt`, `ar38ExcInt`, `ar36ExcInt` and uncertainties | `iso.get_non_detector_corrected_value()`: baseline and blank corrected only, no discrimination, no detector factor *(report)* | blank, discrimination, detector factor and abundance sensitivity applied | "40Ar corrected for mass discrimination only" (and the same of 38Ar, 36Ar) | B64, B68, B72 |
| `ar39ExcInt` and uncertainty | as above: no discrimination, no decay *(report)* | the same stage, which for 39Ar has the decay since irradiation | "39Ar corrected for mass discrimination and decay only" | B66 |
| `ar37ExcInt` and uncertainty | as above *(report)* | the same stage with 37Ar's decay taken out again: no decay | "37Ar corrected for mass discrimination only" | B70 |
| `ar39KReleased` | `k39`, the step's 39ArK in fA | percent of the 39ArK of the step's aliquot; absent on a fusion and for an aliquot with a step not published | "39Ar released in given heating step compared to total 39Ar released across entire step-heating experiment, expressed as a percentage of the total 39Ar" | B77 |
| `cumulative39ArK` | not set | the running sum of the above, percent | "39Ar released up to and including a given heating step compared to total 39Ar released across entire step-heating experiment, expressed as a cumulative percentage of the total 39Ar" | B78 |
| `plateauStepName` | `Yes` on a step with `is_plateau_step`, else not set | when the reported age is the plateau: `Yes` on its steps, `No` on the sample's other analyses; not set otherwise | "Is this step included in plateau age calculation (Y/N/NA)? N/A applies if no plateau could be generated" | B83 |
| `inverseIsoStepName`, `weightedMeanStepName` | never set | `Yes` / `No` likewise when the reported age is the isochron or the weighted mean | "Is this step included in inverse isochron calculation (Y/N/NA)? N/A applies if no inverse isochron could be generated"; "Is this step included in Weighted Mean calculation (Y/N/NA)? N/A applies if no inverse isochron could be generated" (sic) | B84, B85 |
| DataPoint `analyticalUncertaintyTypeName`, `apparentAgeUncertaintyTypeName` | the group's `age_error_kind` (`SD`, `SEM`, `MSEM`) | `1 sigma` | "Analytical uncertainty type quoted for measurements and associated ratios"; "Analytical uncertainty type quoted for ages" | B21, B23 |
| AgeSummary `ageUncertaintyTypeName`, `calculatedAgeUncertaintyTypeName` | the preferred value's `error_kind` | `1 sigma`; the error kind is in the description | "Uncertainty type associated with the age of the group/population (e.g. 1s, 2s)." | B105 |
| AgeSummary `ageUncertainty` | the uncertainty of the group's preferred age as legacy held it; whether J is in it is not determinable from the reference (the report: legacy quotes a mean with J by default and an integrated age without) | one sigma with the J uncertainty for all four kinds (`age_includes_j_error`, default `true`), stated in the description | "Uncertainty of age of group/population (Ma)" | B104 |
| AgeSummary `description` | `Sample <sample>; Project <project>; <comments>` | the sentence of 4.4: what the age is of and what its uncertainty holds | "Additional information about age group/population and age calculation" | B111 |
| AgeSummary: which age, and over what | the analysis group's preferred age, as set in legacy's interface | the rule of 4.4; refused for several step-heated aliquots without a choice (a plateau for them always), and for an integrated or isochron age over analyses of several J | "Type of method used to calculate age of group/population e.g., plateau age, inverse isochron, weighted mean, total fusion. Note that multiple ages can be submitted for one sample and then one can be selected as the 'preferred' age" | B102 |
| `ageCalcTypeName` | the preferred value's `computed_kind` or `kind`, as pychron names it | the sheet's `Age Type` names: `Plateau Age`, `Weighted Mean Age`, `Inverse Isochron Age`, `Total Gas Age` | as above; `Lookup Tables` column H | B102 |
| Measurements sent | one per analysis of the group, whatever it measured | one per analysis that has an age and measured all five of 40Ar, 39Ar, 38Ar, 37Ar and 36Ar (4.1); the others are left out and counted | (the definitions of the fields concerned) | |
| `aliquotName` (Aliquot and Measurement) | `<labnumber>-<aliquot><step>`: one aliquot per heating step | `<identifier>-<aliquot>`: one aliquot per loaded piece, its steps told apart by `stepNumber` | "Aliquot name (e.g. Grain1, Aliquot2). Must be unique. Can be left blank if only a single analysis has been performed per sample." | B30 |
| `aliquotSize`, `aliquotUnits` | the analysis's `weight`, `mg` | the same where the weight is above zero; two weights for one aliquot are refused | "Either number of grains or sample weight"; "mg, g or grain/s" | B97, B98 |
| `beamSize`, `beamSizeUnits` | `beam_diameter`, `microns` | not sent (4.5) | "Diameter of laser spot used (where applicable)"; "Beam size units (e.g., mm, microns)" | B33, B34 |
| `kContent`, `kContentUncertainty` | the analysis's `k2o` | not sent (4.5) | "K content (relevant for K-Ar age only)" | B94 |
| `analysisDate`, `analysisTime` | the first analysis's `rundate` / the analysis's timestamp, formatted from a naive local time (`analysisTime` with a `Z` appended) | UTC; the date is the earliest published analysis's; cut to the second | "Date of data acquisition YYYY-MM-DD"; "e.g. 2023-07-02 14:30 AEST for Sydney time" | B14, B31 |
| `decayConstantName`, `airRatioName` | from the citations on legacy's constants, normalised | from the value the reduction used (4.3); the normaliser serves the overrides | "Reference associated with selected decay constant, selectable from list"; "Reference associated with selected atmospheric 40Ar/36Ar ratio, selectable from list" | B112, B113 |
| Location `lat`, `lon` | each from the first analysis, independently | the pair the analyses agree on (4.2) | (Sample template) | |
| `Sample.description` | the analysis's `sample_note`, else `<sample> (<project>)` | `Uploaded from <software>` | (Sample template) | |
| AgeSummary `nAnalysisGrouped` | `analysis_group.nanalyses`: the count of the group, whatever the preferred age used (`:1103`) | the analyses the reported age used: a plateau's steps, the ages a mean took, an isochron's points, every analysis of a total gas age | "Number of single analysis combined to obtain age of group/population" | B101 |
| AgeSummary `mswd`, `agepvalue` | always, from `get_preferred_mswd_tuple()` (`:1091`, `:1104-1105`) | left out when fewer than two analyses entered the age, and for a total gas age | "Mean squared weighted deviation (MSWD). Statistical indication which assess the degree to which the grouped analyses and uncertainties represent a single population"; "p-value of age (if applicable)" | B107, B108 |
| Measurement `apparentAge` and uncertainty | `analysis.uage` as it stands, in the age unit of legacy's constants (`:1041-1044`; the summary age alone is scaled, `get_ma_scaled_age()`, `:1090`) | Ma, whatever the reduction's age unit | "Apparent age (Ma) for individual heating step" | B81 |
| Measurement `jvalueUncertaintyTypeName`, `apparentAgeUncertainityTypeName` | not set (`build_measurement_payload`, `:983-1076`) | `1 sigma` | the sheet defines only the data point's columns of these names: "Analytical uncertainty type quoted for J Values"; "Analytical uncertainty type quoted for ages" | B25, B23 |
| AgeCalc `fluxMonitorName` | the group's override `eb_flux_monitor`; else `<monitor_name> <monitor_reference>` of the first analysis that has either; else the group's `monitor_info` joined (`_flux_monitor_from_group`, `:2051-2086`) | `SampleOptions::flux_monitor`; else `<MonitorInfo::name> <age> Ma` (or whichever part there is), which the analyses must agree on | "Flux monitor associated with sample, selectable from list" | B114 |
| Sample `igsn`, `stratographicUnitName` | not set (`build_sample_payload`, `:1118-1162`) | `SampleInfo::igsn`, `SampleInfo::unit`, where the catalog has them | Sample template, `Column Descriptions`: "An International Generic Sample Number; to be assigned by the collector, analyst or, in cases where no IGSN exists at the time of data upload, please create using IGSN minting tool on AusGeochem"; "Geological name of the unit from which sample was collected. AusGeochem uses the Geoscience Australia geological unit database for localities within Australia. In cases where samples are from outside Australia, users can manually enter the unit name." | C7, C23 (Sample template) |
| the sheets `Datapoint Props`, `Sample Props` | filled automatically with `pychron.*` name-and-value rows (`build_datapoint_props`, `build_sample_props`, `:1164-1242`; listed in 4.5) | not written. Of what legacy put there, the monitor, the constants, the count, the age's kind, MSWD and p-value, the material and the lithology have fields of their own and are sent in them, and the error kind is in the age summary's description; the experiment and analysis type, irradiation, level and position, extract device, project, principal investigator, repository, grain size and identifier are not sent | (the Ar-Ar template's `Field Definitions` has no row for either sheet) | |
| `ar38Corr` beside `ar36Corr`: chlorine | `get_interference_corrected_value()` of each *(report)* | the two treat chlorine differently. `ar36Corr` has the chlorine-derived 36Ar removed (it is the atmospheric 36Ar); `ar38Corr` keeps the chlorine-derived 38Ar (only the K- and Ca-derived 38Ar are removed). Why: a reader recovers 40Ar* from `ar40Corr` and `ar36Corr` (40Ar* = `ar40Corr` - (40Ar/36Ar)air x `ar36Corr`), which needs `ar36Corr` atmospheric; and recovers the chlorine-derived 38Ar from `ar38Corr` and `ar36Corr` (38ArCl = `ar38Corr` - (38Ar/36Ar)air x `ar36Corr`), which needs it left in `ar38Corr`. With it removed from both, `ar38Corr` would be a fixed multiple of `ar36Corr` and the record would hold nothing of chlorine but `clK`. The definitions do not decide it: the two are worded identically. A question for the EarthBank maintainers (below) | "38Ar corrected for mass discrimination and interferences"; "36Ar corrected for mass discrimination and interferences" | B42, B46 |
| AgeSummary `ageUncertainty`: the J term of a plateau or weighted mean whose analyses do not share one J | not determinable from the reference | the largest relative J uncertainty among the analyses the mean is of (4.4), named in the description. The publication report's "with J" column (`processing::j_relative_error`) instead takes the J of the first analysis of the group that has one, so for such a sample the two numbers differ; with one J they are the same number | "Uncertainty of age of group/population (Ma)" | B104 |
| `analysisScaleName` | `Single Grain` for one analysis, `Multi-Grain Aliquot` for more | not sent (4.5) | "Scale at which analysis was conducted" | B15 |
| `commentAnalyte`, `grainDiameterMin/Max`, AgeCalc `comment` | the sample's notes, its parsed grain size, `<sample> (<project>)` | not sent (4.5) | "Comments relating to analyte (e.g. mineral separation or clearning comments)" (sic); "Minimum grain diameter of analyte (in microns)", "Maximum grain diameter of analyte (in microns)" | B20, B18, B19 |
| The set-up and the irradiation batch | upload: `arArSetUpName` and `arArIrradiationName` beside the record, when the caller gave them (`create_data_point`, `:660-699`; the upload itself never did, `:1429-1434`). Workbook: the record's ids `arArAnalyticalSetUpId` and `irradiationBatchIDId` under `arArSetUp` and `irradiationBatchID` (`SHEET_OVERRIDES`, `xlsx_exporter.py:57-59`) | names in both: `Submission::setup_name` and `irradiation_name` (4.1). An id is never written into a workbook: the columns ask for a term that finds the thing, and a row number read as a term could find another | "Search term to find ArAr SetUp. Must already exist in the system"; "Search term to find Irradiation Batch. Must already exist in the system"; both columns are marked Required | B10, B12 |
| Workbook, `ArArDataPoints.sampleName` | left empty (the data point's payload had no such key) | the sample's name, the same string as `Samples.sampleName` | "Search term to find sample. Usually the sample name assigned by sample collector or IGSN"; marked Required | B9 |
| Workbook, `jValueUncertaintyType` and `jValueUncertaintyUnit` | looked for on the measurement sheet (`SHEET_OVERRIDES` of `ArArMeasurement`: `jvalueUncertaintyType`, `jvalueUncertaintyUnit`), where the 2026 template has no such columns; its columns `jValueUncertaintyType` and `jValueUncertaintyUnit` of the data point sheet matched no field and stayed empty | columns R and S of `ArArDataPoints`, as the 2026 template has them: the unit is the data point's `jvalueUncertaintyUnitName`; the type, which only the measurement record has, is the value the data point's measurements share (left empty, with a note, when they do not share one) | "Analytical uncertainty type quoted for J Values"; "Analytical uncertainty units (i.e., % or absolute)"; both marked Required | B25, B26 |
| Workbook, dates and times | the records' strings as they stood | re-formed as each sheet asks: `analysisDate` `YYYY-MM-DD`; `analysisTime` `YYYY-MM-DD hh:mm UTC`, the seconds cut (the REST route keeps the full `YYYY-MM-DDTHH:MM:SSZ`); `collectDateMin/Max` `dd.mm.yyyy`. `UTC` as the zone word is an assumption (below) | "Date of data acquisition YYYY-MM-DD"; row 2 of `ArArMeasurement`: "YYYY-MM-DD hh:mm z", "e.g. 2023-07-02 14:30 AEST for Sydney time"; row 2 of `Samples`: "Date dd.mm.yyyy" | B14, B31 |
| Workbook, two different samples of one name | each written as its own row | refused: the name is the sample's key on the service, and two rows of one name cannot both be imported. Two data points of one sample (the same record) give one row | "ID of sample, usually assigned by sample collector (Must be unique within whole AusGeochem)" | A1 (Sample template, `Samples`) |
| Workbook, a value in a column that has a list | written as the record holds it | checked against the template's own list (section 6): written in the list's spelling when it is an entry but for case or the spaces around it; a flux monitor found by name and age is written as the list's entry; anything else is written as given and reported. The REST route resolves names on the service instead (5.1) | the lists of `Lookup Tables` and `Lookup tables`, tied to their columns by the sheets' data validations | |
| Workbook, `Samples.locationComment` | not filled | Location `description` (which the mapping does not set today) | "Additional information about the sample location, such as outcrop description or physiographic feature" | C22 (Sample template, `Column Descriptions`) |

**Questions for the EarthBank maintainers.** None blocks the work; each
changes one row above.

- Chlorine in `ar38Corr`. B42 and B46 are worded identically ("corrected for
  mass discrimination and interferences"). Is chlorine-derived 38Ar an
  interference to be removed from `ar38Corr`, as chlorine-derived 36Ar is
  removed from `ar36Corr` here, or does it stay, as here?
- Is `radiogenicArgon` a percent (0 to 100) or a fraction (0 to 1)? B74 says
  "expressed as a proportion"; the column is headed "Radiogenic argon
  proportion (%40Ar*)". Percent is sent.
- Which uncertainty type should an age carry when its uncertainty includes
  the J uncertainty (or the decay constant's)? The list has sigma levels and
  error kinds and nothing for what is in the uncertainty; `1 sigma` is sent
  and the description says the rest.
- Which zone word does the importer take in `analysisTime`? The sheet's
  form is `YYYY-MM-DD hh:mm z` with the example `2023-07-02 14:30 AEST`.
  `UTC` is written (`2023-11-14 08:15 UTC`); if only offsets or a fixed set
  of abbreviations are read, the form changes in one place.
- Are the columns the Ar-Ar template colours as Required all required of
  every row? The colour marks both `laserPower` and `furnaceTemperature`,
  and `stepNumber`, `Ar39KReleased` and `cumulative39ArK`, which a fusion
  has not. The exporter takes it that a row needs a laser power or a furnace
  temperature and not both, and the three step columns only for a step
  heating (section 6), reports every other Required column a row leaves
  empty, and writes the row all the same.
- Is `mineral` on the data point sheet matched case-sensitively against the
  material list? The template gives that column no list (row 2 calls it a
  search term), so pychron's spelling is written there (`sanidine`), while
  `material` on the Sample sheet, which has the list, is written in the
  list's spelling (`Sanidine`).
- Must `stepNumber` be an integer? "Heating step number (if step-heating)"
  (B32). A whole number is sent as a number (1, 2, ...), as legacy sent a
  float (`_step_to_num`, `:1952-1962`).

**Decisions the owner may reverse.** Each is one rule of the mapping with
its tests.

- The J uncertainty is in the reported age's uncertainty by default
  (`PublishOptions::age_includes_j_error`).
- An analysis is published only with all five argon isotopes measured (4.1).
  The alternative, publishing one without 37Ar or 38Ar with the fields of
  that isotope unset, sends an age made without a Ca or a chlorine
  correction.
- A sample with several step-heated aliquots needs an explicit
  `preferred_age` (4.4); the alternative is a default (the integrated age,
  as for one aliquot without a plateau).
- `beamSize` is not sent until the unit of `ExtractionInfo::beam_diameter`
  is confirmed (4.5).
- One aliquot per loaded piece (`<identifier>-<aliquot>`), its steps told
  apart by `stepNumber`, where legacy named one aliquot per step
  (`<labnumber>-<aliquot><step>`, `_aliquot_name`, `:1941-1949`).

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

Four rules that differ from legacy, all because there is no sandbox:

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

- **Never guess an unanswered write.** Before each POST the journal records
  that record as in doubt; the answer clears it. A POST that times out or
  loses its connection may or may not have been written, and the service
  offers no way to ask by name for most records. The sample stops there: the
  user is told which record is in doubt, looks on EarthBank, and re-runs
  saying either that it is there (with its id) or that it is not. Only the
  sample record itself is resolved automatically, by the package-scoped
  search.

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
`apiField` row), fills data rows from row 5 **[legacy]**, and writes the
copy.

```cpp
struct XlsxOptions { std::filesystem::path template_dir; bool per_sample = false; bool samples = true;
                     std::chrono::system_clock::time_point now; };
struct XlsxResult  { std::vector<std::filesystem::path> files; std::vector<std::string> notes; };
Result<XlsxResult> export_xlsx(const std::vector<Submission>&, const std::filesystem::path& out_dir, const XlsxOptions& = {});
Result<std::vector<std::string>> check_against_template(const std::vector<Submission>&, const XlsxOptions& = {});
```

- A column's value is the record's field of the same name, or that name plus
  `Name`, with legacy's per-sheet exceptions (`SHEET_OVERRIDES`) **[legacy]**
  as far as they hold for the 2026 template; where they do not, 4.6 has the
  row. `arArSetUp` and `irradiationBatchID` take the submission's
  `setup_name` and `irradiation_name`. An `id` is never written.
- Rows are linked by `datapointName`, not by id; the website resolves them on
  import **[legacy]**. A submission without a data point name, two with the
  same one, and one whose sample has no name are refused.
- Cells are written as inline strings or numbers, so the workbook's shared
  string table is not rewritten. A number is always a number cell.
- The four header rows of every sheet, and every part of the file the
  exporter does not touch, are byte-for-byte the template's.
- One Ar-Ar workbook for all samples or one per sample; the Sample workbook
  optionally. All the files are written or none: a failure part of the way
  removes those already written and names any it could not remove.
- A newer vendor template is a file swap, provided row 4 still names the
  fields. A field the template has and the records do not is left empty; a
  field the records have and the template lacks is listed in the result.
- The templates are found in the directory `PYCHRON_EARTHBANK_TEMPLATES`
  names, else beside the installed program, else in the source tree when it
  is on this machine. Found nowhere, the error names each place looked in.

**Lists.** The REST route resolves a name on the service (5.1). A workbook
has nobody to ask: the importer gets the text as it was written. But the
template carries the service's lists, in its `Lookup Tables` sheet (`Lookup
tables` in the Sample workbook), and ties a column to its list by a data
validation of the data sheet: in the Sample workbook
`<dataValidation type="list" sqref="E5:E883"><formula1>'Lookup tables'!$C$2:$C$10185</formula1>`,
in the Ar-Ar workbook the same thing in the sheet's extension list
(`x14:dataValidation`, with `xm:f` and `xm:sqref`), and there for row 5
alone, which is taken to mean the column. Neither template defines a name;
a list given by a defined name or as entries in quotes is read as well, and
one that cannot be followed is reported and its column not checked.

| Sheet | Columns with a list | The list (heading in the lookup sheet) |
|---|---|---|
| `ArArDataPoints` | `arArMethod` | `Analytical Method`, D2:D13 |
| | `analysisScale` | `Analysis Scale`, E2:E8 |
| | `analyticalUncertaintyType`, `apparentAgeUncertaintyType`, `jValueUncertaintyType` | `Uncertainty Type`, F2:F21 |
| | `analyticalUncertaintyUnit`, `apparentAgeUncertaintyUnit`, `jValueUncertaintyUnit` | `Uncertainty Unit`, G2:G3 |
| `ArArMeasurement` | `plateauStep`, `inverseIsoStep`, `weightedMeanStep` | `Yes/No`, I2:I4 |
| `ArArAliquot` | none | |
| `ArArAgeSummary` | `dataInterpretationTool` | `Data Interpretation`, J2:J11 |
| | `ageCalcType` | `Age Type`, H2:H9 |
| | `calculatedAgeUncertaintyType` | `Uncertainty Type`, F2:F21 |
| | `calculatedAgeUncertaintyTypeUnits` | `Uncertainty Unit`, G2:G3 |
| | `interpretation` | `Age Interpretation`, K2:K20 |
| `ArArAgeCalc` | `decayConstant` | `Decay Constant`, A2:A5 |
| | `airRatio` | `Air Ratio`, B2:B3 |
| | `fluxMonitor` | `Flux Monitor`, C2:C17 |
| `Samples` | `sampleKind` | `Sample Kind`, A2:A16 |
| | `sampleMethod` | `Sampling Method`, B2:B12 |
| | `material` | `Lithology`, C2:C10185 |
| | `locationKind` | `Location Kind`, D2:D10 |
| | `personRole` (not filled) | `Person Role`, E2:E12 |
| | `lastKnownArchive` | `Last Known Sample Archive`, F2:F28 |

A list is the cells its validation names and those that follow them without
a gap: two lists of the Sample template have grown past their validations
(`Mine Waste` in A17; ten archives in F29:F38, the last of them `New (add
comment)`). Row 1 of a list's column on the lookup sheet is its heading
(`Flux Monitor`, `Lithology`), and the notes name a list by it.

Before a value is written under a column that has a list:

- a value the list has is written as it is (the list's `Renne et al. 2011 `
  ends in a space: without it the value is still that entry);
- a value that is an entry but for the case of its ASCII letters and the
  spaces around it is written as the list spells it, trimmed, and the note
  says what became what, each distinct value once: `Samples.material:
  'sanidine' written as 'Sanidine' (list: Lithology)`. A part of an entry,
  or other spaces inside one, is not the entry. Where two entries differ
  only in case and the value is neither, it is not rewritten;
- the flux monitor only, whose entries are whole citations (`Fish Canyon
  Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)`), is also found
  by name and age. The age is the value's number followed by `Ma`, else its
  last number, and must be the number the entry prints (`28.2010` is
  `28.201`; `28.2` is not): among a monitor's entries the age decides, and a
  value without an age matches nothing. The name is the rest. A small table
  in `vocab.cpp`, built from the list, maps the names pychron labs use to the
  mark of that monitor's entries (`FC-2`, `FC2`, `FCs`, `FCT`, `FCTs`, `Fish
  Canyon` to `FCTs`; `ACs`, `Alder Creek` to `ACs`; `GA-1550`, `GA1550`;
  `MD-2`, `MD2`; `Hb3gr`; `TCs`, `TCR`, `Taylor Creek` to `TCs`; `WA1ms`); a
  name with one of them is looked for among that monitor's entries only, and
  a name with two monitors' matches nothing. A name with none must have
  every one of its words in the entry, one of them of three letters or more
  or the entry's abbreviation in parentheses. Either way a mineral the name
  says must be the entry's: the same tuff has a sanidine and a biotite, and
  the list only one. The mineral words are the list's own, kept as a second
  small table beside the first (`sanidine`, the Taylor Creek entry's
  `Saniditine`, which is sanidine, `biotite`, `hornblende`, `muscovite`),
  compared without case as whole words; a name and an entry that each say
  one must say the same one, so `Fish Canyon biotite 28.201 Ma` and `GA-1550
  sanidine 99.738 Ma` match nothing, and a name that says none (`FC-2 28.201
  Ma`) is held to none. A second material table holds the words a lab writes
  that are not the list's: the short forms of its four minerals (`san`, `bt`,
  `bio`, `hbl`, `ms`, `musc`), which count as those minerals, and materials
  the list has no monitor of (`plagioclase`, `plag`, `feldspar`, `kfeldspar`,
  `kspar`, `spar`, `kfs`, `orthoclase`, `anorthoclase`, `adularia`,
  `microcline`, `amphibole`, `phlogopite`, `sericite`, `glauconite`,
  `alunite`, `jarosite`, `glass`, `groundmass`, `whole rock`, `wholerock`),
  which are no entry's, so `FC-2 plagioclase 28.201 Ma` and `Fish Canyon
  K-spar 28.201 Ma` match nothing. A word of either material table counts
  with an `s` after it too (`biotites` is biotite, `sanidines` sanidine); an
  alias has no plural. A name that holds `FCT-3` or `FCT3`, the Fish Canyon
  biotite's designation, matches nothing either, mineral said or not: `FCT`
  alone would make it the sanidine. One entry left: it is written, and the
  note says what became what (`ArArAgeCalc.fluxMonitor: 'FC-2 28.201 Ma'
  written as 'Fish Canyon ... (28.201 Ma)' (list: Flux Monitor), found by
  its name and age`). Several (`Mt Domedary Biotite 99.44 Ma` is both
  `GA-1550` and `MD-2`): not rewritten, and said;
- anything else is written as it was given and reported, each distinct value
  once, with the list's heading: `ArArAgeCalc.airRatio: 'Mark et al. 2011'
  is not in the template's list 'Air Ratio'; the importer may refuse the
  value`.

A column without a list is written as given, with no note: the data point's
`mineral` and `lithology`, the sample's `unitName`, and the names of the
set-up, the irradiation batch and the sample, which the template calls
search terms. So the data point's `mineral` stays `sanidine` where the
sample's `material` becomes `Sanidine`.

**Required columns.** Both templates colour the title of each column (row 3
of a data sheet) by its obligation and give the legend in the sheet that
describes the columns: `Field Definitions` A3:A5 (`Optional`,
`Recommended`, `Required`) and `Column Descriptions` A1:A3 (`Required
field`, `Highly recomended field`, `Optional`). The exporter finds the
legend's `Required` cell, takes its fill (`FFFAD9D6` in both; read through
the cell's style in `xl/styles.xml`, not written into the code) and treats a
column whose title has that fill as Required. Neither template says
"required" of a column in words, so the colour is the only statement there
is. The columns so marked:

| Sheet | Required |
|---|---|
| `ArArDataPoints` | `datapointName`, `sampleName`, `arArSetUp`, `arArMethod`, `irradiationBatchID`, `analysisScale`, the six uncertainty types and units, `analysisUnits` |
| `ArArMeasurement` | `stepNumber`, `beamSize`, `beamSizeUnits`, `laserPower`, `laserPowerUnits`, `furnaceTemperature`, `ar{40..36}Corr` and uncertainties, `radiogenicArgon`, `ratio40Arx39ArK` and uncertainty, `Ar39KReleased`, `cumulative39ArK`, `jValue`, `jValueUncertainty`, `apparentAge`, `apparentAgeUncertainty` |
| `ArArAliquot` | `aliquotSize`, `aliquotUnits` |
| `ArArAgeSummary` | `age`, `ageUncertainty`, `calculatedAgeUncertaintyType`, `calculatedAgeUncertaintyTypeUnits` |
| `ArArAgeCalc` | `decayConstant`, `airRatio`, `fluxMonitor` |
| `Samples` | `sampleName`, `material`, `latitude`, `longitude` |

The colour is per column and says nothing of when. A few columns are wanted
only of some rows, by what the columns themselves mean, and a row they are
not wanted of is not reported for them (`kConditional` in `xlsx.cpp`):

| Column of `ArArMeasurement` | Wanted of | In the note |
|---|---|---|
| `laserPower`, `laserPowerUnits` | a row that has no `furnaceTemperature` | `... rows that have no furnace temperature` |
| `furnaceTemperature` | a row that has no `laserPower` | `... rows that have no laser power` |
| `stepNumber`, `Ar39KReleased`, `cumulative39ArK` | a row of a step-heating data point: one whose `arArMethod` (the data point's `arMethodName`) begins `Step-heating`, whatever its case | `... rows of a step-heating data point` |

So a row with neither a laser power nor a furnace temperature is missed all
three columns, a fusion is missed no step number, and a data point without
a method is missed the method and not the three columns of a step.

A Required column that a row it is wanted of leaves empty is named in the
notes, once per sheet for the whole export, with its title and the number of
rows: `ArArDataPoints: marked Required by the template's colouring; the
importer may refuse a row that leaves one empty: arArSetUp (Analytical Key)
empty in every row, irradiationBatchID (Irradiation Batch ID) empty in 1 of
2 rows`. The count of a conditional column is of the rows it is wanted of
and says their number and which they are (`stepNumber (Step number) empty
in 2 of the 10 rows of a step-heating data point`, `furnaceTemperature
(Furnace temperature) empty in 1 of the 2 rows that have no laser power`,
`... empty in all 10 rows of a step-heating data point`). The note says what the template marks and what may follow; it does
not order anything, since whether the importer holds a row to the colour is
not known (4.6 has the question), and the row is written all the same. A
template without the legend gets one note saying that empty columns are not
reported for it.

`check_against_template` builds the same workbooks in memory and returns the
notes an export would (or the error it would refuse with), writing nothing:
for `elctl` and the UI to show before the export.

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
  refused. Its vocabularies and response shapes are fixtures written from
  legacy's code, README and probe scripts with invented people and ids; a
  person with a login replaces them, before the first live upload, with
  captures that only read: legacy's `tests/packages_probe.py` (with
  `EARTHBANK_KNOWN_PACKAGES` set to a package of their own),
  `tests/integration_test.py --smoke`, and a plain `GET <endpoint>?size=2000`
  of each list, names and ids of real people and packages replaced. Legacy's
  `tests/arar_post_probe.py`, `tests/integration_test.py` without `--smoke`
  and `tests/live_upload_test.py` write to the production database and are
  not run for this; the bodies of errors are captured during the by-hand
  live run in a throwaway package (section 10, stage 7).
- **Mapping.** A fixture of two samples (one step-heating with a plateau, one
  fusion): every value of every record is compared with the reduction's own
  result for the same analysis, and the fields of section 4.5 are absent.
  A comparison with what legacy builds for the same analyses is made once by
  hand during the live run (section 10, stage 7), not in the suite.
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
