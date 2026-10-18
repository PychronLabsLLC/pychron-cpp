# Noble-gas abundance reduction: He, Ne, Ar, Kr, Xe below any age

Date: 2026-10-18
Status: Draft, not agreed. Nothing built.
Owner: Jake Ross
Depends on: `2026-10-05-measurement-plan-v2-design.md` §3.6 (`[interferences]`,
mass table), §6.1 (`label@detector` keys, record v3), §6.2 and §11.7 (what
that spec defers here); `2026-10-02-arar-reduction-design.md` (shape,
`UFloat`, E1-E5, §12 Phase 2 "K2O, moles, sensitivity", D6);
`2026-10-02-data-browsing-visualization-design.md` §7.5 (reference fits),
§10 (quantities); `2026-10-01-dvc-schema-design.md` §4.3 (`derived_value`),
§6 (reference data); `libs/persistence/AGENTS.md` (a migration costs every
lab a step).

Every physical constant below is marked **[literature]** (published value;
verify against the cited source before a lab relies on it: numbers typed
from memory of the literature, not read from the paper during this draft)
or **[decision]** (decided here). Equations are numbered N1.. so a plan and
tests can cite them; ArAr spec equations keep their E numbers.

## 1. Intent

Acquisition and store are gas-agnostic: labels are strings, mass table has
He3..Xe136 (`libs/systems/src/spectrometer/data_dir.cpp`
`default_molecular_weights`, AME2020), `analysis_isotope.isotope` free text,
`analysis.weight` present, `Quantity` grammar is `isotope.stage`. Reduction
is not: `libs/reduction` past `fits.hpp`/`stats.hpp`/`ufloat.hpp` is Ar-only
(`ArgonIsotope`, `reduce`, `compute_arar`), `reduced.cpp:107` tags every
non-Ar label as `Ar40`, `reference_fit.hpp` source correction is the fixed
Ar40/Ar36 pair with hard-coded legacy masses, sensitivity is one scalar
`ref_object` per spectrometer with no units and no pipette behind it, no
delta, R/Ra, moles, concentration, component or kinetics quantity exists.

This spec: generic per-gas stages that stop before any age. Every stage is
pure (`libs/reduction`, no I/O), values are `UFloat`, correlations flow
through shared variables as the ArAr spec does. Ar keeps its pipeline
unchanged; it also gets everything here as a gas.

## 2. Goals and non-goals

### 2.1 Goals

1. Mass discrimination per isotope pair from air or standard runs, three
   laws, reference-ratio table per gas, generalised from the Ar40/Ar36
   source correction. §4.
2. Interference correction from monitor labels (`Ar40++` on `Ne20`,
   `CO2++` on `Ne22`, `HD` on `He3`, hydrides and `HCl` on Ar), ratios
   measured on monitor runs or in the run. §5.
3. Absolute amounts: pipette model with reservoir depletion, sensitivity per
   gas (mol/fA or mol/cps) fitted over time, moles, ccSTP, atoms,
   concentration per gram via `analysis.weight`. §6.
4. Isotope dilution (3He spike for 4He). §7.
5. Component deconvolution, two and three endmembers, correlated errors. §8.
6. Named quantities: delta (‰), R/Ra, excess, moles, concentration,
   component. §9.
7. Step-heat kinetics: fractional release, D/a² (Fechtig and Kalbitzer),
   Arrhenius fit; output reusable by MDD. §10.
8. Generic isochron input: any two ratio quantities to York. §11.
9. Fix `reduced.cpp:107`. §3.3.
10. Store placement without a migration. §12.

### 2.2 Non-goals

- (U-Th)/He age: Ft, parent nuclides, zonation. Consumes `He4.moles` from
  here; own spec.
- Exposure and burial ages: production rates, scaling schemes, shielding.
  Here: concentrations (`atoms/g`, with error) exported as CSV for CRONUS,
  CREp, HeFTy; `elctl export` gains those columns in a
  follow-up to `libs/processing/AGENTS.md` report rules.
- Figures (three-isotope plot, Arrhenius plot, ratio isochron scene): a
  figure spec. This spec gives the values and the York input.
- MDD modelling (Lovera). §10 gives it `KineticsSeries`.
- Live conditionals for non-Ar quantities (`compute_arar` untouched; a
  conditional can already compare intercept ratios).
- Cylinder geometry in §10 (sphere and plane sheet only).
- K2O (ArAr spec §12): legacy constants unexplained; left there.
- A gas measured on another spectrometer (v2 §11.7).
- Changes to the ArAr age equations, presets or golden vectors.

## 3. Gases, labels, signals

### 3.1 Label classes **[decision]**

A label is one of:

- isotope: `^(He|Ne|Ar|Kr|Xe)([0-9]{1,3})$`, mass from the mass table. Its
  gas is the element.
- monitor or other: anything else (`Ar40++`, `CO2`, `CO2++`, `HD`, `H2`,
  `HCl`, `Ar40H`, `mass35`, `PHHCl`). Gets stages through `ic_corrected`
  (§3.2); belongs to no gas; never discriminated, never given moles.

`gas_of(label) -> optional<Gas>`; `Gas { element; isotopes (ascending
mass); normalising }`. Normalising isotope per gas, lab-configurable
(`[reduction.gases.<gas>] normalising = "Xe132"`), defaults:

| Gas | Normalising | Discrimination pair default |
|---|---|---|
| He | He4 | He3/He4 |
| Ne | Ne22 | Ne20/Ne22 (Ne21/Ne22 reported as check, §4.4) |
| Ar | Ar36 | Ar40/Ar36 (legacy) |
| Kr | Kr84 | Kr86/Kr84 |
| Xe | Xe130 | Xe136/Xe130 |

Open question Q3 (Xe 130 or 132, Kr 84).

### 3.2 Per-label stages

ArAr E1-E3 extended with a subtractive monitor term `M_i` and the existing
`D_i` slot (`IsotopeSignal::discrimination`, always 1 on the DVC path today):

```
N1  bc_i = I_i - (include_baseline_error ? B_i : nom(B_i))        E1
N2  nd_i = bc_i - (correct_for_blank ? Bk_i : 0)                  E2
N3  mc_i = nd_i * IC_i - M_i            monitor_corrected (new stage)
N4  s_i  = mc_i * D_i                   ic_corrected (legacy get_intensity when M_i = 0)
```

`M_i` from §5 (exact 0 without a relation), `D_i` from §4 (exact 1 without a
discrimination row). With `M_i = 0`, N3-N4 equal E3 (`nd * D * IC`,
commutative) to the bit under `-ffp-contract=off`: ArAr golden vectors
unchanged. Deadtime E5 stays on the intercept, any label, per detector
`tau` (He3 on a counting detector needs it).

`IsotopeSignal` gains `UFloat interference` (tag `<label> if`, exact 0
default). `processing::Stage` gains `MonitorCorrected` (`monitor_corrected`)
between `BlankCorrected` and `IcCorrected`. `InterferenceCorrected`
(`interference_corrected`, E14, Ar production) keeps its name: a file-format
string in figure options.

### 3.3 Fix: `reduced.cpp:107` **[decision]**

`r::make_signal(ar.value_or(r::ArgonIsotope::Ar40), m)` tags `He3` as
`Ar40`, `Ar40 bs`, `Ar40 bk`, `Ar40 IC`: error components of any non-Ar
quantity are summed under Ar40. Fix: `make_signal(std::string_view label,
const MeasuredSignal&)` mints tags `<label>`, `<label> bs`, `<label> bk`,
`<label> IC`, `<label> if`; the `ArgonIsotope` overload calls it with
`to_string(iso)`. `reduced.cpp` calls the label overload with
`iso.isotope`; the Ar slots still take `argon(iso.isotope)` for `reduce()`.
Test: a He3 stage's `error_components` carry tag `He3`, none `Ar40`.

### 3.4 Units

`analysis_isotope.units` (store) and the record's series units reach
`IsotopeData::units` (`"fA"`, `"cps"`; empty = unknown). Every amount (§6)
needs the sensitivity's units to match the signal's; mismatch or unknown
gives `reduction_error` for that gas's amounts, not a number.

`analysis.weight`, `ExtractionInfo::weight`: milligrams **[decision]**
(legacy `k2o`: `2 * self.weight * 0.001` grams; `load_position.weight` mg).
`Named::Weight` label becomes `Weight (mg)`.

## 4. Mass discrimination

### 4.1 Reference-ratio table

Reference document `noble/reference_ratios/<name>` (§12), default name
`air`. Per gas, ratios to the normalising isotope, value and 1σ, source:

| Gas | Ratio | Value **[literature]** | Source |
|---|---|---|---|
| He | He3/He4 | 1.384e-6 ± 0.006e-6 (Ra) | Clarke et al. 1976; Mamyrin et al. 1970 give 1.399e-6 |
| Ne | Ne20/Ne22 | 9.80 ± 0.08 | Eberhardt et al. 1965, Bottomley et al. 1984; Honda et al. 2015: 9.777 ± 0.004 |
| Ne | Ne21/Ne22 | 0.0290 ± 0.0003 | Eberhardt et al. 1965; Honda et al. 2015: 0.02905 ± 0.00003 |
| Ar | Ar40/Ar36 | 298.56 ± 0.31 | Lee et al. 2006 (ArAr `Default` preset); Nier 1950: 295.5 ± 0.5 (`Legacy`) |
| Ar | Ar38/Ar36 | 0.1885 ± 0.0003 | Lee et al. 2006; Nier 1950: 0.1876 (= 295.5 / 1575) |
| Kr | Kr78/Kr84 | 0.006087 | Basford et al. 1973 (normalised 84Kr = 1) |
| Kr | Kr80/Kr84 | 0.03960 | Basford et al. 1973 |
| Kr | Kr82/Kr84 | 0.20217 | Basford et al. 1973 |
| Kr | Kr83/Kr84 | 0.20136 | Basford et al. 1973 |
| Kr | Kr86/Kr84 | 0.30524 | Basford et al. 1973; Aregbe et al. 2001 |
| Xe | Xe124/Xe130 | 0.02337 | Basford et al. 1973 (normalised 130Xe = 1) |
| Xe | Xe126/Xe130 | 0.02180 | Basford et al. 1973 |
| Xe | Xe128/Xe130 | 0.4715 | Basford et al. 1973 |
| Xe | Xe129/Xe130 | 6.496 | Basford et al. 1973 |
| Xe | Xe131/Xe130 | 5.213 | Basford et al. 1973 |
| Xe | Xe132/Xe130 | 6.607 | Basford et al. 1973 |
| Xe | Xe134/Xe130 | 2.563 | Basford et al. 1973 |
| Xe | Xe136/Xe130 | 2.176 | Basford et al. 1973 |

Kr and Xe errors not listed: table ships them as 0 until the owner fills
them (Q1). A ratio to another normalising isotope is derived at load:
`(A/N') = (A/N) / (N'/N)`, error propagated with the two variables. ArAr
constants `atm4036`, `atm4038` stay where they are (D1): this table is not
their source; a test asserts the `air` document's Ar rows equal the
`Default` preset so the two cannot drift silently.

Document shape (JSON in `ref_document.content_json`):

```json
{"schema": 1, "name": "air",
 "gases": {"Ne": {"normalising": "Ne22",
                  "ratios": {"Ne20/Ne22": {"v": 9.80, "e": 0.08, "source": "Eberhardt 1965"},
                             "Ne21/Ne22": {"v": 0.0290, "e": 0.0003, "source": "Eberhardt 1965"}}}}}
```

Masses: `default_molecular_weights()` (AME2020), overridable by the lab's
`molecular_weights.toml` as today. Not legacy's four-decimal
`kSourceMasses` (§4.5).

### 4.2 Laws **[decision]**

Measured ratio `R_m` of isotopes `i` over `j`, masses `m_i`, `m_j`,
`Δm = m_i - m_j`, true ratio `R_t`. Per-amu factor `ε` (linear, power) or
exponent `β` (exponential):

```
N5  linear:       R_t = R_m (1 + ε Δm)
N6  power:        R_t = R_m (1 + ε)^Δm                 legacy set_discrimination: disc^(m - m36), disc per amu
N7  exponential:  R_t = R_m (m_i / m_j)^β               legacy set_beta: (m40/m)^beta
```

From a reference pair `(p, q)` measured on a reference run,
`v = R_m(p/q) / R_ref(p/q)` (a `UFloat`: fit error and table error):

```
N8  linear:       ε = (1/v - 1) / (m_p - m_q)
N9  power:        ε = (1/v)^(1/(m_p - m_q)) - 1
N10 exponential:  β = ln(1/v) / ln(m_p / m_q)
```

Per-isotope factor relative to the normalising isotope `N`, applied in N4
to every isotope of the gas, `D_N = 1`:

```
N11 linear:       D_i = 1 + ε (m_i - m_N)
N12 power:        D_i = (1 + ε)^(m_i - m_N)
N13 exponential:  D_i = (m_i / m_N)^β
```

A ratio `s_i / s_N` is then `R_t`. Which law: `spectrometer.toml`
`[discrimination] law = "exponential"`, per gas override
`[discrimination.gases.Ne] law = "linear"`; default `exponential` (Q2).
Where the law is a per-run property it is snapshotted with the
spectrometer settings (`spectrometer_snapshot.settings`), so reduction reads
the law the run was taken under, not today's config.

He: one pair, `D_He3 = 1/v` under every law (N11-N13 collapse at the pair
itself); the law matters only for a third isotope. Gases with one pair get
`v` stored, law recorded, no number interpreted as ε.

### 4.3 Fit over reference runs

`reference_fit.hpp` gains `ReferenceFitTarget::Discrimination`. Per gas row:
`gas`, `pair` (default §3.1), `reference_table` (document name, default
`air`), `fit`, `error`, `nsigma`, `skip_reviewed`. For each reference (types
default `air`, `cocktail`, per lab; found by `find_references` as today): the
ratio of the pair's `monitor_corrected` stages (N3: IC applied, monitors
removed, no `D`), divided by the table ratio, is one `ReferencePoint`
`v(t)`. Every fit kind and error kind of §7.5 applies. The model at the
unknown's time gives `v`; N8-N10 give the factor; stored per unknown (§12).

Legacy `source_correction` mode of `icfactor_fit` (Ar40/Ar36 → β → IC
factors on the Ar36..Ar39 detectors, `kSourceMasses`) stays for parity, and
becomes the special case `apply = "per_detector"` of this target: for a
multicollected gas whose detectors are not otherwise intercalibrated, N13
per isotope is written as that isotope's detector IC factor
(`icfactor_value` row per detector, `source_correction = 1`), as today. The
default `apply = "per_isotope"` writes one discrimination row for the gas
(§12) and leaves detector IC factors alone. The two are not combined for one
gas in one analysis: `per_detector` rows and a `disc:<gas>` row for the same
gas is a validation error of the fit set.

### 4.4 Diagnostics

For a gas with a second reference pair in the table (Ne21/Ne22), the
residual after N4 is reported: `misfit(Ne21/Ne22) = (R_t / R_ref - 1) *
1000` ‰ with error, as derived name `disc_misfit.Ne21/Ne22`. A figure may
plot it; reduction does not act on it.

`Diagnostic` enum (generic, new header, not the Ar one): `NoDiscrimination`
(gas has isotopes but no row: `D_i = 1`, ratios reported uncorrected),
`DiscriminationNonPositive` (`v <= 0` or `1 + ε <= 0`: `D_i` absent, stages
after N3 NaN for the gas), `NoReferenceRatio`, `NonFiniteResult`.

### 4.5 Masses parity note

Legacy source correction used `39.9624`, `35.9675`, `36.9668`, `37.9627`,
`38.964`; AME2020 gives `39.9623831`, `35.9675451`, ... `k = ln(m40/m) /
ln(m40/m36)` moves by under 2e-5 relative. The legacy mode keeps legacy
masses (golden parity); the generic target uses the mass table. A test pins
the difference so nobody "fixes" one into the other by accident.

## 5. Interference correction from monitors

### 5.1 Relations **[decision]**

v2 §3.6 lists `[interferences]` loosely (`Ar40++ corrects Ne20`, `CO2
corrects Ne22`, `HD corrects He3`). Fixed form, in `spectrometer.toml`,
snapshotted into the record's spectrometer settings (so reduction has it
without the config):

```toml
[[interferences]]
target = "Ne20"              # label corrected
parent = "Ar40"              # label whose intensity in this run is scaled and subtracted
ratio = "Ar40++/Ar40"        # optional: numerator/denominator labels; absent = parent subtracted as is
ratio_from = "monitors"      # "monitors" (fitted over monitor analyses) or "run" (both labels in this run)

[[interferences]]
target = "Ne22"
parent = "CO2"
ratio = "CO2++/CO2"
ratio_from = "monitors"

[[interferences]]             # resolved HD (Helix SFT): measured at m/z 3 beside He3
target = "He3"
parent = "HD"

[[interferences]]             # unresolved HD: from H2 in the run and HD/H2 on hydrogen-only monitors
target = "He3"
parent = "H2"
ratio = "HD/H2"
ratio_from = "monitors"

[[interferences]]             # argon hydrides, ratio from the same run (v2 §7.4 plan)
target = "Ar37"
parent = "Ar36"
ratio = "Ar40H/Ar40"
ratio_from = "run"

[[interferences]]
target = "Ar39"
parent = "Ar38"
ratio = "Ar40H/Ar40"
ratio_from = "run"

[[interferences]]             # HCl at the resolved shoulder (PHHCl), subtracted as measured
target = "Ar36"
parent = "HCl"
```

Validation (plan load, `doctor`): every label in a relation resolves in the
mass table (`Ar40++ = 19.98`, `CO2++ = 21.995` must be added to the example
table beside `CO2 = 43.99`); `target` is an isotope; `ratio` names two
labels; `ratio_from = "run"` requires both ratio labels collected in the
plan; a relation whose `parent` is not collected in a run is skipped for
that run with `reduction_error` naming it, never applied as 0.

v2 §3.6's example `CO2 corrects Ne22` reads here as `parent = "CO2", ratio =
"CO2++/CO2"`; v2 is amended in the branch that builds this (its text is an
example, not an interface).

### 5.2 Correction

For relation `k` with target `t`, parent `p`, ratio `r` (exact 1 when
absent):

```
N14  M_t += r * ic(p)       ic(p) = nd_p * IC_p when p has no earlier relation of its own,
                            else mc_p (its monitor_corrected value)
```

Relations apply in file order; "earlier" means an earlier entry in the
file. So `HCl` on `Ar36` is listed before the hydride on `Ar37` that uses
`Ar36`, and the hydride then sees the HCl-corrected `Ar36`. A cycle (a
target that is a parent of its own parent) is a validation error.

`r` is a `UFloat`: `ratio_from = "run"`: `ic(num) / ic(den)` of this run's
labels (correlated with them). `ratio_from = "monitors"`: the fitted value
at the run's time (§5.3), one fresh variable tagged `<num>/<den>`. Parents
and monitor labels on different detectors are handled by `IC_p` in `ic(p)`,
which is why N3 applies IC before `M`. No `D` on monitors: `Ar40++` arrives
at m/z 20 through the same optics as `Ne20`; the ratio was measured in the
same raw space.

### 5.3 Monitor ratio fit

`ReferenceFitTarget::Interferences`, one row per relation with `ratio_from
= "monitors"`: references are analyses of configured types (default
`interference_monitor`, a new free-text `analysis_type`; v2 §2 observation
3: an interference monitor is an analysis type as well as a position) in
which both ratio labels have intercepts; the point is `ic(num) / ic(den)`
(N3 without `M`) against time; fit and error kinds as blanks; default fit
`average`, window `hours` as `ReferenceQuery`. Air is not a valid monitor
for `Ar40++/Ar40`: air has Ne20 at m/z 20. The default type list is per
relation in config (`monitor_types = ["interference_monitor"]`).

Result per unknown: one row per relation (§12) with value, error, fit,
references, exclusion flags, as blank rows.

## 6. Absolute amounts

### 6.1 Pipette model **[decision]**

Reference document `noble/pipette/<ms>/<name>` (scope `mass_spectrometer_uuid`
set on the `ref_object`):

```json
{"schema": 1, "name": "air_pipette", "gas_source": "air",
 "filled_utc": "2026-03-01T00:00:00Z", "shots_at_fill": 0,
 "reservoir": {"volume_cc": 2000, "volume_err": 10, "pressure_torr": 0.1, "pressure_err": 0.001,
               "temperature_k": 295.0, "temperature_err": 1.0},
 "pipette_volume_cc": 0.2, "pipette_volume_err": 0.002,
 "composition": {"He4": {"v": 5.24e-6, "e": 0.0}, "Ne20": {"v": 1.645e-5, "e": 0.0}, "...": {}},
 "moles_override": {"He4": {"v": 1.0e-13, "e": 2.0e-15}}}
```

```
N15  n_0,g   = P V_r / (R T) * x_g            moles of isotope g in the reservoir at fill
                                              R = 8.314462618 J/(mol K) [literature, CODATA 2018]; P in Pa (torr * 133.322368)
N16  f       = V_p / (V_p + V_r)              fraction removed per shot
N17  n_k,g   = n_0,g * f * (1 - f)^k           moles delivered by shot k (k = 0 first shot after fill)
```

`composition` is mole fraction per isotope; air defaults **[literature,
Ozima and Podosek 2002]**: He 5.24 ppm, Ne 18.18 ppm, Ar 9340 ppm, Kr 1.14
ppm, Xe 0.087 ppm by volume, split per isotope with the `air` reference
ratios (§4.1) and `moles = fraction * Σ`. `moles_override` replaces N15 for
an isotope when a pipette was calibrated by another route (a manometrically
measured 4He standard). Errors: `V_r`, `V_p`, `P`, `T` and each composition
entry are variables tagged by name; `k` exact.

Shot index `k`: recorded in the run by the extraction host's `pipette(name)`
call (counter per pipette in the line's state, refilled by `elctl line
pipette refill <name>` which writes a new document revision with
`shots_at_fill`); reaches the record as `Results.pipette = {name, shot}`
and the store through `analysis_meta` (free-form, no migration). Missing
counter: the per-analysis row `pipette:<gas>` (§12) holds `k` entered by
hand. `k < shots_at_fill` is an error.

### 6.2 Sensitivity

Per gas, per spectrometer: `S_g` in `mol/fA` or `mol/cps`
(`units` must match `IsotopeData::units`, §3.4).

```
N18  S_g(t_ref) = n_k,N / s_N                  from a standard run: moles of the normalising isotope delivered over its ic_corrected signal
N19  moles_i    = S_g * s_i                    every isotope of the gas (s_i from N4, so discrimination is in)
```

Fit over standard runs: `ReferenceFitTarget::Sensitivity`, row per gas:
reference types (default `air`, `standard`), pipette document name, fit
kinds and errors as blanks, points `S_g(t)` against time. The drift series
is this fit's `ReferenceModel`. The value at the unknown's time is stored
per unknown (§12), with the pipette document revision used (so a refill or
a recalibration changes nothing already saved until refitted).

Fallback order at reduction: per-analysis `sens:<gas>` row; else
`ref_object` type `sensitivity`, key `<ms>/<gas>` head (new keys beside the
legacy `<ms>`; `SensitivityValue::extra` carries `{"units": "mol/fA",
"gas": "Ar"}`; legacy `<ms>` rows count as Ar, units `mol/fA` as legacy
`sensitivity_units` default); else `NoSensitivity` diagnostic, no amounts.
Legacy "latest record before the run date" (`set_sensitivity`) is not
reproduced: head is "what a new reduction uses" (dvc §6.2); a lab that wants
a date-dependent value fits it.

### 6.3 Derived amounts

```
N20  ccstp_i   = moles_i * 22413.97          cm³ STP per mol [literature, CODATA 2018: 22.41396954 L/mol at 273.15 K, 101.325 kPa]
N21  atoms_i   = moles_i * 6.02214076e23      exact
N22  conc_i    = moles_i / (weight_mg * 1e-3) mol/g; ccstp/g and atoms/g likewise; absent without weight
```

Errors through `UFloat`; `weight` has no error (store has none) unless a
lab's `[reduction] weight_err_mg` gives one, then a variable tagged
`weight`.

## 7. Isotope dilution

Spike document `noble/spike/<ms>/<name>`: `{"isotope": "He3",
"ratio": {"He4/He3": {"v": r_s, "e": ...}}, "pipette": "<pipette document
name>"}`: the spike is a pipette (§6.1) whose composition is the spike gas;
`n_k` of the spike isotope per shot comes from N17 or `moles_override`.

Sample 4He from measured `R_m = He4/He3` (N4 ratio), spike `He4/He3 = r_s`,
sample's own `He3/He4 = R_x` (default 0: negligible sample 3He, the
(U-Th)/He case; else a configured or table ratio):

```
N23  a = c (R_m - r_s) / (1 - R_m R_x)        a = sample He4 moles, c = spike He3 moles (shot k)
```

Derivation: sample `(a, R_x a)`, spike `(r_s c, c)`; `R_m = (a + r_s c) /
(R_x a + c)`. Spike blank: a `spike_blank` analysis type (spike only, no
sample) gives `R_m` of the blank; blank 4He moles `a_bk` by N23 with the same
spike; subtracted: `a_sample = a - a_bk`, `a_bk` fitted over blanks as a
blank is (points in moles, `ReferenceFitTarget::Blanks` row with `units =
"mol"`). Spike calibration (`c` against a known 4He pipette): N23 solved for
`c = a_std (1 - R_m R_x) / (R_m - r_s)` over `spike_standard` runs; the fit
writes `moles_override.He3` into a new spike document revision; not done
inside a reduction. `R_m = r_s` or `R_m R_x = 1`: `IsotopeDilutionUndefined`,
value absent. Independent of §6 sensitivity by construction; a reduction
that has both reports both (`He4.moles` from N19, `He4.moles_id` from N23).
Generic in the isotope: any gas with a spike document of that gas uses N23
with its own pair.

## 8. Component deconvolution

Endmember document `noble/endmembers/<name>`: per gas, named endmembers,
each ratios to the normalising isotope with errors and source. Shipped
example `default` **[literature, verify]**:

| Gas | Endmember | Ratios |
|---|---|---|
| Ne | `air` | from the `air` reference table |
| Ne | `cosmogenic` | Ne20/Ne22 ≈ 0.80 ± 0.10, Ne21/Ne22 ≈ 0.80 ± 0.10 (quartz spallation, Niedermann 2002, Rev. Mineral. Geochem. 47) |
| Ne | `nucleogenic_21` | pure 21Ne from 18O(α,n): `pure = "Ne21"` |
| Ne | `nucleogenic_22` | pure 22Ne from 19F(α,n): `pure = "Ne22"` |
| Xe | `air` | reference table |
| Xe | `fission_238U` | Xe131/Xe136 = 0.0776, Xe132/Xe136 = 0.595, Xe134/Xe136 = 0.832 (spontaneous fission, Ozima and Podosek 2002; Ragettli et al. 1994 ≈ 0.0791 : 0.593 : 0.833), re-normalised to Xe130 at load |
| Xe | `iodine_129` | pure Xe129: `pure = "Xe129"` |
| He | `air` | Ra |
| He | `magmatic` | not a constant: from a paired crush analysis (below) |
| He | `cosmogenic` | `pure = "He3"` (Kurz 1986 two-component case) |

An endmember source is one of **[decision]**: `table` (document value, one
fresh variable per ratio per analysis, D6 default; a group-shared variable
when the group runs in shared-constants mode), `analysis:<uuid>` (that
analysis's N4 ratio as a `UFloat`, correlated with it; the crush-melt pair:
magmatic He3/He4 from the crush of the same sample, picked by the figure or
by rule `same sample, extraction kind crush`), or `manual {v, e}`.

### 8.1 Two endmembers, one ratio

```
N24  f_A = (R_m - R_B) / (R_A - R_B)          fraction of the normalising isotope N from A; f_B = 1 - f_A
N25  X_A = f_A * N_tot * (X/N)_A               amount of isotope X from A, in whatever unit N_tot is (signal or moles)
```

`pure = X` endmember: `(X/N)_A → ∞`, so the formula is written on the
other axis: `X_A = X_tot - N_tot * (X/N)_B` (the excess, N29), `f_A`
undefined. `R_A = R_B`: `ComponentsUndefined`.

### 8.2 Three endmembers, two ratios

Measured `(x_m, y_m)` (`Ne20/Ne22`, `Ne21/Ne22`), endmembers `(x_i, y_i)`,
fractions `f_i` of N with `Σ f_i = 1`:

```
N26  | x_1 x_2 x_3 | | f_1 |   | x_m |
     | y_1 y_2 y_3 | | f_2 | = | y_m |        solved by Cramer's rule in UFloat arithmetic
     |  1   1   1  | | f_3 |   |  1  |
N27  X_i = f_i * N_tot * (X/N)_i
```

`x_m` and `y_m` share `N` so they are correlated; `UFloat` carries it.
Determinant 0 (collinear endmembers): `ComponentsUndefined`. A `pure`
endmember on axis `y` (nucleogenic 21Ne) is handled by substituting its row
with the limit: `f_pure` is then `y_m - Σ_{i≠pure} f_i y_i` after the 2×2
solve on `x` alone; written out in the plan from this rule. Negative
fractions are reported, not clamped (a lab reads them as "outside the
triangle"); a `Diagnostic` `FractionOutsideUnitInterval` flags them.

### 8.3 Group results

Fractions and amounts are per analysis. Means of components over a group
(a weighted mean of `Ne21_cosmogenic` over steps) use `stats.hpp` as any
quantity does; nothing new.

## 9. Named quantities

### 9.1 Grammar additions

```
term     := isotope ( "." stage )? | name | family "." key | fn "(" args ")"
stage    += monitor_corrected | moles | moles_id | ccstp | atoms | conc | conc_ccstp | conc_atoms
fn       := delta | rra | excess | component | fraction | disc_misfit
args     := ratio                       delta(Ne20/Ne22)   ‰ against the reference table (default "air"; "air" is the only table in V1)
          | ratio                       rra(He3/He4)       R / Ra, Ra = the table's He3/He4; bare `rra` = rra(He3/He4)
          | isotope                     excess(Xe129)      N29 against the reference table
          | isotope "," endmember       component(Ne21, cosmogenic)   N25 / N27 amount, unit of the isotope's stage (ic_corrected)
          | gas "," endmember           fraction(Ne, cosmogenic)      f_i
          | ratio                       disc_misfit(Ne21/Ne22)        §4.4
```

```
N28  delta(A/N)  = (R_t / R_ref - 1) * 1000     R_t from N4 stages; R_ref one fresh variable per analysis (D6)
N29  excess(X)   = X_t - N_t * (X/N)_ref         X_t, N_t the ic_corrected stages; "Xe129*", "Ne21_ex" in papers
```

A function term may carry a stage suffix for the amount it is in:
`excess(Xe129).moles`, `component(Ne21, cosmogenic).atoms`; default is the
`ic_corrected` signal unit. `eval` returns nullopt when the gas has no
discrimination (`NoDiscrimination`) for `delta` and `rra` only when
`[reduction] require_discrimination = true` (default false: the uncorrected
ratio is still a number, with the diagnostic on the analysis).

`available_quantities` lists per present gas: its ratios, `delta(...)` for
table ratios, `rra` when He is present, `excess(X)` for every non-normalising
isotope, amounts when a sensitivity resolved, components when an endmember
document is configured, `kinetics.*` (§10) for step-heated aliquots.

Labels: `δ²⁰Ne/²²Ne (‰)`, `R/Ra`, `¹²⁹Xe* (fA)`, `⁴He (mol)`, `⁴He (mol/g)`,
`²¹Ne cosmogenic (atoms/g)` with the existing superscript helper.

### 9.2 Ratios of amounts

`He4.moles/He3.moles` equals `He4/He3` exactly (`S_g` cancels in the
derivative merge); a test asserts it.

## 10. Step-heat kinetics

Input: the steps of one aliquot (same identifier and aliquot, `increment`
ascending), each with temperature `T_i` (K: `extract_value` in `C` → `+
273.15`, in `K` as is; `W` or empty units → `KineticsUndefined`, laser
power is not a temperature), duration `t_i` (s: `extract_duration`), and the
released amount `a_i` of a chosen quantity (option `gas_quantity`, default
`He4` for He, `Ar39` (`k39` when an ArAr result exists) for Ar, the
normalising isotope otherwise; any stage, moles preferred when present).
Temperature error: option `temperature_err_k` (default 0; a furnace
calibration gives one).

```
N30  F_i    = Σ_{j≤i} a_j / Σ_all a_j                cumulative fractional release, UFloat (correlated through the total)
N31  sphere (Fechtig and Kalbitzer 1966; McDougall and Harrison 1999 eqs 5.26-5.28) [literature], g(F) = D t / a²:
       F ≤ 0.1:        g = π F² / 36
       0.1 < F ≤ 0.9:  g = (1/π²) (2π - π² F / 3 - 2π sqrt(1 - π F / 3))
       F > 0.9:        g = -(1/π²) ln((π² / 6) (1 - F))
N32  plane sheet, half-thickness a [literature, same source]:
       F ≤ 0.6:        g = π F² / 4
       F > 0.6:        g = -(4/π²) ln((π² / 8) (1 - F))
N33  (D/a²)_i = (g(F_i) - g(F_{i-1})) / t_i          F_{-1} = 0; the last step (F = 1) has no value: g infinite
N34  Arrhenius: x_i = 1e4 / T_i, y_i = ln((D/a²)_i), sx_i = 1e4 σ_T / T_i², sy_i = σ(y_i), ρ = 0
       york_fit (stats.hpp, NewYork) over included steps:  y = ln(D0/a²) - (Ea / R) (x / 1e4)
       Ea = -slope * R * 1e4  (J/mol; reported kJ/mol), ln(D0/a²) = intercept, both with York errors and covariance
```

Each end of a step uses the regime its own `F` falls in; at a regime
boundary the piecewise `g` is continuous to ~1e-3 (F and K's
approximations), not exact; documented, not patched. `a_i ≤ 0` or a
non-finite `a_i`: the step is excluded from the sums with a diagnostic and
`F` is computed over the rest. `σ_T = 0` gives `sx = 0`; `york_fit` refuses a
zero sigma, so the fit then uses `sx = 1e-12` with the choice recorded (Q5
asks whether OLS with `fits.hpp` is wanted instead).

Output `KineticsSeries { geometry; steps: {T_k, t_s, a, F, g, lnDa2 (UFloat), included}; arrhenius: optional<YorkFit>;
ea_kj_mol, ln_d0_a2 (UFloat) }`. MDD consumes `steps` and nothing else;
the Arrhenius figure (own spec) draws the same struct. Per step derived
names: `kinetics.F`, `kinetics.lnDa2`, `kinetics.log10Da2` (= `lnDa2 /
ln 10`); per aliquot `kinetics.Ea`, `kinetics.lnD0a2` are group results, not
`derived_value` rows (they belong to the set of steps, like a plateau).

## 11. Generic isochron input

A unit `ratio_isochron` (and the existing inverse isochron as its Ar
instance later): options `x`, `y` (any two ratio quantities of §9, e.g. `x
= "Xe128/Xe132"`, `y = "Xe129/Xe132"` for I-Xe, Hohenberg and Pravdivtseva
2008), `method` (york, new_york, reed), group. For each analysis `X = eval(x)`,
`Y = eval(y)` as `UFloat`; point `{nom(X), std(X), nom(Y), std(Y), rho =
correlation(X, Y)}` (shared denominator and shared discrimination give a
real ρ, which the ArAr inverse isochron today also gets from its stages);
`york_fit` from `stats.hpp`. Output: `YorkFit` plus the points and which
were excluded; slope and intercept named `isochron.slope`,
`isochron.intercept` with the quantities' labels (`¹²⁹Xe/¹³²Xe vs
¹²⁸Xe/¹³²Xe`). What a slope means (129I/127I, a 3He/4He of a trapped
component) is the lab's reading; this spec stops at the fit. Scene: the
isochron scene builder generalised from `isochron.cpp` with axis labels
from the quantities; the inset of `2026-10-13-isochron-inset-design.md`
applies unchanged.

## 12. Store

Rules: `libs/persistence/AGENTS.md`. No migration in this spec
**[decision]**; the alternatives it rejects are named.

| What | Where | Why there |
|---|---|---|
| Reference-ratio table, endmembers, pipette, spike | `ref_object` type `document`, keys `noble/reference_ratios/<name>`, `noble/endmembers/<name>`, `noble/pipette/<ms>/<name>`, `noble/spike/<ms>/<name>`; payload `ref_document.content_json`; `mass_spectrometer_uuid` set for pipette and spike (indexed, migration 0005) | Same kind of thing as `molecular_weights.json`, which already lives there. Revisioned, pinned by `refpins`, in every `derived_value` fingerprint through "resolved reference revisions". Rejected: new `ref_type` values (CHECK list change = migration, `revision`/`ref_object` rebuild on SQLite). Promote to typed `ref_type`s only in the one migration that also brings A1 `arar_constants`, if ever. |
| Per-analysis discrimination, interference ratios, sensitivity, pipette shot, spike shot | `icfactors` revision, `icfactor_value` rows with pseudo-detector keys: `disc:<gas>` (`discrimination = 1`, `value/error` = `v` of the pair, `reference_detector` = pair as `"Ne20/Ne22"`, `standard_ratio` = table ratio, `reference_data` = `{"law", "factor": {v, e}, "table": {"name", "revision"}, "masses": "ame2020"}`, `references` = the fit's references); `if:<target>:<num>/<den>` (ratio with `reference_data = {"parent", "ratio_from"}`, references); `sens:<gas>` (`value/error` mol per unit, `extra = {"units", "pipette": {"name", "revision"}}`, references); `pipette:<gas>` (`value` = shot `k`, `extra = {"name"}`); `spike:<isotope>` (`value/error` = spike moles, `extra = {"name", "revision", "shot"}`). `IcFactorRow` already has every field. | These are corrections applied at reduction, fitted from references, reviewed, manually overridable: what `icfactors` is. `discrimination` flag exists for this and is unused. Old builds: `StoreSource::load` matches rows to isotopes by detector; a row no isotope's detector matches is ignored (to be verified by a test that loads such an analysis with the current loader before any row is written: `tests/processing/test_store_source.cpp`). Rejected: a new revision kind `corrections` (CHECK on `revision.kind` = migration and a `revision` table rebuild on SQLite, 312 thousand rows in one lab's store). Changeset message `<ICFactor> disc:Ne(average), if:Ne20:Ar40++/Ar40(average)`. |
| Monitor labels' intercepts, `Ar40++`, `HD` | `intercept_value` as any label (free text `isotope`) | Already so. |
| `[interferences]`, discrimination law | `spectrometer_snapshot.settings` JSON (record v3 spectrometer snapshot) | Reduction reads what the run had. Already JSON, no change. |
| Shot counter | `analysis_meta` (`pipette`, `shot`) via the record | Free-form; no change. |
| Signal units | `analysis_isotope.units` → `IsotopeData::units` | Column exists; loader change only. |
| Derived amounts, deltas, components, kinetics per step, misfits | `derived_value` rows, `name` = the quantity text (`He4.moles`, `delta(Ne20/Ne22)`, `component(Ne21, cosmogenic).atoms`, `kinetics.lnDa2`), `units`, `reduction_version = "noble-1"` (new constant beside `kReductionVersion`; both in the fingerprint) | Cache, deletable, no change. |
| Group results (Ea, isochron fits, component means) | not stored (as plateaus); an interpreted-value kind is a later decision | |

Fingerprint: the noble documents an analysis resolved (reference table,
endmembers, pipette, spike) join the "resolved reference revisions" set of
dvc §4.3; the loader lists them per analysis the way it lists flux and
production.

Queries: documents by key prefix `noble/` are read once per reduction run
and shared across analyses (AGENTS "read once and shared"), never per
analysis; the `UNIQUE (ref_type, key)` index serves `ref_type = 'document'
AND key = ?`. Pipette and spike by `mass_spectrometer_uuid` use
`ref_object_mass_spectrometer_ix`. No new per-analysis statement: the
`icfactors` head is already loaded.

## 13. Data model and API

Header `libs/reduction/include/pychron/reduction/noble.hpp` (+
`noble_types.hpp`), namespace `pychron::reduction`, pure, `Result` errors
`ErrorKind::Config` prefixed `reduction: ` as the Ar API.

```cpp
enum class Gas : std::uint8_t { He, Ne, Ar, Kr, Xe };
struct IsotopeLabel { Gas gas; int mass_number; };
std::optional<IsotopeLabel> parse_isotope_label(std::string_view label);   // §3.1 regex
std::string_view to_string(Gas);

enum class DiscriminationLaw { Linear, Power, Exponential };               // N5-N7
struct ReferenceRatio { std::string numerator, denominator; Measured value; std::string source; };
struct ReferenceTable { std::string name; std::map<Gas, std::vector<ReferenceRatio>> ratios;
                        std::map<Gas, std::string> normalising; };
Result<UFloat> reference_ratio(const ReferenceTable&, std::string_view num, std::string_view den);  // renormalises; fresh variable

struct DiscriminationFactor { DiscriminationLaw law; UFloat factor;        // ε or β, N8-N10
                              std::string pair_num, pair_den; };
Result<DiscriminationFactor> discrimination_from_pair(DiscriminationLaw, const UFloat& v, double m_p, double m_q);
UFloat discrimination_of(const DiscriminationFactor&, double m_i, double m_n);  // N11-N13

struct Relation { std::string target, parent; std::optional<std::pair<std::string,std::string>> ratio;
                  enum class From { Run, Monitors } from = From::Monitors; };
// N14 over a map of label -> (nd * IC) in file order; returns M per target and the order it used.
Result<std::map<std::string, UFloat>> monitor_corrections(std::span<const Relation>,
    const std::map<std::string, UFloat>& ic_by_label, const std::map<std::string, UFloat>& monitor_ratios);

struct Pipette { ... N15-N17 fields as §6.1 ... };
Result<UFloat> pipette_moles(const Pipette&, std::string_view isotope, int shot);        // N17
struct Sensitivity { UFloat value; std::string units; };
UFloat moles(const Sensitivity&, const UFloat& signal);                                   // N19
struct Amounts { UFloat moles, ccstp, atoms; std::optional<UFloat> conc, conc_ccstp, conc_atoms; };
Amounts amounts(const UFloat& moles, std::optional<UFloat> weight_mg);                    // N20-N22
Result<UFloat> isotope_dilution(const UFloat& r_m, const UFloat& r_s, const UFloat& r_x, const UFloat& c);  // N23

struct Endmember { std::string name; std::map<std::string, UFloat> ratios; std::optional<std::string> pure; };
struct Components { std::vector<std::pair<std::string, UFloat>> fractions; std::vector<Diagnostic> diagnostics; };
Result<Components> two_components(const UFloat& r_m, const Endmember& a, const Endmember& b, std::string_view ratio);        // N24
Result<Components> three_components(const UFloat& x_m, const UFloat& y_m, std::span<const Endmember, 3>,
                                    std::string_view x_ratio, std::string_view y_ratio);                                     // N26
UFloat component_amount(const Components&, std::string_view endmember, const UFloat& n_tot, const Endmember&, std::string_view isotope);  // N25/N27

UFloat delta_permil(const UFloat& r_t, const UFloat& r_ref);               // N28
UFloat excess(const UFloat& x_t, const UFloat& n_t, const UFloat& ratio_ref); // N29

enum class Geometry { Sphere, PlaneSheet };
struct KineticsStep { double t_k, dt_s; UFloat amount; bool included = true; };
struct KineticsSeries { ... §10 ... };
Result<KineticsSeries> kinetics(std::span<const KineticsStep>, Geometry, double temperature_err_k);  // N30-N34

inline constexpr std::string_view kNobleReductionVersion = "noble-1";
```

`libs/processing`: `ReducedAnalysis` gains `std::vector<GasResult> gases`
(`gas`, `normalising`, `discrimination` (optional factor and diagnostics),
`ratios` to the normalising isotope, `amounts` per isotope, `components`
when configured, diagnostics) and `std::map<std::string, UFloat> monitor_ratios`;
`reduce_analysis` runs N1-N4 for every label, then per gas §4, §6, §7, §8;
`ReductionSettings` gains `reference_table` (name), `endmembers` (name),
`require_discrimination`. Reduction context (`ReductionContext`) carries the
resolved documents (`ReferenceTable`, endmembers, pipettes, spikes) read
once per run by the source. `Quantity::eval` covers §9. `reference_fit.hpp`
gains the three targets (§4.3, §5.3, §6.2) with schemas, rows and
`save_reference_fits` writing the pseudo-detector rows (§12).

Live path: nothing. `compute_arar` unchanged (parity test 8.2 stays).

## 14. Numerical policy

As ArAr §7: exact `== 0.0` zero tests on nominals, no epsilons, IEEE
otherwise, `NonFiniteResult` flagged, values kept; undefined quantities
absent with a `Diagnostic`, never a sentinel. `-ffp-contract=off` inherited
from `pychron::reduction`. Negative amounts (blank over signal) propagate,
as negative Ar signals do. Fractions outside [0, 1] reported.

## 15. Testing

No legacy implementation exists for any of this, so no legacy golden
vectors. Instead:

- `tools/noble_golden/generate.py`: an independent Python implementation of
  N5-N34 on `numpy` + `uncertainties` 3.2.3 (pinned as the ArAr generator),
  written from this spec by someone other than the C++ author of each
  function, producing `tests/reduction/golden/noble_*.json` in the ArAr
  §9.3 case format (`--check` drift detector, deterministic bytes). Same
  tolerances as ArAr §4.7.
- Published worked examples as cases, each citing its source and marked
  **[literature]** in the case `source` field: Fechtig and Kalbitzer's
  sphere table (a few `F` → `g` points), a Ne three-isotope
  decomposition from Niedermann 2002, an isotope-dilution example from a
  (U-Th)/He methods paper, Kurz 1986 crush/melt. Numbers typed into the
  generator are checked against the paper by the owner before the case is
  trusted (the draft's constants are from memory, see the preamble).
- Properties (gtest, no golden): `delta` of the reference table's own ratio
  is `0 ± 0`; `rra` of air is 1; `D_N = 1` under every law; `v = 1` gives
  `D_i = 1`; three endmembers with the measurement at a vertex gives that
  fraction `1 ± 0` and the others `0`; N23 round trip (compose then solve);
  pipette shots monotone with `k`; `He4.moles/He3.moles == He4/He3` to
  1e-15; kinetics `F` of the last step is exactly 1; N3-N4 with `M = 0`
  bit-equal to E3 for the ArAr golden inputs (parity guard).
- `reduced.cpp` fix: tag test §3.3; a He-Ne-Ar multi-gas fixture record
  (v2 §7.6 plan run in the simulator once that plan lands; until then a
  hand-written `Analysis`) reduces to three `GasResult`s.
- Store: loader test for pseudo-detector rows (§12) on SQLite and, with
  `PYCHRON_TEST_PG_URL`, PostgreSQL; `derived_value` fingerprint changes
  when a noble document revision changes.
- Reference fit targets: one test per target with three references and one
  unknown, each fit kind, as the blank fit tests.

## 16. Order of work

Each item a plan task group; test first; the spec changes with the code.

1. `make_signal(label)`, tags fix, `Stage::MonitorCorrected`, `IsotopeSignal::interference`, N3-N4 parity guard.
2. `Gas`, labels, normalising config, `GasResult` with plain ratios (no discrimination yet); `Quantity` ratios of any gas; `available_quantities`.
3. Reference-ratio document (load, renormalise, `air` default shipped under `configs/examples`), laws N5-N13, `Discrimination` fit target, `disc:<gas>` rows, `delta`, `rra`, `excess`, `disc_misfit`.
4. `[interferences]` config, snapshot into the record, N14, `Interferences` fit target, `if:` rows; `Ar40++`, `CO2++` added to the example mass table.
5. Pipette and spike documents, shot counter in the extraction host and record, `Sensitivity` fit target, `sens:` rows, N15-N22, amount stages.
6. Isotope dilution N23, `spike_blank` handling, `moles_id`.
7. Endmember document, N24-N27, `component`, `fraction`.
8. Kinetics N30-N34, `kinetics.*` names.
9. `ratio_isochron` unit (values only; scene in the figure spec).
10. `elctl export` concentration columns (after `libs/processing/AGENTS.md` report rules).

## Open questions

Only what the owner decides; everything else above is decided or verifiable.

- **Q1** Reference-ratio values and errors to ship as `air`: Eberhardt 1965
  (9.80, 0.0290) or Honda 2015 (9.777, 0.02905) for Ne; Clarke 1976 or
  Mamyrin 1970 for Ra; Basford 1973 for Kr and Xe with which errors. Which
  labs want which (the table is per lab, the shipped default is one
  choice).
- **Q2** Default discrimination law (`exponential` proposed), and whether a
  lab may set it per gas (proposed yes) or only per spectrometer.
- **Q3** Kr and Xe normalising isotope: per-lab config (proposed:
  `[reduction.gases.Xe] normalising`), defaults Kr84 and Xe130; or fixed
  Xe132 as some labs report.
- **Q4** Pseudo-detector rows in `icfactors` (§12, no migration) versus a
  new `corrections` revision kind (one migration, `revision` rebuild on
  SQLite). Proposed: rows now; promote only alongside A1.
- **Q5** Arrhenius fit when temperature error is 0: York with a token
  `sx` (proposed) or plain OLS through `fits.hpp`.
- **Q6** Should Ar's `ic_corrected` adopt a `disc:Ar` row when present
  (then the ArAr age sees law-based discrimination through `D_i`), or must
  Ar discrimination stay IC-factor-only (legacy)? Proposed: adopt when
  present, never both.
