# Flux Fitting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compute J for every position of an irradiation level from its fluence monitor analyses, and save it, from `elctl flux`.

**Architecture:** Three layers, the split the ArAr reduction already has. Pure math in `libs/reduction` (`flux.hpp`: J of an analysis, mean J of a position, nine models). Pure orchestration in `libs/processing` (`flux_fit.hpp`: inputs + options + edits -> tables). Store and JSON in the `processing_store` adapter (`flux_store.hpp`: monitor sets, `load_level`, `save_level`). `apps/elctl` prints and saves.

**Tech Stack:** C++20, GoogleTest, nlohmann_json (adapter and elctl only), the DVC store (`pychron::persistence`), numpy for the checked-in reference script.

**Spec:** `docs/superpowers/specs/2026-10-06-flux-fitting-design.md`. Read it first; section numbers below (S5.3, X4, F13) refer to it.

## Global Constraints

- Qt must not appear in any header of `libs/reduction` or `libs/processing`, nor nlohmann in `libs/reduction` or `libs/processing` proper (they do not link it).
- `libs/reduction` compiles with `-ffp-contract=off`; never write code that depends on an FMA.
- Functions return `Result<T>`; errors are `fail(ErrorKind::Config, "flux: ...")`. Nothing throws.
- No store migration (F11). No Monte Carlo, no `position_jerr` on new fits (F5).
- `j_err` never includes the monitor age or decay constant uncertainty (F4).
- Legacy model strings, verbatim, in `options_json`: `"Plane"`, `"Bowl"`, `"Weighted Mean"`, `"Matching"`, `"Nearest Neighbors"`, `"Bracketing"`, `"LeastSquares1D"`, `"WeightedMean1D"`, `"Bracketing1D"`.
- Changeset message of a save, verbatim: `fit flux for <irradiation><level>` (no separator, e.g. `fit flux for NM-300A`).
- A file written for the user (`--csv`) goes through `pychron::mark_as_user_file`.
- Commits: Conventional Commits with the component as scope (`feat(reduction): ...`). Branch `feat/flux-fitting` cut from `origin/develop`.
- Build and test: `cmake --build build/dev -j 10` then `ctest --preset dev -j 8`. Never skip or disable a failing test.
- A failure on one compiler only is a real bug.

## Review Focus

Inputs the spec implies but does not spell out; each has a test in the task named.

1. **Monitors that cannot determine the surface**: all on one line for Plane, or two at the same x, y. Expected: an error saying the monitor positions do not determine the model, never NaN or a huge J. (Task 3)
2. **Non-finite input**: a NaN or infinite `j`, `j_err`, x or y. Expected: an error naming the monitor; no NaN in any output. (Tasks 1, 2)
3. **A flag naming something that is not there**: `--omit` with a record id the level does not have, `--exclude-position` or `--no-save-position` with a hole that is not a position of the level, `--monitors` with an unknown set. Expected: an error naming it and listing what exists; a typo must not silently change nothing. (Tasks 5, 8)
4. **A saved revision that is only a J**: estimated in entry or imported, with no `options_json` and no analyses. Expected: shown as the saved J, no saved options or omissions, and the fit runs with defaults. (Task 6)
5. **Text that needs quoting in the CSV**: a sample named `FC-2, "new"`. Expected: RFC 4180 quoting, every row the header's width. (Task 8)

---

## File Structure

| File | Responsibility |
|---|---|
| `libs/reduction/src/least_squares.hpp` (new, private) | the QR solver now inside `fits.cpp`, shared by `fits.cpp` and `flux.cpp` |
| `libs/reduction/include/pychron/reduction/flux.hpp`, `src/flux.cpp` (new) | S5: `j_of`, `mean_j`, `fit_flux` |
| `tools/flux_reference.py`, `tests/reduction/flux_golden.hpp` (new) | reference numbers for the least-squares kinds |
| `tests/reduction/test_flux.cpp` (new) | math tests |
| `libs/processing/include/pychron/processing/flux_fit.hpp`, `src/flux_fit.cpp` (new) | S6.2 types and `fit_level`, model/option names; no store, no JSON |
| `tests/processing/test_flux_fit.cpp` (new) | pure orchestration tests |
| `libs/processing/adapters/store/include/pychron/processing/flux_store.hpp`, `src/flux_monitors.cpp`, `src/flux_options.cpp`, `src/flux_store.cpp` (new) | S4 monitor sets, S6.4 options JSON, S6.1 `load_level`, S6.3 `save_level` |
| `tests/processing/test_flux_store.cpp`, `tests/processing/flux_store_fixture.hpp` (new) | store tests on SQLite (and PostgreSQL when `PYCHRON_TEST_PG_URL` is set) |
| `apps/elctl/src/flux.hpp`, `flux.cpp`, `flux_stub.cpp` (new); `cli.cpp`, `CMakeLists.txt` (modify) | S7 |
| `apps/elctl/tests/test_flux_cmd.cpp` (new) | command tests |
| `docs/flux.md` (new), `AGENTS.md` (modify) | S9 |

---

### Task 1: J of an analysis and mean J of a position

**Files:**
- Create: `libs/reduction/include/pychron/reduction/flux.hpp`, `libs/reduction/src/flux.cpp`
- Test: `tests/reduction/test_flux.cpp`

**Interfaces:**
- Consumes: `UFloat` (`ufloat.hpp`), `weighted_mean`, `arithmetic_mean`, `MeanErrorKind`, `Mean` (`stats.hpp`).
- Produces (namespace `pychron::reduction`):

```cpp
struct MonitorConstants { double age_a = 0; double lambda_k = 0; };
Result<UFloat> j_of(const UFloat& f, const MonitorConstants& monitor);

struct MonitorAnalysis { std::string record_id; UFloat f; bool omitted = false; };
enum class MeanKind { Arithmetic, Weighted };
std::string_view to_string(MeanKind) noexcept;                       // "arithmetic", "weighted"
std::optional<MeanKind> parse_mean_kind(std::string_view) noexcept;  // any case

struct PositionMean {
  double j = 0, j_err = 0, mswd = 0;
  bool mswd_acceptable = false;
  int n = 0;
  std::vector<std::string> rejected;
};
Result<PositionMean> mean_j(std::span<const MonitorAnalysis> analyses, const MonitorConstants& monitor,
                            MeanKind kind, MeanErrorKind error);
```

- [ ] **Step 1: Write the failing tests** in `tests/reduction/test_flux.cpp` (suite `Flux`):

```cpp
TEST(Flux, JInvertsTheAgeEquation) {           // legacy argon_calculations_test.py:91-97
  const MonitorConstants m{28.201e6, 5.463e-10};
  const double f = 10.0;
  const double j = (std::exp(m.age_a * m.lambda_k) - 1.0) / f;
  auto r = j_of(UFloat(f, 0.01), m);
  ASSERT_TRUE(r);
  EXPECT_NEAR(r->nominal(), j, 1e-15);
  EXPECT_NEAR(r->std_dev(), j * 0.01 / f, 1e-15);   // sigma_J = J sigma_F / F (F4)
}
TEST(Flux, ZeroNegativeOrNonFiniteFIsAnError) {    // X9, Review Focus 2
  for (double f : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()})
    EXPECT_FALSE(j_of(UFloat(f, 0.1), {28.201e6, 5.463e-10})) << f;
}
TEST(Flux, WeightedMeanJ)        // two analyses whose J are 80e-5 and 90e-5 with weights 20 and 30 (see the step note):
                                 // j == 86.0e-5, j_err (Sem) == 50^-0.5 * 1e-5
TEST(Flux, ArithmeticMeanOfOneAnalysisHasThatAnalysisError)   // X6: n == 1, j_err == sigma of that J, mswd == 0
TEST(Flux, OmittedAnalysesTakeNoPart)                          // n counts used only
TEST(Flux, AnAnalysisWithNoJIsRejectedAndNamed)                // F = 0 -> rejected == {"66001-02"}, n excludes it
TEST(Flux, NoAnalysisLeftIsAnError)                            // all omitted, or all rejected
TEST(Flux, MsemScalesOnlyWhenMswdAboveOne)                     // scattered set: Msem == Sem * sqrt(mswd); tight set: Msem == Sem
```

Step note for `WeightedMeanJ`: legacy `error_propagation.py:333-355` is values 80 and 90 with weights 20 and 30, that is errors `20^-0.5` and `30^-0.5`: mean `(80*20 + 90*30) / 50 = 86.0`, SEM `50^-0.5 = 0.1414213562373095`. Scale both by 1e-5, build each F as `(exp(lambda t) - 1) / J` with the matching relative error, and assert `j == 86.0e-5` and `j_err == 0.1414213562373095e-5` to 1e-12 relative. Use the accessor names `UFloat` really has (check `ufloat.hpp`).

- [ ] **Step 2: Run to verify failure.** `cmake --build build/dev --target pychron_reduction_tests -j 10` fails to compile (no `flux.hpp`).

- [ ] **Step 3: Implement.** `j_of`: `(exp(lambda_k * age_a) - 1) / f` with the constant as a plain double, so only F's uncertainty propagates. `mean_j`: J of each non-omitted analysis; a failed `j_of` goes to `rejected`; the nominal values and standard deviations go to `weighted_mean` or `arithmetic_mean` with `error`; copy `value`, `error`, `mswd`, `mswd_acceptable`, `n`. For `MeanKind::Weighted`, an analysis whose J has zero or non-finite error is rejected and named (X5), not silently skipped as `weighted_mean` would.

- [ ] **Step 4: Run.** `build/dev/tests/reduction/pychron_reduction_tests --gtest_filter='Flux.*'` -> all pass.

- [ ] **Step 5: Commit.** `git commit -m "feat(reduction): J of a monitor analysis and the mean J of a position"`

---

### Task 2: The neighbour and mean models

**Files:**
- Modify: `libs/reduction/include/pychron/reduction/flux.hpp`, `libs/reduction/src/flux.cpp`
- Test: `tests/reduction/test_flux.cpp`

**Interfaces:**
- Produces:

```cpp
struct Point { double x = 0, y = 0; };
struct Monitor { std::string label; Point at; double j = 0, j_err = 0; };

enum class ModelKind { Plane, Bowl, WeightedMean, Matching, NearestNeighbors, Bracketing,
                       LeastSquares1D, WeightedMean1D, Bracketing1D };
enum class Interpolation { WeightedMean, Average, Linear };
enum class Axis { X, Y };

struct FitOptions {
  ModelKind kind = ModelKind::Plane;
  bool weighted = false;
  MeanErrorKind error = MeanErrorKind::Msem;
  int n_neighbors = 2;
  Interpolation interpolation = Interpolation::WeightedMean;
  Axis axis = Axis::X;
  int degree = 1;
  friend bool operator==(const FitOptions&, const FitOptions&) = default;
};

struct Predicted { double j = 0, j_err = 0; };
enum class FitNote { Extrapolated, MswdOutsideLimits };
struct PointNote { std::size_t point = 0; FitNote note = FitNote::Extrapolated;
                   friend bool operator==(const PointNote&, const PointNote&) = default; };
struct FluxFit {
  std::vector<Predicted> at;
  std::vector<double> parameters;
  double mswd = 0;
  int dof = 0;
  std::vector<PointNote> notes;
};

bool is_least_squares(ModelKind) noexcept;                 // Plane, Bowl, LeastSquares1D
std::size_t minimum_monitors(const FitOptions& options);   // S5.3 table
Result<FluxFit> fit_flux(std::span<const Monitor> monitors, std::span<const Point> predict_at,
                         const FitOptions& options);
```

In this task `fit_flux` returns `fail(ErrorKind::Config, "flux: model not implemented")` for the three least-squares kinds; Task 3 fills them.

- [ ] **Step 1: Write the failing tests.** Golden values from legacy `core/regression/tests/regression.py`:

```cpp
TEST(FluxModels, BracketingLinearTwoMonitors) {    // :300-331
  const std::vector<Monitor> m{{"1", {0, 0}, 1.0, 0.1}, {"2", {10, 0}, 2.0, 0.2}};
  auto f = fit_flux(m, std::vector<Point>{{5, 0}, {5, 3}}, {.kind = ModelKind::Bracketing, .interpolation = Interpolation::Linear});
  ASSERT_TRUE(f);
  EXPECT_DOUBLE_EQ(f->at[0].j, 1.5);
  EXPECT_DOUBLE_EQ(f->at[0].j_err, std::sqrt(0.05 * 0.05 + 0.1 * 0.1));
  EXPECT_DOUBLE_EQ(f->at[1].j, 1.5);               // off the segment: projected
}
TEST(FluxModels, BracketingLinearPicksTheTwoNearestOfFour)   // :334-372: x = 0,10,20,30; J = 1,2,4,8; e = 0.1,0.2,0.4,0.8;
                                                             // at (12,0): j == 2.4, j_err == sqrt((0.8*0.2)^2 + (0.2*0.4)^2)
TEST(FluxModels, Bracketing1D)                               // :579-656: xs 0,10,20; ys 1,2,4; es 0.1,0.2,0.4;
                                                             // p=5 -> 1.5 (err sqrt(0.05^2+0.1^2)); p=2.5 -> 1.25; p=-10 -> 0; p=30 -> 6;
                                                             // unsorted monitors give the same; Axis::Y uses y
TEST(FluxModels, ExtrapolationIsNoted)                       // X10: p=-10 and p=30 above carry PointNote{i, Extrapolated}; p=5 does not.
                                                             // Bracketing Linear with f < 0 or f > 1 likewise
TEST(FluxModels, MatchingTakesTheNearestMonitor)             // j and j_err of the nearest; tie -> the earlier monitor
TEST(FluxModels, NearestNeighborsIsTheInverseVarianceMeanOfN)// n=3: j == sum(j/e^2)/sum(1/e^2), j_err == sum(1/e^2)^-0.5
TEST(FluxModels, BracketingAverageUsesTheSampleSd)           // X8: J 1 and 3 -> j == 2, j_err == sqrt(2) (n-1), not 1
TEST(FluxModels, WeightedMeanGivesEveryPointTheSameJ)        // J 80, 90 with errors 20^-0.5, 30^-0.5 -> 86.0, Sem 50^-0.5; WeightedMean1D identical;
                                                             // mswd and dof (n-1) reported; Sd allowed here (F13)
TEST(FluxModels, MinimumMonitors)                            // Plane 4, Bowl 6, LeastSquares1D degree+2, WeightedMean 1, WeightedMean1D 1,
                                                             // Matching 1, NearestNeighbors n_neighbors, Bracketing 2, Bracketing1D 2
TEST(FluxModels, TooFewMonitorsIsAnErrorNamingTheCount)      // X4/X11: NearestNeighbors n=3 with 2 monitors ->
                                                             // what contains "nearest neighbors needs 3 monitor positions, 2 used"
TEST(FluxModels, ZeroErrorInAWeightedModelNamesTheMonitor)   // X5: WeightedMean with monitor "7" j_err 0 -> what contains "7"
TEST(FluxModels, NonFiniteInputNamesTheMonitor)              // Review Focus 2: NaN j, inf j_err, NaN x -> error containing the label;
                                                             // a NaN predict_at point -> error containing its index
TEST(FluxModels, Bracketing1DWithCoincidentMonitors)         // x1 == x0: f == 0, j == j0, no NaN
TEST(FluxModels, BadOptionsAreErrors)                        // n_neighbors < 1, degree outside 1..4
```

- [ ] **Step 2: Run to verify failure** (compile error: no `fit_flux`).

- [ ] **Step 3: Implement.** Validate first (finite inputs, option ranges, `minimum_monitors`). Nearness: plain Euclidean distance; a stable sort so a tie keeps monitor order. Bracketing Linear: `f = ((p - p0) . (p1 - p0)) / |p1 - p0|^2`, unclamped; `|p1 - p0| == 0` -> `f = 0`. Bracketing1D: project on `axis`, sort, `hi = clamp(upper_bound, 1, n - 1)`, `lo = hi - 1`. The mean kinds call `weighted_mean(js, errs, options.error)` and set `mswd`, `dof = n - 1`, and `MswdOutsideLimits` (point 0) when `!mswd_acceptable` and `n > 1`. Error text uses lower-case model names: "plane", "bowl", "weighted mean", "matching", "nearest neighbors", "bracketing", "least squares 1D", "weighted mean 1D", "bracketing 1D".

- [ ] **Step 4: Run.** `--gtest_filter='Flux*'` -> all pass.

- [ ] **Step 5: Commit.** `git commit -m "feat(reduction): the neighbour and mean flux models"`

---

### Task 3: The least-squares models, with reference numbers

**Files:**
- Create: `libs/reduction/src/least_squares.hpp`, `tools/flux_reference.py`, `tests/reduction/flux_golden.hpp`
- Modify: `libs/reduction/src/fits.cpp` (remove its private `Matrix`, `LeastSquares`, `least_squares`; include the new header), `libs/reduction/src/flux.cpp`
- Test: `tests/reduction/test_flux.cpp`

**Interfaces:**
- Consumes: `fit_flux` and types of Task 2.
- Produces: `libs/reduction/src/least_squares.hpp`, namespace `pychron::reduction::detail`: `using Matrix = std::vector<std::vector<double>>; struct LeastSquares { std::vector<double> beta; Matrix cov_unscaled; }; Result<LeastSquares> least_squares(Matrix a, std::vector<double> y);` (`inline`, the body moved unchanged from `fits.cpp:23-103`).
- `FluxFit::parameters` order: Plane `[a, b, c]` for `a x + b y + c`; Bowl `[a, b, c, d, e]` for `a x^2 + b y^2 + c x + d y + e`; LeastSquares1D highest power first, constant last.

- [ ] **Step 1: Move the solver.** Cut `Matrix`, `LeastSquares`, `least_squares` from `fits.cpp` into `least_squares.hpp`; `fits.cpp` adds `using detail::least_squares; using detail::LeastSquares; using detail::Matrix;`. Build and run `--gtest_filter='Fit*:Fits*'`: unchanged, all pass. Commit `refactor(reduction): the least-squares solver in its own header`.

- [ ] **Step 2: Write `tools/flux_reference.py`** (numpy only; `python3 tools/flux_reference.py > tests/reduction/flux_golden.hpp`). It defines one fixed monitor set and prediction points and, for each case below, writes out the formulas of S5.3 directly: `W = diag(1/e^2)` or identity; `beta = (X'WX)^-1 X'Wy`; `C = (X'WX)^-1`; `r = y - X beta`; `s2 = r'Wr/(n-q)`; reported `mswd = sum((r/e)^2)/(n-q)`; variance `x C x'` (weighted Sem), `x C x' * max(s2, 1)` (weighted Msem), `s2 * x C x'` (unweighted, both kinds).

Monitor set (x, y, J, sigma), eight holes on a ring of radius 10 at 45 degree steps starting at 0, with `J = 1e-3 * (1 + 0.002 x - 0.001 y)` plus the fixed offsets `[+2, -1, +3, -2, +1, -3, +2, -1] * 1e-7` and sigma `[2, 3, 2, 4, 2, 3, 2, 4] * 1e-7`. Prediction points: `(0, 0)`, `(5, 5)`, `(-7, 2)`, `(10, 0)`.

Cases, each emitted as a `struct` of arrays (`parameters`, `mswd`, `dof`, `j[4]`, `j_err[4]`) printed with `repr` (17 significant digits): `plane_unweighted_sem`, `plane_weighted_sem`, `plane_weighted_msem`, `bowl_unweighted_sem`, `bowl_weighted_msem`, `ls1d_x_degree1_weighted_msem`, `ls1d_y_degree2_unweighted_sem`. The header also carries the monitor set and the points, so the test does not retype them. The file starts with a comment naming the script and command. The script prints nothing else and is not run in CI.

- [ ] **Step 3: Write the failing tests:**

```cpp
TEST(FluxLeastSquares, MatchesTheReference)     // every case of flux_golden.hpp: parameters, mswd, j and j_err to 1e-10 relative; dof exact
TEST(FluxLeastSquares, PlaneRecoversAKnownPlane)   // legacy error_propagation.py:809-880: z = x + 2y on a 5x5 grid ->
                                                   // parameters == [1, 2, 0] (1e-12), predict (1,1) == 3
TEST(FluxLeastSquares, BowlRecoversAKnownBowl)     // z = x + 2y on the same grid -> [0, 0, 1, 2, 0]
TEST(FluxLeastSquares, MswdIsAboutTheSurfaceNotTheMean)  // X1: exact plane with a steep gradient, sigma 1e-7 -> mswd < 1e-6
TEST(FluxLeastSquares, WeightedIsHonouredByAllThree)     // X3: for Plane, Bowl and LeastSquares1D, weighted and unweighted
                                                         // parameters differ on the reference set
TEST(FluxLeastSquares, SdIsAnError)                      // F13/X13: error == Sd for each of the three ->
                                                         // what contains "sd is not an error kind of a fitted surface"
TEST(FluxLeastSquares, OneFewerThanTheMinimumIsAnError)  // X4/X12: Plane with 3 -> "plane needs 4 monitor positions, 3 used";
                                                         // Bowl with 5; LeastSquares1D degree 2 with 3
TEST(FluxLeastSquares, MonitorsThatDoNotDetermineTheSurface) // Review Focus 1: Plane with 4 collinear monitors; Plane with two
                                                         // of 4 at the same x,y and the rest collinear with them; LeastSquares1D
                                                         // with every monitor at the same x -> error containing
                                                         // "monitor positions do not determine"; no NaN anywhere
TEST(FluxLeastSquares, ZeroErrorInAWeightedFitNamesTheMonitor)   // X5
TEST(FluxLeastSquares, MswdOutsideLimitsIsNoted)         // offsets scaled x20 -> notes contains {0, MswdOutsideLimits}
```

- [ ] **Step 4: Run to verify failure** ("flux: model not implemented").

- [ ] **Step 5: Implement.** Weighted: scale each row of `X` and `y` by `1 / j_err` and call `detail::least_squares`; its `cov_unscaled` is then `(X'WX)^-1`. Unweighted: call it on `X`, `y` as they are. A "singular design matrix" or "underdetermined" error from the solver becomes `flux: monitor positions do not determine a <model>`. `dof = n - q`. Variance per S5.3; `Sd` is rejected before anything is solved.

- [ ] **Step 6: Run.** `--gtest_filter='Flux*'` and the whole `pychron_reduction_tests` -> all pass.

- [ ] **Step 7: Commit.** `git commit -m "feat(reduction): plane, bowl and 1-D least-squares flux models"`

---

### Task 4: Monitor sets

**Files:**
- Create: `libs/processing/adapters/store/include/pychron/processing/flux_store.hpp`, `libs/processing/adapters/store/src/flux_monitors.cpp`, `tests/processing/flux_store_fixture.hpp`, `tests/processing/test_flux_store.cpp`
- Modify: `libs/processing/CMakeLists.txt` (link `nlohmann_json::nlohmann_json` PRIVATE to `pychron_processing_store`), `tests/processing/CMakeLists.txt` (add `test_flux_store.cpp` to `pychron_processing_store_tests`), `libs/processing/include/pychron/processing/flux_fit.hpp` (create with `MonitorSet` only)

**Interfaces:**
- Consumes: the document pattern of `libs/entry/src/settings.cpp` (`kSettingsKey`, `load_settings`, `save_settings`): a `RefType::Document` object, payload `DocumentValue::content_json`.
- Produces, in `flux_fit.hpp` (namespace `pychron::processing`):

```cpp
struct MonitorSet {
  std::string name, sample, material;
  double age_ma = 0, age_err_ma = 0;
  reduction::Measured lambda_ec, lambda_b;                 // 1/a
  reduction::Measured lambda_k() const;                    // sum, errors in quadrature
  reduction::MonitorConstants constants() const;           // {age_ma * 1e6, lambda_k().value}
  friend bool operator==(const MonitorSet&, const MonitorSet&) = default;
};
```

- Produces, in `flux_store.hpp`:

```cpp
inline constexpr std::string_view kFluxMonitorsKey = "pychron/flux_monitors.json";
struct MonitorSets {
  std::string default_name;
  std::vector<MonitorSet> sets;
  std::string other_json = "{}";
  const MonitorSet* find(std::string_view name) const;     // empty name: the default
  friend bool operator==(const MonitorSets&, const MonitorSets&) = default;
};
MonitorSets default_monitor_sets();                         // the two sets of S4, verbatim
Result<MonitorSets> parse_monitor_sets(std::string_view json);
std::string to_json(const MonitorSets&);
struct LoadedMonitorSets { MonitorSets sets; std::optional<persistence::Uuid> ref_object, head; };
Result<LoadedMonitorSets> load_monitor_sets(persistence::IStore&);
Result<persistence::CommitOutcome> save_monitor_sets(persistence::IStore&, const persistence::Actor&,
                                                     const MonitorSets&, const LoadedMonitorSets&);
```

Use the field names `reduction::Measured` really has (check `arar_types.hpp`).

- [ ] **Step 1: Write `flux_store_fixture.hpp`**: `class FluxStoreTest : public ::testing::Test` opening a temp-file SQLite store as `StoreSourceTest` does (`tests/processing/test_store_source.cpp:30-52`), registering a reduction client and user, exposing `store()`, `actor()`, `url()`. When `PYCHRON_TEST_PG_URL` is set, a second parameterization opens PostgreSQL in a throwaway schema the way the persistence tests do (follow `tests/persistence`'s helper; reuse it if it is a header).

- [ ] **Step 2: Write the failing tests** (suite `FluxMonitors`):

```cpp
TEST_F(FluxMonitors, AStoreWithNoDocumentHasTheTwoDefaults)
  // load: sets.size() == 2, default_name == "FC-2 (Kuiper 2008)", !ref_object;
  // [0]: sample "FC-2", material "sanidine", age_ma 28.201, age_err_ma 0.046,
  //      lambda_ec {5.80e-11, 9.9e-13}, lambda_b {4.883e-10, 1.4e-12};
  // [1]: name "FC-2 (Renne 1998)", age_ma 28.02, age_err_ma 0.16, lambda_ec {5.81e-11, 0}, lambda_b {4.962e-10, 0}
TEST_F(FluxMonitors, LambdaKIsTheSumWithErrorsInQuadrature)   // [0]: 5.463e-10, sqrt(9.9e-13^2 + 1.4e-12^2)
TEST_F(FluxMonitors, SaveThenLoadRoundTripsAndKeepsUnknownKeys)  // parse '{"default":..,"monitors":[..],"lab":"NMGRL"}' -> save -> load == ; "lab" survives
TEST_F(FluxMonitors, ASaveOnAStaleHeadIsAConflict)             // two loads, two saves: the second is persistence::Conflict
TEST(FluxMonitorsParse, RejectsWhatIsNotValid)
  // each -> error whose `what` names the key: not an object; "monitors" not an array; a set with no "name";
  // two sets with the same name; "default" naming no set; age_ma <= 0; age_err_ma < 0; lambda_b not a [value, sigma] pair;
  // a negative sigma; an empty "monitors"
```

- [ ] **Step 3: Run to verify failure.** `cmake --build build/dev --target pychron_processing_store_tests -j 10` fails to compile.

- [ ] **Step 4: Implement** `flux_monitors.cpp`, copying the load/save shape of `libs/entry/src/settings.cpp`. A parse failure of a stored document is an error (never silently the defaults).

- [ ] **Step 5: Run.** `build/dev/tests/processing/pychron_processing_store_tests --gtest_filter='FluxMonitors*'` -> pass.

- [ ] **Step 6: Commit.** `git commit -m "feat(processing): flux monitor sets as a revisioned document"`

---

### Task 5: `fit_level`

**Files:**
- Modify: `libs/processing/include/pychron/processing/flux_fit.hpp`
- Create: `libs/processing/src/flux_fit.cpp`
- Test: `tests/processing/test_flux_fit.cpp` (globbed into the processing tests; check `tests/processing/CMakeLists.txt` and add it if the list is explicit)

**Interfaces:**
- Consumes: Tasks 1-3; `MonitorSet` (Task 4).
- Produces (namespace `pychron::processing`):

```cpp
struct FluxOptions {
  reduction::FitOptions fit;
  reduction::MeanKind mean = reduction::MeanKind::Arithmetic;
  reduction::MeanErrorKind mean_error = reduction::MeanErrorKind::Msem;
  friend bool operator==(const FluxOptions&, const FluxOptions&) = default;
};
std::string_view legacy_model_name(reduction::ModelKind) noexcept;            // "Plane", "Nearest Neighbors", ...
std::optional<reduction::ModelKind> parse_model_kind(std::string_view) noexcept;
   // the legacy strings and the CLI spellings: plane, bowl, weighted-mean, matching, nearest, bracketing, ls1d, mean1d, bracketing1d

struct LevelAnalysis {
  std::string uuid, record_id, tag;
  std::optional<reduction::UFloat> f;   // nullopt: the reduction failed
  std::string reduction_error;
};
struct SavedFlux {                      // the head flux_position revision of a position, as read
  std::string revision;                 // uuid text; the CAS expectation
  std::optional<double> j, j_err, mean_j, mean_j_err, mean_j_mswd;
  std::optional<FluxOptions> options;   // nullopt: none saved, or not one of the nine models
  std::optional<bool> used_in_fit;
  std::string monitor_set;              // options' monitor_reference; may be empty
  std::set<std::string> omitted;        // record ids saved with is_omitted
  std::string saved_by, saved_utc;      // for `show` and conflicts
};
struct LevelPosition {
  int hole = 0;
  std::string position_uuid, identifier, sample;
  double x = 0, y = 0;
  bool monitor = false;
  std::vector<LevelAnalysis> analyses;  // monitors only
  std::optional<SavedFlux> saved;
};
struct LevelInputs {
  std::string irradiation, level, holder;
  MonitorSet monitor_set;
  std::vector<LevelPosition> positions;        // by hole
  std::optional<FluxOptions> saved_options;    // of the level's last fit (any monitor position's)
};
struct Edits {
  std::set<std::string> omit, include;
  std::set<int> exclude_positions;
  bool reset_omits = false;
};
enum class PositionNote { Extrapolated, MeanMswdOutsideLimits, NoUsableAnalysis, LeftOutOfFit, AnalysisRejected, AnalysisNotReduced };
struct FittedPosition {
  int hole = 0; std::string position_uuid, identifier, sample; double x = 0, y = 0; bool monitor = false;
  int n = 0;
  std::optional<double> saved_j, saved_j_err, mean_j, mean_j_err, mean_j_mswd;
  double j = 0, j_err = 0;
  std::optional<double> dev_percent;          // (saved - predicted) / predicted * 100
  bool used_in_fit = false;
  struct UsedAnalysis { std::string uuid, record_id; bool omitted = false; };
  std::vector<UsedAnalysis> analyses;
  std::vector<PositionNote> notes;
  std::vector<std::string> rejected;          // record ids
  std::optional<std::string> saved_revision;
};
struct LevelFit {
  std::string irradiation, level, holder;
  MonitorSet monitor_set;
  FluxOptions options;
  std::vector<FittedPosition> positions;      // by hole
  std::vector<double> parameters;
  double mswd = 0; int dof = 0; bool mswd_outside_limits = false;
  double min_j = 0, max_j = 0, delta_j_percent = 0;   // (max - min) / max * 100 over predicted J
};
Result<LevelFit> fit_level(const LevelInputs&, const FluxOptions&, const Edits&);
```

- [ ] **Step 1: Write the failing tests** (suite `FluxFitLevel`), on a hand-built `LevelInputs`: the eight ring monitors of `flux_golden.hpp` (three analyses each, F chosen so the arithmetic mean J is the golden J), four unknowns at the golden prediction points.

```cpp
TEST(FluxFitLevel, TablesEqualTheMathLayer)        // plane, weighted: each unknown's j, j_err == fit_flux on the monitors' mean_j; fit mswd, dof equal
TEST(FluxFitLevel, MonitorsGetAPredictedJToo)      // F8: monitor j is the model's, mean_j its own; dev_percent from saved
TEST(FluxFitLevel, TagsStartAnAnalysisOmitted)     // tags "omit","invalid","outlier","skip" -> omitted; "ok" not; n counts used
TEST(FluxFitLevel, IncludeOverridesATagAndASavedOmission)
TEST(FluxFitLevel, SavedOmissionsAndExclusionsApplyUnlessReset)   // F9: saved.omitted and saved.used_in_fit == false honoured; reset_omits clears both
TEST(FluxFitLevel, AnExcludedMonitorStillGetsAPredictedJ)         // used_in_fit false, note LeftOutOfFit, j from the others
TEST(FluxFitLevel, AMonitorWithNoUsableAnalysisIsLeftOutAndNoted) // all omitted -> NoUsableAnalysis, no mean, still predicted
TEST(FluxFitLevel, AFailedReductionTakesNoPartAndIsNoted)         // f == nullopt -> AnalysisNotReduced
TEST(FluxFitLevel, LevelSummary)                   // min_j, max_j, delta_j_percent over all positions
TEST(FluxFitLevel, NoMonitorsIsAnError)            // what contains "no monitor positions"
TEST(FluxFitLevel, TooFewUsedMonitorsIsTheModelsError)   // bowl with 5 used -> "bowl needs 6 monitor positions, 5 used"
TEST(FluxFitLevel, UnknownRecordIdOrHoleIsAnError) // Review Focus 3: omit {"99999-01"} -> what contains "99999-01" and "not an analysis of";
                                                   // include likewise; exclude_positions {99} -> what contains "99" and the level's holes
TEST(FluxFitLevel, ModelNames)                     // legacy_model_name / parse_model_kind round trip for all nine, CLI spellings, "RBF" -> nullopt
```

- [ ] **Step 2: Run to verify failure** (compile).

- [ ] **Step 3: Implement.** Monitors passed to `fit_flux` in hole order with `label` the hole number. `fit_level` never reads `LevelInputs::saved_options`: the caller resolves options (Task 8).

- [ ] **Step 4: Run.** `--gtest_filter='FluxFitLevel.*'` -> pass.

- [ ] **Step 5: Commit.** `git commit -m "feat(processing): fit the flux of a level from its inputs"`

---

### Task 6: `load_level` and the options JSON

**Files:**
- Modify: `flux_store.hpp`
- Create: `libs/processing/adapters/store/src/flux_options.cpp`, `libs/processing/adapters/store/src/flux_store.cpp`
- Test: `tests/processing/test_flux_store.cpp`, `tests/processing/flux_store_fixture.hpp`

**Interfaces:**
- Consumes: `IStore::irradiations`, `levels`, `level_sheet`, `ref_objects`, `head`, `load_payload`, `history` (`persistence/store.hpp`); `IAnalysisSource::browse`, `load` (`processing/source.hpp`); `reduce` (`processing/reduced.hpp`); Tasks 4-5.
- Produces:

```cpp
struct FluxOptionsDoc {                 // S6.4
  std::optional<FluxOptions> options;   // nullopt: no model_kind, or not one of the nine
  std::string monitor_set, monitor_sample;
  std::optional<bool> used_in_fit;
  bool sd_replaced = false;             // F13: a least-squares model saved with SD reads as Msem
};
FluxOptionsDoc parse_flux_options(std::string_view options_json);    // tolerant: never fails
std::string flux_options_json(const FluxOptions&, const MonitorSet&, bool used_in_fit, double fit_mswd, int fit_dof,
                              std::string_view software);

struct MonitorSelection { std::string monitor_set; std::optional<std::string> sample; bool all_positions = false; };
Result<LevelInputs> load_level(IAnalysisSource&, persistence::IStore&, std::string_view irradiation,
                               std::string_view level, const MonitorSelection&);
```

- [ ] **Step 1: Extend the fixture** with `seed_level()`: irradiation `NM-300`, a 12-hole holder published as an `irradiation_holder` reference (hole ids `"1"`..`"12"`: the eight golden ring holes then the four golden prediction points), level `A` on it, sample `FC-2` (sanidine) in holes 1-8 with identifiers `66001`..`66008` and three ingested analyses each, sample `unk` in holes 9-12 with identifiers `66101`..`66104`. Analyses are ingested as `StoreSourceTest::seed` does, with isotope intercepts chosen so F is a known number, plus production and chronology references so the reduction succeeds. Helpers: `save_flux(hole, FluxValue)` (publishes a `flux_position` revision), `tag(record_id, name)`.

- [ ] **Step 2: Write the failing tests:**

```cpp
TEST(FluxOptionsJson, RoundTrip)             // every FluxOptions field, monitor set name and sample, used_in_fit; keys are S6.4's
TEST(FluxOptionsJson, ReadsALegacyDict)      // {"model_kind":"Bowl","use_weighted_fit":true,"predicted_j_error_type":"SEM",
                                             //  "interpolation_style":"Linear","monitor_reference":"FC Min"} -> Bowl, weighted, Sem, Linear, "FC Min"
TEST(FluxOptionsJson, LegacyMsemString)      // "SE but if MSWD>1 use SE * sqrt(MSWD)" -> Msem
TEST(FluxOptionsJson, UnknownModelOrGarbageMeansNoOptions)   // "RBF", "", "{}", "not json", "[]" -> options == nullopt, no failure
TEST(FluxOptionsJson, SdOnASurfaceReadsAsMsem)               // F13: Plane + "SD" -> Msem, sd_replaced

TEST_F(FluxLoadLevel, PositionsMonitorsAndGeometry)   // 12 positions by hole; 1-8 monitor with 3 analyses and f present; 9-12 not; x,y from the holder
TEST_F(FluxLoadLevel, GeometryIsByHoleIdNotByIndex)   // publish the holder with its holes in reverse order -> same x,y per hole
TEST_F(FluxLoadLevel, MonitorSetResolution)           // none named, nothing saved -> the document default; a saved fit naming
                                                      // "FC-2 (Renne 1998)" -> that; named explicitly -> that; unknown name ->
                                                      // error containing the name and both available names (Review Focus 3)
TEST_F(FluxLoadLevel, SampleOverrideAndAllPositions)  // sample "unk" -> 9-12 are the monitors; all_positions -> every position with
                                                      // analyses is a monitor and none is an unknown
TEST_F(FluxLoadLevel, ReadsTheSavedFit)               // after save_flux with options and analyses: saved j, options, omitted, used_in_fit,
                                                      // revision; LevelInputs::saved_options set
TEST_F(FluxLoadLevel, ASavedRevisionThatIsOnlyAJ)     // Review Focus 4: FluxValue{j, j_err} only -> saved->j set, options nullopt,
                                                      // omitted empty, saved_options nullopt; fit_level then runs with defaults
TEST_F(FluxLoadLevel, TagsAreCarried)                 // tag("66001-02", "omit") -> that analysis's tag == "omit"
TEST_F(FluxLoadLevel, ErrorsNameTheCause)             // unknown irradiation; unknown level; level without holder
                                                      // ("level A of NM-300 has no holder"); a position whose hole the holder lacks
                                                      // ("hole 12 is not on holder ...")
TEST_F(FluxLoadLevel, AnImportedLegacyLevelLoadsAndRefits)   // a FluxValue shaped as the importer writes it (legacy options dict,
                                                      // analyses with is_omitted, lambda_k_total): loads as saved, fit_level succeeds
```

- [ ] **Step 3: Run to verify failure** (compile).

- [ ] **Step 4: Implement.** Monitor analyses: `source.browse` with `irradiations`, `levels`, `identifiers` of the monitor positions and `exclude_tags` empty (tags decide omission, not presence; `invalid` analyses are loaded and start omitted), paging until `next` is empty; `source.load` then `reduce`; `f` from `ReducedAnalysis::arar->f` (use the value member `FResult` really has), else `reduction_error`. The position's flux reference: `ref_objects(RefType::FluxPosition, irradiation)` matched by key `<irrad>/<level>/<pos>`, then `head` and `load_payload`; `saved_by` and `saved_utc` from `history`. Parsing uses `parse_flux_options`.

- [ ] **Step 5: Run.** `--gtest_filter='FluxOptionsJson.*:FluxLoadLevel.*'` -> pass.

- [ ] **Step 6: Commit.** `git commit -m "feat(processing): load a level's monitors, geometry and saved flux"`

---

### Task 7: `save_level`

**Files:**
- Modify: `flux_store.hpp`, `libs/processing/adapters/store/src/flux_store.cpp`
- Test: `tests/processing/test_flux_store.cpp`

**Interfaces:**
- Consumes: `IStore::begin`, `IUnitOfWork::add_revision`, `move_head`, `commit`, `add_ref_object`; Tasks 5-6.
- Produces:

```cpp
struct SaveSelection { std::set<int> skip_positions; };
struct SaveOutcome {
  int written = 0, unchanged = 0, skipped = 0;
  std::optional<persistence::Conflict> conflict;   // set: nothing was written
  std::string conflict_position;                   // "hole 7"
};
Result<SaveOutcome> save_level(persistence::IStore&, const persistence::Actor&, const LevelFit&,
                               const SaveSelection&, std::string_view software);
persistence::FluxValue flux_value_of(const LevelFit&, const FittedPosition&, std::string_view software);
```

- [ ] **Step 1: Write the failing tests** (suite `FluxSaveLevel`, on `seed_level()`):

```cpp
TEST_F(FluxSaveLevel, WritesOneRevisionPerPositionInOneChangeset)
  // 12 written; each head FluxValue: j, j_err == predicted; monitors have mean_j, mean_j_err, mean_j_mswd, unknowns do not;
  // lambda_k_total 5.463e-10 and its error; monitor_name "FC-2 (Kuiper 2008)", monitor_material "sanidine",
  // monitor_age 28.201, monitor_age_err 0.046; position_jerr == nullopt (F5); analyses with is_omitted;
  // all 12 revisions share one changeset whose message == "fit flux for NM-300A" and kind == Reference
TEST_F(FluxSaveLevel, ThenLoadShowsTheSavedJ)            // load_level: saved->j == predicted; saved_options == the options used
TEST_F(FluxSaveLevel, ASecondSaveWritesNothing)          // written 0, unchanged 12; no new changeset (latest_change_seq unchanged)
TEST_F(FluxSaveLevel, OmissionsAndExclusionsSurviveSaveAndRefit)   // fit with omit + exclude -> save -> load -> fit with empty Edits
                                                         // == the first fit; with reset_omits it differs
TEST_F(FluxSaveLevel, SkippedPositionsKeepTheirHead)     // skip {9}: hole 9's head unchanged, skipped 1, written 11
TEST_F(FluxSaveLevel, APositionWithNoReferenceObjectGetsOne)   // fresh level: ref_objects gains key "NM-300/A/9"
TEST_F(FluxSaveLevel, AMovedHeadIsAConflictAndNothingIsWritten)
  // load, fit; publish another revision on hole 7; save -> conflict set, conflict_position "hole 7", written 0,
  // and every other head is what it was
TEST_F(FluxSaveLevel, AnUnknownsAgeChangesAndAPinnedOneDoesNot)
  // ingest an analysis on 66101 and one on 66102 with its flux pinned to the old revision (as test_store_source.cpp pins);
  // save a fit with a different J; reload both through a StoreSource: 66101's age changed, 66102's did not
```

- [ ] **Step 2: Run to verify failure** (compile).

- [ ] **Step 3: Implement.** One `begin(actor)`. For each position not skipped: build `flux_value_of`; if it equals the head's payload count it unchanged; else `add_revision(object, Kind::RefValue, payload, expected_head)` with the expectation read at load (`saved_revision`), creating the reference object first when there is none (`RefObjectSpec` scoped to the irradiation, level and position as the importer scopes it; see `libs/dvc/src/meta_adapter.cpp`). If nothing changed, return without committing. `commit(ChangesetKind::Reference, "fit flux for " + irradiation + level)`; a `Conflict` outcome fills `conflict` and the position its subject belongs to. `fit_mswd` and `fit_dof` in the options JSON are the level's; `used_in_fit` the position's.

- [ ] **Step 4: Run.** `pychron_processing_store_tests` whole -> pass (also with `PYCHRON_TEST_PG_URL` if a PostgreSQL with PostGIS is at hand; say in the commit body whether it was).

- [ ] **Step 5: Commit.** `git commit -m "feat(processing): save a level's flux as one changeset"`

---

### Task 8: `elctl flux fit`

**Files:**
- Create: `apps/elctl/src/flux.hpp`, `apps/elctl/src/flux.cpp`, `apps/elctl/src/flux_stub.cpp`, `apps/elctl/tests/test_flux_cmd.cpp`
- Modify: `apps/elctl/CMakeLists.txt` (add `flux` to the stub handling exactly as `export`: `PYCHRON_ELCTL_FLUX_STUB`, the `REMOVE_ITEM` list, the `EXCLUDE REGEX` alternation `(import|entry|export|flux)`), `apps/elctl/src/cli.cpp` (dispatch `flux` beside `export` at `:220`; a "Flux" block in the help text after the export block)

**Interfaces:**
- Consumes: Tasks 4-7; `elctl::Io`, `kUsage` (`cli.hpp`); the store-opening and actor helpers `export.cpp` and `entry.cpp` use for `--db`; `mark_as_user_file` (`pychron/core/user_file.hpp`); `PYCHRON_ELCTL_VERSION`.
- Produces: `int elctl::flux_command(const std::vector<std::string>& args, Io io);`

Command line (S7; the store is named with `--db <url>` as every store command of `elctl` is):

```
elctl flux fit <irradiation> [<level>] --db <url>
    [--model plane|bowl|weighted-mean|matching|nearest|bracketing|ls1d|mean1d|bracketing1d]
    [--weighted | --unweighted] [--mean arithmetic|weighted] [--mean-error sem|msem|sd] [--fit-error sem|msem|sd]
    [--neighbors N] [--interpolation weighted|average|linear] [--axis x|y] [--degree 1..4]
    [--monitors NAME] [--sample NAME] [--all-positions]
    [--omit RECORD_ID]... [--include RECORD_ID]... [--reset-omits]
    [--exclude-position HOLE]... [--no-save-position HOLE]... [--csv FILE] [--save] [--user NAME]
```

Exit codes, as `export`: 0 done (warnings included); 1 a level could not be fitted, or a save conflicted; 2 usage or a fatal error (no store, bad flag).

Option resolution, per level: start from `LevelInputs::saved_options`, else `FluxOptions{}`; each flag given replaces its one field. A saved least-squares fit read with `sd_replaced` prints the warning `saved fit used SD, which a fitted surface does not have: using msem`.

Output, in this order (exact column heads):

```
NM-300 A   holder 12-hole   monitors FC-2 (Kuiper 2008): 28.201 +/- 0.046 Ma, lambda_k 5.463e-10
model plane, weighted; mean arithmetic (msem); fit error msem

Monitors
hole  identifier  sample  n  saved J  +/-  mean J  +/-  %  MSWD  pred J  +/-  %  dev %  fit
...
Unknowns
hole  identifier  sample  saved J  +/-  pred J  +/-  %  dev %
...
fit MSWD 1.12 (5 dof)   J min 1.0012e-03  max 1.0241e-03  delta 2.24 %
warning: ...
```

`fit` is `yes` or `no`. J in `%.4e`, percentages and MSWD in `%.2f`, an absent value `-`. Warnings, one line each: rejected and unreduced analyses by record id, extrapolated holes, a mean MSWD outside its limits (by hole), the fit MSWD outside its limits, monitors left out of the fit. With `--save`: `saved 12 positions (0 unchanged)`, or `nothing to save: 12 positions unchanged`, or on a conflict `not saved: hole 7 was saved by <user> at <utc> since this fit was loaded` and exit 1.

CSV (`--csv`): header `kind,irradiation,level,hole,identifier,sample,x,y,n,saved_j,saved_j_err,mean_j,mean_j_err,mean_j_mswd,j,j_err,dev_percent,used_in_fit,notes`; `kind` is `monitor` or `unknown`; numbers with 17 significant digits; RFC 4180 quoting (reuse the CSV writer `processing/report.cpp` or `export.cpp` uses if it is reachable; do not write a second quoting rule if one can be shared). With no `<level>`, one file holds every level.

- [ ] **Step 1: Write the failing tests.** `test_flux_cmd.cpp` follows `test_export_cmd.cpp`: the stub test under `#ifndef PYCHRON_ELCTL_HAS_STORE`, else a fixture seeding the level of Task 6 (share `tests/processing/flux_store_fixture.hpp`'s seeding by moving `seed_level` into a header both can include, without a GoogleTest fixture dependency).

```cpp
TEST(FluxCmd, StubWithoutPersistence)            // run_raw({"flux","fit","NM-300","A","--db","sqlite::memory:"}) -> kUsage, "elctl was built without persistence"
TEST_F(FluxCmd, PrintsBothTablesAndWritesNothing)   // exit 0; out contains "Monitors", "Unknowns", "66001", "66101", "model plane";
                                                    // the store's latest_change_seq is unchanged
TEST_F(FluxCmd, SaveThenRepeatSaysUnchanged)        // --save -> "saved 12 positions"; again -> "nothing to save: 12 positions unchanged"
TEST_F(FluxCmd, OptionsComeFromTheSavedFit)         // --model bowl --weighted --save; then plain `fit` prints "model bowl, weighted"
TEST_F(FluxCmd, FlagsReplaceOneFieldOfTheSavedFit)  // after the above, `fit --unweighted` prints "model bowl, unweighted"
TEST_F(FluxCmd, OmitAndExcludeChangeTheFit)         // --omit 66001-02 --exclude-position 3: hole 1 n == 2, hole 3 fit "no"
TEST_F(FluxCmd, AWholeIrradiationContinuesPastAFailingLevel)   // add level B with no monitors: A's tables printed,
                                                    // err contains "NM-300 B" and "no monitor positions", exit 1
TEST_F(FluxCmd, PerLevelFlagsNeedALevel)            // `fit NM-300 --omit x` -> kUsage, "--omit needs a level"
TEST_F(FluxCmd, WhatIsNotThereIsNamed)              // Review Focus 3: --omit 99999-01 -> exit 1, err contains "99999-01";
                                                    // --no-save-position 99 -> err contains "99"; --monitors nope -> err lists both sets
TEST_F(FluxCmd, BadFlagsAreUsageErrors)             // --model rbf; --degree 9; --fit-error sd with --model plane
                                                    // ("sd is not an error kind of a fitted surface"); no --db; no irradiation
TEST_F(FluxCmd, CsvIsRfc4180AndEveryRowTheHeaderWidth)   // Review Focus 5: rename the monitor sample to `FC-2, "new"`, run with
                                                    // --sample and --csv: parse the file with a real RFC 4180 reader in the test;
                                                    // 13 rows of 19 fields; the sample field reads back exactly
TEST_F(FluxCmd, ASaveConflictExitsOne)              // a hook is not needed: fit+save from two `run_raw` calls cannot race, so test the
                                                    // message path through flux_command's formatter given a SaveOutcome with conflict set
```

- [ ] **Step 2: Run to verify failure** (`pychron_elctl_tests` does not compile).

- [ ] **Step 3: Implement** `flux.cpp` (parse, resolve, `load_level`, `fit_level`, print, optional CSV and `save_level`), `flux_stub.cpp`, the CMake and `cli.cpp` edits. Keep the table printer and the CSV writer as free functions taking a `LevelFit` so the conflict message test can call the formatter directly. A header comment in `flux.hpp` documents the command as `export.hpp` does.

- [ ] **Step 4: Run.** `ctest --preset dev -R 'FluxCmd'` -> pass; then the whole elctl suite.

- [ ] **Step 5: Commit.** `git commit -m "feat(elctl): flux fit, the J of an irradiation level from its monitors"`

---

### Task 9: `elctl flux show`, `history`, `monitors`

**Files:**
- Modify: `apps/elctl/src/flux.cpp`, `apps/elctl/src/flux.hpp`, `apps/elctl/src/cli.cpp` (help text), `apps/elctl/tests/test_flux_cmd.cpp`

**Interfaces:**
- Consumes: `load_level`, `load_monitor_sets`, `save_monitor_sets`, `parse_monitor_sets`, `to_json`; `IStore::history`.

```
elctl flux show <irradiation> <level> --db <url>
elctl flux history <irradiation> <level> [<hole>] --db <url>
elctl flux monitors [list | show NAME | set FILE | default NAME] --db <url> [--user NAME]
```

- `show`: columns `hole  identifier  sample  J  +/-  %  model  saved by  saved (UTC)`; a position with no flux prints `-`.
- `history`: newest first, one line per changeset: `saved (UTC)  by  message  positions`, with the holes it touched; with `<hole>`, one line per revision of that position with its J.
- `monitors` or `monitors list`: `name  sample  material  age (Ma)  +/-  lambda_k`, the default marked `*`. `show NAME` prints that set as JSON. `set FILE` parses and validates the file, saves, prints `saved N monitor sets`. `default NAME` changes the default.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(FluxCmd, ShowListsTheSavedJ)                 // after --save: 12 rows, "Plane", the user; before any save: every J "-"
TEST_F(FluxCmd, HistoryIsNewestFirstByChangeset)    // two saves with different models: two lines, both "fit flux for NM-300A",
                                                    // newest first; `history NM-300 A 9`: two revisions with their J
TEST_F(FluxCmd, MonitorsListsTheDefaults)           // "FC-2 (Kuiper 2008)" with "*", "28.201", "FC-2 (Renne 1998)"
TEST_F(FluxCmd, MonitorsSetAndDefault)              // set a file with a third set -> list shows 3; default <third> -> "*" moves;
                                                    // a following `flux fit` header names the third set
TEST_F(FluxCmd, MonitorsSetRejectsABadFile)         // duplicate names -> exit 2, message names the key, nothing saved;
                                                    // a missing file -> exit 2
TEST_F(FluxCmd, MonitorsDefaultOfAnUnknownNameListsWhatExists)
TEST_F(FluxCmd, UnknownSubcommandIsUsage)           // `flux nope` -> kUsage and the flux help
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** `ctest --preset dev -R 'FluxCmd'` -> pass.
- [ ] **Step 5: Commit.** `git commit -m "feat(elctl): flux show, history and monitor sets"`

---

### Task 10: User guide, agent guide, and the full suite

**Files:**
- Create: `docs/flux.md`
- Modify: `AGENTS.md`, `docs/superpowers/specs/2026-10-06-flux-fitting-design.md` (status line, and a section "12. Implementation notes" recording anything that ended up different from the spec)

- [ ] **Step 1: Write `docs/flux.md`** (S9): the workflow with the commands of Tasks 8-9 as a worked example on one level; each of the nine models in one paragraph with when to use it and how many monitors it needs; the options; how each error is formed (S5.3 in words); what a save writes and what happens to the unknowns' ages and to pinned analyses; monitor sets and how to change them; and a section "How this differs from legacy Pychron" with every row of the spec's 5.4 table and F4, F5, F13.

- [ ] **Step 2: Add to `AGENTS.md`**, in "Build and test", one bullet: where the three layers live (`libs/reduction` `flux.hpp`, `libs/processing` `flux_fit.hpp`, the `processing_store` adapter's `flux_store.hpp`); that `tests/reduction/flux_golden.hpp` is generated by `tools/flux_reference.py` and never edited by hand; that the legacy model strings and the changeset message `fit flux for <irrad><level>` are a file format (imported and new revisions must read alike); that `elctl flux` is split into `flux.cpp` / `flux_stub.cpp`; user guide `docs/flux.md`.

- [ ] **Step 3: Full build and test.**

```bash
cmake --build build/dev -j 10 && ctest --preset dev -j 8
```

Expected: `100% tests passed`. Then, if Qt is installed, `cmake --preset dev-ui && cmake --build build/dev-ui -j 10 && ctest --preset dev-ui -j 8` (nothing in the UI changes, but the UI links `pychron::processing`).

- [ ] **Step 4: Build once without persistence** to prove the stub: `cmake -S . -B build/nopersist -DPYCHRON_PERSISTENCE=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build/nopersist -j 10 --target pychron_elctl_tests pychron_reduction_tests pychron_processing_tests && ctest --test-dir build/nopersist -R 'Flux'`. Expected: the math and `fit_level` tests and `FluxCmd.StubWithoutPersistence` pass; no store test is built.

- [ ] **Step 5: Commit.** `git commit -m "docs: flux fitting user guide"`

- [ ] **Step 6: Land** as `AGENTS.md` says: rebase on `origin/develop`, run the tests again, merge into `develop`, push. Only when the owner asks.
