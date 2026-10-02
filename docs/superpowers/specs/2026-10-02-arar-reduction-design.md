# Ar-Ar reduction: core age pipeline with error propagation

Date: 2026-10-02
Status: Implemented (owner decisions D1-D6 recorded, section 13; implementation
notes and amendments in section 14)
Owner: Jake Ross
Depends on: `2026-09-29-persistence-adr.md` (reduction values are revisioned
payloads), `2026-10-01-dvc-schema-design.md` sections 4 and 6 (payload and
reference-data shapes), `2026-10-02-conditionals-design.md` section 5 (live
Ar-Ar quantities that must keep working).
Reference: legacy pychron at commit `26e77ad17d9e971093f0f82771047f2056cd92b4`
(`/Users/jakeross/Programming/pychron`, read only). Unless a path is given,
citations are under `pychron/processing/`.

## 1. Goals and non-goals

### 1.1 Goals

1. Port the legacy single-analysis age pipeline to `libs/reduction`:
   isotope arithmetic (baseline, blank, IC factor, discrimination, abundance
   sensitivity, optional deadtime), 37Ar/39Ar decay since irradiation,
   interference corrections (K, Ca, Cl), atmospheric and cosmogenic
   components, F, the age equation with its three J variants, K/Ca, K/Cl and
   per-source error components.
2. Full linear (first-order) error propagation with correlations, through a
   small header-only value-with-uncertainty type `UFloat` modelled on Python
   `uncertainties` 3.2.3, which legacy uses everywhere.
3. Numerical parity with legacy, proven by golden vectors that a Python script
   produces by calling the legacy functions, and C++ tests that assert
   agreement to stated tolerances (section 9).
4. Inputs in the shapes the DVC schema stores (intercept, baseline, blank and
   IC-factor rows; production, flux and chronology reference data) so a
   reduction client can feed the database rows straight in.
5. Keep `compute_arar` (live conditionals) working with its current contract,
   implemented on the same equation kernels so live and final values cannot
   drift.

### 1.2 Non-goals (this phase)

Isochrons, plateaus, flux/J calculation, `convert_age`, fractional loss,
weighted means and every other multi-analysis statistic. They are listed with
their legacy locations in section 12. Isotope fitting is not part of this
spec: intercepts come from `fits.hpp` (already ported, `tests/data/series/`
fixtures) or from stored DB rows. No Qt, no I/O, no database access in
`libs/reduction`.

## 2. Legacy map

The pipeline as `ArArAge.calculate_age` runs it (`arar_age.py:443-453`):

| Step | Legacy | C++ (this spec) |
|---|---|---|
| Per-isotope intensity: intercept - baseline - blank, x discrimination x IC | `isotope.py:715-736`, `:820-854` | `corrected_intensity` |
| Abundance sensitivity | `argon_calculations.py:363-372`, called `arar_age.py:589-591` | `abundance_sensitivity_correction` |
| 37/39 decay factors (M&H) | `argon_calculations.py:305-360`, `arar_age.py:455-463` | `decay_factors` |
| Apply decay factors to 37 and 39 | `arar_age.py:596-599` | inside `reduce` |
| Interference corrections | `argon_calculations.py:375-426` | `interference_corrections` |
| Atmospheric / chlorine | `argon_calculations.py:429-487` | `atmospheric_components` |
| Cosmogenic (optional) | `argon_calculations.py:490-513` | `cosmogenic_components` |
| F, rad40, yield, F without irradiation errors | `argon_calculations.py:516-591` | `calculate_f` |
| Age, three J variants | `argon_calculations.py:603-630`, `arar_age.py:658-689` | `age_equation`, `reduce` |
| K/Ca, K/Cl | `arar_age.py:534-566` | inside `reduce` |
| Error components | `arar_age.py:214-229`, `:688-689` | `error_components` |

Data loading that feeds it (DVC path): intercepts `dvc/dvc_analysis.py:707-720`,
manual values `:722-737`, baselines incl. `modifier_error` `:739-751`, blanks
`:688-705`, IC factors `:753-761`, production `:364-368`, chronology
`:370-395`, flux J and `lambda_k` override `dvc/dvc.py:2296-2305`,
`dvc/meta_repo.py:667-708`. Analysis types that skip blank correction:
`pychron_constants.py:264`, applied at `dvc/dvc_analysis.py:779`.

Constants come from `ArArConstants` (`arar_constants.py:27-283`). Its trait
defaults are overridden by user preferences when the preference store has
them (`arar_constants.py:96-141`); the preference defaults differ for some
fields (section 10, Q19). Constants are not stored in the legacy database;
here they become versioned reference data (decision D1).

## 3. Equations

Notation: `nom(x)` is the nominal value of `x` with its uncertainty dropped
(a constant). All argon intensities are in the detector signal unit (fA in
practice); the pipeline is unit-agnostic except where stated. Isotope order is
`ARGON_KEYS = (Ar40, Ar39, Ar38, Ar37, Ar36)` (`pychron_constants.py:286`).

### 3.1 Isotope arithmetic (per isotope `i`)

```
E1  bc_i  = I_i - (include_baseline_error ? B_i : nom(B_i))            isotope.py:715-736
E2  nd_i  = bc_i - (correct_for_blank ? Bk_i : 0)                       isotope.py:848-854
E3  s_i   = nd_i * D_i * IC_i                                           isotope.py:820-835
```

`I` intercept, `B` baseline (per detector, `modifier_error` replaces its
error when present), `Bk` blank, `D` discrimination (1 unless supplied; the
DVC path never sets it), `IC` the detector IC factor. `correct_for_blank` is
false for analysis types starting with `blank`, `detector_ic`, `background`.
`include_baseline_error` is the stored per-intercept flag, default false.
The legacy `background` term (`isotope.py:852-853`) is always zero on the DVC
path and is not ported.

```
E4  abundance sensitivity alpha (argon_calculations.py:363-372):
    n40 = s40 - alpha*(s39 + s39)      n39 = s39 - alpha*(s40 + s38)
    n38 = s38 - alpha*(s39 + s37)      n37 = s37 - alpha*(s38 + s36)
    n36 = s36 - alpha*(s37 + s37)
```

```
E5  deadtime (optional, off by default; formula deadtime.py:54-55; decision D4):
    n   = s_fA * 6241.509          fA -> counts/s (exact e/1e-15 C; legacy tool used 6240, deadtime.py:62)
    n'  = n / (1 - n*tau)          tau in s, from the detector config, per detector
    s'  = n' / 6241.509            applied to the intercept, before E1
```

### 3.2 Decay since irradiation

```
E6  segments (dvc/dvc_analysis.py:370-395, dvc/meta_object.py:99-105):
      t_k  = (end_k - start_k) in days
      dt_k = (analysis_time - (use_irradiation_endtime ? end_k : start_k)) in days
    decay_days = (analysis_time - start_0) in days                   arar_age.py:699-704
                 start_0 = start of the first dose                   dvc/dvc_analysis.py:388-392
E7  McDougall & Harrison eq 3.22 (argon_calculations.py:349-360):
      P   = sum_k p_k t_k
      b   = sum_k p_k (1 - exp(-l37 t_k)) / (l37 exp(l37 dt_k))
      c   = sum_k p_k (1 - exp(-l39 t_k)) / (l39 exp(l39 dt_k))
      df37 = b != 0 ? P / b : 1         df39 = c != 0 ? P / c : 1
    guard: |l * max(|t_k|, |dt_k|)| > 50 for l in {l37, l39} is an error   :253-279
    no segments: df37 = df39 = 1                                          :341-342
E8  a37 = n37 * df37      a39 = n39 * df39                              arar_age.py:596-599
```

`l37`, `l39` are per day (`arar_constants.py:40-45`, docstring
`argon_calculations.py:318-322`) and enter as nominal values only
(`arar_age.py:459-460`). The Dalrymple variant (`:282-302`) is reachable only
with `use_mh=False`, which no caller passes (`arar_age.py:461`); it is not
ported (section 12).

### 3.3 Interference corrections (`argon_calculations.py:399-426`)

Production ratios `K4039, K3839, K3739, Ca3937, Ca3837, Ca3637, Cl3638`
(missing = 0) and `Ca_K, Cl_K` (missing or zero = factor 1).

```
E9  normal mode (k3739_mode == Normal and no per-analysis fixed_k3739):  :410-414
      k39  = (a39 - Ca3937 a37) / (1 - K3739 Ca3937)
      k37  = K3739 k39
      ca37 = a37 - k37
      ca39 = Ca3937 ca37
E10 fixed mode (per-analysis fixed_k3739, else constants.fixed_k3739):  :375-396, :415-418
      x = fixed_k3739;  y = nom(Ca3937) == 0 ? 1 : 1 / Ca3937
      ca37 = a39 x y / (x + y);   ca39 = Ca3937 ca37
      k39  = a39 - ca39;          k37  = x k39
E11 k38 = K3839 k39                                                       :420
    if !allow_negative_ca_correction and !(nom(ca37) > 0): ca37 = 0 (exact)  :421-422
    ca36 = Ca3637 ca37;   ca38 = Ca3837 ca37                              :423-424
```

### 3.4 Atmospheric, chlorine, cosmogenic

```
E12 (argon_calculations.py:468-487)
      lCl   = fresh variable (lambda_Cl36, per day)
      r3836 = fresh variable (atm3836): nominal atm4036/atm4038, std the
              quadrature of their errors; legacy re-wraps the ratio of two
              fresh reads as one ufloat(nom, std, tag="atm3836")
              (arar_constants.py:225-226, argon_calculations.py:470-479)
      m     = Cl3638 * lCl * decay_days
      atm36 = (a36 - ca36 - m (a38 - k38 - ca38)) / (1 - m r3836)
      atm38 = r3836 atm36
      cl38  = a38 - atm38 - k38 - ca38
      cl36  = m cl38
E13 cosmogenic, when enabled (argon_calculations.py:490-513, :542-545):
      rm = atm38 / atm36;  rs = solar3836;  rc = cosmo3836
      fs = (rc - rm) / (rc - rs);  fc = 1 - fs
      noncosmo38 = fs atm38;  cosmo38 = atm38 - noncosmo38
      cosmo36 = fc atm36;     noncosmo36 = atm36 - cosmo36
      atm36 := noncosmo36;    atm38 := noncosmo38
```

### 3.5 F and radiogenic yield (`argon_calculations.py:526-591`)

```
E14 T     = fresh variable (atm4036)            "trapped_4036", :530-531
    atm40 = atm36 T
    k40   = K4039 k39
    rad40 = n40 - atm40 - k40
    F     = rad40 / k39                          :550-553
    yield = 100 rad40 / n40                      :554-557
    interference-corrected: Ar40 = n40 - k40, Ar39 = k39, Ar38 = a38,
                            Ar37 = a37, Ar36 = atm36        :582
E15 F_err_wo_irrad = std of F excluding the seven interference-ratio variables
    (legacy reruns calc_f with zero-error ratios, :585-589; identical by
    linearity, verified numerically to 1e-16 relative)
```

Note `n40` is not decay corrected; `a38 = n38`, `a36 = n36`.

### 3.6 Age (`argon_calculations.py:603-630`, `arar_age.py:658-689`)

```
E16 lambda_K = flux.lambda_k_total if present and not exactly 0 +- 0  dvc/dvc.py:2303-2305
               (Python truthiness, `if lk:`; a 0 +- e override is used)
               ONE variable tagged `lambda_k`: legacy lambda_b + lambda_e,
               sigma in quadrature (no consumer queries the separate tags)
               else lambda_b + lambda_e                            arar_constants.py:263-267
    lambda   = include_decay_error ? lambda_K : nom(lambda_K)      :622-623
    t_years  = ln(1 + J F) / lambda                                 :626
    t        = t_years * scale(age_units)                           arar_constants.py:143-170
E17 J variants (arar_age.py:673-686), all on the same F:
      age                = t(J' = fresh(nom(J), 0))       "J_no_err": analytical error only
      age_w_j_err        = t(J)                            J as supplied ("J")
      age_w_position_err = t(J'' = fresh(nom(J), position_jerr))  "Position"
E18 error budget (new, section 10 Q13):
      age_err_wo_irrad   = std(age) excluding interference-ratio variables
      age_err_wo_j_irrad = same as age_err_wo_irrad (age already has no J error)
```

### 3.7 K/Ca, K/Cl, error components (`arar_age.py:214-229`, `:534-566`)

```
E19 kca = k39 / ca37 * (1 / Ca_K);  cak = 1 / kca
    kcl = k39 / cl38 * (1 / Cl_K);  clk = 1 / kcl
    (Ca_K, Cl_K missing or nominal zero -> factor 1, :560-566)
E20 component(tag) = 100 * sum_{v: tag(v) == tag} (dt/dv * sigma_v)^2 / var(age_w_j_err)
```

Legacy `get_error_component` takes the first variable whose tag matches
(`:220-223`); with the tags in section 5.3 every isotope tag is unique, so the
sum equals legacy.

### 3.8 Units

| Quantity | Unit | Source |
|---|---|---|
| `lambda_b`, `lambda_e`, `lambda_K` | 1/a | `arar_constants.py:29-33` |
| `lambda_Ar37`, `lambda_Ar39`, `lambda_Cl36` | 1/day | `arar_constants.py:38-45` |
| segment `t`, `dt`, `decay_days` | day | `dvc/dvc_analysis.py:373-374` |
| age | a internally, reported in `AgeUnits` (default Ma) | `arar_constants.py:143-170` |
| deadtime `tau` | s, per detector from detector config; fA->cps factor 6241.509 (legacy tool: ns and 6240) | `deadtime.py:62-65` |
| J, F, ratios | dimensionless | |

## 4. UFloat

Header-only: `libs/reduction/include/pychron/reduction/ufloat.hpp`,
namespace `pychron::reduction`.

### 4.1 Model

A `UFloat` is a nominal value plus a sorted list of terms, one per
independent variable it depends on:

```
x = nominal + sum_i d_i * (v_i - nom(v_i))      linear in independent variables v_i
var(x)      = sum_i (d_i * sigma_i)^2
cov(x, y)   = sum_{i shared} d_i^x d_i^y sigma_i^2
```

This is exactly the first-order propagation `uncertainties` performs
(`std_dev` from `error_components`, `covariance_matrix` over shared
`Variable`s). Correlation arises only through shared variables: two UFloats
built from the same `J` or the same blank are correlated; two variables created
separately are independent even if they have the same value and tag.

### 4.2 API

```cpp
namespace pychron::reduction {

using VariableId = std::uint64_t;  // process-unique, never 0, never reused
using TagId = std::uint32_t;       // 0 = untagged

// Interned, process-wide, append-only. Thread-safe. Names are never freed.
TagId intern_tag(std::string_view name);
std::string_view tag_name(TagId tag);  // "" for 0 or unknown

class UFloat {
 public:
  struct Term {
    VariableId id;
    double sigma;   // the variable's standard deviation (immutable)
    double deriv;   // d(this)/d(variable)
    TagId tag;
  };

  UFloat() noexcept = default;          // exact 0
  UFloat(double exact) noexcept;        // implicit: exact constant, no terms

  // New independent variable. sigma must be finite and >= 0 (asserted in
  // debug builds; inputs are validated at the reduction API boundary).
  // sigma == 0 returns an exact constant (no term).
  static UFloat variable(double value, double sigma, TagId tag = 0);
  static UFloat variable(double value, double sigma, std::string_view tag);

  double nominal() const noexcept;
  double std_dev() const noexcept;      // sqrt(variance())
  double variance() const noexcept;
  bool is_exact() const noexcept;       // no terms
  std::span<const Term> terms() const noexcept;  // ascending id
  double derivative(VariableId id) const noexcept;  // 0 if absent
  // Single-term UFloats (fresh variables) expose their id; 0 otherwise.
  VariableId variable_id() const noexcept;

  UFloat& operator+=(const UFloat&);  UFloat& operator-=(const UFloat&);
  UFloat& operator*=(const UFloat&);  UFloat& operator/=(const UFloat&);
  // + - * / binary on (UFloat, UFloat), (UFloat, double), (double, UFloat); unary -.
};

UFloat exp(const UFloat&);   UFloat log(const UFloat&);   UFloat log10(const UFloat&);
UFloat sqrt(const UFloat&);  UFloat abs(const UFloat&);
UFloat pow(const UFloat& x, double y);
UFloat pow(double x, const UFloat& y);
UFloat pow(const UFloat& x, const UFloat& y);

double covariance(const UFloat& a, const UFloat& b);
double correlation(const UFloat& a, const UFloat& b);     // 0 if either std is 0
std::vector<double> covariance_matrix(std::span<const UFloat> xs);  // row-major n*n

// Error budget helpers.
double std_dev_excluding(const UFloat& x, std::span<const VariableId> ids);
double std_dev_excluding_tags(const UFloat& x, std::span<const TagId> tags);
std::vector<std::pair<TagId, double>> error_components(const UFloat& x);  // per tag: sqrt(sum (d sigma)^2), ascending TagId
double variance_percent(const UFloat& x, TagId tag);  // E20; 0 when variance is 0

}  // namespace pychron::reduction
```

No comparison operators. Python's ordering and equality on `UFloat`s are
easy to misuse (`==` is true only when the difference has zero nominal and
zero error); callers compare `nominal()` explicitly. The one legacy use,
`max(ufloat(0, 0), ca37)`, is written out in E11.

### 4.3 Derivative rules

| Op | Nominal | Derivative terms |
|---|---|---|
| `x + y`, `x - y` | `x0 +- y0` | `dx +- dy` |
| `x * y` | `x0 y0` | `y0 dx + x0 dy` |
| `x / y` | `x0 / y0` | `dx / y0 - x0 dy / y0^2` |
| `exp(x)` | `e^x0` | `e^x0 dx` |
| `log(x)` | `ln x0` | `dx / x0` |
| `sqrt(x)` | `sqrt x0` | `dx / (2 sqrt x0)` |
| `pow(x, c)` | `x0^c` | `c x0^(c-1) dx`; `c == 0` gives exact 1; `x0 == 0` and `c > 1` gives derivative 0 |
| `pow(x, y)` | `x0^y0` | `y0 x0^(y0-1) dx + ln(x0) x0^y0 dy`, with the `uncertainties` special cases at `x0 == 0` |

`pow(x, y)` with `x0 == 0` follows `pow(x, c)` for the base partial: 0 for
exponent > 1, IEEE inf/NaN otherwise. Legacy gives NaN for a non-integer
exponent > 1; this documented divergence is pinned by the golden case
`pow_at_zero_non_integer`.

Terms are merged by id in one linear pass. A merged derivative that is exactly
`0.0` is dropped (so `x - x` is exact zero, as `uncertainties` reports
`0+/-0`). Arithmetic follows IEEE: division by an exact-zero nominal yields
inf/NaN nominal and derivatives; `log` of a non-positive nominal yields NaN.
`UFloat` never throws. Domain checks belong to the reduction functions
(section 7), which check divisors before dividing.

### 4.4 Variable identity

- Ids come from one `std::atomic<std::uint64_t>` (`fetch_add`, relaxed),
  starting at 1. Ids are never serialized or compared across processes.
- Copying a `UFloat` copies its terms; both copies refer to the same
  variables. This is how sharing is expressed: build `J` once, pass the same
  `UFloat` to every analysis that shares it.
- A term carries its variable's `sigma` and `tag` inline, so there is no
  variable registry to look up and no lifetime to manage.
- `variable(value, 0)` creates no variable. `uncertainties` does create one;
  the difference is invisible in values, errors, and covariances.

### 4.5 Thread safety

`UFloat` is a value type with no shared mutable state: concurrent reads of one
object and concurrent use of distinct objects are safe; concurrent mutation of
one object is not (same as `std::vector`). `variable()` touches only the
atomic counter. `intern_tag` takes a mutex; `tag_name` reads append-only
storage (`std::deque<std::string>`) under the same mutex. Interning happens
when inputs are built, never inside arithmetic.

### 4.6 Performance

- Terms live in a `std::vector<Term>` (32 bytes each), sorted by id. A binary
  op is an `O(n + m)` merge with one allocation; a full single-analysis
  reduction has about 40 variables and a few hundred ops.
- Target: `reduce()` under 50 us per analysis in a release build on the lab
  machine class (Apple M-series). Checked manually (Task 11, Step 6), not
  gated in CI.
- A small-buffer optimisation is a later option if profiling shows allocation
  dominating; the API does not expose the container.
- Build flags: no `-ffast-math` (it breaks parity and NaN checks).
  `-ffp-contract` differences between compilers (FMA on arm64) are absorbed by
  the tolerances in 4.7. Parity between `compute_arar` and `reduce` (8.2)
  additionally requires `-ffp-contract=off` for the reduction library and
  everything that links it (a PUBLIC option, section 7), so both paths round
  identically.
- Measured: `reduce()` takes 3.1-4.8 us per call on an M5 Pro (dev and Release
  builds), against the 50 us target.

### 4.7 Numerical agreement with `uncertainties`

`uncertainties` evaluates the same analytic partials, so values agree to
rounding. Agreement is asserted as `|got - want| <= atol + rtol * |want|`:

| Quantity | rtol | atol |
|---|---|---|
| nominal values | 1e-12 | per case, default 0 |
| standard deviations, covariances | 1e-10 | per case, default 0 |
| decay factors (plain doubles) | 1e-13 | 0 |
| error-component percentages | 0 | 1e-8 (percentage points) |
| `compute_arar` vs `reduce` nominal | 1e-12 | 0 |

`atol` is set per case only where a quantity is the difference of nearly equal
values (for example `rad40` of an air shot, a cosmogenic fraction near 0); the
generator sets it to `1e-12 * max |input|` there and records why.

## 5. Data model

Header `libs/reduction/include/pychron/reduction/arar_types.hpp`.

### 5.1 Values and stored rows

```cpp
struct Measured { double value = 0.0, error = 0.0; };  // 1 sigma

// One payload row (dvc schema 4.1: intercept_value, baseline_value,
// blank_value, icfactor_value) reduced to what the age needs.
struct StoredValue {
  double value = 0.0, error = 0.0;
  bool use_manual_value = false;  double manual_value = 0.0;
  bool use_manual_error = false;  double manual_error = 0.0;
  std::optional<double> modifier_error;  // baseline_value only
};
// manual_* replace value/error when flagged; modifier_error replaces the error
// (dvc/dvc_analysis.py:722-737, :745-746).
Measured resolve(const StoredValue& row) noexcept;
```

### 5.2 Isotopes

```cpp
enum class ArgonIsotope : std::uint8_t { Ar40, Ar39, Ar38, Ar37, Ar36 };  // ARGON_KEYS order
inline constexpr std::array<ArgonIsotope, 5> kArgonKeys{...};
std::string_view to_string(ArgonIsotope);   // "Ar40" ...
inline constexpr std::size_t index(ArgonIsotope i) noexcept;

struct IsotopeSignal {
  UFloat intercept;                     // tag "<iso>"
  UFloat baseline;                      // tag "<iso> bs"
  bool include_baseline_error = false;  // intercept_value.include_baseline_error
  UFloat blank;                         // tag "<iso> bk"; exact 0 when none
  bool correct_for_blank = true;        // false for blank/detector_ic/background types
  UFloat ic_factor = 1.0;               // tag "<iso> IC"
  UFloat discrimination = 1.0;
  std::optional<double> deadtime_tau_s; // E5, from the detector config; absent = off (D4)
};

struct MeasuredSignal {
  Measured intercept, baseline, blank;
  Measured ic_factor{1.0, 0.0};
  bool include_baseline_error = false, correct_for_blank = true;
  std::optional<double> deadtime_tau_s;
};

// Mints fresh variables with the legacy tags (isotope.py:469, :713, :770;
// dvc/dvc_analysis.py:759). Legacy never shares these between isotopes or
// analyses; callers that want sharing build IsotopeSignal directly.
IsotopeSignal make_signal(ArgonIsotope iso, const MeasuredSignal& m);

// Analysis types whose blank is not subtracted (pychron_constants.py:264).
bool corrects_for_blank(std::string_view analysis_type) noexcept;
```

The isotope key mapping (`H1:Ar40`, peak-hop names) stays with the caller:
`reduce` takes the five argon slots.

### 5.3 Constants

```cpp
enum class K3739Mode { Normal, Fixed };
enum class AgeUnits { a, ka, Ma, Ga };

struct CosmogenicRatios { Measured solar3836, cosmo3836; };

// Plain record (decision D1): every member is value-initialised to zero /
// false / Normal / Ma and carries no physics default. Real values come from
// the arar_constants reference record or from one of the named presets.
struct ReductionConstants {
  Measured lambda_b, lambda_e;                  // 1/a
  Measured lambda_cl36, lambda_ar37, lambda_ar39;  // 1/day
  Measured atm4036, atm4038;                    // E12 reads them as one atm3836 variable
  K3739Mode k3739_mode = K3739Mode::Normal;
  Measured fixed_k3739;
  double abundance_sensitivity = 0.0;
  bool allow_negative_ca_correction = false;
  bool use_irradiation_endtime = false;
  std::optional<CosmogenicRatios> cosmogenic;   // use_cosmogenic_correction
  bool include_decay_error = false;
  AgeUnits age_units = AgeUnits::Ma;
};

enum class ConstantsPreset {
  Default,            // D2/D5: lab default for new configurations
  Legacy,             // ArArConstants trait defaults (arar_constants.py:28-94)
  LegacyPreferences,  // legacy preference-pane defaults (constants/tasks/arar_constants_preferences.py:144-167)
};
ReductionConstants constants_preset(ConstantsPreset p) noexcept;
std::string_view to_string(ConstantsPreset p) noexcept;  // "default", "legacy", "legacy_preferences"
// lambda_b + lambda_e as independent variables (sigma combines in quadrature).
Measured lambda_k(const ReductionConstants& c) noexcept;

inline constexpr std::string_view kReductionVersion = "arar-1";
```

Preset values (fields not listed are equal across presets: `lambda_cl36` 6.308e-9 +- 0, `lambda_ar37` 0.01975 +- 0,
`lambda_ar39` 7.068e-6 +- 0, `atm4038` 1575 +- 2, `fixed_k3739` value 0.01,
`k3739_mode` Normal, `abundance_sensitivity` 0, no cosmogenic,
`include_decay_error` false, `age_units` Ma):

| Field | `Default` | `Legacy` | `LegacyPreferences` |
|---|---|---|---|
| `atm4036` | 298.56 +- 0.31 (Lee et al. 2006) | 295.5 +- 0.5 | 295.5 +- 0 |
| `lambda_e` | 5.81e-11 +- 1.6e-13 | 5.81e-11 +- 1.6e-13 | 5.81e-11 +- 0 |
| `lambda_b` | 4.962e-10 +- 9.3e-13 | 4.962e-10 +- 9.3e-13 | 4.962e-10 +- 0 |
| `fixed_k3739` error | 0.01 | 0.0001 | 0.01 |
| `allow_negative_ca_correction` | false | true | false |

`Default` follows decisions D2 and D5 (non-zero uncertainties, conservative Ca
clamp). The two legacy presets reproduce the two default sets a legacy
installation could have run with; golden cases cover both (section 9.2).
Golden cases always pass every constant explicitly and never rely on a preset.

`kReductionVersion` is the `reduction_version` written into
`derived_value` rows (dvc schema 4.3); bump it whenever a golden vector
changes for a reason other than a bug fix in the test.

Tags used for constants: `trapped_4036`, `atm3836`,
`lambda_Cl36`, `k3739`, `solar3836`, `cosmo3836`, `lambda_k` (legacy tags,
`argon_calculations.py:470-479`, `:530-531`, `arar_constants.py:228-231`).

### 5.4 Production ratios

```cpp
struct ProductionRatios {
  Measured k4039, k3839, k3739, ca3937, ca3837, ca3637, cl3638;  // missing = {0, 0}
  std::optional<Measured> ca_k, cl_k;                            // RATIO_KEYS
};
// Keys INTERFERENCE_KEYS + RATIO_KEYS (pychron_constants.py:277-278), i.e. the
// production_value rows (dvc schema 6.1). Unknown key or non-finite value ->
// ErrorKind::Config.
Result<ProductionRatios> production_from_rows(const std::map<std::string, Measured, std::less<>>& rows);

struct ProductionVariables {
  UFloat k4039, k3839, k3739, ca3937, ca3837, ca3637, cl3638;  // tags = key names
  std::optional<UFloat> ca_k, cl_k;
  std::array<VariableId, 7> interference_ids() const;  // for E15/E18 exclusion
};
// One fresh variable per ratio, once per analysis (meta_object.py:239-243).
ProductionVariables make_production_variables(const ProductionRatios& p);
```

### 5.5 Irradiation and flux

```cpp
struct Dose { double power = 0; std::int64_t start_utc_s = 0, end_utc_s = 0; };  // chronology_dose
struct DecaySegment { double power = 0, duration_days = 0, dt_days = 0; };
struct Irradiation { std::vector<DecaySegment> segments; double decay_days = 0; };

// E6. Doses keep their order; decay_days is from the first dose's start.
Irradiation irradiation_from_doses(std::span<const Dose> doses, std::int64_t analysis_utc_s,
                                   bool use_irradiation_endtime);

struct DecayFactors { double df37 = 1.0, df39 = 1.0; };

struct Flux {                                   // flux_value (dvc schema 6.1)
  Measured j;
  double position_jerr = 0.0;
  std::optional<Measured> lambda_k_total;       // overrides lambda_b + lambda_e unless exactly 0 +- 0 (`if lk:`)
};
UFloat make_j(const Flux& f);                   // tag "J" (dvc/meta_repo.py:701)
```

### 5.6 Reduction input and result

```cpp
struct ReductionInput {
  std::array<IsotopeSignal, 5> isotopes;        // indexed by ArgonIsotope
  ReductionConstants constants;
  ProductionVariables production;
  Irradiation irradiation;
  std::optional<UFloat> j;                      // absent: no ages (arar_age.py:659-660)
  double position_jerr = 0.0;
  std::optional<Measured> lambda_k_total;       // from Flux
  std::optional<Measured> fixed_k3739;          // per-analysis override (arar_age.py:68); exactly 0 +- 0 is unset (`not fixed_k3739`)
};

enum class Diagnostic : std::uint8_t {
  FUndefined,            // k39 == 0; legacy F = 1 +- 0
  YieldUndefined,        // n40 == 0; legacy 0 +- 0
  AgeUndefined,          // 1 + J F <= 0; legacy 0 +- 0
  KCaUndefined,          // ca37 == 0 (kca, cak absent), or ca37 != 0 but kca == 0 (kca kept, cak absent)
  KClUndefined,          // cl38 == 0 (kcl, clk absent), or cl38 != 0 but kcl == 0 (kcl kept, clk absent)
  CaClampedToZero,       // E11 clamp applied
  FixedK3739ZeroCa3937,  // E10 y = 1 fallback
  NonFiniteResult,       // a computed value is NaN/inf
};
std::string_view to_string(Diagnostic);

struct InterferenceComponents { UFloat k37, k38, k39, ca36, ca37, ca38, ca39; };
struct AtmosphericComponents { UFloat atm36, atm38, cl36, cl38; };
struct CosmogenicComponents { UFloat cosmo36, cosmo38, noncosmo36, noncosmo38; };

struct FResult {
  std::optional<UFloat> f;
  double f_err_wo_irrad = 0.0;                  // 0 when f absent
  UFloat atm40, k40, rad40;
  std::optional<UFloat> radiogenic_yield;       // percent
  InterferenceComponents interference;
  AtmosphericComponents atmospheric;            // after cosmogenic split when enabled
  std::optional<CosmogenicComponents> cosmogenic;
  std::array<UFloat, 5> interference_corrected; // E14
  std::vector<Diagnostic> diagnostics;
};

struct AgeSet {
  UFloat age, age_w_j_err, age_w_position_err;  // in constants.age_units
  double age_err_wo_irrad = 0.0, age_err_wo_j_irrad = 0.0;
};

struct ArArResult {
  DecayFactors decay;
  std::array<UFloat, 5> corrected;              // E3-E5, E8 ("corrected_intensities")
  FResult f;
  std::optional<AgeSet> ages;
  std::optional<UFloat> kca, cak, kcl, clk;
  std::map<std::string, double> age_error_components;  // E20 by isotope name, on age_w_j_err
  std::vector<Diagnostic> diagnostics;          // union of all steps, in order
};
```

## 6. Public API

Header `libs/reduction/include/pychron/reduction/arar_reduction.hpp`. All
functions are pure: no I/O, no clocks, no globals except the `UFloat` id
counter and tag table. `Result` errors use `ErrorKind::Config` (the
`fits.hpp` precedent) with messages prefixed `reduction: `.

```cpp
// Section 3.1
UFloat baseline_corrected(const IsotopeSignal& s);                      // E1
UFloat corrected_intensity(const IsotopeSignal& s);                     // E1-E3
std::array<UFloat, 5> abundance_sensitivity_correction(const std::array<UFloat, 5>& s,
                                                       double alpha);   // E4
inline constexpr double kFaToCountsPerSecond = 6241.509;  // D4
Result<UFloat> deadtime_correct(const UFloat& signal_fa, double tau_s,
                                double fa_to_cps = kFaToCountsPerSecond);  // E5; error if 1 - n tau <= 0
// Section 3.2
Result<DecayFactors> decay_factors(double lambda37_per_day, double lambda39_per_day,
                                   std::span<const DecaySegment> segments);  // E7
// Section 3.3
struct InterferenceOptions {
  K3739Mode mode = K3739Mode::Normal;
  std::optional<UFloat> fixed_k3739;            // per-analysis value forces fixed mode
  UFloat constants_fixed_k3739;                 // used when mode == Fixed and no per-analysis value
  bool allow_negative_ca_correction = false;
};
InterferenceComponents interference_corrections(const UFloat& a39, const UFloat& a37,
                                                const ProductionVariables& p,
                                                const InterferenceOptions& o,
                                                std::vector<Diagnostic>* diagnostics = nullptr);  // E9-E11
// Section 3.4
Result<AtmosphericComponents> atmospheric_components(const UFloat& a38, const UFloat& a36,
                                                     const UFloat& k38, const UFloat& ca38,
                                                     const UFloat& ca36, double decay_days,
                                                     const UFloat& cl3638,
                                                     const ReductionConstants& c);  // E12; error if 1 - m r3836 == 0
Result<CosmogenicComponents> cosmogenic_components(const UFloat& c36, const UFloat& c38,
                                                   const CosmogenicRatios& r);       // E13; error if c36 == 0 or rc == rs
// Section 3.5
Result<FResult> calculate_f(const std::array<UFloat, 5>& n, double decay_days,
                            const ProductionVariables& p, const ReductionConstants& c,
                            std::optional<Measured> fixed_k3739 = std::nullopt);    // E9-E15
// Section 3.6
Result<UFloat> age_equation(const UFloat& j, const UFloat& f, const ReductionConstants& c,
                            std::optional<Measured> lambda_k_total = std::nullopt);  // E16; error if 1 + J F <= 0
double age_scale(AgeUnits from, AgeUnits to) noexcept;
// Whole pipeline
Result<ArArResult> reduce(const ReductionInput& in);
```

`reduce` fails (`Result` error) only for invalid input (non-finite or
negative sigma anywhere, non-finite value, negative `abundance_sensitivity`,
deadtime `tau < 0`, a zero `lambda_b + lambda_e`) or a legacy crash case (E7 guard, E12 or E13 zero
divisor). The `lambda_b + lambda_e == 0` check is exempted when a truthy
`lambda_k_total` override is present. Undefined derived quantities (F, yield, age, ratios) are absent with
a `Diagnostic`, never errors, so the rest of the result is still usable.

## 7. Numerical policy

| Situation | Legacy | Decision |
|---|---|---|
| Non-finite intercept from a fit | coerced to 0 (`isotope.py:465-468`) | **Fix**: `reduce` returns an error naming the slot. |
| Negative signal (blank > signal, negative baseline-corrected 36) | propagated | Replicate. No clamping except E11. `yield` may exceed 100 or go negative. |
| `k39 == 0` | `F = 1 +- 0` (`argon_calculations.py:550-553`) | **Fix**: F, ages absent; `FUndefined`. |
| `n40 == 0` | `yield = 0 +- 0` (`:554-557`) | **Fix**: absent; `YieldUndefined`. |
| `1 + J F <= 0` | `age = 0 +- 0` (`:629-630`) | **Fix**: ages absent; `AgeUndefined`. |
| `ca37 == 0` / `cl38 == 0` | ratio 0 (`arar_age.py:541-545`, `:555-558`) | **Fix**: absent; `KCaUndefined` / `KClUndefined`. |
| `kca == 0` with `ca37 != 0` (likewise `kcl == 0` with `cl38 != 0`) | `cak = 1/kca` raises, caught at `:540` | `kca` kept as computed, `cak` absent, `KCaUndefined` (`kcl` kept, `clk` absent, `KClUndefined`): the legacy single-try path. |
| `kca == 0` for `cak` | `ZeroDivisionError` caught at `:540` | covered by the row above |
| Fixed mode, `Ca3937 == 0` | `y = 1` (`:387-390`) | Replicate; `FixedK3739ZeroCa3937`. |
| `1 - m r3836 == 0`, cosmogenic `c36 == 0` or `rc == rs` | uncaught `ZeroDivisionError` | Error. |
| Decay guard `l t > 50` | `ValueError` (`:260-279`) | Error, same threshold. |
| `b == 0` or `c == 0` in E7 | factor 1 (`:358-359`) | Replicate. |
| Near-zero (non-zero) divisors | divide | Replicate: no epsilon thresholds; the zero tests above are exact `== 0.0` on nominals, matching Python's `ZeroDivisionError`. Huge results surface through `NonFiniteResult` only if they overflow. |
| Huge input errors (sigma >> value) | linear propagation | Replicate; no clipping. Variances are summed without scaling, so `(d sigma)^2` overflows above ~1e154 exactly as legacy; documented, not guarded. |
| Missing interference key | 0 (`:407-408`, `:420-424`) | Replicate via `ProductionRatios` defaults. |
| Missing or zero `Ca_K` / `Cl_K` | factor 1 (`arar_age.py:560-566`) | Replicate. |
| NaN/inf produced by valid inputs | propagates | Flag `NonFiniteResult`; values kept; non-finite F yields no ages and no error. |

Numerical policy: build with `-ffp-contract=off` (no FMA contraction) so the
live and `reduce` paths agree to rtol 1e-12; never `-ffast-math`. The flag is a
PUBLIC compile option of `pychron::reduction` (GCC/Clang): `UFloat`'s
arithmetic (`combine`, `variance`) is inline in a public header, so every
consumer must compile it unfused too, or the linker may keep a contracted
inline copy. Deadtime
correction applies to the intercept before E1 (E5).

Golden cases that hit a **Fix** row carry the legacy sentinel under
`legacy_sentinel` and the expected diagnostic, and the C++ test asserts the
divergence (section 9.4).

## 8. Integration

### 8.1 Shared kernels

The equations E4 and E9-E16, E19 are written once as function templates over
the number type in `libs/reduction/src/arar_kernels.hpp` (private header),
instantiated for `UFloat` (`reduce` and the step functions) and `double`
(`compute_arar`). A `nominal(x)` overload (`double` identity, `UFloat`
`nominal()`) serves the branch tests in E10, E11 and the zero checks.

### 8.2 `compute_arar` (conditionals spec 5)

Contract kept: same signature, same keys, same omissions, existing tests in
`tests/reduction/test_arar.cpp` unchanged and passing. Changes:

- Reimplemented on the `double` kernels. With the existing fields (no
  `K3739`, `K3839`, `Ca3837`, `Cl3638`) the kernels reduce to the formulas in
  the header comment, so outputs are bit-for-bit or within 1 ulp.
- `ArArConstants` gains fields with defaults that change nothing:
  `k3739 = 0`, `k3839 = 0`, `ca3837 = 0`, `allow_negative_ca_correction = true`,
  and `std::optional<LiveChlorine> chlorine` (`cl3638`, `lambda_cl36`,
  `decay_days`, `atm4038`, `cl_k_factor`). With `chlorine` set and `Ar38`
  present, `compute_arar` also emits `cl36`, `kcl`, `clk` (E12, E19). `Ar38`
  is required only when the chlorine correction is not a no-op
  (`m = Cl3638 * lambda_Cl36 * decay_days != 0`), so instant ages survive a
  missing `Ar38` otherwise.
- With `k3739 != 0`, `ca37`/`ca36`/`ca39` need `Ar39` and are omitted without it.
- Defaults stay as they are; `atm4036 = 298.56` (Lee et al. 2006) already
  equals the `Default` preset (D5). A lab's live constants come
  from the same source as the final reduction through:

```cpp
ArArConstants to_live_constants(const ReductionConstants& c, const ProductionRatios& p,
                                const Flux& flux, const DecayFactors& df,
                                std::optional<double> decay_days = std::nullopt);
```

  `kca_factor = 1 / Ca_K` (1 when missing or zero), `lambda_total = nom(lambda_K)`.
  `chlorine` is set only when `decay_days` is given. A per-analysis
  `fixed_k3739` of 0 (including 0 +- e) is unset on the live double path:
  callers apply truthiness before setting `ArArConstants::analysis_fixed_k3739`.
- Parity test: for complete inputs with no blanks (cosmogenic and nonzero
  abundance-sensitivity cases are excluded: the live path does not model them), `compute_arar(to_live_constants(...))`
  equals the nominal values of `reduce` (rtol 1e-12) for `age`, `kca`, `cak`,
  `radiogenic_yield`, `rad40`, `atm40`, `k39`, `ca37`, `ca39`, `ca36`, and,
  with chlorine, `kcl`, `clk`, `cl36`.

### 8.3 Conditionals static validation

`MetricCatalog` gains `bool chlorine = false`. `validate.cpp` reports
`kcl`/`clk`/`cl36` as unavailable only when `!catalog.chlorine`; the existing
test (`tests/experiment/test_conditionals_runtime.cpp:369`) keeps passing
because the default is false.

### 8.4 Database shapes

| Reduction input | DVC source (schema section) |
|---|---|
| `MeasuredSignal::intercept` | `intercept_value` via `resolve` (4.1) |
| `MeasuredSignal::baseline` | `baseline_value` of the isotope's detector, `modifier_error` honoured (4.1) |
| `include_baseline_error` | `intercept_value.include_baseline_error` (4.1) |
| `MeasuredSignal::blank` | `blank_value` (4.1) |
| `MeasuredSignal::ic_factor` | `icfactor_value` of the detector (4.1); `AnalysisRecord::Results::icfactors` has no error and is not a reduction source |
| `correct_for_blank` | `analysis.analysis_type` via `corrects_for_blank` |
| `ProductionRatios` | `production_value` rows of the level's production (6.1) |
| `Dose` | `chronology_dose` (6.1) |
| `Flux` | `flux_value` (`j`, `j_err`, `position_jerr`, `lambda_k_total`, `lambda_k_total_err`) (6.1) |
| `ReductionConstants` | new reference type `arar_constants`, pinned by revision through `refpins` (decision D1; follow-up A1 amends dvc schema section 6.1) |
| output | `derived_value` cache rows keyed by fingerprint incl. `kReductionVersion` (4.3) |

`reduce` takes resolved values; selecting the pinned or head revision is the
DVC client's job (schema 6.2).

## 9. Golden vectors

### 9.1 Generator

`tools/reduction_golden/generate.py`, one file, standard library plus the
legacy code. It imports the legacy functions from the legacy checkout and
never writes into it.

Run (from the repo root):

```
uv run --no-project --python 3.12 \
  --with numpy==2.4.4 --with scipy==1.17.1 --with statsmodels==0.14.6 \
  --with uncertainties==3.2.3 --with traits==7.1.0 --with pyyaml==6.0.3 \
  python tools/reduction_golden/generate.py \
    --legacy /Users/jakeross/Programming/pychron --out tests/reduction/golden
```

`--check` regenerates into a temporary directory and exits 1 if any file
differs from `--out` (drift detector). Versions are the legacy `uv.lock` pins;
Python 3.12 matches the legacy `requires-python`. This import chain
(`arar_age`, `argon_calculations`, `arar_constants`, `isotope`) was verified
to load without Qt. Importing `pychron.paths` creates `~/.pychron.0`, so the
script points `HOME` at a temporary directory before importing anything from
pychron, sets `sys.dont_write_bytecode = True` (no `__pycache__` in the legacy
tree), and silences the `std_dev==0` `UserWarning` from `uncertainties`.

Determinism: no timestamps or paths in output; fixed `numpy.random.default_rng`
seeds for the seeded series in `arar_age_test.py:26-36`; floats via `repr`
(shortest round-trip); `json.dumps(..., sort_keys=True, indent=1,
allow_nan=False)` plus a newline; non-finite numbers encoded as the strings
`"nan"`, `"inf"`, `"-inf"`. Two runs produce identical bytes (`--check`).
Each file header records the legacy commit (`git -C <legacy> rev-parse HEAD`),
whether the legacy tree was dirty, and the package versions.

### 9.2 Files (`tests/reduction/golden/`)

| File | Legacy functions | Harvested from |
|---|---|---|
| `ufloat.json` | `uncertainties` only | op programs (9.3): arithmetic, functions, self-cancellation, shared variables, covariance matrices, zero-sigma, huge sigma |
| `constants.json` | `ArArConstants`, preference defaults | `arar_constants_test.py` (trait defaults, `lambda_k`, `atm3836`, `scale_age`, `to_dict`); preference-pane defaults read from `constants/tasks/arar_constants_preferences.py` |
| `isotope_arithmetic.json` | `Isotope.get_baseline_corrected_value`, `get_non_detector_corrected_value`, `get_intensity`, `abundance_sensitivity_correction`, deadtime formula | `isotope_arithmetic_test.py:182-233` (`CorrectionMethodsTest`), `argon_calculations_test.py` `AbundanceSensitivityTest`; include/exclude baseline error; `correct_for_blank` false; IC factor 0; deadtime from `deadtime.py:54-55` (formula only; the module imports Qt) with factor 6241.509, plus one flagged case using legacy 6240 whose `legacy_sentinel` records the divergence (D4) |
| `decay_factors.json` | `calculate_arar_decay_factors`, `set_chronology` arithmetic | `DecayFactorsTest` (single segment, near-zero lambda, no segments, guard error), multi-segment with gaps, `dt < 0`, `use_irradiation_endtime` both ways, `arar_age_test.py:167-175` |
| `interference.json` | `interference_corrections`, `apply_fixed_k3739` | `InterferenceCorrectionsTest`, `FixedK3739Test`, `InterferenceCorrectionsFixedModeTest`; clamp on/off with negative `a37`; fixed mode with `Ca3937 = 0` |
| `atmospheric.json` | `calculate_atmospheric`, `calculate_cosmogenic_components` | `AtmosphericTest`, `CalculateAtmosphericChlorineTest`, `CosmogenicComponentsTest` (pure solar, pure cosmogenic, mixed) |
| `calculate_f.json` | `calculate_f` | `CalculateFTest`, `CalculateFEdgeCasesTest`; `k39 = 0`, `n40 = 0`, negative 36, huge errors |
| `age.json` | `age_equation`, `ArArConstants.scale_age` | `AgeEquationTest`, `AgeEquationEdgeCasesTest` (decay error, explicit `lambda_k`, negative argument), all four units |
| `pipeline.json` | `ArArAge.calculate_age` end to end | `arar_age_test.py:52-72` (`_build_age`, seeded linear fits stored as intercept value/error), J variants (`:131-160`), K/Ca (`:189-220`), segments, fixed `k3739`, abundance sensitivity, cosmogenic, missing isotope (error), zero/negative signals, huge errors, `lambda_k` override, error components |
| `chlorine.json` | `calculate_f` and `ArArAge` with Cl | `Cl3638 > 0` with several `decay_days`, missing Cl production (no `Cl3638`, no `Cl_K`), `Cl_K = 0`, `cl38 = 0` |
| `correlation.json` | legacy functions with shared inputs | two analyses sharing one `J`, two sharing one blank `UFloat`, `covariance_matrix` of their F and ages |

Preset coverage (D2): every case in `interference.json`, `calculate_f.json`,
`age.json`, `pipeline.json` and `chlorine.json` that depends on the
differing fields is emitted twice, suffixed `@legacy` (trait defaults) and
`@legacy_preferences` (pane defaults), with all constants written into
`inputs.constants`. The generator sets the legacy `ArArConstants` traits to
those values explicitly. `constants.json` additionally records the three
preset tables so the C++ `constants_preset` is checked field by field (the
`Default` preset is spec-defined, not legacy, and is checked against this
spec's table).

The `data/` databases (`omassspecdata.db`, `opychrondata.db`) were checked:
they hold three analysis rows with no isotopes, productions, flux or
chronology, so nothing is harvested from them.

### 9.3 Case format

```json
{
 "schema": 1,
 "legacy_commit": "26e77ad...",
 "legacy_dirty": false,
 "packages": {"python": "3.12.x", "uncertainties": "3.2.3", "numpy": "2.4.4"},
 "cases": [
  {
   "name": "calculate_f/young_volcanic",
   "source": "argon_calculations_test.py:CalculateFTest",
   "inputs": {"isotopes": {"Ar40": {"v": 1000.0, "e": 1.0}, "...": {}},
              "decay_days": 365.0, "production": {"Ca3937": {"v": 0.0007, "e": 0.0}}},
   "expected": {"f": {"v": 4.1, "e": 0.038}, "f_err_wo_irrad": 0.0386, "...": {}},
   "tol": {"rtol": 1e-12, "rtol_err": 1e-10, "atol": 0.0, "atol_err": 0.0, "why": ""},
   "legacy_sentinel": null,
   "expect_diagnostics": [],
   "expect_error": null
  }
 ]
}
```

Values with uncertainty are `{"v": ..., "e": ...}`. Covariances are
`{"cov": [[a, b, value], ...]}` by expected-key name. `ufloat.json` cases are
small programs the C++ test interprets:

```json
{"vars": {"x": {"v": 2.0, "e": 0.1, "tag": "x"}, "y": {"v": 3.0, "e": 0.2}},
 "steps": [["a", "mul", "x", "y"], ["b", "div", "a", "x"], ["c", "log", "b"], ["d", "pow", "a", 2.5]],
 "expected": {"c": {"v": 1.0986, "e": 0.0667}},
 "cov": [["a", "c", 0.0123]]}
```

Ops: `add sub mul div neg exp log log10 sqrt abs pow` with a number or a name
as each operand.

### 9.4 C++ side

- `tests/reduction/golden.hpp` (test-only): a minimal JSON reader (objects,
  arrays, strings, numbers via `std::strtod`, `true`/`false`/`null`, the three
  non-finite strings) and helpers `load_golden(file)`, `expect_close(got, want,
  tol, what)`, `expect_ufloat(UFloat, json, tol, what)`. It does not reuse the
  JSON parser in `libs/experiment/src/record/serialize.cpp` (anonymous
  namespace, and `pychron_reduction_tests` must not link experiment). No new
  dependency.
- `tests/reduction/CMakeLists.txt` adds
  `PYCHRON_REDUCTION_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"`.
- One gtest per file iterates all cases with `SCOPED_TRACE(case name)`. A case
  with `legacy_sentinel` asserts the C++ value is absent and the listed
  diagnostic is present; a case with `expect_error` asserts the `Result` error
  substring.
- Regenerating changes golden files only via the generator; hand edits are a
  review failure.

## 10. Legacy behaviour that is surprising, and decisions

Default is parity. Every row is either **Replicate** (kept, documented,
covered by a golden case) or **Fix** (changed, golden case asserts the
divergence).

| # | Legacy behaviour | Decision |
|---|---|---|
| Q1 | Every `ArArConstants` property access mints a fresh `ufloat` (`arar_constants.py:228-231`): `atm3836` (E12, the ratio of two fresh reads re-wrapped as one variable tagged `atm3836`) is independent of `trapped_4036` (E14), and constants are independent between analyses. | **Replicate**: `reduce` mints one variable per legacy access site. Not shared across analyses by default (D6); phase 2 adds an explicit shared-constants mode. |
| Q2 | Stored intercepts, baselines, blanks and IC factors are fresh variables per isotope, even when two isotopes share a detector or two analyses share a blank (`isotope.py:600`, `dvc/dvc_analysis.py:759`). | **Replicate** in `make_signal`. Sharing is available by building `IsotopeSignal` from shared `UFloat`s (owner requirement), covered by `correlation.json`. |
| Q3 | Baseline error excluded unless the intercept row says `include_baseline_error` (`isotope.py:728-736`). | **Replicate**. |
| Q4 | `age_err` excludes J error; J error only in `uage_w_j_err` (`arar_age.py:680-686`). | **Replicate** with explicit names (`age`, `age_w_j_err`, `age_w_position_err`). |
| Q5 | Decay constants enter decay factors and (by default) the age as nominal values (`arar_age.py:459-460`, `argon_calculations.py:622-623`). | **Replicate**; `include_decay_error` exposed for the age only. |
| Q6 | Sentinels: F = 1 when `k39 = 0`, age = 0 when `1 + JF <= 0`, yield = 0, kca/kcl = 0. | **Fix** (D3): absent plus `Diagnostic` in the core API; mapping to legacy sentinels belongs to future export adapters (follow-up A2). |
| Q7 | NaN/inf fitted intercept becomes 0 (`isotope.py:465-468`). | **Fix**: error. |
| Q8 | The E11 clamp runs after `ca39` was computed from the unclamped `ca37`, so `k39` keeps the negative Ca contribution. | **Replicate**, flagged in E11 and a golden case. |
| Q9 | Abundance sensitivity uses `s39 + s39` for 40 and `s37 + s37` for 36 (no 41/35 neighbours). | **Replicate**. |
| Q10 | Fixed K37/K39 with `Ca3937 = 0` uses `y = 1` (`argon_calculations.py:387-390`). | **Replicate** with `FixedK3739ZeroCa3937`. |
| Q11 | M&H docstring says `dt` runs from the end of each segment (`:312-316`); the DVC loader measures it from the start unless `use_irradiation_endtime` (`dvc/dvc_analysis.py:376-381`, `dvc/meta_object.py:103`). | **Replicate** the loader; option kept. |
| Q12 | `decay_days` uses `time.mktime` on naive local datetimes (`dvc/dvc_analysis.py:392`). | **Fix**: UTC epoch seconds. Differs from legacy by up to 1 h across a DST change (only `cl36` and `m` move). Golden files pass `decay_days` directly. |
| Q13 | `age_err_wo_irrad` and `age_err_wo_j_irrad` are declared (`arar_age.py:93-94`) but never assigned (always 0). | **Fix**: computed by exclusion (E18). The generator computes the legacy-equivalent exclusion from `uage.error_components()`. |
| Q14 | `calculate_error_F` hard-codes 295.5 (`:649`); `calculate_error_t` calls `ArArConstants()()` and cannot run (`:673`). Neither is called. | **Not ported**; AD propagation supersedes both. |
| Q15 | `age_units` defaults to `""`, which `scale_age` treats as years (`arar_constants.py:66`, `:149-170`). | **Fix**: `AgeUnits` enum, default Ma. Generator sets `age_units = "Ma"` unless a case tests units. |
| Q16 | `1 - m atm3836 = 0`, `c36 = 0` or `rc = rs` crash the legacy calculation. | Error `Result`. |
| Q17 | Deadtime is not applied anywhere in the legacy age path (only the calibration tool `deadtime.py` and a MassSpec column). | New, optional, off by default, applied to intercepts with per-detector `tau` and 6241.509 (D4, E5). |
| Q18 | IC factor with nominal 0 is honoured (`isotope_arithmetic_test.py:226-233`). | **Replicate**. |
| Q19 | Trait defaults and preference defaults disagree: `allow_negative_ca_correction` True vs False (`constants/tasks/arar_constants_preferences.py:161`), `lambda_b` error 9.3e-13 vs 0, `k3739` error 1e-4 vs 1e-2, `atm4036` error 0.5 vs 0, `lambda_e` error 1.6e-13 vs 0 (`:144-149`). | No hidden defaults: explicit fields, named presets `Default` (D2), `Legacy` (traits), `LegacyPreferences` (pane). Parity is shown against both legacy sets. |
| Q20 | `compute_arar` default `atm4036 = 298.56` vs legacy 295.5. | `Default` preset is 298.56 (D5), matching `compute_arar`; `Legacy` presets keep 295.5; `to_live_constants` always sets it. |

## 11. Risks

| Risk | Mitigation |
|---|---|
| Golden generator bit-rot when legacy changes | Header records commit; `--check`; vectors only change via the generator. |
| Hidden correlation mistakes (a variable accidentally shared or duplicated) | `correlation.json`; Review Focus 1-2; tests that assert specific `derivative(id)` values. |
| Allocation-heavy `UFloat` too slow for batch reduction | Target in 4.6; SBO later without API change. |
| `compute_arar` drift from `reduce` | Shared kernels (8.1) and the parity test (8.2). |
| Variance overflow with absurd sigmas | Documented (section 7); parity with legacy preferred over silent rescaling. |
| Constants not in the DB make ages non-reproducible | D1 (`arar_constants` reference type, pinned); `kReductionVersion` in fingerprints. |
| Legacy env cannot be installed offline | Committed JSON is the test input; the generator is needed only to regenerate. |

## 12. Phase 2

| Item | Legacy |
|---|---|
| Isochrons (York / New York / Reed) | `argon_calculations.py:33-137`, `tests/isochron_test.py` |
| Plateau ages, weighted means, MSWD | `argon_calculations.py:140-220`, `plateau.py`, `core/stats/core.py` |
| Flux / J from monitors, `model_j` | `argon_calculations.py:223-246`, `arar_age.py:425-428` |
| `convert_age` (monitor age / decay constant conversion; legacy path currently raises `TypeError`, `argon_calculations_test.py` `ConvertAgeTest`) | `argon_calculations.py:594-600`, `age_converter.py` |
| Fractional loss | `argon_calculations.py:678-707` |
| Dalrymple decay factors | `argon_calculations.py:282-302` |
| Discrimination / beta / transform IC factors | `arar_age.py:322-408` |
| Instant and equilibration ages from sniff windows (beyond `compute_arar`) | `arar_age.py:465-531`, `isotope.py:719-724` |
| K2O, moles, sensitivity | `arar_age.py:137-159`, `:312-317`, `:706-720` |
| Background subtraction | `isotope.py:852-853` |
| Explicit shared-constants mode for weighted means and isochrons (correlated `atm4036`, `lambda_K`) | D6; default stays per-read fresh values (Q1) |
| MassSpec comparison vectors | `test/processing/arar_diff.py` (needs a lab DB) |

## 13. Decisions (owner, 2026-10-02)

These replace the open questions of the first draft.

| # | Decision | Effect in this spec |
|---|---|---|
| D1 | Constants are reference data: `ref_type = arar_constants`, versioned and pinned by revision on analyses like productions and flux. In C++ the constants record is a plain struct with no hidden defaults beyond the named presets. | 5.3 `ReductionConstants` (zero-initialised) and `constants_preset`; 8.4 table. The DVC schema spec is not rewritten here (follow-up A1). |
| D2 | Default set: `allow_negative_ca_correction = false`, `lambda_b` error 9.3e-13, K37/K39 error 0.01 (non-zero uncertainty, conservative). All fields explicit and overridable. Golden cases cover both legacy default sets. | 5.3 presets `Default`, `Legacy`, `LegacyPreferences`; 9.2 every pipeline-level golden family runs under both legacy sets. |
| D3 | Absent values stay absent in the core API; no F = 1 / age = 0 sentinels. Mapping to legacy sentinels belongs in future export adapters. | Section 7 and Q6 unchanged in substance; follow-up A2. |
| D4 | Deadtime is an optional step on intercepts, off by default; `tau` from the detector config (per detector, never hard-coded); conversion 6241.509 fA -> counts/s. Legacy 6240 appears only in one flagged golden case documenting the divergence. | E5, 3.8, 5.2 `deadtime_tau_s`, 6 `deadtime_correct`, 9.2. |
| D5 | Default atmospheric 40/36 = 298.56 +- 0.31 (Lee et al. 2006); the named preset `Legacy` carries 295.5 +- 0.5. Golden vectors always pass explicit constants. | 5.3 preset table; 8.2. |
| D6 | Constants are not shared across analyses by default (legacy parity: a fresh independent value per read). Phase 2 adds an explicit shared-constants mode for weighted means and isochrons. | Q1, section 12. |

### 13.1 Follow-up action items

- **A1** (not approved; carried, DVC schema spec untouched) Amend `2026-10-01-dvc-schema-design.md` section 6.1 (reference data)
  with an `arar_constants` row: key `<lab>` (abundance sensitivity possibly
  per `<ms>`), payload = the `ReductionConstants` fields with value/error
  columns, head semantics and `refpins` pinning as for `production`; add it
  to the `derived_value` fingerprint inputs (section 4.3). Owner: DVC schema.
- **A2** Export adapters (MassSpec, legacy tables) map absent F, age, yield,
  K/Ca, K/Cl to the legacy sentinels at the boundary if the target requires
  them. Not in `libs/reduction`.
- **A3** Detector config gains an optional per-detector `deadtime_s`
  (`tau`) that the reduction client copies into `MeasuredSignal`.

## 14. Implementation notes / amendments

Status: Implemented. Controller rulings made during implementation, already
folded into the sections above:

- **Follow-ups.** A1 carried (not approved; the DVC schema spec is unchanged).
  A2 and A3 are carried: neither belongs to `libs/reduction`.
- **lambda_K (E16, 5.3).** One variable tagged `lambda_k`; legacy
  `lambda_b + lambda_e`, sigma in quadrature. The override applies unless it is
  exactly `0 +- 0` (`if lk:`); a `0 +- e` override is used and then fails as a
  zero lambda_K (Config error). The zero `lambda_b + lambda_e` check is exempt
  when a truthy override is present.
- **Fixed K37/K39.** Per-analysis `fixed_k3739` exactly `0 +- 0` is unset. The
  live double path treats any 0 as unset; callers apply truthiness before
  setting `ArArConstants::analysis_fixed_k3739`.
- **K/Ca, K/Cl.** `kca == 0` with `ca37 != 0` keeps `kca`, omits `cak`,
  `KCaUndefined`; likewise `kcl`/`clk`/`KClUndefined` (section 7).
- **Types.** `ArArResult` lives in `arar_reduction.hpp` beside `FResult` and
  `AgeSet`, not in `arar_types.hpp`. `age_error_components` is a
  `std::map<std::string, double, std::less<>>`.
- **Non-finite F.** No ages and no error; `NonFiniteResult` raised; values
  kept. `missing_isotope` is the caller's responsibility: `reduce` takes five
  slots and the test harness uses a NaN intercept.
- **pow at zero (4.3).** Base partial follows `pow(x, c)`; golden case
  `pow_at_zero_non_integer` pins the divergence from legacy NaN.
- **Parity (8.2).** Live-vs-`reduce` parity excludes cosmogenic and
  nonzero-abundance-sensitivity cases. `to_live_constants` sets `chlorine` only
  when `decay_days` is given, and `Ar38` is required only when `m != 0`.
- **Single `atm3836` variable (3.4)**, `-ffp-contract=off` (section 7),
  `LegacyPreferences` pane parity (5.3), and deadtime on the intercept before
  E1 are as stated in those sections.
- **Performance (4.6).** Measured `reduce()` 3.1-4.8 us per call on an M5 Pro
  (dev and Release builds); target 50 us.
