# Ar-Ar Reduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the legacy pychron single-analysis Ar-Ar age pipeline (isotope arithmetic, 37/39 decay, K/Ca/Cl interference corrections, atmospheric and cosmogenic components, F, age with J variants, K/Ca, K/Cl, error components) to `libs/reduction` with full correlated error propagation, proven against golden vectors produced by the legacy code.

**Architecture:** A header-only `UFloat` (nominal value plus sorted per-variable derivative terms, modelled on Python `uncertainties`) carries every value. The equations are written once as templates over the number type in a private `arar_kernels.hpp`; the `UFloat` instantiation backs the new `reduce()` and step functions, the `double` instantiation backs the existing `compute_arar` used by live conditionals. A Python generator under `tools/reduction_golden/` calls the legacy functions and writes JSON golden files that the C++ tests read through a test-only JSON reader.

**Tech Stack:** C++20, GoogleTest, Python 3.12 via `uv` (generator only; legacy pins `uncertainties==3.2.3`, `numpy==2.4.4`, `scipy==1.17.1`, `statsmodels==0.14.6`, `traits==7.1.0`, `pyyaml==6.0.3`).

**Spec:** `docs/superpowers/specs/2026-10-02-arar-reduction-design.md`

## Global Constraints

- `libs/reduction` stays Qt-free, I/O-free and clock-free; no new third-party dependency in C++ (the golden JSON reader is test-only).
- No exceptions cross the library boundary: fallible functions return `Result<T>` with `ErrorKind::Config` and messages prefixed `reduction: ` (the `fits.hpp` precedent).
- `PYCHRON_WARNINGS_AS_ERRORS=ON`: new code builds warning-free with `pychron_set_warnings` on clang, gcc 14 and MSVC `/W4`. No `-ffast-math`.
- Sources and tests are globbed (`libs/reduction/CMakeLists.txt`, `tests/reduction/CMakeLists.txt`); add files, do not list them. Only Task 1 edits `tests/reduction/CMakeLists.txt`.
- Existing `compute_arar` contract and `tests/reduction/test_arar.cpp` stay unchanged and passing in every task.
- Tolerances (spec 4.7): nominal rtol 1e-12, std/cov rtol 1e-10, decay factors rtol 1e-13, error-component percentages atol 1e-8; per-case `atol` only with a recorded `why`. A failing golden comparison is fixed in the C++ code, never by loosening a tolerance or editing a golden file by hand.
- Golden files change only by running the generator (Task 1 command). The legacy checkout `/Users/jakeross/Programming/pychron` is read-only.
- Owner decisions D1-D6 (spec section 13) are binding: `ReductionConstants` has no physics defaults (presets only); absent values never become sentinels; deadtime is off by default, per-detector `tau`, factor 6241.509; constants are fresh per read, never shared across analyses.
- Golden cases always pass explicit constants; preset-sensitive families appear as `@legacy` and `@legacy_preferences` variants (spec 9.2).
- Legacy citations in code comments use `legacy:<file>:<line>` against commit `26e77ad1`.
- Verification commands: `cmake --build --preset dev --parallel && ctest --preset dev --output-on-failure`. Commit messages follow the repo style (`feat(reduction): ...`) and end with the attribution trailer given to the session.

## Review Focus

1. Correlation correctness: a variable reached by several paths (Ar39 in `k39` and in `F`'s denominator, Ar37 in `ca37` and `k39`) gets one merged derivative; `x - x` is exact zero; the two legacy `atm4036` copies (`trapped_4036` in E14, the one inside `atm3836` in E12) are distinct variables (Tasks 2, 3, 8, 9 tests assert `derivative(id)` values, not only `std_dev`).
2. J shared error: `age` has zero derivative with respect to the supplied J; `age_w_j_err` has `dt/dJ = F / (lambda (1 + J F))` scaled to units; `age_w_position_err` uses an independent `Position` variable; two analyses built with one J `UFloat` have `cov(age_w_j_err_A, age_w_j_err_B) = dA/dJ * dB/dJ * sigma_J^2` (Tasks 10, 11; `correlation.json`).
3. Near-zero divisors: zero tests are exact `== 0.0` on nominals, matching Python `ZeroDivisionError` (k39 = 1e-300 is not zero); every legacy sentinel becomes an absent value plus the spec 5.6 `Diagnostic`, never a silent 1 or 0 (Tasks 9-12).
4. Parity tolerance: no case passes only because of a widened tolerance; `f_err_wo_irrad` excludes exactly the seven interference-ratio variable ids (not `Ca_K`, `Cl_K`, not the constants' `k3739`), and equals the legacy second-pass value to rtol 1e-10 (Task 9).
5. E11 ordering: the Ca clamp runs after `ca39` and `k39` used the unclamped `ca37`; the clamped `ca37` is exact zero with no terms (Task 7).

---

### Task 1: Golden generator, fixtures and C++ golden reader

**Files:**
- Create: `tools/reduction_golden/generate.py`, `tests/reduction/golden/*.json` (the eleven files below, generated), `tests/reduction/golden.hpp`, `tests/reduction/test_golden_reader.cpp`
- Modify: `tests/reduction/CMakeLists.txt` (add `PYCHRON_REDUCTION_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"` to `target_compile_definitions`)

**Golden files produced:** `ufloat.json`, `constants.json`, `isotope_arithmetic.json`, `decay_factors.json`, `interference.json`, `atmospheric.json`, `calculate_f.json`, `age.json`, `pipeline.json`, `chlorine.json`, `correlation.json`.

**Interfaces:**
- Produces (CLI): `generate.py --legacy PATH --out DIR [--check]`. Module docstring states the `uv run` command from spec 9.1 verbatim.
- Produces (test-only C++, namespace `pychron::reduction::golden`):
  ```cpp
  struct Json;  // variant: null, bool, double, string, array, object (std::map, sorted keys)
  Json parse(std::string_view text, std::string* error);           // non-finite strings "nan","inf","-inf" -> double via as_number()
  Json load(std::string_view file);                                 // PYCHRON_REDUCTION_GOLDEN_DIR/file; ADD_FAILURE on error
  struct Tol { double rtol = 1e-12, rtol_err = 1e-10, atol = 0, atol_err = 0; };
  Tol tol_of(const Json& c);
  void expect_close(double got, double want, double rtol, double atol, std::string_view what);
  ```
- Case format exactly as spec 9.3 (`name`, `source`, `inputs`, `expected`, `tol`, `legacy_sentinel`, `expect_diagnostics`, `expect_error`; `ufloat.json` uses `vars`/`steps`/`expected`/`cov`).

- [ ] **Step 1: Write the generator.** One function per golden file; each builds cases from the legacy test sources listed in spec 9.2 and the edge cases there (zero/negative signals, missing Cl, fixed `k3739` by mode and per analysis, `Ca3937 = 0` in fixed mode, huge errors, `1 + JF <= 0`, guard error). Before importing pychron: `os.environ["HOME"] = tempfile.mkdtemp()`, `sys.dont_write_bytecode = True`, `sys.path.insert(0, legacy)`, `warnings.filterwarnings("ignore", message="Using UFloat objects with std_dev==0")`. Pipeline cases build `ArArAge` with stored values (`Isotope.set_uvalue`, `set_baseline`, `set_blank`, `ic_factor = ufloat(v, e, tag="<iso> IC")`, `include_baseline_error`, `correct_for_blank`) and `arar_constants.age_units = "Ma"`; the seeded `_build_isotope` series of `arar_age_test.py:26-36` are fitted by legacy and stored as the intercept value/error. Sentinel cases record the legacy value under `legacy_sentinel` and the spec 5.6 diagnostic under `expect_diagnostics`. Every case writes all constants into `inputs.constants` and sets the legacy `ArArConstants` traits from them; preset-sensitive families (interference, F, age, pipeline, chlorine) are emitted for both legacy default sets (`@legacy`: traits; `@legacy_preferences`: `allow_negative_ca_correction = False`, `lambda_b` error 0, `k3739` error 0.01; spec 5.3). `constants.json` records both legacy default tables. `isotope_arithmetic.json` deadtime cases use 6241.509 plus one case named `deadtime/legacy_6240` with `legacy_sentinel` holding the 6240 result. `pipeline.json` also records `age_err_wo_irrad` as `std` of legacy `uage` excluding the interference-ratio tags (spec Q13). Correlation cases call legacy functions with one shared `ufloat` (J or blank) and record `uncertainties.covariance_matrix`. Header per spec 9.1 (commit, dirty flag, versions); output `json.dumps(sort_keys=True, indent=1, allow_nan=False) + "\n"`.
- [ ] **Step 2: Generate and check determinism:** run the spec 9.1 command, then the same with `--check` → exit 0. Confirm `git -C /Users/jakeross/Programming/pychron status --short` is empty and no `__pycache__` appeared there.
- [ ] **Step 3: Write failing test** `tests/reduction/test_golden_reader.cpp`:
  - `GoldenReader.ParsesScalarsArraysObjectsAndNonFinite`: literal text covering nested objects, negative exponents, `"nan"`/`"inf"`/`"-inf"`, escapes `\"` `\\` `\n` `µ`.
  - `GoldenReader.RejectsMalformed`: trailing comma, unterminated string, bare `NaN` → error string non-empty.
  - `GoldenReader.EveryGoldenFileLoadsWithHeader`: each of the eleven files parses, has `schema == 1`, a 40-hex `legacy_commit`, and a non-empty `cases` array whose names are unique.
  - `GoldenReader.ConstantsAlwaysExplicitAndBothLegacySetsPresent`: every case outside `ufloat.json`/`decay_factors.json` has a complete `inputs.constants`; each preset-sensitive file has both `@legacy` and `@legacy_preferences` variants of every such case.
- [ ] **Step 4: Run** `ctest --preset dev -R GoldenReader` → FAIL (header missing).
- [ ] **Step 5: Implement** `golden.hpp` (recursive descent, `std::strtod` with the "C" locale, ~200 lines, header-only, `inline`).
- [ ] **Step 6: Run** `cmake --build --preset dev --parallel && ctest --preset dev --output-on-failure` → PASS.
- [ ] **Step 7: Commit** `test(reduction): legacy golden-vector generator and fixtures`.

### Task 2: `UFloat` core

**Files:**
- Create: `libs/reduction/include/pychron/reduction/ufloat.hpp`
- Test: `tests/reduction/test_ufloat.cpp`

**Golden files consumed:** `ufloat.json` (cases whose `steps` use only `add sub mul div neg`).

**Interfaces:**
- Produces (spec 4.2): `VariableId`, `TagId`, `intern_tag`, `tag_name`, `class UFloat` with `Term`, `variable(value, sigma, tag)`, `nominal`, `std_dev`, `variance`, `is_exact`, `terms`, `derivative`, `variable_id`, compound and binary `+ - * /` with `double` on either side, unary `-`.

- [ ] **Step 1: Write failing tests:**
  - `UFloat.VariableHasUnitDerivativeAndUniqueId`: two `variable(1, 0.1)` → distinct non-zero ids, `derivative(own id) == 1`, `std_dev == 0.1`.
  - `UFloat.ZeroSigmaIsExact`: `variable(5, 0).is_exact()`; `UFloat(3.0).terms().empty()`.
  - `UFloat.SelfCancellation` (Review Focus 1): `x - x` exact zero with no terms; `x / x` nominal 1, std 0; `(x + x).std_dev() == 2 * sigma`.
  - `UFloat.ProductAndQuotientRules`: hand-computed derivatives for `x*y`, `x/y`, `2/x`, `x*3`.
  - `UFloat.TermsSortedAndMergedOnce`: `a + b + a` has two terms, ascending id, `derivative(a) == 2`.
  - `UFloat.TagsInterned`: `intern_tag("J") == intern_tag("J")`, `tag_name(intern_tag("J")) == "J"`, `tag_name(0) == ""`.
  - `UFloat.IdsUniqueAcrossThreads`: 8 threads x 10 000 `variable()` → all ids distinct.
  - `UFloat.GoldenArithmetic`: interpret the `ufloat.json` arithmetic programs; compare `expected` with spec 4.7 tolerances.
- [ ] **Step 2: Run** `ctest --preset dev -R UFloat` → FAIL (header missing).
- [ ] **Step 3: Implement.** `std::vector<Term>` sorted by id; one merge routine `combine(a, ca, b, cb)` producing `ca*da + cb*db` and dropping exact `0.0` derivatives; ids from a function-local `inline std::atomic<std::uint64_t>`; tag table function-local static `std::mutex` + `std::deque<std::string>` + `std::unordered_map<std::string_view, TagId>`. `assert` (not throw) on negative/non-finite sigma. No comparison operators.
- [ ] **Step 4: Run** `ctest --preset dev -R UFloat` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): add header-only UFloat with derivative tracking`.

### Task 3: `UFloat` functions, covariance and error budget

**Files:**
- Modify: `libs/reduction/include/pychron/reduction/ufloat.hpp`
- Test: `tests/reduction/test_ufloat.cpp` (append)

**Golden files consumed:** `ufloat.json` (all remaining programs: `exp log log10 sqrt abs pow`, covariance matrices, zero-sigma and huge-sigma cases).

**Interfaces:**
- Produces (spec 4.2-4.3): `exp`, `log`, `log10`, `sqrt`, `abs`, `pow(UFloat,double)`, `pow(double,UFloat)`, `pow(UFloat,UFloat)`, `covariance`, `correlation`, `covariance_matrix`, `std_dev_excluding(ids)`, `std_dev_excluding_tags(tags)`, `error_components`, `variance_percent`.

- [ ] **Step 1: Write failing tests:**
  - `UFloat.FunctionDerivatives`: `exp`, `log`, `sqrt`, `pow(x, 2.5)`, `pow(x, y)`, `pow(2, y)` against hand-computed derivatives; `pow(x, 0)` exact 1; `pow(0-nominal x, 2)` derivative 0.
  - `UFloat.DomainIsIeee`: `log(variable(-1, 0.1))` NaN nominal; `variable(1, 0.1) / UFloat(0)` non-finite; neither throws.
  - `UFloat.CovarianceViaSharedVariables`: `a = x + y`, `b = x - y` → `cov == sx^2 - sy^2`; independent variables → 0; `covariance_matrix` symmetric with variances on the diagonal.
  - `UFloat.ExclusionAndComponents`: `std_dev_excluding(f, {id_y})` equals the std of the same expression rebuilt with `y` exact; `error_components` aggregates two variables with one tag in quadrature; `variance_percent` over all tags sums to 100.
  - `UFloat.CopiesShareVariables`: copy of J used in two expressions → covariance non-zero.
  - `UFloat.GoldenFunctionsAndCovariance`: remaining `ufloat.json` programs including their `cov` entries.
- [ ] **Step 2: Run** `ctest --preset dev -R UFloat` → new tests FAIL.
- [ ] **Step 3: Implement** with the derivative rules in spec 4.3, including the `uncertainties` 3.2.3 special cases of `pow` at `x0 == 0` (check them against the golden programs, which include them).
- [ ] **Step 4: Run** `ctest --preset dev -R UFloat` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): UFloat math functions, covariance and error budget`.

### Task 4: Data model, constants, stored-row resolution

**Files:**
- Create: `libs/reduction/include/pychron/reduction/arar_types.hpp`, `libs/reduction/src/arar_types.cpp`
- Test: `tests/reduction/test_arar_types.cpp`

**Golden files consumed:** `constants.json`.

**Interfaces:**
- Produces (spec 5.1-5.5): `Measured`, `StoredValue`, `resolve`, `ArgonIsotope`, `kArgonKeys`, `to_string(ArgonIsotope)`, `index`, `IsotopeSignal`, `MeasuredSignal`, `make_signal`, `corrects_for_blank`, `K3739Mode`, `AgeUnits`, `CosmogenicRatios`, `ReductionConstants` (all members zero/false; no physics defaults), `ConstantsPreset {Default, Legacy, LegacyPreferences}`, `constants_preset`, `to_string(ConstantsPreset)`, `lambda_k`, `kReductionVersion`, `ProductionRatios`, `production_from_rows`, `ProductionVariables` (+ `interference_ids()`), `make_production_variables`, `Dose`, `DecaySegment`, `Irradiation`, `DecayFactors`, `Flux`, `make_j`.

- [ ] **Step 1: Write failing tests:**
  - `ArArTypes.ConstantsHaveNoHiddenDefaults` (D1): value-initialised `ReductionConstants` has every `Measured` at `{0, 0}` and `allow_negative_ca_correction == false`.
  - `ArArTypes.LegacyPresetsMatchLegacy`: `constants_preset(Legacy)` and `constants_preset(LegacyPreferences)` equal the two tables in `constants.json` field by field; `lambda_k()` value 5.543e-10, error in quadrature.
  - `ArArTypes.DefaultPresetMatchesSpec` (D2, D5): `atm4036 == {298.56, 0.31}`, `lambda_b.error == 9.3e-13`, `fixed_k3739.error == 0.01`, `allow_negative_ca_correction == false`.
  - `ArArTypes.ResolveAppliesManualAndModifier`: manual value only, manual error only, both, `modifier_error` replaces error.
  - `ArArTypes.MakeSignalUsesLegacyTagsAndFreshVariables`: tags `Ar40`, `Ar40 bs`, `Ar40 bk`, `Ar40 IC`; two calls give disjoint variable ids (spec Q2); zero-error inputs give exact values.
  - `ArArTypes.CorrectsForBlank`: `blank_unknown`, `blank_air`, `detector_ic`, `background` → false; `unknown`, `air`, `cocktail` → true.
  - `ArArTypes.ProductionFromRows`: all nine keys parse; missing interference key → `{0,0}`; missing `Ca_K` → nullopt; unknown key `K4139` and NaN value → error containing the key.
  - `ArArTypes.ProductionVariablesTaggedOnce`: tags equal key names; `interference_ids()` has seven distinct ids, none equal to `ca_k`'s.
  - `ArArTypes.MakeJTagged`: tag `J`, sigma `j.error`.
- [ ] **Step 2: Run** `ctest --preset dev -R ArArTypes` → FAIL.
- [ ] **Step 3: Implement.** `corrects_for_blank` uses `starts_with` on `blank`, `detector_ic`, `background` (legacy `pychron_constants.py:264`, `dvc/dvc_analysis.py:779`).
- [ ] **Step 4: Run** `ctest --preset dev -R ArArTypes` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): Ar-Ar data model, constants and production ratios`.

### Task 5: Isotope arithmetic and the kernels header

**Files:**
- Create: `libs/reduction/include/pychron/reduction/arar_reduction.hpp`, `libs/reduction/src/arar_kernels.hpp` (private), `libs/reduction/src/arar_reduction.cpp`
- Test: `tests/reduction/test_isotope_arithmetic.cpp`

**Golden files consumed:** `isotope_arithmetic.json`.

**Interfaces:**
- Consumes: `UFloat` (Tasks 2-3), `IsotopeSignal` (Task 4).
- Produces: `baseline_corrected`, `corrected_intensity`, `abundance_sensitivity_correction`, `kFaToCountsPerSecond = 6241.509`, `deadtime_correct(signal_fa, tau_s, fa_to_cps)` (spec 6, E1-E5); `IsotopeSignal`/`MeasuredSignal::deadtime_tau_s` (D4). Kernel templates `kernels::abundance_sensitivity<T>(std::array<T,5>, double)` and `inline double nominal(double)`, `inline double nominal(const UFloat&)`.

- [ ] **Step 1: Write failing tests:**
  - `IsotopeArithmetic.BaselineErrorExcludedByDefault`: `include_baseline_error = false` → result has no baseline variable term but the baseline's nominal is subtracted; `true` → term present with derivative -1.
  - `IsotopeArithmetic.BlankSkippedWhenNotCorrecting`.
  - `IsotopeArithmetic.OrderIsSubtractThenDiscThenIc`: derivative with respect to the blank equals `-D*IC`.
  - `IsotopeArithmetic.IcFactorZeroHonoured` (spec Q18).
  - `IsotopeArithmetic.AbundanceSensitivityNeighbours`: alpha 1e-4 reproduces the five legacy formulas incl. `2*s39` and `2*s37` (spec Q9); alpha 0 returns inputs unchanged (same variables).
  - `IsotopeArithmetic.DeadtimeFormulaAndDomain` (D4): signal converted with 6241.509, corrected `n/(1 - n tau)`, converted back; derivative `1/(1 - n tau)^2`; `n tau >= 1` → error; `tau` absent → intercept unchanged (same variables).
  - `IsotopeArithmetic.DeadtimeLegacy6240Divergence`: the `deadtime/legacy_6240` golden case matches only when `fa_to_cps = 6240` is passed; with the default factor it differs from `legacy_sentinel` by more than the tolerance (documents the divergence).
  - `IsotopeArithmetic.Golden`: every case in `isotope_arithmetic.json`.
- [ ] **Step 2: Run** `ctest --preset dev -R IsotopeArithmetic` → FAIL.
- [ ] **Step 3: Implement** in `arar_reduction.cpp` on the kernel templates; comments cite `legacy:processing/isotope.py:715-854`, `legacy:processing/argon_calculations.py:363-372`, `legacy:processing/deadtime.py:54-55`.
- [ ] **Step 4: Run** `ctest --preset dev -R IsotopeArithmetic` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): isotope corrections and abundance sensitivity`.

### Task 6: Decay factors and chronology

**Files:**
- Create: `libs/reduction/src/decay.cpp`
- Modify: `libs/reduction/include/pychron/reduction/arar_reduction.hpp`, `libs/reduction/include/pychron/reduction/arar_types.hpp` (declaration of `irradiation_from_doses` if not yet present)
- Test: `tests/reduction/test_decay.cpp`

**Golden files consumed:** `decay_factors.json`.

**Interfaces:**
- Produces: `Result<DecayFactors> decay_factors(double, double, std::span<const DecaySegment>)` (E7), `Irradiation irradiation_from_doses(std::span<const Dose>, std::int64_t, bool)` (E6).

- [ ] **Step 1: Write failing tests:**
  - `Decay.NoSegmentsIsUnity`.
  - `Decay.KnownSingleSegment`: legacy `DecayFactorsTest.test_known_single_segment` values (1.022997480219743, 1.0223515753822912) at rtol 1e-13.
  - `Decay.UnitGuardErrors`: seconds instead of days → error containing `same unit`; exactly 50 passes, just above fails.
  - `Decay.ZeroDenominatorIsUnity`: all powers zero → `{1, 1}`.
  - `Decay.SegmentsFromDosesStartVsEnd` (spec Q11): two doses, `use_irradiation_endtime` false measures `dt` from start, true from end; durations in days; `decay_days` from the first dose's start; UTC seconds, no time zone (spec Q12).
  - `Decay.Golden`: all `decay_factors.json` cases (including expected errors).
- [ ] **Step 2: Run** `ctest --preset dev -R Decay` → FAIL.
- [ ] **Step 3: Implement** with plain `1 - std::exp(-l*t)` (not `expm1`, for parity) and the guard on `|l * max(|t|, |dt|)| > 50`.
- [ ] **Step 4: Run** `ctest --preset dev -R Decay` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): 37Ar/39Ar decay factors and chronology segments`.

### Task 7: Interference corrections

**Files:**
- Modify: `libs/reduction/src/arar_kernels.hpp`, `libs/reduction/src/arar_reduction.cpp`, `libs/reduction/include/pychron/reduction/arar_reduction.hpp`
- Test: `tests/reduction/test_interference.cpp`

**Golden files consumed:** `interference.json`.

**Interfaces:**
- Produces: `InterferenceOptions`, `InterferenceComponents`, `interference_corrections(a39, a37, ProductionVariables, InterferenceOptions, std::vector<Diagnostic>*)` (E9-E11); `enum class Diagnostic` and `to_string(Diagnostic)` (spec 5.6); kernel `kernels::interference<T>(...)`.

- [ ] **Step 1: Write failing tests:**
  - `Interference.PureKNoCa`: legacy `test_pure_k_no_ca` (k39 = 100, ca37 = 0, k38 = 1.3).
  - `Interference.NormalModeDerivatives`: `dk39/da37 == -Ca3937 / (1 - K3739 Ca3937)` exactly as computed by the kernel.
  - `Interference.FixedModeByConstantsAndByAnalysis`: per-analysis value forces fixed mode even when `mode == Normal`; `k37 == x * k39`; `a37` has no influence (zero derivative).
  - `Interference.FixedModeZeroCa3937UsesYOne` (spec Q10): `ca37 == a39 x / (x + 1)` and `FixedK3739ZeroCa3937` emitted.
  - `Interference.ClampAfterCa39` (Review Focus 5): negative `a37`, clamp on → `ca37` exact zero (no terms), `ca36 == ca38 == 0`, but `ca39` and `k39` still carry the negative `ca37`; `CaClampedToZero` emitted; clamp off → negative values kept.
  - `Interference.Golden`: all `interference.json` cases.
- [ ] **Step 2: Run** `ctest --preset dev -R Interference` → FAIL.
- [ ] **Step 3: Implement** the kernel; the fixed-mode zero test is `nominal(ca3937) == 0.0`; the clamp test is `!(nominal(ca37) > 0.0)` (Python `max(ufloat(0,0), ca37)` semantics).
- [ ] **Step 4: Run** `ctest --preset dev -R Interference` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): K/Ca interference corrections`.

### Task 8: Atmospheric, chlorine formula and cosmogenic components

**Files:**
- Modify: `libs/reduction/src/arar_kernels.hpp`, `libs/reduction/src/arar_reduction.cpp`, `libs/reduction/include/pychron/reduction/arar_reduction.hpp`
- Test: `tests/reduction/test_atmospheric.cpp`

**Golden files consumed:** `atmospheric.json`.

**Interfaces:**
- Produces: `AtmosphericComponents`, `CosmogenicComponents`, `Result<AtmosphericComponents> atmospheric_components(a38, a36, k38, ca38, ca36, decay_days, cl3638, constants)` (E12), `Result<CosmogenicComponents> cosmogenic_components(c36, c38, CosmogenicRatios)` (E13).

- [ ] **Step 1: Write failing tests:**
  - `Atmospheric.NoChlorineIsSimpleSubtraction`: `Cl3638 = 0` → `atm36 == a36 - ca36`, `cl36` exact 0.
  - `Atmospheric.Atm38FollowsRatioWithError`: `atm38 == atm3836 * atm36` and has a term tagged `atm4036` and one tagged `atm4038`.
  - `Atmospheric.FreshConstantVariablesPerCall` (spec Q1, Review Focus 1): two calls produce disjoint `atm4036` variable ids.
  - `Atmospheric.ChlorineBranch`: legacy `test_with_chlorine_yields_nonzero_cl` inputs; `cl36 != 0`; derivative with respect to `Cl3638` matches the analytic E12 partial.
  - `Atmospheric.SingularDenominatorErrors` (spec Q16): choose `Cl3638` so `1 - m r3836 == 0` exactly in nominal → error.
  - `Cosmogenic.PureSolarPureCosmoMixed`: legacy `CosmogenicComponentsTest` three cases; `c36 = 0` and `rc == rs` → error.
  - `Atmospheric.Golden`, `Cosmogenic.Golden`: all `atmospheric.json` cases.
- [ ] **Step 2: Run** `ctest --preset dev -R "Atmospheric|Cosmogenic"` → FAIL.
- [ ] **Step 3: Implement**; `lambda_Cl36`, `atm4036`, `atm4038` minted inside the function with the legacy tags.
- [ ] **Step 4: Run** `ctest --preset dev -R "Atmospheric|Cosmogenic"` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): atmospheric, chlorine and cosmogenic components`.

### Task 9: F, radiogenic yield and F without irradiation errors

**Files:**
- Modify: `libs/reduction/src/arar_kernels.hpp`, `libs/reduction/src/arar_reduction.cpp`, `libs/reduction/include/pychron/reduction/arar_reduction.hpp`
- Test: `tests/reduction/test_calculate_f.cpp`

**Golden files consumed:** `calculate_f.json`.

**Interfaces:**
- Consumes: Tasks 7-8.
- Produces: `FResult`, `Result<FResult> calculate_f(const std::array<UFloat,5>& n, double decay_days, const ProductionVariables&, const ReductionConstants&, std::optional<Measured> fixed_k3739)` (E9-E15).

- [ ] **Step 1: Write failing tests:**
  - `CalculateF.EqualsRad40OverK39` and `CalculateF.InterferenceCorrectedMap` (legacy `CalculateFTest` keys and `ifc` definitions).
  - `CalculateF.WithoutIrradExcludesExactlyInterferenceIds` (Review Focus 4): equals `std_dev_excluding(f, p.interference_ids())`; not affected by `Ca_K`/`Cl_K`; less than or equal to `f.std_dev()`.
  - `CalculateF.TrappedAndRatioAtmAreDistinct` (Review Focus 1): with `Cl3638 > 0`, `F` has two distinct terms tagged `trapped_4036` and `atm4036`.
  - `CalculateF.K39ZeroIsUndefined` (spec Q6/D3, Review Focus 3): `f` absent (no `1 +- 0` sentinel), `FUndefined`, `rad40` still present; `k39 = 1e-300` gives a (huge) value, no diagnostic.
  - `CalculateF.A40ZeroYieldUndefined`: `radiogenic_yield` absent, `YieldUndefined`.
  - `CalculateF.CosmogenicReplacesAtm`: enabled → `interference_corrected[Ar36] == noncosmo36`.
  - `CalculateF.Golden`: every `calculate_f.json` case incl. `legacy_sentinel` divergence assertions.
- [ ] **Step 2: Run** `ctest --preset dev -R CalculateF` → FAIL.
- [ ] **Step 3: Implement** a single pass (no second `calc_f` with zeroed ratios; spec E15).
- [ ] **Step 4: Run** `ctest --preset dev -R CalculateF` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): F, radiogenic yield and irradiation-free F error`.

### Task 10: Age equation, J variants and error budget

**Files:**
- Modify: `libs/reduction/src/arar_kernels.hpp`, `libs/reduction/src/arar_reduction.cpp`, `libs/reduction/include/pychron/reduction/arar_reduction.hpp`
- Test: `tests/reduction/test_age.cpp`

**Golden files consumed:** `age.json`, `correlation.json` (J-sharing cases).

**Interfaces:**
- Produces: `Result<UFloat> age_equation(j, f, constants, lambda_k_total)` (E16), `double age_scale(AgeUnits, AgeUnits)`, `AgeSet` and an internal `make_age_set(j, position_jerr, f, constants, lambda_k_total, interference_ids)` used by Task 11 (E17-E18).

- [ ] **Step 1: Write failing tests:**
  - `Age.KnownValueAndUnits`: `ln(1 + J F) / lambda` in a, ka, Ma, Ga against `age.json`.
  - `Age.DecayErrorOnlyWhenRequested`: `include_decay_error = false` → no `lambda_k` term; true → term present and larger `std_dev`.
  - `Age.LambdaKOverride`: `lambda_k_total` replaces `lambda_b + lambda_e`; zero value ignored (legacy `if lk:`).
  - `Age.NonPositiveArgumentErrors` (spec Q6): `J = 1, F = -2` → error; `reduce` will map it to `AgeUndefined`.
  - `Age.JVariants` (Review Focus 2): `age` has zero derivative with respect to J's id; `age_w_j_err`'s derivative equals `F / (lambda (1 + J F)) * scale`; `age_w_position_err` has one `Position`-tagged term with sigma `position_jerr`; all three share F's other terms identically.
  - `Age.ErrorBudgetWithoutIrrad` (spec Q13): `age_err_wo_irrad == std_dev_excluding(age, interference_ids)`, `age_err_wo_j_irrad == age_err_wo_irrad`.
  - `Age.Golden`, `Age.SharedJCorrelationGolden`: `age.json`; the J cases of `correlation.json` (covariance of two ages sharing J at rtol 1e-10).
- [ ] **Step 2: Run** `ctest --preset dev -R Age` → FAIL.
- [ ] **Step 3: Implement**; J variants mint fresh variables tagged `J_no_err` (exact, so no term) and `Position`.
- [ ] **Step 4: Run** `ctest --preset dev -R Age` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): age equation with J variants and error budget`.

### Task 11: `reduce()` pipeline, K/Ca and error components

**Files:**
- Modify: `libs/reduction/src/arar_reduction.cpp`, `libs/reduction/include/pychron/reduction/arar_reduction.hpp`, `libs/reduction/include/pychron/reduction/arar_types.hpp` (`ReductionInput`, `ArArResult`)
- Test: `tests/reduction/test_reduce.cpp`

**Golden files consumed:** `pipeline.json` (all cases except those tagged `chlorine` in their name), `correlation.json` (shared-blank cases).

**Interfaces:**
- Consumes: Tasks 4-10.
- Produces: `ReductionInput`, `ArArResult`, `Result<ArArResult> reduce(const ReductionInput&)`; K/Ca per E19 (`kca`, `cak`); `age_error_components` per E20 keyed `Ar40`..`Ar36`.

- [ ] **Step 1: Write failing tests:**
  - `Reduce.ValidatesInputs`: NaN intercept (spec Q7), negative sigma, negative `abundance_sensitivity`, negative `deadtime_tau_s`, zero `lambda_b + lambda_e` (e.g. a value-initialised `ReductionConstants`) → error naming the field.
  - `Reduce.DeadtimeOffByDefault` (D4): no `deadtime_tau_s` on any isotope → result identical to the same input without the field.
  - `Reduce.ConstantsNotSharedAcrossAnalyses` (D6): two `reduce` calls with the same `ReductionConstants` → `covariance(F_A, F_B)` has no contribution from `trapped_4036`/`atm4036` (disjoint ids).
  - `Reduce.BothLegacyPresetsGolden` (D2): every `@legacy` and `@legacy_preferences` pipeline case passes with constants read from the case (never from a preset).
  - `Reduce.StepOrder`: abundance sensitivity before decay; decay factors applied to 37 and 39 only; `corrected` equals legacy `corrected_intensities`.
  - `Reduce.NoJNoAges`: `j` absent → `ages` absent, F present.
  - `Reduce.KCaUsesClampedCa37AndCaK`: `kca == k39 / ca37 / Ca_K`; missing or zero `Ca_K` → factor 1; `ca37 == 0` → absent with `KCaUndefined`.
  - `Reduce.ErrorComponentsSumWithJ`: isotope components plus `J` plus the rest sum to 100 within 1e-8.
  - `Reduce.SharedBlankCorrelation` (owner requirement): two inputs whose Ar36 `blank` is the same `UFloat` → `covariance(F_A, F_B)` matches `correlation.json`; with `make_signal` (fresh blanks) the covariance is 0.
  - `Reduce.Golden`: all non-chlorine `pipeline.json` cases (J variants, segments, fixed `k3739`, abundance sensitivity, cosmogenic, zero/negative signals, huge errors, `lambda_k` override, error components, sentinels).
- [ ] **Step 2: Run** `ctest --preset dev -R Reduce` → FAIL.
- [ ] **Step 3: Implement** `reduce` as: validate → `corrected_intensity` x5 → optional deadtime → abundance sensitivity → `decay_factors` → apply df → `calculate_f` → ages (if `j`) → K/Ca → components; diagnostics appended in that order.
- [ ] **Step 4: Run** `ctest --preset dev -R Reduce` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): single-analysis reduce pipeline`.
- [ ] **Step 6: Performance check (manual, not gated):** configure `mac-release`, time 10 000 `reduce` calls on the first `pipeline.json` case with a throwaway local driver; record the per-call time in the commit message body. Target under 50 us (spec 4.6).

### Task 12: Chlorine: K/Cl, Cl/K, cl36 and missing-Cl behaviour

**Files:**
- Modify: `libs/reduction/src/arar_reduction.cpp`
- Test: `tests/reduction/test_chlorine.cpp`

**Golden files consumed:** `chlorine.json`, `pipeline.json` (cases with `chlorine` in their name).

**Interfaces:**
- Produces: `ArArResult::kcl`, `ArArResult::clk` (E19); `decay_days` from `Irradiation` drives `m` in E12.

- [ ] **Step 1: Write failing tests:**
  - `Chlorine.KClUsesResidualCl38AndClK`: `kcl == k39 / cl38 / Cl_K`, `clk == 1 / kcl`.
  - `Chlorine.MissingClProduction`: no `Cl3638`, no `Cl_K` → `cl36` exact 0, `cl38` is the residual 38 (legacy behaviour), `kcl` present with factor 1.
  - `Chlorine.Cl38ZeroUndefined`: `cl38 == 0` → `kcl`/`clk` absent, `KClUndefined`.
  - `Chlorine.DecayDaysScalesCl36`: doubling `decay_days` roughly doubles `m` and changes `atm36` per E12.
  - `Chlorine.Golden`: all `chlorine.json` cases and the chlorine cases of `pipeline.json`.
- [ ] **Step 2: Run** `ctest --preset dev -R Chlorine` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset dev -R "Chlorine|Reduce"` → PASS.
- [ ] **Step 5: Commit** `feat(reduction): chlorine-derived K/Cl and Cl/K`.

### Task 13: `compute_arar` on the shared kernels, live constants, conditionals

**Files:**
- Modify: `libs/reduction/include/pychron/reduction/arar.hpp`, `libs/reduction/src/arar.cpp`, `libs/experiment/include/pychron/experiment/conditionals/validate.hpp` (`MetricCatalog::chlorine`), `libs/experiment/src/conditionals/validate.cpp`
- Test: `tests/reduction/test_arar.cpp` (existing cases untouched; append), `tests/experiment/test_conditionals_runtime.cpp` (append)

**Golden files consumed:** `pipeline.json` (complete-input cases without blanks, for the parity test).

**Interfaces:**
- Produces: `ArArConstants` gains `k3739 = 0`, `k3839 = 0`, `ca3837 = 0`, `allow_negative_ca_correction = true`, `std::optional<LiveChlorine> chlorine` with `struct LiveChlorine { double cl3638 = 0, lambda_cl36 = 6.308e-9, decay_days = 0, atm4038 = 1575.0, cl_k_factor = 1.0; }`; `ArArConstants to_live_constants(const ReductionConstants&, const ProductionRatios&, const Flux&, const DecayFactors&, std::optional<double> decay_days = std::nullopt)` (spec 8.2); `MetricCatalog::chlorine = false`.

- [ ] **Step 1: Write failing tests:**
  - `ArAr.HandComputedFixture`, `ArAr.OmitsWhatCannotBeComputed`: unchanged, must keep passing.
  - `ArAr.MatchesReduceNominal`: for each complete, blank-free `pipeline.json` case, `compute_arar(intensities after IC, to_live_constants(...))` equals `reduce(...)` nominal values for every shared key at rtol 1e-12.
  - `ArAr.ChlorineKeysOnlyWhenConfigured`: `chlorine` set and `Ar38` present → `kcl`, `clk`, `cl36`; otherwise absent.
  - `ArAr.LiveDefaultsEqualDefaultPreset` (D5): `ArArConstants{}.atm4036 == constants_preset(Default).atm4036.value`.
  - `ArAr.K3739NeedsAr39`: `k3739 != 0` and no `Ar39` → `ca37`, `ca36`, `ca39` omitted.
  - `ConditionalsValidate.ChlorineCatalogAllowsKcl`: `computed = true, chlorine = true` → no `kcl` diagnostic; the existing `kcl still unavailable` assertion at `test_conditionals_runtime.cpp:369` still passes with the default.
- [ ] **Step 2: Run** `ctest --preset dev -R "ArAr|Conditional"` → new tests FAIL, existing PASS.
- [ ] **Step 3: Implement** `compute_arar` over `kernels::...<double>` preserving its omission rules (partial isotopes, `j > 0`, zero divisors); update the header comment block to the new formulas; `validate.cpp` checks `catalog.chlorine`.
- [ ] **Step 4: Run** `cmake --build --preset dev --parallel && ctest --preset dev --output-on-failure` → PASS (whole suite: collector and conditionals tests use `compute_arar`).
- [ ] **Step 5: Commit** `feat(reduction): compute_arar on shared kernels; live constants from reduction inputs`.

### Task 14: Documentation

**Files:**
- Modify: `docs/superpowers/specs/2026-10-02-conditionals-design.md` (section 5: formulas now include `K3739`, clamp and chlorine keys behind `MetricCatalog::chlorine`; section 9: remove chlorine from out of scope), `docs/superpowers/specs/2026-10-02-arar-reduction-design.md` (Status: Implemented; record the measured `reduce` time; mark follow-ups A1-A3 done or carried); the DVC schema spec only if the owner has approved follow-up A1, otherwise leave it, `docs/dev_setup.md` (one paragraph: regenerating golden vectors with the spec 9.1 command and `--check`)
- Test: none

**Golden files consumed:** none.

- [ ] **Step 1: Edit** the three documents. Every legacy behaviour row of spec section 10 must still match the code; if an implementation task changed a decision, update the row and say why.
- [ ] **Step 2: Run** `uv run ... generate.py --check` (spec 9.1 command) → exit 0, then `cmake --build --preset dev --parallel && ctest --preset dev --output-on-failure` → PASS.
- [ ] **Step 3: Commit** `docs(reduction): golden-vector workflow and conditionals chlorine metrics`.

---

## Self-review notes

- **Spec coverage:** UFloat 4.1-4.7 → Tasks 2-3; data model 5 → Task 4 (+ 5.6 in Tasks 7, 9, 11); E1-E5 → 5; E6-E8 → 6; E9-E11 → 7; E12-E13 → 8; E14-E15 → 9; E16-E18 → 10; E19-E20 and `reduce` → 11-12; numerical policy 7 → tests in 5-12; integration 8 → 13; golden vectors 9 → 1 (+ every task); decisions 10 → tests named in each task; docs → 14.
- **Order:** the generator lands first so every implementation task starts from committed golden files; UFloat precedes everything that propagates errors; `reduce` waits until every step function is golden-verified.
- **Deliberate deviations from the spec:** none. Dalrymple decay factors, isochrons, plateaus, flux, `convert_age` and fractional loss stay in spec section 12.
- **Not covered by this plan:** wiring `reduce` to a DVC client (no C++ DVC library exists yet); the `arar_constants` reference type in the DVC schema (follow-up A1); legacy-sentinel export adapters (A2); the detector-config `deadtime_s` field (A3).
