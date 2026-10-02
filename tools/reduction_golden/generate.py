#!/usr/bin/env python3
r"""Generate the Ar-Ar reduction golden vectors by calling legacy pychron.

Spec: docs/superpowers/specs/2026-10-02-arar-reduction-design.md, section 9
(golden vectors), 4.7 (tolerances), 5.3 (constants presets), 5.6
(diagnostics). The committed JSON under tests/reduction/golden/ is the test
input of the C++ reduction tests; this script is only needed to regenerate it.
Never edit the JSON by hand.

Run (from the repo root):

    uv run --no-project --python 3.12 \
      --with numpy==2.4.4 --with scipy==1.17.1 --with statsmodels==0.14.6 \
      --with uncertainties==3.2.3 --with traits==7.1.0 --with pyyaml==6.0.3 \
      python tools/reduction_golden/generate.py \
        --legacy /Users/jakeross/Programming/pychron --out tests/reduction/golden

Add `--check` to regenerate into a temporary directory and exit 1 when any
file differs from `--out` (drift detector).

The legacy checkout is read only: before importing pychron the script points
HOME at a temporary directory (pychron.paths creates ~/.pychron.0), sets
sys.dont_write_bytecode (no __pycache__ in the legacy tree) and silences the
`std_dev==0` UserWarning of `uncertainties` and its comparison
FutureWarnings.

File format (all files)
=======================

    {"schema": 1, "legacy_commit": "<40 hex>", "legacy_dirty": bool,
     "packages": {"python": ..., "numpy": ..., "scipy": ..., "statsmodels": ...,
                  "uncertainties": ..., "traits": ..., "pyyaml": ...},
     "cases": [case, ...]}

Every case has exactly the keys `name` (unique per file), `source` (legacy
test or code location the case was harvested from), `inputs`, `expected`,
`tol`, `legacy_sentinel`, `expect_diagnostics`, `expect_error`.

* Value with uncertainty: {"v": nominal, "e": 1 sigma}. A plain number is a
  double. Non-finite numbers are the strings "nan", "inf", "-inf".
* `tol`: {"rtol", "rtol_err", "atol", "atol_err", "why"}. Defaults per spec
  4.7: rtol 1e-12 on nominals, rtol_err 1e-10 on sigmas and covariances,
  atol/atol_err 0. decay_factors.json uses rtol 1e-13. Error-component
  percentages (pipeline `age_error_components`) are compared with the fixed
  spec tolerance atol 1e-8, rtol 0, not with the case `tol`. A non-zero atol is
  set only where an expected value is a near cancellation and `why` says so;
  it is 1e-12 * max |input value|.
* `legacy_sentinel`: null, or an object holding what legacy produced for the
  quantities that the C++ API reports as absent (spec 7 "Fix" rows, D3), e.g.
  {"f": {"v": 1.0, "e": 0.0}}. The C++ test asserts the value is absent and
  the diagnostics are present. Special case: isotope_arithmetic
  `deadtime/legacy_6240` (D4) where the sentinel holds the result with the
  legacy 6240 fA->cps factor and `expected` holds the 6241.509 result.
* `expect_diagnostics`: spec 5.6 `Diagnostic` names in pipeline order
  (FixedK3739ZeroCa3937, CaClampedToZero, FUndefined, YieldUndefined,
  AgeUndefined, KCaUndefined, KClUndefined). Legacy has no diagnostics; they
  are derived here from the legacy intermediates with these rules:
    - FixedK3739ZeroCa3937: fixed K37/K39 mode is active (constants
      k3739_mode == "Fixed" or a per-analysis fixed_k3739) and
      nominal(Ca3937) == 0 (missing counts as 0).
    - CaClampedToZero: allow_negative_ca_correction is false and the
      unclamped nominal(ca37) <= 0 (legacy `max(ufloat(0, 0), ca37)` returned
      its first operand; this includes ca37 exactly 0).
    - FUndefined: nominal(k39) == 0.   YieldUndefined: nominal(a40) == 0.
    - AgeUndefined: F defined and 1 + nominal(J) * nominal(F) <= 0.
    - KCaUndefined: nominal(ca37) == 0 (after the clamp).
    - KClUndefined: nominal(cl38) == 0.
  Cases where kca or kcl would be exactly 0 with a non-zero divisor are not
  generated (the generator refuses them) because the spec does not define them.
* `expect_error`: null, or a substring the C++ `Result` error message must
  contain: "same unit" (decay guard, as legacy), "zero divisor" (E12/E13
  singular denominators, legacy ZeroDivisionError), "1 + J F" (age argument
  <= 0), "deadtime" (1 - n tau <= 0), or the isotope name for a missing slot.

Constants (`inputs.constants`, every case outside ufloat.json and
decay_factors.json): all fields of `ReductionConstants` (spec 5.3) with C++
member names: lambda_b, lambda_e, lambda_cl36, lambda_ar37, lambda_ar39,
atm4036, atm4038, fixed_k3739 ({v, e}); k3739_mode ("Normal" | "Fixed");
abundance_sensitivity (number); allow_negative_ca_correction,
use_irradiation_endtime, include_decay_error (bool); cosmogenic (null or
{"solar3836": {v, e}, "cosmo3836": {v, e}}); age_units ("a" | "ka" | "Ma" |
"Ga"). The legacy ArArConstants traits are set from these values; no case
relies on a legacy default. Preset-sensitive files (interference, calculate_f,
age, pipeline, chlorine) emit every case twice, named "<name>@legacy" (trait
defaults, arar_constants.py:28-94) and "<name>@legacy_preferences" (brief /
spec 5.3: trait defaults with allow_negative_ca_correction = false, lambda_b
error 0 and fixed_k3739 error 0.01); per-case overrides are applied on top.

Production (`inputs.production`): the production_value rows as given, key ->
{v, e}; missing keys are absent (spec 5.4 `production_from_rows`). Legacy
receives the INTERFERENCE_KEYS as tagged ufloats (interference_corrections)
and Ca_K/Cl_K as `production_ratios`, as dvc_analysis.set_production does.

Per file
========

ufloat.json (uncertainties only): inputs = {"vars": {name: {v, e, tag?}},
  "steps": [[out, op, a] | [out, op, a, b]]}; operands are a variable/step
  name (string) or a literal number. Ops: add sub mul div neg exp log log10
  sqrt abs pow. expected = {out: {v, e}} for every step output, plus
  "cov": [[a, b, cov], ...] over all pairs (a <= b in step order, diagonal
  included) of step outputs (uncertainties.covariance_matrix). inputs.ops
  lists the ops used (Task 2 takes the cases using only add sub mul div neg).

constants.json: "preset/<default|legacy|legacy_preferences>": expected =
  {"constants": table, "lambda_k": {v, e}} (+ "atm3836", "to_dict" from
  legacy for the two legacy tables). "preset/legacy_preferences" also records
  `inputs.pane_defaults` (raw defaults parsed from
  constants/tasks/arar_constants_preferences.py) and
  `inputs.pane_differs_from_preset` (fields where the pane default differs
  from the preset). "scale_age/<from>_to_<to>": inputs {value, current,
  target}, expected {"value"}.

isotope_arithmetic.json: inputs.function is
  - "isotope": {isotope, intercept, baseline, blank, ic_factor,
    discrimination, include_baseline_error, correct_for_blank};
    expected {baseline_corrected (E1), non_detector_corrected (E2),
    intensity (E3)}.
  - "abundance_sensitivity": {signals: {Ar40..Ar36}, abundance_sensitivity};
    expected {Ar40..Ar36}.
  - "deadtime": {signal (fA), tau_s, fa_to_cps}; expected {"corrected"}
    (E5, formula of deadtime.py:54-55 evaluated with uncertainties).

decay_factors.json (tol rtol 1e-13): inputs.function is
  - "decay_factors": {lambda37, lambda39 (per day), segments: [{power,
    duration_days, dt_days}]}; expected {df37, df39}.
  - "irradiation_from_doses": {doses: [{power, start_utc_s, end_utc_s}],
    analysis_utc_s, use_irradiation_endtime, lambda37, lambda39}; expected
    {segments, decay_days, df37, df39}. Segment arithmetic is
    dvc_analysis.set_chronology (:370-395) on UTC datetimes; decay_days is from
    the first dose's start in UTC (spec Q12 Fix).

interference.json: {a39, a37, production, constants, fixed_k3739 ({v, e} per
  analysis, or null)}; expected {k37, k38, k39, ca36, ca37, ca38, ca39}.

atmospheric.json: inputs.function is
  - "atmospheric_components": {a38, a36, k38, ca38, ca36, decay_days,
    production (Cl3638 only), constants}; expected {atm36, atm38, cl36, cl38}.
  - "cosmogenic_components": {c36, c38, constants (cosmogenic set)};
    expected {cosmo36, cosmo38, noncosmo36, noncosmo38}.

calculate_f.json: {isotopes: {Ar40..Ar36: {v, e}} (the n values: already
  corrected, decay corrected), decay_days, production, constants,
  fixed_k3739}; expected = FResult: {f, f_err_wo_irrad, atm40, k40, rad40,
  radiogenic_yield, interference: {k37..ca39}, atmospheric: {atm36, atm38,
  cl36, cl38} (after the cosmogenic split when enabled), cosmogenic: {cosmo36,
  cosmo38, noncosmo36, noncosmo38} (only when enabled),
  interference_corrected: {Ar40..Ar36}}. Absent f / radiogenic_yield mean
  undefined (see legacy_sentinel); f_err_wo_irrad is then 0.

age.json: {j, f, constants, lambda_k_total ({v, e} or null)}; expected
  {"age"} in constants.age_units.

pipeline.json (ArArAge.calculate_age): inputs = {function: "reduce",
  isotopes: {Ar40..Ar36: {intercept, baseline, blank, ic_factor,
  include_baseline_error, correct_for_blank}}, constants, production,
  irradiation: {segments, decay_days}, j ({v, e} or null), position_jerr,
  lambda_k_total, fixed_k3739}; expected = {decay: {df37, df39}, corrected:
  {Ar40..Ar36}, f: FResult (as calculate_f.json), ages: {age, age_w_j_err,
  age_w_position_err, age_err_wo_irrad, age_err_wo_j_irrad}, kca, cak, kcl,
  clk, age_error_components: {Ar40..Ar36: percent}}; undefined members are
  absent. Seeded intercepts are the legacy fit of arar_age_test._build_isotope
  (arar_age_test.py:26-36), stored as value/error. age_err_wo_irrad is the
  std of legacy uage over its variables whose tag is not an interference key
  (spec Q13); age_err_wo_j_irrad is the same number. Cases whose name contains
  "chlorine" have Cl3638 > 0 (Task 12); all other cases still carry legacy
  kcl/clk computed from the residual cl38.

chlorine.json: inputs.function "calculate_f" (calculate_f.json format) or
  "reduce" (pipeline.json format).

correlation.json (one family, @legacy constants only, not preset suffixed):
  - "age_equation_pair": {j, f_a, f_b, constants, lambda_k_total}; one J
    ufloat shared by both ages. expected {f_a, f_b, age_a, age_b, cov}.
  - "reduce_pair": {analyses: {A: pipeline inputs without constants, B: ...},
    constants, shared: {"j": true} | {"blank": "Ar36"}}; with "j" both
    analyses use the J of A as one variable; with "blank" both use A's blank
    of that isotope as one variable (B's own blank row is ignored).
    expected {A.f, B.f, A.age, B.age, A.age_w_j_err, B.age_w_j_err, cov}.
  `cov` entries are [key_a, key_b, covariance] over all pairs (diagonal
  included) of the listed expected keys, from uncertainties.covariance_matrix.

Deliberate departures from the harvested legacy test inputs
============================================================

* Ar37 = 1.0 with Ar39 = 100, K3739 = 0.01, Ca3937 = 0.0007
  (argon_calculations_test._isotopes, InterferenceCorrectionsTest) makes
  ca37 = a37 - K3739 k39 a total cancellation whose sign is set by rounding
  (and decides the E11 clamp); those cases use Ar37 = 5.0. The generator
  refuses any case with |unclamped ca37| <= 1e-9 |a37|.
* No case overflows a variance: Python raises OverflowError on (d sigma)^2 >
  DBL_MAX where C++ yields inf, so "huge sigma" stays at <= 1e152.
* Ca_K/Cl_K are passed as `production_ratios` (dvc_analysis.set_production),
  not inside the interference dict as arar_age_test._build_age does.
* `ufloat/func/pow_at_zero_non_integer` records uncertainties' NaN
  derivative for pow(x, 2.5) at x = 0 (spec 4.3 says 0; Task 3 decides).
"""

from __future__ import annotations

import argparse
import ast
import copy
import importlib.metadata
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import warnings
from datetime import datetime, timezone

SCHEMA = 1
ARGON_KEYS = ("Ar40", "Ar39", "Ar38", "Ar37", "Ar36")
INTERFERENCE_KEYS = ("K4039", "K3839", "K3739", "Ca3937", "Ca3837", "Ca3637", "Cl3638")
RATIO_KEYS = ("Ca_K", "Cl_K")
DETECTORS = dict(zip(ARGON_KEYS, ("H1", "AX", "L1", "L2", "CDD")))
CONSTANT_KEYS = (
    "lambda_b",
    "lambda_e",
    "lambda_cl36",
    "lambda_ar37",
    "lambda_ar39",
    "atm4036",
    "atm4038",
    "k3739_mode",
    "fixed_k3739",
    "abundance_sensitivity",
    "allow_negative_ca_correction",
    "use_irradiation_endtime",
    "cosmogenic",
    "include_decay_error",
    "age_units",
)
FILES = (
    "ufloat.json",
    "constants.json",
    "isotope_arithmetic.json",
    "decay_factors.json",
    "interference.json",
    "atmospheric.json",
    "calculate_f.json",
    "age.json",
    "pipeline.json",
    "chlorine.json",
    "correlation.json",
)
PRESET_SENSITIVE = (
    "interference.json",
    "calculate_f.json",
    "age.json",
    "pipeline.json",
    "chlorine.json",
)
DEFAULT_TOL = {"rtol": 1e-12, "rtol_err": 1e-10, "atol": 0.0, "atol_err": 0.0, "why": ""}
DECAY_TOL = {"rtol": 1e-13, "rtol_err": 1e-10, "atol": 0.0, "atol_err": 0.0, "why": ""}
FA_TO_CPS = 6241.509  # spec D4
LEGACY_FA_TO_CPS = 6240.0  # deadtime.py:62
LEGACY_PACKAGES = (
    ("numpy", "numpy"),
    ("scipy", "scipy"),
    ("statsmodels", "statsmodels"),
    ("uncertainties", "uncertainties"),
    ("traits", "traits"),
    ("pyyaml", "PyYAML"),
)


class _Legacy:
    """Legacy and `uncertainties` symbols, bound by bootstrap()."""


L = _Legacy()


def bootstrap(legacy):
    os.environ["HOME"] = tempfile.mkdtemp(prefix="reduction-golden-home-")
    sys.dont_write_bytecode = True
    sys.path.insert(0, legacy)
    warnings.filterwarnings("ignore", message="Using UFloat objects with std_dev==0")
    warnings.filterwarnings("ignore", message=r"AffineScalarFunc\..* is deprecated")

    import uncertainties
    from uncertainties import umath

    from pychron.processing import argon_calculations
    from pychron.processing.arar_age import ArArAge
    from pychron.processing.arar_constants import ArArConstants
    from pychron.processing.isotope import Isotope
    from pychron.processing.tests import arar_age_test, isotope_arithmetic_test

    L.unc = uncertainties
    L.umath = umath
    L.ufloat = uncertainties.ufloat
    L.AffineScalarFunc = uncertainties.core.AffineScalarFunc
    L.ac = argon_calculations
    L.ArArAge = ArArAge
    L.ArArConstants = ArArConstants
    L.Isotope = Isotope
    L.arar_age_test = arar_age_test
    L.isotope_arithmetic_test = isotope_arithmetic_test


# ----------------------------------------------------------------------------
# encoding helpers
# ----------------------------------------------------------------------------


def num(x):
    x = float(x)
    if math.isnan(x):
        return "nan"
    if math.isinf(x):
        return "inf" if x > 0 else "-inf"
    return x


def fval(x):
    if isinstance(x, str):
        return {"nan": math.nan, "inf": math.inf, "-inf": -math.inf}[x]
    return float(x)


def nom(x):
    return float(L.unc.nominal_value(x))


def sd(x):
    return float(L.unc.std_dev(x))


def uv(x):
    return {"v": num(nom(x)), "e": num(sd(x))}


def mv(v, e=0.0):
    return {"v": num(v), "e": num(e)}


def ve(m):
    """{v, e} -> (v, e) floats."""
    return fval(m["v"]), fval(m["e"])


def case(name, source, inputs, expected, tol=None, sentinel=None, diagnostics=(), error=None):
    t = dict(DEFAULT_TOL)
    if tol:
        t.update(tol)
    return {
        "name": name,
        "source": source,
        "inputs": inputs,
        "expected": expected,
        "tol": t,
        "legacy_sentinel": sentinel if sentinel else None,
        "expect_diagnostics": list(diagnostics),
        "expect_error": error,
    }


def cancellation_tol(inputs, why):
    """atol = 1e-12 * max |input value| (spec 4.7) with the reason recorded.

    Input values are the nominals and plain numbers of `inputs`, excluding
    sigmas, constants, times (decay_days, irradiation) and position_jerr.
    """
    biggest = [0.0]

    def walk(o, key=None):
        if key in ("constants", "e", "tau_s", "fa_to_cps", "legacy_fa_to_cps", "decay_days",
                   "irradiation", "position_jerr"):
            return
        if isinstance(o, dict):
            for k, v in o.items():
                walk(v, k)
        elif isinstance(o, list):
            for v in o:
                walk(v)
        elif isinstance(o, float) and math.isfinite(o):
            biggest[0] = max(biggest[0], abs(o))

    walk(inputs)
    return {"atol": 1e-12 * biggest[0], "why": why}


def cov_entries(names, values):
    m = L.unc.covariance_matrix(values)
    out = []
    for i, a in enumerate(names):
        for j in range(i, len(names)):
            out.append([a, names[j], num(m[i][j])])
    return out


# ----------------------------------------------------------------------------
# constants
# ----------------------------------------------------------------------------

# Spec 5.3 preset table (checked against the legacy traits at run time).
SPEC_LEGACY = {
    "lambda_b": (4.962e-10, 9.3e-13),
    "lambda_e": (5.81e-11, 1.6e-13),
    "lambda_cl36": (6.308e-9, 0.0),
    "lambda_ar37": (0.01975, 0.0),
    "lambda_ar39": (7.068e-6, 0.0),
    "atm4036": (295.5, 0.5),
    "atm4038": (1575.0, 2.0),
    "fixed_k3739": (0.01, 0.0001),
}
# Brief / spec 5.3: `@legacy_preferences` = trait defaults with these changes.
LEGACY_PREFERENCES_OVERRIDES = {
    "allow_negative_ca_correction": False,
    "lambda_b": (4.962e-10, 0.0),
    "fixed_k3739": (0.01, 0.01),
}
# Spec 5.3 `Default` preset (D2, D5): not legacy, recorded for Task 4.
DEFAULT_OVERRIDES = {
    "atm4036": (298.56, 0.31),
    "lambda_b": (4.962e-10, 9.3e-13),
    "fixed_k3739": (0.01, 0.01),
    "allow_negative_ca_correction": False,
}
_MEASURED = ("lambda_b", "lambda_e", "lambda_cl36", "lambda_ar37", "lambda_ar39", "atm4036",
             "atm4038", "fixed_k3739")
_TRAIT = {
    "lambda_b": "lambda_b",
    "lambda_e": "lambda_e",
    "lambda_cl36": "lambda_Cl36",
    "lambda_ar37": "lambda_Ar37",
    "lambda_ar39": "lambda_Ar39",
    "atm4036": "atm4036",
    "atm4038": "atm4038",
    "fixed_k3739": "k3739",
}


def legacy_trait_table():
    ac = L.ArArConstants()
    t = {}
    for key in _MEASURED:
        attr = _TRAIT[key]
        t[key] = mv(getattr(ac, attr + "_v"), getattr(ac, attr + "_e"))
        if (fval(t[key]["v"]), fval(t[key]["e"])) != SPEC_LEGACY[key]:
            raise SystemExit("legacy trait {} = {} differs from spec 5.3".format(key, t[key]))
    t["k3739_mode"] = str(ac.k3739_mode)
    t["abundance_sensitivity"] = num(ac.abundance_sensitivity)
    t["allow_negative_ca_correction"] = bool(ac.allow_negative_ca_correction)
    t["use_irradiation_endtime"] = bool(ac.use_irradiation_endtime)
    t["cosmogenic"] = None
    if ac.use_cosmogenic_correction:
        raise SystemExit("legacy default has cosmogenic correction enabled")
    t["include_decay_error"] = False  # age_equation default argument
    t["age_units"] = "Ma"  # legacy "" means years; spec Q15 Fix
    if t["k3739_mode"] != "Normal" or not t["allow_negative_ca_correction"]:
        raise SystemExit("legacy trait defaults differ from spec 5.3")
    return t


def with_overrides(table, overrides):
    t = copy.deepcopy(table)
    for k, v in overrides.items():
        if k not in CONSTANT_KEYS:
            raise KeyError(k)
        if k in _MEASURED:
            t[k] = mv(*v)
        elif k == "cosmogenic":
            t[k] = None if v is None else {"solar3836": mv(*v[0]), "cosmo3836": mv(*v[1])}
        elif k == "abundance_sensitivity":
            t[k] = num(v)
        else:
            t[k] = v
    return t


def presets():
    legacy = legacy_trait_table()
    return {
        "legacy": legacy,
        "legacy_preferences": with_overrides(legacy, LEGACY_PREFERENCES_OVERRIDES),
        "default": with_overrides(legacy, DEFAULT_OVERRIDES),
    }


def make_constants(c):
    """Legacy ArArConstants with every trait set from the case constants."""
    ac = L.ArArConstants()
    apply_constants(ac, c)
    return ac


def apply_constants(ac, c):
    for key in _MEASURED:
        v, e = ve(c[key])
        setattr(ac, _TRAIT[key] + "_v", v)
        setattr(ac, _TRAIT[key] + "_e", e)
    ac.k3739_mode = c["k3739_mode"]
    ac.abundance_sensitivity = fval(c["abundance_sensitivity"])
    ac.allow_negative_ca_correction = c["allow_negative_ca_correction"]
    ac.use_irradiation_endtime = c["use_irradiation_endtime"]
    if c["cosmogenic"]:
        ac.set_cosmogenic_ratios(ve(c["cosmogenic"]["solar3836"]), ve(c["cosmogenic"]["cosmo3836"]))
    else:
        ac.use_cosmogenic_correction = False
    ac.age_units = c["age_units"]
    return ac


def parse_pane_defaults(legacy):
    path = os.path.join(legacy, "pychron", "constants", "tasks", "arar_constants_preferences.py")
    with open(path, encoding="utf-8") as fh:
        tree = ast.parse(fh.read())
    cls = next(
        n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "ArArConstantsPreferences"
    )
    wanted = (
        "ar40_ar36_atm", "ar40_ar36_atm_error", "ar40_ar38_atm", "ar40_ar38_atm_error",
        "lambda_e", "lambda_e_error", "lambda_b", "lambda_b_error", "lambda_cl36",
        "lambda_cl36_error", "lambda_ar37", "lambda_ar37_error", "lambda_ar39",
        "lambda_ar39_error", "ar37_ar39_mode", "ar37_ar39", "ar37_ar39_error",
        "allow_negative_ca_correction", "use_irradiation_endtime", "abundance_sensitivity",
        "age_units",
    )
    out = {}
    for node in cls.body:
        if not (isinstance(node, ast.Assign) and len(node.targets) == 1):
            continue
        name = getattr(node.targets[0], "id", None)
        if name not in wanted:
            continue
        if isinstance(node.value, ast.Name):  # bare trait type, e.g. `Bool`
            kind, args = node.value.id, []
        elif isinstance(node.value, ast.Call):
            kind = node.value.func.id
            args = [ast.literal_eval(a) for a in node.value.args]
        else:
            continue
        if kind == "Float":
            out[name] = num(args[0] if args else 0.0)
        elif kind == "Bool":
            out[name] = bool(args[0]) if args else False
        elif kind == "Enum":
            out[name] = args[0]
        else:
            raise SystemExit("unexpected trait {} for {}".format(kind, name))
    missing = sorted(set(wanted) - set(out))
    if missing:
        raise SystemExit("pane defaults missing {}".format(missing))
    return out


def pane_differences(pane, table):
    pairs = {
        "lambda_b": ("lambda_b", "lambda_b_error"),
        "lambda_e": ("lambda_e", "lambda_e_error"),
        "lambda_cl36": ("lambda_cl36", "lambda_cl36_error"),
        "lambda_ar37": ("lambda_ar37", "lambda_ar37_error"),
        "lambda_ar39": ("lambda_ar39", "lambda_ar39_error"),
        "atm4036": ("ar40_ar36_atm", "ar40_ar36_atm_error"),
        "atm4038": ("ar40_ar38_atm", "ar40_ar38_atm_error"),
        "fixed_k3739": ("ar37_ar39", "ar37_ar39_error"),
    }
    diffs = []
    for key, (pv, pe) in pairs.items():
        if fval(pane[pv]) != fval(table[key]["v"]):
            diffs.append(key + ".v")
        if fval(pane[pe]) != fval(table[key]["e"]):
            diffs.append(key + ".e")
    for key, pk in (
        ("k3739_mode", "ar37_ar39_mode"),
        ("allow_negative_ca_correction", "allow_negative_ca_correction"),
        ("use_irradiation_endtime", "use_irradiation_endtime"),
        ("abundance_sensitivity", "abundance_sensitivity"),
    ):
        if pane[pk] != table[key]:
            diffs.append(key)
    return diffs


# ----------------------------------------------------------------------------
# legacy call helpers
# ----------------------------------------------------------------------------


def interferences(rows):
    return {k: L.ufloat(v, e, tag=k) for k, (v, e) in rows.items() if k in INTERFERENCE_KEYS}


def ratios(rows):
    return {k: L.ufloat(v, e, tag=k) for k, (v, e) in rows.items() if k in RATIO_KEYS}


def rows_json(rows):
    for k in rows:
        if k not in INTERFERENCE_KEYS + RATIO_KEYS:
            raise KeyError(k)
    return {k: mv(v, e) for k, (v, e) in rows.items()}


def fixed_arg(fixed):
    # Per-analysis value as k3739_edit.py:53 builds it (untagged ufloat).
    return L.ufloat(*fixed) if fixed is not None else False


def fixed_json(fixed):
    return mv(*fixed) if fixed is not None else None


class Capture:
    """Record the intermediates calculate_f computes but does not return.

    Wraps argon_calculations.calculate_atmospheric /
    calculate_cosmogenic_components and the module-level `max` used by the
    E11 clamp. calc_f runs twice per calculate_f (zero-error ratios first);
    the last record is the real pass.
    """

    def __enter__(self):
        mod = L.ac
        self.atm, self.cosmo, self.clamp = [], [], []
        self._orig = (mod.calculate_atmospheric, mod.calculate_cosmogenic_components)
        orig_atm, orig_cosmo = self._orig

        def atm(*a, **kw):
            r = orig_atm(*a, **kw)
            self.atm.append(r)
            return r

        def cosmo(*a, **kw):
            r = orig_cosmo(*a, **kw)
            self.cosmo.append(r)
            return r

        def recording_max(*a, **kw):
            r = max(*a, **kw)
            if len(a) == 2 and isinstance(a[1], L.AffineScalarFunc):
                self.clamp.append((nom(a[1]), r is a[0]))
            return r

        mod.calculate_atmospheric = atm
        mod.calculate_cosmogenic_components = cosmo
        mod.max = recording_max
        return self

    def __exit__(self, *exc):
        mod = L.ac
        mod.calculate_atmospheric, mod.calculate_cosmogenic_components = self._orig
        del mod.max
        return False


def check_ca37_conditioning(a37, ca37_out, clamp_records, c):
    """Refuse cases where ca37 = a37 - k37 is a total cancellation.

    There the sign of ca37 (and so the E11 clamp and K/Ca) is decided by
    rounding, which C++ need not reproduce.
    """
    if c["allow_negative_ca_correction"] or not clamp_records:
        unclamped = nom(ca37_out)
    else:
        unclamped = clamp_records[-1][0]
    if a37 != 0 and abs(unclamped) <= 1e-9 * abs(a37):
        raise RuntimeError("ill-conditioned ca37 {} for a37 {}".format(unclamped, a37))


def interference_diagnostics(c, rows, fixed, clamp_records):
    diags = []
    fixed_active = c["k3739_mode"].lower() != "normal" or (
        fixed is not None and (fixed[0] != 0 or fixed[1] != 0)
    )
    if fixed_active and rows.get("Ca3937", (0.0, 0.0))[0] == 0:
        diags.append("FixedK3739ZeroCa3937")
    if not c["allow_negative_ca_correction"]:
        if not clamp_records:
            raise RuntimeError("clamp not observed")
        unclamped, applied = clamp_records[-1]
        if applied != (not (unclamped > 0)):
            raise RuntimeError("clamp rule mismatch")
        if applied:
            diags.append("CaClampedToZero")
    return diags


def excluded_std(x, tags):
    s = 0.0
    for var, err in x.error_components().items():
        if var.tag not in tags:
            s += err**2
    return math.sqrt(s)


def f_result(f, f_wo_std, nar, comp, ifc, cap, c, rows, fixed):
    """FResult expectation (spec 5.6) from legacy calculate_f outputs."""
    diags = interference_diagnostics(c, rows, fixed, cap.clamp)
    check_ca37_conditioning(nom(ifc["Ar37"]), nar["ca37"], cap.clamp, c)
    atm36, atm38, cl36, cl38 = cap.atm[-1]
    exp = {
        "atm40": uv(comp["atm40"]),
        "k40": uv(nar["k40"]),
        "rad40": uv(comp["rad40"]),
        "interference": {
            "k37": uv(nar["k37"]),
            "k38": uv(nar["k38"]),
            "k39": uv(comp["k39"]),
            "ca36": uv(nar["ca36"]),
            "ca37": uv(nar["ca37"]),
            "ca38": uv(nar["ca38"]),
            "ca39": uv(nar["ca39"]),
        },
        "atmospheric": {
            "atm36": uv(atm36),
            "atm38": uv(atm38),
            "cl36": uv(nar["cl36"]),
            "cl38": uv(nar["cl38"]),
        },
        "interference_corrected": {k: uv(ifc[k]) for k in ARGON_KEYS},
    }
    if nom(cl36) != nom(nar["cl36"]) or nom(cl38) != nom(nar["cl38"]):
        raise RuntimeError("capture out of sync")
    if c["cosmogenic"]:
        cosmo36, cosmo38, noncosmo36, noncosmo38 = cap.cosmo[-1]
        exp["cosmogenic"] = {
            "cosmo36": uv(cosmo36),
            "cosmo38": uv(cosmo38),
            "noncosmo36": uv(noncosmo36),
            "noncosmo38": uv(noncosmo38),
        }
        exp["atmospheric"]["atm36"] = uv(noncosmo36)
        exp["atmospheric"]["atm38"] = uv(noncosmo38)
    sentinel = {}
    if nom(comp["k39"]) == 0:
        if (nom(f), sd(f)) != (1.0, 0.0):
            raise RuntimeError("legacy F sentinel expected")
        diags.append("FUndefined")
        sentinel["f"] = uv(f)
        exp["f_err_wo_irrad"] = 0.0
    else:
        exp["f"] = uv(f)
        exp["f_err_wo_irrad"] = num(f_wo_std)
        alt = excluded_std(f, INTERFERENCE_KEYS)
        if abs(alt - f_wo_std) > 1e-9 * max(abs(f_wo_std), 1e-300):
            raise RuntimeError("E15 identity failed: {} vs {}".format(alt, f_wo_std))
    if nom(comp["a40"]) == 0:
        diags.append("YieldUndefined")
        sentinel["radiogenic_yield"] = uv(comp["radiogenic_yield"])
    else:
        exp["radiogenic_yield"] = uv(comp["radiogenic_yield"])
    return exp, sentinel, diags


# ----------------------------------------------------------------------------
# ufloat.json
# ----------------------------------------------------------------------------

UNARY = {
    "neg": lambda a: -a,
    "exp": lambda a: L.umath.exp(a),
    "log": lambda a: L.umath.log(a),
    "log10": lambda a: L.umath.log10(a),
    "sqrt": lambda a: L.umath.sqrt(a),
    "abs": lambda a: abs(a),
}
BINARY = {
    "add": lambda a, b: a + b,
    "sub": lambda a, b: a - b,
    "mul": lambda a, b: a * b,
    "div": lambda a, b: a / b,
    "pow": lambda a, b: a**b,
}

UFLOAT_PROGRAMS = (
    (
        "arith/basic",
        {"x": (2.0, 0.1, "x"), "y": (3.0, 0.2, "y")},
        [
            ["a", "add", "x", "y"],
            ["b", "sub", "x", "y"],
            ["c", "mul", "x", "y"],
            ["d", "div", "x", "y"],
            ["e", "neg", "x"],
            ["f", "mul", 2.5, "x"],
            ["g", "div", 1.0, "x"],
            ["h", "sub", "x", 1.5],
            ["i", "add", 4.0, "y"],
            ["k", "div", "y", 4.0],
            ["m", "sub", 10.0, "y"],
        ],
    ),
    (
        "arith/self_cancellation",
        {"x": (2.0, 0.1, "x")},
        [
            ["a", "sub", "x", "x"],
            ["b", "div", "x", "x"],
            ["c", "add", "x", "x"],
            ["d", "mul", "x", "x"],
            ["e", "sub", "d", "d"],
            ["f", "sub", "c", "x"],
        ],
    ),
    (
        "arith/shared_variables",
        {"x": (2.0, 0.1, "x"), "y": (3.0, 0.2, "y"), "z": (5.0, 0.3, "z")},
        [
            ["a", "add", "x", "y"],
            ["b", "sub", "x", "y"],
            ["c", "mul", "a", "b"],
            ["d", "div", "c", "z"],
            ["e", "sub", "d", "a"],
        ],
    ),
    (
        "arith/zero_sigma",
        {"x": (5.0, 0.0, "x"), "y": (2.0, 0.3, "y")},
        [
            ["a", "mul", "x", "y"],
            ["b", "div", "x", "y"],
            ["c", "add", "x", 1.0],
            ["d", "sub", "a", "y"],
        ],
    ),
    (
        "arith/huge_sigma",
        # (d sigma)^2 must stay finite: uncertainties raises OverflowError (Python
        # float **) where C++ would give inf, so no case overflows the variance.
        {"x": (1.0, 1e150, "x"), "y": (2.0, 1e152, "y")},
        [
            ["a", "mul", "x", "x"],
            ["b", "add", "x", 1.0],
            ["c", "mul", "y", 1.0],
            ["d", "add", "b", "x"],
        ],
    ),
    (
        "arith/independent_same_value_and_tag",
        {"x": (1.0, 0.1, "J"), "y": (1.0, 0.1, "J")},
        [
            ["a", "add", "x", 0.0],
            ["b", "add", "y", 0.0],
            ["c", "sub", "x", "y"],
        ],
    ),
    (
        "func/exp_log_sqrt",
        {"x": (0.5, 0.01, "x"), "y": (4.0, 0.2, "y")},
        [
            ["a", "exp", "x"],
            ["b", "log", "y"],
            ["c", "log10", "y"],
            ["d", "sqrt", "y"],
            ["e", "log", "a"],
            ["f", "mul", "b", "d"],
        ],
    ),
    (
        "func/pow_constant_exponent",
        {"x": (2.0, 0.1, "x")},
        [
            ["a", "pow", "x", 2.5],
            ["b", "pow", "x", 0.0],
            ["c", "pow", "x", -1.0],
            ["d", "pow", "x", 3.0],
            ["e", "pow", "x", 1.0],
        ],
    ),
    (
        "func/pow_variable_exponent",
        {"x": (2.0, 0.1, "x"), "y": (3.0, 0.2, "y")},
        [
            ["a", "pow", "x", "y"],
            ["b", "pow", 2.0, "y"],
            ["c", "pow", "y", "x"],
            ["d", "pow", "a", 0.5],
        ],
    ),
    (
        "func/pow_at_zero",
        {"z": (0.0, 0.1, "z"), "w": (2.0, 0.1, "w")},
        [
            ["a", "pow", "z", 2.0],
            ["b", "pow", "z", 0.0],
            ["c", "pow", "z", 1.0],
            ["d", "pow", "z", 3.0],
            ["e", "pow", "z", "w"],
        ],
    ),
    (
        "func/pow_at_zero_non_integer",
        {"z": (0.0, 0.1, "z")},
        [
            ["a", "pow", "z", 2.5],
        ],
    ),
    (
        "func/abs",
        {"x": (-3.0, 0.1, "x"), "y": (2.0, 0.1, "y")},
        [
            ["a", "abs", "x"],
            ["b", "abs", "y"],
            ["c", "mul", "a", "x"],
        ],
    ),
    (
        "cov/matrix",
        {"x": (2.0, 0.1, "x"), "y": (3.0, 0.2, "y"), "z": (5.0, 0.3, "z")},
        [
            ["a", "mul", "x", "y"],
            ["b", "div", "y", "z"],
            ["c", "log", "a"],
            ["d", "add", "c", "b"],
            ["e", "sqrt", "z"],
        ],
    ),
)


def gen_ufloat():
    cases = []
    for name, vars_, steps in UFLOAT_PROGRAMS:
        env = {k: L.ufloat(v, e, tag=t) for k, (v, e, t) in vars_.items()}

        def operand(o):
            return env[o] if isinstance(o, str) else float(o)

        outs = []
        ops = set()
        for step in steps:
            out, op = step[0], step[1]
            ops.add(op)
            if op in UNARY:
                env[out] = UNARY[op](operand(step[2]))
            else:
                env[out] = BINARY[op](operand(step[2]), operand(step[3]))
            outs.append(out)
        expected = {o: uv(env[o]) for o in outs}
        expected["cov"] = cov_entries(outs, [env[o] for o in outs])
        inputs = {
            "vars": {k: {"v": num(v), "e": num(e), "tag": t} for k, (v, e, t) in vars_.items()},
            "steps": [[s if isinstance(s, str) else num(s) for s in step] for step in steps],
            "ops": sorted(ops),
        }
        tol = None
        if name == "arith/self_cancellation":
            tol = {"atol": 1e-12 * 2.0, "atol_err": 1e-12 * 0.1,
                   "why": "x - x and d - d are exact cancellations (expected 0 +- 0)"}
        cases.append(case("ufloat/" + name, "uncertainties 3.2.3", inputs, expected, tol=tol))
    return cases


# ----------------------------------------------------------------------------
# constants.json
# ----------------------------------------------------------------------------


def gen_constants(legacy):
    pr = presets()
    pane = parse_pane_defaults(legacy)
    cases = []
    for key, source in (
        ("legacy", "arar_constants.py:28-94 (ArArConstants trait defaults); arar_constants_test.py"),
        (
            "legacy_preferences",
            "spec 5.3 / task brief: trait defaults with allow_negative_ca_correction=False, "
            "lambda_b error 0, k3739 error 0.01 (constants/tasks/arar_constants_preferences.py:144-167)",
        ),
        ("default", "spec 5.3 Default preset (D2, D5); not legacy"),
    ):
        table = pr[key]
        ac = make_constants(table)
        expected = {"constants": table, "lambda_k": uv(ac.lambda_k)}
        inputs = {"constants": table, "preset": key}
        if key != "default":
            expected["atm3836"] = uv(ac.atm3836)
            expected["to_dict"] = {k: num(v) for k, v in ac.to_dict().items()}
        if key == "legacy_preferences":
            inputs["pane_defaults"] = pane
            inputs["pane_differs_from_preset"] = pane_differences(pane, table)
        cases.append(case("preset/" + key, source, inputs, expected))
    ac = make_constants(pr["legacy"])
    for cur in ("a", "ka", "Ma", "Ga"):
        for tgt in ("a", "ka", "Ma", "Ga"):
            value = 7.25
            got = ac.scale_age(value, target=tgt, current=cur)
            cases.append(
                case(
                    "scale_age/{}_to_{}".format(cur, tgt),
                    "arar_constants.py:143-170; arar_constants_test.py:ScaleAgeTest",
                    {"constants": pr["legacy"], "value": value, "current": cur, "target": tgt},
                    {"value": num(got)},
                )
            )
    return cases


# ----------------------------------------------------------------------------
# isotope_arithmetic.json
# ----------------------------------------------------------------------------


def fitted_intercept(intercept, name="Ar40", detector="H1"):
    """Legacy fit of isotope_arithmetic_test._build_iso, as (value, error)."""
    iso = L.isotope_arithmetic_test._build_iso(name=name, detector=detector, intercept=intercept)
    u = iso.uvalue
    return nom(u), sd(u)


def run_isotope(spec):
    name = spec.get("isotope", "Ar40")
    iso = L.Isotope(name, DETECTORS[name])
    iso.set_uvalue(spec["intercept"])
    iso.set_baseline(*spec.get("baseline", (0.0, 0.0)))
    iso.set_blank(*spec.get("blank", (0.0, 0.0)))
    iso.include_baseline_error = spec.get("include_baseline_error", False)
    iso.correct_for_blank = spec.get("correct_for_blank", True)
    ic = spec.get("ic_factor", (1.0, 0.0))
    disc = spec.get("discrimination", (1.0, 0.0))
    iso.ic_factor = L.ufloat(*ic, tag="{} IC".format(name))
    iso.discrimination = L.ufloat(*disc)
    inputs = {
        "function": "isotope",
        "isotope": name,
        "intercept": mv(*spec["intercept"]),
        "baseline": mv(*spec.get("baseline", (0.0, 0.0))),
        "blank": mv(*spec.get("blank", (0.0, 0.0))),
        "ic_factor": mv(*ic),
        "discrimination": mv(*disc),
        "include_baseline_error": iso.include_baseline_error,
        "correct_for_blank": iso.correct_for_blank,
    }
    expected = {
        "baseline_corrected": uv(iso.get_baseline_corrected_value()),
        "non_detector_corrected": uv(iso.get_non_detector_corrected_value()),
        "intensity": uv(iso.get_intensity()),
    }
    return inputs, expected


def deadtime(signal, tau, factor):
    # deadtime.py:54-55 (`v / (1 - v * tau)`), applied on counts/s.
    n = signal * factor
    return (n / (1 - n * tau)) / factor


def gen_isotope_arithmetic():
    pr = presets()
    c = pr["legacy"]
    cases = []
    src = "isotope_arithmetic_test.py:182-233 (CorrectionMethodsTest)"
    i100 = fitted_intercept(100.0)
    i50 = fitted_intercept(50.0)
    specs = (
        ("isotope/baseline_only", src + ":test_baseline_only",
         {"intercept": i100, "baseline": (5.0, 0.0), "correct_for_blank": False}),
        ("isotope/blank_after_baseline", src + ":test_blank_after_baseline",
         {"intercept": i100, "baseline": (5.0, 0.0), "blank": (3.0, 0.0)}),
        ("isotope/disc_correction", src + ":test_disc_correction",
         {"intercept": i100, "discrimination": (1.02, 0.0)}),
        ("isotope/ic_correction", src + ":test_ic_correction",
         {"intercept": i100, "ic_factor": (0.98, 0.0)}),
        ("isotope/intensity_disc_and_ic", src + ":test_get_intensity_combines_disc_and_ic",
         {"intercept": i100, "discrimination": (1.02, 0.0), "ic_factor": (0.98, 0.0)}),
        ("isotope/ic_factor_zero", src + ":test_ic_factor_zero_ufloat_not_replaced (spec Q18)",
         {"intercept": i50, "ic_factor": (0.0, 0.0)}),
        ("isotope/exclude_baseline_error", "isotope.py:715-736 (spec Q3)",
         {"intercept": (100.0, 0.05), "baseline": (5.0, 0.2), "include_baseline_error": False}),
        ("isotope/include_baseline_error", "isotope.py:715-736 (spec Q3)",
         {"intercept": (100.0, 0.05), "baseline": (5.0, 0.2), "include_baseline_error": True}),
        ("isotope/full_chain_with_errors", "isotope.py:715-854",
         {"intercept": (100.0, 0.05), "baseline": (5.0, 0.2), "blank": (3.0, 0.1),
          "ic_factor": (1.01, 0.002), "discrimination": (1.003, 0.001),
          "include_baseline_error": True}),
        ("isotope/correct_for_blank_false", "isotope.py:848-854; pychron_constants.py:264",
         {"intercept": (100.0, 0.05), "baseline": (5.0, 0.2), "blank": (3.0, 0.1),
          "ic_factor": (1.01, 0.002), "correct_for_blank": False}),
        ("isotope/negative_after_blank", "spec 7 (negative signal replicated)",
         {"isotope": "Ar36", "intercept": (0.01, 0.002), "baseline": (0.001, 0.0005),
          "blank": (0.02, 0.003), "ic_factor": (1.05, 0.01)}),
        ("isotope/huge_errors", "spec 7 (huge input errors)",
         {"isotope": "Ar39", "intercept": (10.0, 1e4), "baseline": (1.0, 1e3),
          "blank": (0.5, 1e5), "ic_factor": (1.0, 10.0), "include_baseline_error": True}),
    )
    for name, source, spec in specs:
        inputs, expected = run_isotope(spec)
        inputs["constants"] = c
        cases.append(case(name, source, inputs, expected))

    asrc = "argon_calculations.py:363-372; argon_calculations_test.py:AbundanceSensitivityTest"
    for name, alpha, sigs in (
        ("abundance_sensitivity/zero_alpha", 0.0,
         ((1000.0, 0.0), (100.0, 0.0), (10.0, 0.0), (5.0, 0.0), (2.0, 0.0))),
        ("abundance_sensitivity/known_alpha", 0.0001,
         ((1000.0, 0.0), (100.0, 0.0), (10.0, 0.0), (5.0, 0.0), (2.0, 0.0))),
        ("abundance_sensitivity/known_alpha_with_errors", 0.0001,
         ((1000.0, 1.0), (100.0, 0.5), (10.0, 0.05), (5.0, 0.01), (2.0, 0.01))),
        ("abundance_sensitivity/large_alpha", 0.01,
         ((1000.0, 1.0), (100.0, 0.5), (10.0, 0.05), (5.0, 0.01), (2.0, 0.01))),
    ):
        vals = [L.ufloat(v, e, tag=k) for k, (v, e) in zip(ARGON_KEYS, sigs)]
        out = L.ac.abundance_sensitivity_correction(vals, alpha)
        inputs = {
            "function": "abundance_sensitivity",
            "signals": {k: mv(*s) for k, s in zip(ARGON_KEYS, sigs)},
            "abundance_sensitivity": num(alpha),
            "constants": with_overrides(c, {"abundance_sensitivity": alpha}),
        }
        cases.append(case(name, asrc, inputs, {k: uv(v) for k, v in zip(ARGON_KEYS, out)}))

    dsrc = "deadtime.py:54-55 formula (module imports Qt); spec E5, D4"
    for name, sig, tau in (
        ("deadtime/typical", (1000.0, 1.0), 20e-9),
        ("deadtime/small_signal", (10.0, 0.01), 20e-9),
        ("deadtime/zero_tau", (1000.0, 1.0), 0.0),
        ("deadtime/large_tau", (5000.0, 5.0), 1e-8),
    ):
        s = L.ufloat(*sig, tag="Ar40")
        inputs = {"function": "deadtime", "signal": mv(*sig), "tau_s": num(tau),
                  "fa_to_cps": FA_TO_CPS, "constants": c}
        cases.append(case(name, dsrc, inputs, {"corrected": uv(deadtime(s, tau, FA_TO_CPS))}))
    sig, tau = (1000.0, 1.0), 20e-9
    s = L.ufloat(*sig, tag="Ar40")
    inputs = {"function": "deadtime", "signal": mv(*sig), "tau_s": num(tau),
              "fa_to_cps": FA_TO_CPS, "legacy_fa_to_cps": LEGACY_FA_TO_CPS, "constants": c}
    cases.append(
        case(
            "deadtime/legacy_6240",
            dsrc + "; legacy factor 6240 deadtime.py:62",
            inputs,
            {"corrected": uv(deadtime(s, tau, FA_TO_CPS))},
            sentinel={"corrected": uv(deadtime(s, tau, LEGACY_FA_TO_CPS))},
        )
    )
    # 1 - n tau <= 0: n = 1000 * 6241.509 = 6.24e6 cps, tau 1e-3 s.
    inputs = {"function": "deadtime", "signal": mv(1000.0, 1.0), "tau_s": num(1e-3),
              "fa_to_cps": FA_TO_CPS, "constants": c}
    cases.append(case("deadtime/saturated", dsrc, inputs, {}, error="deadtime"))
    return cases


# ----------------------------------------------------------------------------
# decay_factors.json
# ----------------------------------------------------------------------------


def seg_json(segs):
    return [{"power": num(p), "duration_days": num(t), "dt_days": num(dt)} for p, t, dt in segs]


def gen_decay_factors():
    cases = []
    src = "argon_calculations_test.py:DecayFactorsTest"
    lam = (0.01975, 7.068e-6)
    specs = (
        ("decay/known_single_segment", src + ":test_known_single_segment", 7.2e-10, 7.0e-10,
         [(1.0, 86400.0, 86400.0 * 365)], None),
        ("decay/near_zero_lambda", src + ":test_zero_decay_constants_approach_unity", 1e-12,
         1e-12, [(1.0, 86400.0, 86400.0 * 365)], None),
        ("decay/no_segments", src + ":test_no_segments_returns_unity", 1e-10, 1e-10, [], None),
        ("decay/unit_guard_seconds", src + ":test_unit_mismatch_raises", 0.02, 7e-6,
         [(1.0, 86400.0, 86400.0 * 365.0)], "same unit"),
        ("decay/days_one_year", src + ":test_days_units_pass_guard", 0.02, 7e-6,
         [(1.0, 1.0, 365.0)], None),
        ("decay/guard_exactly_50", "argon_calculations.py:260-279 (boundary passes)", 0.5,
         7e-6, [(1.0, 100.0, 10.0)], None),
        ("decay/guard_just_above_50", "argon_calculations.py:260-279", 0.5, 7e-6,
         [(1.0, 100.000001, 10.0)], "same unit"),
        ("decay/guard_on_dt", "argon_calculations.py:260-279 (max(|t|, |dt|))", 0.02, 7e-6,
         [(1.0, 1.0, 2600.0)], "same unit"),
        ("decay/arar_age_segment", "arar_age_test.py:167-175", lam[0], lam[1],
         [(1.0, 1.0, 365.0)], None),
        ("decay/multi_segment", "argon_calculations.py:349-360", lam[0], lam[1],
         [(1.0, 0.5, 400.0), (0.8, 0.75, 380.0), (1.2, 0.25, 360.5)], None),
        ("decay/zero_power", "argon_calculations.py:358-359 (b == 0 -> 1)", lam[0], lam[1],
         [(0.0, 1.0, 10.0)], None),
        ("decay/negative_dt", "argon_calculations.py:349-360 (dt < 0)", lam[0], lam[1],
         [(1.0, 2.0, -1.0)], None),
    )
    for name, source, l37, l39, segs, err in specs:
        inputs = {"function": "decay_factors", "lambda37": num(l37), "lambda39": num(l39),
                  "segments": seg_json(segs)}
        legacy_segs = [(p, t, dt, None, None) for p, t, dt in segs] if segs else None
        if err:
            try:
                L.ac.calculate_arar_decay_factors(l37, l39, legacy_segs)
            except ValueError as e:
                if err not in str(e):
                    raise
            else:
                raise RuntimeError("expected legacy ValueError for " + name)
            cases.append(case(name, source, inputs, {}, tol=DECAY_TOL, error=err))
            continue
        d37, d39 = L.ac.calculate_arar_decay_factors(l37, l39, legacy_segs)
        cases.append(case(name, source, inputs, {"df37": num(d37), "df39": num(d39)},
                          tol=DECAY_TOL))

    def utc(*a):
        return int(datetime(*a, tzinfo=timezone.utc).timestamp())

    dsrc = "dvc/dvc_analysis.py:370-395 set_chronology (UTC, spec Q11/Q12)"
    doses_sets = (
        ("chronology/two_doses_with_gap",
         [(1.0, utc(2024, 3, 1, 8), utc(2024, 3, 1, 20)),
          (0.9, utc(2024, 3, 3, 6), utc(2024, 3, 4, 2, 30))],
         utc(2024, 9, 15, 13, 45, 10)),
        ("chronology/single_dose",
         [(1.0, utc(2023, 1, 10, 0), utc(2023, 1, 12, 6))],
         utc(2023, 2, 1, 12)),
        ("chronology/analysis_before_end",
         [(1.0, utc(2023, 1, 10, 0), utc(2023, 1, 12, 6))],
         utc(2023, 1, 11, 12)),
    )
    for base, doses, analysis in doses_sets:
        for endtime in (False, True):
            ats = datetime.fromtimestamp(analysis, timezone.utc)
            segs = []
            for p, st, en in doses:
                std = datetime.fromtimestamp(st, timezone.utc)
                end = datetime.fromtimestamp(en, timezone.utc)
                t = end if endtime else std
                segs.append(
                    (p, (end - std).total_seconds() / (60.0 * 60 * 24),
                     (ats - t).total_seconds() / (60.0 * 60 * 24))
                )
            first = datetime.fromtimestamp(doses[0][1], timezone.utc)
            decay_days = (ats - first).total_seconds() / (60 * 60 * 24)
            d37, d39 = L.ac.calculate_arar_decay_factors(
                lam[0], lam[1], [(p, t, dt, None, None) for p, t, dt in segs])
            inputs = {
                "function": "irradiation_from_doses",
                "doses": [{"power": num(p), "start_utc_s": st, "end_utc_s": en}
                          for p, st, en in doses],
                "analysis_utc_s": analysis,
                "use_irradiation_endtime": endtime,
                "lambda37": num(lam[0]),
                "lambda39": num(lam[1]),
            }
            expected = {"segments": seg_json(segs), "decay_days": num(decay_days),
                        "df37": num(d37), "df39": num(d39)}
            name = "{}/{}".format(base, "endtime" if endtime else "starttime")
            cases.append(case(name, dsrc, inputs, expected, tol=DECAY_TOL))
    return cases


# ----------------------------------------------------------------------------
# preset variants
# ----------------------------------------------------------------------------


def for_presets(build):
    """Run build(preset_name, constants_table) for both legacy sets."""
    pr = presets()
    out = []
    for key in ("legacy", "legacy_preferences"):
        for c in build(key, pr[key]):
            c["name"] = "{}@{}".format(c["name"], key)
            out.append(c)
    return out


# ----------------------------------------------------------------------------
# interference.json
# ----------------------------------------------------------------------------

TEST_PR = {"Ca3937": (0.0007, 0.0), "K3739": (0.01, 0.0), "K3839": (0.013, 0.0),
           "Ca3637": (0.00026, 0.0), "Ca3837": (0.00019, 0.0)}
ERR_PR = {"Ca3937": (0.0007, 2e-5), "K3739": (0.01, 0.0005), "K3839": (0.013, 0.0002),
          "Ca3637": (0.00026, 4e-6), "Ca3837": (0.00019, 3e-6)}

INTERFERENCE_SPECS = (
    ("interference/pure_k_no_ca", "argon_calculations_test.py:InterferenceCorrectionsTest:"
     "test_pure_k_no_ca", (100.0, 1.0), (0.0, 0.0),
     {"Ca3937": (0.0, 0.0), "K3739": (0.0, 0.0), "K3839": (0.013, 0.0), "Ca3637": (0.0, 0.0),
      "Ca3837": (0.0, 0.0)}, {}, None),
    ("interference/k38_proportional", "argon_calculations_test.py:InterferenceCorrectionsTest:"
     "test_k38_proportional_to_k39 (a37 5.0 instead of 1.0, see F_ISOTOPES)", (100.0, 1.0),
     (5.0, 0.05), TEST_PR, {}, None),
    ("interference/ratio_errors", "argon_calculations.py:399-426", (100.0, 1.0), (5.0, 0.05),
     ERR_PR, {}, None),
    ("interference/negative_a37", "argon_calculations.py:421-422 (spec Q8)", (100.0, 1.0),
     (-0.5, 0.01), ERR_PR, {}, None),
    ("interference/zero_a37_with_k3739", "argon_calculations.py:410-422", (100.0, 1.0),
     (0.0, 0.01), ERR_PR, {}, None),
    ("interference/fixed_by_analysis", "argon_calculations_test.py:"
     "InterferenceCorrectionsFixedModeTest:test_explicit_fixed_k3739_dispatches", (100.0, 1.0),
     (1.0, 0.01), {"Ca3937": (0.0007, 0.0), "K3839": (0.013, 0.0), "Ca3637": (0.00026, 0.0),
                   "Ca3837": (0.00019, 0.0)}, {}, (0.05, 0.001)),
    ("interference/fixed_by_constants", "argon_calculations_test.py:"
     "InterferenceCorrectionsFixedModeTest:test_fixed_k3739_from_arar_constants", (100.0, 1.0),
     (1.0, 0.01), {"Ca3937": (0.0007, 0.0)}, {"k3739_mode": "Fixed"}, None),
    ("interference/fixed_by_constants_ratio_errors", "argon_calculations.py:375-396, :415-418",
     (100.0, 1.0), (1.0, 0.01), ERR_PR, {"k3739_mode": "Fixed"}, None),
    ("interference/fixed_zero_ca3937", "argon_calculations_test.py:FixedK3739Test:"
     "test_zero_ca_passthrough (spec Q10)", (100.0, 1.0), (1.0, 0.01),
     {"Ca3937": (0.0, 0.0), "K3839": (0.013, 0.0)}, {}, (0.05, 0.0)),
    ("interference/fixed_missing_ca3937", "argon_calculations.py:386-390 (spec Q10)",
     (100.0, 1.0), (1.0, 0.01), {"K3839": (0.013, 0.0)}, {"k3739_mode": "Fixed"}, None),
    ("interference/fixed_k37_proportional", "argon_calculations_test.py:FixedK3739Test:"
     "test_k37_proportional_to_k39", (100.0, 0.0), (0.0, 0.0), {"Ca3937": (0.001, 0.0)}, {},
     (0.05, 0.0)),
    ("interference/missing_keys", "argon_calculations.py:407-424 (missing = 0)", (100.0, 1.0),
     (1.0, 0.01), {"K3839": (0.013, 0.0001)}, {}, None),
    ("interference/huge_errors", "spec 7 (huge input errors)", (100.0, 1e5), (2.0, 1e4),
     ERR_PR, {}, None),
)


def run_interference(a39, a37, rows, c, fixed):
    ac = make_constants(c)
    A39 = L.ufloat(*a39, tag="Ar39")
    A37 = L.ufloat(*a37, tag="Ar37")
    with Capture() as cap:
        out = L.ac.interference_corrections(A39, A37, interferences(rows), ac, fixed_arg(fixed))
    names = ("k37", "k38", "k39", "ca36", "ca37", "ca38", "ca39")
    check_ca37_conditioning(a37[0], out[4], cap.clamp, c)
    return {n: uv(v) for n, v in zip(names, out)}, interference_diagnostics(
        c, rows, fixed, cap.clamp)


def gen_interference():
    def build(_key, base):
        cases = []
        for name, source, a39, a37, rows, over, fixed in INTERFERENCE_SPECS:
            c = with_overrides(base, over)
            expected, diags = run_interference(a39, a37, rows, c, fixed)
            inputs = {"a39": mv(*a39), "a37": mv(*a37), "production": rows_json(rows),
                      "constants": c, "fixed_k3739": fixed_json(fixed)}
            cases.append(case(name, source, inputs, expected, diagnostics=diags))
        return cases

    return for_presets(build)


# ----------------------------------------------------------------------------
# atmospheric.json
# ----------------------------------------------------------------------------


def find_singular_cl3638(c, decay_days):
    """Cl3638 with 1 - ((Cl3638 * lCl) * dd) * r3836 == 0 exactly (legacy op order)."""
    lcl = fval(c["lambda_cl36"]["v"])
    r = fval(c["atm4036"]["v"]) / fval(c["atm4038"]["v"])
    cl = 1.0 / (lcl * decay_days * r)
    for _ in range(64):
        if 1 - ((cl * lcl) * decay_days) * r == 0:
            return cl
        cl = math.nextafter(cl, math.inf if ((cl * lcl) * decay_days) * r < 1 else -math.inf)
    raise RuntimeError("no exactly singular Cl3638 found")


def gen_atmospheric():
    c = presets()["legacy"]
    cases = []
    asrc = "argon_calculations_test.py:"
    specs = (
        ("atmospheric/no_chlorine", asrc + "AtmosphericTest:test_no_chlorine_atm36_is_nonradiogenic",
         (0.5, 0.005), (0.1, 0.001), (0.013, 0.0001), (0.00019, 0.0), (0.00026, 0.0), 365.0,
         {"Cl3638": (0.0, 0.0)}),
        ("atmospheric/atm38_ratio", asrc + "AtmosphericTest:test_atm38_proportional_to_atm36",
         (0.5, 0.0), (0.1, 0.0), (0.013, 0.0), (0.0, 0.0), (0.0, 0.0), 365.0,
         {"Cl3638": (0.0, 0.0)}),
        ("atmospheric/with_chlorine", asrc + "CalculateAtmosphericChlorineTest:"
         "test_with_chlorine_yields_nonzero_cl", (0.5, 0.005), (0.1, 0.001), (0.013, 0.0001),
         (0.0, 0.0), (0.0, 0.0), 365.0, {"Cl3638": (250.0, 5.0)}),
        ("atmospheric/chlorine_decay_days_0", "argon_calculations.py:468-487", (0.5, 0.005),
         (0.1, 0.001), (0.013, 0.0001), (0.0001, 1e-6), (0.0002, 2e-6), 0.0,
         {"Cl3638": (250.0, 5.0)}),
        ("atmospheric/chlorine_decay_days_3650", "argon_calculations.py:468-487", (0.5, 0.005),
         (0.1, 0.001), (0.013, 0.0001), (0.0001, 1e-6), (0.0002, 2e-6), 3650.0,
         {"Cl3638": (250.0, 5.0)}),
        ("atmospheric/missing_cl3638", "argon_calculations.py:481 (missing = 0)", (0.5, 0.005),
         (0.1, 0.001), (0.013, 0.0001), (0.0, 0.0), (0.0, 0.0), 365.0, {}),
    )
    for name, source, a38, a36, k38, ca38, ca36, dd, rows in specs:
        ac = make_constants(c)
        vals = [L.ufloat(*x) for x in (a38, a36, k38, ca38, ca36)]
        out = L.ac.calculate_atmospheric(*vals, dd, interferences(rows), ac)
        inputs = {"function": "atmospheric_components", "a38": mv(*a38), "a36": mv(*a36),
                  "k38": mv(*k38), "ca38": mv(*ca38), "ca36": mv(*ca36), "decay_days": num(dd),
                  "production": rows_json(rows), "constants": c}
        expected = {k: uv(v) for k, v in zip(("atm36", "atm38", "cl36", "cl38"), out)}
        cases.append(case(name, source, inputs, expected))

    dd = 365.0
    cl = find_singular_cl3638(c, dd)
    rows = {"Cl3638": (cl, 0.0)}
    try:
        L.ac.calculate_atmospheric(L.ufloat(0.5, 0.005), L.ufloat(0.1, 0.001), L.ufloat(0.0, 0),
                                   L.ufloat(0.0, 0), L.ufloat(0.0, 0), dd, interferences(rows),
                                   make_constants(c))
    except ZeroDivisionError:
        pass
    else:
        raise RuntimeError("expected legacy ZeroDivisionError")
    inputs = {"function": "atmospheric_components", "a38": mv(0.5, 0.005), "a36": mv(0.1, 0.001),
              "k38": mv(0.0), "ca38": mv(0.0), "ca36": mv(0.0), "decay_days": num(dd),
              "production": rows_json(rows), "constants": c}
    cases.append(case("atmospheric/singular_denominator",
                      "argon_calculations.py:482 (spec Q16); Cl3638 chosen so that "
                      "1 - ((Cl3638 * lambda_Cl36) * decay_days) * (atm4036 / atm4038) == 0",
                      inputs, {}, error="zero divisor"))

    csrc = asrc + "CosmogenicComponentsTest"
    cosmo = ((0.1869, 0.001), (0.65, 0.01))
    cc = with_overrides(c, {"cosmogenic": cosmo})
    exact_why = ("pure end-member: fs is exactly 1 or 0, so one pair of components is an exact "
                 "cancellation (expected 0)")
    cspecs = (
        ("cosmogenic/pure_solar", csrc + ":test_pure_solar_returns_all_solar", (1.0, 0.0),
         (0.1869, 0.0), cc, exact_why, None),
        ("cosmogenic/pure_cosmo", csrc + ":test_pure_cosmogenic_returns_all_cosmo", (1.0, 0.0),
         (0.65, 0.0), cc, exact_why, None),
        ("cosmogenic/mixed", csrc + ":test_components_sum_to_total", (1.0, 0.0), (0.3, 0.0), cc,
         None, None),
        ("cosmogenic/mixed_with_errors", "argon_calculations.py:490-513", (2.0, 0.02),
         (0.5, 0.004), cc, None, None),
        ("cosmogenic/zero_c36", "argon_calculations.py:501 (spec Q16)", (0.0, 0.01), (0.3, 0.0),
         cc, None, "zero divisor"),
        ("cosmogenic/rc_equals_rs", "argon_calculations.py:505 (spec Q16)", (1.0, 0.0),
         (0.3, 0.0), with_overrides(c, {"cosmogenic": ((0.4, 0.001), (0.4, 0.01))}), None,
         "zero divisor"),
    )
    for name, source, c36, c38, consts, why, err in cspecs:
        ac = make_constants(consts)
        inputs = {"function": "cosmogenic_components", "c36": mv(*c36), "c38": mv(*c38),
                  "constants": consts}
        if err:
            try:
                L.ac.calculate_cosmogenic_components(L.ufloat(*c36), L.ufloat(*c38), ac)
            except ZeroDivisionError:
                pass
            else:
                raise RuntimeError("expected legacy ZeroDivisionError for " + name)
            cases.append(case(name, source, inputs, {}, error=err))
            continue
        out = L.ac.calculate_cosmogenic_components(L.ufloat(*c36), L.ufloat(*c38), ac)
        expected = {k: uv(v) for k, v in
                    zip(("cosmo36", "cosmo38", "noncosmo36", "noncosmo38"), out)}
        tol = cancellation_tol(inputs, why) if why else None
        cases.append(case(name, source, inputs, expected, tol=tol))
    return cases


# ----------------------------------------------------------------------------
# calculate_f.json
# ----------------------------------------------------------------------------

# argon_calculations_test._isotopes() except Ar37: with Ar37 = 1.0, Ar39 = 100 and
# K3739 = 0.01, Ca3937 = 0.0007 legacy ca37 = 1 - 0.01 * k39 is a total
# cancellation (|ca37| ~ 1e-16, sign decided by rounding, which also decides the
# E11 clamp), so Ar37 is raised to 5.0 to keep the vectors well conditioned.
F_ISOTOPES = {"Ar40": (1000.0, 1.0), "Ar39": (100.0, 0.5), "Ar38": (10.0, 0.05),
              "Ar37": (5.0, 0.025), "Ar36": (2.0, 0.01)}
F_PR = dict(TEST_PR, K4039=(0.0002, 0.0), Cl3638=(0.0, 0.0))
F_ERR_PR = dict(ERR_PR, K4039=(0.0002, 1e-5), Cl3638=(250.0, 5.0))


def run_calculate_f(isos, dd, rows, c, fixed):
    ac = make_constants(c)
    vals = [L.ufloat(*isos[k], tag=k) for k in ARGON_KEYS]
    with Capture() as cap:
        f, f_wo, nar, comp, ifc = L.ac.calculate_f(vals, dd, interferences(rows), ac,
                                                    fixed_arg(fixed))
    return f_result(f, sd(f_wo), nar, comp, ifc, cap, c, rows, fixed)


def f_case(name, source, isos, dd, rows, c, fixed=None, why=None):
    expected, sentinel, diags = run_calculate_f(isos, dd, rows, c, fixed)
    inputs = {"function": "calculate_f",
              "isotopes": {k: mv(*isos[k]) for k in ARGON_KEYS}, "decay_days": num(dd),
              "production": rows_json(rows), "constants": c, "fixed_k3739": fixed_json(fixed)}
    tol = cancellation_tol(inputs, why) if why else None
    return case(name, source, inputs, expected, tol=tol, sentinel=sentinel, diagnostics=diags)


def gen_calculate_f():
    src = "argon_calculations_test.py:"
    cosmo = ((0.18, 0.001), (0.65, 0.01))

    def build(_key, base):
        def iso(**kw):
            d = dict(F_ISOTOPES)
            d.update(kw)
            return d

        return [
            f_case("calculate_f/young_volcanic", src + "CalculateFTest", F_ISOTOPES, 365.0, F_PR,
                   base),
            f_case("calculate_f/ratio_errors", "argon_calculations.py:516-591 (spec E15)",
                   F_ISOTOPES, 365.0, F_ERR_PR, base),
            f_case("calculate_f/cosmogenic", src + "CalculateFEdgeCasesTest:"
                   "test_cosmogenic_correction_enabled", F_ISOTOPES, 365.0,
                   {"Cl3638": (0.0, 0.0), "K4039": (2e-4, 0.0)},
                   with_overrides(base, {"cosmogenic": cosmo})),
            f_case("calculate_f/no_interferences", src + "CalculateFEdgeCasesTest:test_default_args",
                   F_ISOTOPES, 365.0, {}, base),
            f_case("calculate_f/fixed_by_analysis", "argon_calculations.py:516-591",
                   iso(Ar37=(5.0, 0.02)), 365.0, F_ERR_PR, base, fixed=(0.05, 0.001)),
            f_case("calculate_f/fixed_by_constants", "argon_calculations.py:516-591",
                   iso(Ar37=(5.0, 0.02)), 365.0, F_ERR_PR,
                   with_overrides(base, {"k3739_mode": "Fixed"})),
            f_case("calculate_f/negative_ar36", "spec 7 (negative signal replicated)",
                   iso(Ar36=(-0.05, 0.01)), 365.0, F_PR, base),
            f_case("calculate_f/negative_ar37", "spec Q8 (clamp after ca39)",
                   iso(Ar37=(-0.4, 0.01)), 365.0, F_ERR_PR, base),
            f_case("calculate_f/huge_errors", "spec 7 (huge input errors)",
                   {"Ar40": (1000.0, 1e6), "Ar39": (100.0, 1e5), "Ar38": (10.0, 1e4),
                    "Ar37": (5.0, 1e3), "Ar36": (2.0, 1e3)}, 365.0, F_ERR_PR, base),
            f_case("calculate_f/k39_zero", "argon_calculations.py:550-553 (spec Q6, D3)",
                   iso(Ar39=(0.0, 0.5), Ar37=(0.0, 0.005)), 365.0, F_PR, base),
            f_case("calculate_f/a40_zero", "argon_calculations.py:554-557 (spec Q6, D3)",
                   iso(Ar40=(0.0, 1.0)), 365.0, F_PR, base),
            f_case("calculate_f/air_shot", "spec 4.7 (rad40 of an air shot)",
                   {"Ar40": (590.92, 0.5), "Ar39": (0.01, 0.002), "Ar38": (0.3754, 0.001),
                    "Ar37": (0.001, 0.0005), "Ar36": (2.0, 0.004)}, 365.0, F_PR, base,
                   why="air shot: rad40 = a40 - atm40 is a near cancellation"),
        ]

    return for_presets(build)


# ----------------------------------------------------------------------------
# age.json
# ----------------------------------------------------------------------------


def run_age(j, f, c, lk):
    ac = make_constants(c)
    J = L.ufloat(*j, tag="J")
    F = L.ufloat(*f, tag="F")
    LK = L.ufloat(*lk, tag="lambda_k") if lk is not None else None
    return L.ac.age_equation(J, F, include_decay_error=c["include_decay_error"], lambda_k=LK,
                             arar_constants=ac)


def gen_age():
    src = "argon_calculations_test.py:"

    def build(_key, base):
        specs = [
            ("age/known_jf", src + "AgeEquationTest:test_age_for_known_jf", (1e-3, 1e-6),
             (10.0, 0.01), {}, None),
            ("age/zero_jf", src + "AgeEquationTest:test_zero_jf_returns_zero", (0.0, 0.0),
             (1.0, 0.0), {}, None),
            ("age/include_decay_error", src + "AgeEquationEdgeCasesTest:"
             "test_include_decay_error_increases_uncertainty", (1e-3, 1e-6), (10.0, 0.01),
             {"include_decay_error": True}, None),
            ("age/explicit_lambda_k", src + "AgeEquationEdgeCasesTest:test_explicit_lambda_k_used",
             (1e-3, 0.0), (10.0, 0.0), {}, (1e-15, 0.0)),
            ("age/lambda_k_override_with_error", "dvc/dvc.py:2303-2305; argon_calculations.py:614",
             (1e-3, 1e-6), (10.0, 0.01), {"include_decay_error": True}, (5.5305e-10, 1.1e-12)),
            ("age/lambda_k_zero_ignored", "argon_calculations.py:614 (`if not lambda_k`)",
             (1e-3, 1e-6), (10.0, 0.01), {}, (0.0, 0.0)),
            ("age/negative_argument", src + "AgeEquationEdgeCasesTest:"
             "test_negative_argument_returns_zero (spec Q6)", (1.0, 0.0), (-2.0, 0.0), {}, None),
            ("age/argument_exactly_zero", "argon_calculations.py:626-630 (log(0))", (1.0, 0.0),
             (-1.0, 0.0), {}, None),
            ("age/huge_errors", "spec 7 (huge input errors)", (1e-3, 1.0), (10.0, 1e3), {}, None),
            ("age/old_sample", "argon_calculations.py:603-630", (0.0125, 2e-5), (250.0, 0.4), {},
             None),
        ]
        for unit in ("a", "ka", "Ma", "Ga"):
            specs.append(("age/units_" + unit, "arar_constants.py:143-170 (scale_age)",
                          (1e-3, 1e-6), (10.0, 0.01), {"age_units": unit}, None))
        cases = []
        for name, source, j, f, over, lk in specs:
            c = with_overrides(base, over)
            age = run_age(j, f, c, lk)
            inputs = {"j": mv(*j), "f": mv(*f), "constants": c,
                      "lambda_k_total": mv(*lk) if lk is not None else None}
            if 1 + j[0] * f[0] <= 0:
                cases.append(case(name, source, inputs, {}, sentinel={"age": uv(age)},
                                  error="1 + J F"))
            else:
                cases.append(case(name, source, inputs, {"age": uv(age)}))
        return cases

    return for_presets(build)


# ----------------------------------------------------------------------------
# pipeline
# ----------------------------------------------------------------------------


def seeded_intercepts(intensities=None, seed=0):
    """Legacy fits of arar_age_test._build_isotope, as {iso: (value, error)}."""
    intensities = intensities or {"Ar40": 1000.0, "Ar39": 100.0, "Ar38": 10.0, "Ar37": 50.0,
                                  "Ar36": 2.0}
    out = {}
    for i, (name, intercept) in enumerate(intensities.items()):
        iso = L.arar_age_test._build_isotope(name, DETECTORS[name], intercept, seed=seed + i)
        u = iso.uvalue
        out[name] = (nom(u), sd(u))
    return out


def base_rows():
    rows = {k: (nom(v), sd(v)) for k, v in L.arar_age_test._interferences().items()}
    return rows


def pipeline_spec(**kw):
    """Python-level pipeline input; defaults follow arar_age_test._build_age."""
    spec = {
        "isotopes": {k: {"intercept": v} for k, v in seeded_intercepts().items()},
        "rows": base_rows(),
        "segments": [],
        "decay_days": 1825.0,  # timestamp = 86400 * 365 * 5, irradiation_time = 0
        "j": (1e-3, 1e-6),
        "position_jerr": 5e-7,
        "lambda_k_total": None,
        "fixed": None,
        "overrides": {},
    }
    spec.update(kw)
    return spec


def iso_defaults(s):
    return {
        "intercept": tuple(s["intercept"]),
        "baseline": tuple(s.get("baseline", (0.0, 0.0))),
        "blank": tuple(s.get("blank", (0.0, 0.0))),
        "ic_factor": tuple(s.get("ic_factor", (1.0, 0.0))),
        "include_baseline_error": s.get("include_baseline_error", False),
        "correct_for_blank": s.get("correct_for_blank", True),
    }


def pipeline_inputs_json(spec, c):
    inputs = {
        "function": "reduce",
        "isotopes": {},
        "production": rows_json(spec["rows"]),
        "irradiation": {"segments": seg_json(spec["segments"]),
                        "decay_days": num(spec["decay_days"])},
        "j": mv(*spec["j"]) if spec["j"] is not None else None,
        "position_jerr": num(spec["position_jerr"]),
        "lambda_k_total": mv(*spec["lambda_k_total"]) if spec["lambda_k_total"] else None,
        "fixed_k3739": fixed_json(spec["fixed"]),
    }
    if c is not None:
        inputs["constants"] = c
    for k in ARGON_KEYS:
        if k in spec["isotopes"]:
            d = iso_defaults(spec["isotopes"][k])
            inputs["isotopes"][k] = {
                "intercept": mv(*d["intercept"]),
                "baseline": mv(*d["baseline"]),
                "blank": mv(*d["blank"]),
                "ic_factor": mv(*d["ic_factor"]),
                "include_baseline_error": d["include_baseline_error"],
                "correct_for_blank": d["correct_for_blank"],
            }
    return inputs


class _SharedBlank:
    """Stand-in for Isotope.blank whose uvalue is one shared ufloat."""

    def __init__(self, u):
        self.uvalue = u
        self.value = nom(u)
        self.error = sd(u)


def build_age(spec, c, shared_j=None, shared_blank=None):
    age = L.ArArAge()
    apply_constants(age.arar_constants, c)
    for k in ARGON_KEYS:
        if k not in spec["isotopes"]:
            continue
        d = iso_defaults(spec["isotopes"][k])
        iso = L.Isotope(k, DETECTORS[k])
        iso.set_uvalue(d["intercept"])
        iso.set_baseline(*d["baseline"])
        iso.set_blank(*d["blank"])
        if shared_blank is not None and shared_blank[0] == k:
            iso.blank = _SharedBlank(shared_blank[1])
        iso.include_baseline_error = d["include_baseline_error"]
        iso.correct_for_blank = d["correct_for_blank"]
        iso.ic_factor = L.ufloat(*d["ic_factor"], tag="{} IC".format(k))  # dvc_analysis.py:759
        age.isotopes[k] = iso
    age.irradiation_time = 0
    age.timestamp = spec["decay_days"] * 86400.0
    if age.decay_days != spec["decay_days"]:
        raise RuntimeError("decay_days does not round-trip")
    if spec["segments"]:
        age.chron_segments = [(p, t, dt, None, None) for p, t, dt in spec["segments"]]
    if shared_j is not None:
        age.j = shared_j
        age.position_jerr = spec["position_jerr"]
    elif spec["j"] is not None:
        age.set_j(*spec["j"])
        age.position_jerr = spec["position_jerr"]
    age.interference_corrections = interferences(spec["rows"])
    age.production_ratios = ratios(spec["rows"])
    if spec["lambda_k_total"]:
        age.arar_constants.lambda_k = L.ufloat(*spec["lambda_k_total"], tag="lambda_k")
    if spec["fixed"] is not None:
        age.fixed_k3739 = L.ufloat(*spec["fixed"])
    return age


def run_age_pipeline(age, c):
    with Capture() as cap:
        age.calculate_age(force=True, include_decay_error=c["include_decay_error"])
    return cap


def pipeline_result(age, cap, spec, c):
    """(expected, sentinel, diagnostics) for one ArArAge after calculate_age."""
    isotopes = age.isotopes
    nar, comp = age.non_ar_isotopes, age.computed
    ifc = {k: isotopes[k].interference_corrected_value for k in ARGON_KEYS}
    fexp, sentinel, diags = f_result(age.uF, age.F_err_wo_irrad, nar, comp, ifc, cap, c,
                                     spec["rows"], spec["fixed"])
    expected = {
        "decay": {"df37": num(age.ar37decayfactor), "df39": num(age.ar39decayfactor)},
        "corrected": {k: uv(age.corrected_intensities[k]) for k in ARGON_KEYS},
        "f": fexp,
    }
    f_defined = "f" in fexp
    if age.j is not None:
        ages = {
            "age": uv(age.uage),
            "age_w_j_err": uv(age.uage_w_j_err),
            "age_w_position_err": uv(age.uage_w_position_err),
        }
        components = {k: num(isotopes[k].age_error_component) for k in ARGON_KEYS}
        if not f_defined:
            sentinel["ages"] = ages
            sentinel["age_error_components"] = components
        elif 1 + nom(age.j) * nom(age.uF) <= 0:
            diags.append("AgeUndefined")
            sentinel["ages"] = ages
            sentinel["age_error_components"] = components
        else:
            wo = excluded_std(age.uage, INTERFERENCE_KEYS)
            ages["age_err_wo_irrad"] = num(wo)
            ages["age_err_wo_j_irrad"] = num(wo)
            expected["ages"] = ages
            expected["age_error_components"] = components
    ca37 = nar["ca37"]
    if nom(ca37) == 0:
        diags.append("KCaUndefined")
        sentinel["kca"] = uv(age.kca)
        sentinel["cak"] = uv(age.cak)
    else:
        if nom(age.kca) == 0:
            raise RuntimeError("kca == 0 with ca37 != 0 is not defined by the spec")
        expected["kca"] = uv(age.kca)
        expected["cak"] = uv(age.cak)
    cl38 = nar["cl38"]
    if nom(cl38) == 0:
        diags.append("KClUndefined")
        sentinel["kcl"] = uv(age.kcl)
        sentinel["clk"] = uv(age.clk)
    else:
        if nom(age.kcl) == 0:
            raise RuntimeError("kcl == 0 with cl38 != 0 is not defined by the spec")
        expected["kcl"] = uv(age.kcl)
        expected["clk"] = uv(age.clk)
    return expected, sentinel, diags


def pipeline_case(name, source, spec, base, why=None):
    c = with_overrides(base, spec["overrides"])
    inputs = pipeline_inputs_json(spec, c)
    missing = [k for k in ARGON_KEYS if k not in spec["isotopes"]]
    age = build_age(spec, c)
    cap = run_age_pipeline(age, c)
    if missing:
        if age.uF is not None:
            raise RuntimeError("legacy computed F with a missing isotope")
        return case(name, source, inputs, {}, sentinel={"f": None, "ages": None},
                    error=missing[0])
    expected, sentinel, diags = pipeline_result(age, cap, spec, c)
    tol = cancellation_tol(inputs, why) if why else None
    return case(name, source, inputs, expected, tol=tol, sentinel=sentinel, diagnostics=diags)


def r3836(c):
    return fval(c["atm4036"]["v"]) / fval(c["atm4038"]["v"])


def gen_pipeline():
    src = "arar_age_test.py:"

    def build(_key, base):
        seeded = seeded_intercepts()

        def isos(**kw):
            d = {k: {"intercept": v} for k, v in seeded.items()}
            for k, v in kw.items():
                d[k] = v
            return d

        err_rows = dict(base_rows())
        err_rows.update({k: v for k, v in F_ERR_PR.items() if k != "Cl3638"})
        cases = [
            pipeline_case("pipeline/base", src + "52-72 (_build_age) ArArAgePipelineTest",
                          pipeline_spec(), base),
            pipeline_case("pipeline/j_err_large_position_small",
                          src + "JVariantCorrectnessTest:test_position_err_uses_position_jerr_only",
                          pipeline_spec(j=(1e-3, 1e-5), position_jerr=1e-8), base),
            pipeline_case("pipeline/j_err_huge", src + "JVariantCorrectnessTest:"
                          "test_uage_strips_j_error", pipeline_spec(j=(1e-3, 1e-3),
                                                                    position_jerr=5e-4), base),
            pipeline_case("pipeline/segments", src + "DecayFactorCachingTest (167-175)",
                          pipeline_spec(segments=[(1.0, 1.0, 365.0)]), base),
            pipeline_case("pipeline/multi_segments", "arar_age.py:455-463, :596-599",
                          pipeline_spec(segments=[(1.0, 0.5, 400.0), (0.8, 0.75, 380.0),
                                                  (1.2, 0.25, 360.5)], decay_days=400.0), base),
            pipeline_case("pipeline/ratio_errors", "argon_calculations.py:585-589 (spec E15)",
                          pipeline_spec(rows=err_rows), base),
            pipeline_case("pipeline/fixed_k3739_by_analysis", "arar_age.py:68, :615",
                          pipeline_spec(rows=err_rows, fixed=(0.05, 0.001)), base),
            pipeline_case("pipeline/fixed_k3739_by_constants", "argon_calculations.py:415-418",
                          pipeline_spec(rows=err_rows, overrides={"k3739_mode": "Fixed"}), base),
            pipeline_case("pipeline/abundance_sensitivity", "arar_age.py:589-591",
                          pipeline_spec(overrides={"abundance_sensitivity": 1e-4}), base),
            pipeline_case("pipeline/cosmogenic", src + "SetCosmogenicCorrectionTest",
                          pipeline_spec(overrides={"cosmogenic": ((0.18, 0.001), (0.65, 0.01))}),
                          base),
            pipeline_case("pipeline/include_decay_error", "arar_age.py:624-668",
                          pipeline_spec(overrides={"include_decay_error": True}), base),
            pipeline_case("pipeline/lambda_k_override", "dvc/dvc.py:2303-2305",
                          pipeline_spec(lambda_k_total=(5.5305e-10, 1.1e-12),
                                        overrides={"include_decay_error": True}), base),
            pipeline_case("pipeline/lambda_k_zero_ignored", "dvc/dvc.py:2303-2305 (`if lk:`)",
                          pipeline_spec(lambda_k_total=(0.0, 0.0)), base),
            pipeline_case("pipeline/units_ka", "arar_constants.py:143-170",
                          pipeline_spec(overrides={"age_units": "ka"}), base),
            pipeline_case("pipeline/no_j", "arar_age.py:659-660",
                          pipeline_spec(j=None), base),
            pipeline_case(
                "pipeline/baselines_blanks_ic", "isotope.py:715-854; dvc_analysis.py:688-761",
                pipeline_spec(isotopes={
                    "Ar40": {"intercept": (1003.2, 0.31), "baseline": (0.012, 0.002),
                             "blank": (2.1, 0.05), "ic_factor": (1.0, 0.0),
                             "include_baseline_error": True},
                    "Ar39": {"intercept": (101.7, 0.12), "baseline": (-0.003, 0.001),
                             "blank": (0.05, 0.004), "ic_factor": (1.0021, 0.0011)},
                    "Ar38": {"intercept": (10.4, 0.02), "baseline": (0.001, 0.0005),
                             "blank": (0.01, 0.001), "ic_factor": (1.0021, 0.0011),
                             "include_baseline_error": True},
                    "Ar37": {"intercept": (50.3, 0.04), "baseline": (0.002, 0.0004),
                             "blank": (0.03, 0.002), "ic_factor": (0.9987, 0.0009)},
                    "Ar36": {"intercept": (2.05, 0.004), "baseline": (0.0011, 0.0002),
                             "blank": (0.006, 0.0004), "ic_factor": (1.0352, 0.0021)},
                }), base),
            pipeline_case(
                "pipeline/blank_type_no_blank_correction", "pychron_constants.py:264",
                pipeline_spec(isotopes={
                    k: {"intercept": v, "blank": (0.5, 0.05), "correct_for_blank": False}
                    for k, v in seeded.items()}), base),
            pipeline_case("pipeline/negative_ar36", "spec 7 (negative signal replicated)",
                          pipeline_spec(isotopes=isos(Ar36={"intercept": (0.01, 0.003),
                                                            "blank": (0.05, 0.004)})), base),
            pipeline_case("pipeline/zero_ar37", "arar_age.py:534-545 (spec Q6)",
                          pipeline_spec(isotopes=isos(Ar37={"intercept": (0.0, 0.0)}),
                                        rows={k: v for k, v in base_rows().items()
                                              if k != "K3739"}), base),
            pipeline_case("pipeline/zero_ar40", "argon_calculations.py:554-557 (spec Q6)",
                          pipeline_spec(isotopes=isos(Ar40={"intercept": (0.0, 0.5)})), base),
            pipeline_case(
                "pipeline/zero_k39", "argon_calculations.py:550-553 (spec Q6); cl38 and ca37 "
                "also exactly 0 (Ar38 = r3836 * Ar36, no Ca/Cl production)",
                pipeline_spec(
                    isotopes={"Ar40": {"intercept": (1000.0, 1.0)},
                              "Ar39": {"intercept": (0.0, 0.5)},
                              "Ar38": {"intercept": (r3836(base) * 2.0, 0.05)},
                              "Ar37": {"intercept": (0.0, 0.005)},
                              "Ar36": {"intercept": (2.0, 0.01)}},
                    rows={"K4039": (0.0002, 0.0), "K3839": (0.0, 0.0), "Ca_K": (2.0, 0.0),
                          "Cl_K": (0.1, 0.0)}), base),
            pipeline_case(
                "pipeline/age_undefined", "argon_calculations.py:626-630 (spec Q6)",
                pipeline_spec(isotopes=isos(Ar40={"intercept": (10.0, 0.1)},
                                            Ar39={"intercept": (0.1, 0.01)})), base),
            pipeline_case(
                "pipeline/huge_errors", "spec 7 (huge input errors)",
                pipeline_spec(isotopes={
                    "Ar40": {"intercept": (1000.0, 1e6)}, "Ar39": {"intercept": (100.0, 1e5)},
                    "Ar38": {"intercept": (10.0, 1e4)}, "Ar37": {"intercept": (50.0, 1e4)},
                    "Ar36": {"intercept": (2.0, 1e3)}}), base),
            pipeline_case(
                "pipeline/air_shot", "spec 4.7 (rad40 of an air shot)",
                pipeline_spec(isotopes={
                    "Ar40": {"intercept": (590.92, 0.5)}, "Ar39": {"intercept": (0.01, 0.002)},
                    "Ar38": {"intercept": (0.3754, 0.001)}, "Ar37": {"intercept": (0.02, 0.001)},
                    "Ar36": {"intercept": (2.0, 0.004)}}), base,
                why="air shot: rad40 = a40 - atm40 is a near cancellation"),
            pipeline_case("pipeline/missing_isotope", src + "AssemblyByNameTest; arar_age.py:568-581",
                          pipeline_spec(isotopes={k: {"intercept": v} for k, v in seeded.items()
                                                  if k != "Ar36"}), base),
            pipeline_case("pipeline/chlorine_cl3638", "argon_calculations.py:468-487",
                          pipeline_spec(rows=dict(base_rows(), Cl3638=(250.0, 5.0))), base),
            pipeline_case("pipeline/chlorine_with_segments", "argon_calculations.py:468-487",
                          pipeline_spec(rows=dict(err_rows, Cl3638=(250.0, 5.0)),
                                        segments=[(1.0, 1.0, 365.0)], decay_days=365.0), base),
        ]
        return cases

    return for_presets(build)


# ----------------------------------------------------------------------------
# chlorine.json
# ----------------------------------------------------------------------------


def gen_chlorine():
    def build(_key, base):
        cl_rows = dict(F_PR, Cl3638=(250.0, 5.0))
        cases = []
        for dd in (0.0, 30.0, 365.0, 3650.0):
            cases.append(f_case("chlorine/calculate_f_decay_days_{:g}".format(dd),
                                "argon_calculations.py:468-487, :516-591",
                                F_ISOTOPES, dd, cl_rows, base))
        cases.append(f_case("chlorine/calculate_f_missing_cl3638",
                            "argon_calculations.py:481 (missing = 0)", F_ISOTOPES, 365.0,
                            {k: v for k, v in F_PR.items() if k != "Cl3638"}, base))
        cases.append(f_case("chlorine/calculate_f_ratio_errors",
                            "argon_calculations.py:468-487", F_ISOTOPES, 365.0, F_ERR_PR, base))
        rows = dict(base_rows(), Cl3638=(250.0, 5.0))
        cases.append(pipeline_case("chlorine/reduce_cl3638", "arar_age.py:547-558",
                                   pipeline_spec(rows=rows), base))
        cases.append(pipeline_case("chlorine/reduce_cl3638_decay_days_3650",
                                   "arar_age.py:547-558, :699-704",
                                   pipeline_spec(rows=rows, decay_days=3650.0), base))
        no_cl = {k: v for k, v in base_rows().items() if k not in ("Cl3638", "Cl_K")}
        cases.append(pipeline_case("chlorine/reduce_missing_cl_production",
                                   "arar_age.py:560-566 (factor 1); argon_calculations.py:481",
                                   pipeline_spec(rows=no_cl), base))
        cases.append(pipeline_case("chlorine/reduce_cl_k_zero", "arar_age.py:565 (`or 1.0`)",
                                   pipeline_spec(rows=dict(rows, Cl_K=(0.0, 0.0))), base))
        # cl38 exactly 0: no K38/Ca38/Cl, Ar38 = r3836 * Ar36 (all exact arithmetic).
        cases.append(pipeline_case(
            "chlorine/reduce_cl38_zero", "arar_age.py:547-558 (spec Q6)",
            pipeline_spec(
                isotopes={"Ar40": {"intercept": (1000.0, 1.0)},
                          "Ar39": {"intercept": (100.0, 0.5)},
                          "Ar38": {"intercept": (r3836(base) * 2.0, 0.05)},
                          "Ar37": {"intercept": (5.0, 0.025)},
                          "Ar36": {"intercept": (2.0, 0.01)}},
                rows={"K4039": (0.0002, 0.0), "K3739": (0.01, 0.0), "Ca3937": (0.0007, 0.0),
                      "Ca_K": (2.0, 0.0), "Cl_K": (0.1, 0.0)}), base))
        return cases

    return for_presets(build)


# ----------------------------------------------------------------------------
# correlation.json
# ----------------------------------------------------------------------------


def gen_correlation():
    c = presets()["legacy"]
    cases = []
    # Two ages from one J ufloat (age_equation).
    for name, j, fa, fb, over in (
        ("correlation/shared_j_age_equation", (1e-3, 1e-6), (10.0, 0.01), (25.0, 0.03), {}),
        ("correlation/shared_j_age_equation_decay_error", (2.5e-3, 4e-6), (8.0, 0.02),
         (8.1, 0.02), {"include_decay_error": True}),
    ):
        cc = with_overrides(c, over)
        ac = make_constants(cc)
        J = L.ufloat(*j, tag="J")
        FA = L.ufloat(*fa, tag="F")
        FB = L.ufloat(*fb, tag="F")
        age_a = L.ac.age_equation(J, FA, include_decay_error=cc["include_decay_error"],
                                  arar_constants=ac)
        age_b = L.ac.age_equation(J, FB, include_decay_error=cc["include_decay_error"],
                                  arar_constants=ac)
        names = ["f_a", "f_b", "age_a", "age_b"]
        vals = [FA, FB, age_a, age_b]
        expected = {n: uv(v) for n, v in zip(names, vals)}
        expected["cov"] = cov_entries(names, vals)
        inputs = {"function": "age_equation_pair", "j": mv(*j), "f_a": mv(*fa), "f_b": mv(*fb),
                  "constants": cc, "lambda_k_total": None}
        cases.append(case(name, "argon_calculations.py:603-630 with one shared J ufloat",
                          inputs, expected))

    # Two ArArAge analyses sharing one J / one Ar36 blank ufloat.
    spec_a = pipeline_spec(isotopes={
        "Ar40": {"intercept": (1003.2, 0.31), "blank": (2.1, 0.05)},
        "Ar39": {"intercept": (101.7, 0.12), "blank": (0.05, 0.004)},
        "Ar38": {"intercept": (10.4, 0.02), "blank": (0.01, 0.001)},
        "Ar37": {"intercept": (50.3, 0.04), "blank": (0.03, 0.002)},
        "Ar36": {"intercept": (2.05, 0.004), "blank": (0.006, 0.0004)},
    })
    spec_b = pipeline_spec(isotopes={
        "Ar40": {"intercept": (812.4, 0.27), "blank": (2.1, 0.05)},
        "Ar39": {"intercept": (64.2, 0.09), "blank": (0.05, 0.004)},
        "Ar38": {"intercept": (7.9, 0.02), "blank": (0.01, 0.001)},
        "Ar37": {"intercept": (12.1, 0.03), "blank": (0.03, 0.002)},
        "Ar36": {"intercept": (1.21, 0.003), "blank": (0.006, 0.0004)},
    }, j=(1e-3, 1e-6))
    for name, shared in (
        ("correlation/shared_j_reduce", {"j": True}),
        ("correlation/shared_blank_reduce", {"blank": "Ar36"}),
        ("correlation/independent_reduce", {}),
    ):
        shared_j = L.ufloat(*spec_a["j"], tag="J") if shared.get("j") else None
        sb = None
        if shared.get("blank"):
            iso = shared["blank"]
            sb = (iso, L.ufloat(*spec_a["isotopes"][iso]["blank"], tag="{} bk".format(iso)))
        age_a = build_age(spec_a, c, shared_j=shared_j, shared_blank=sb)
        age_b = build_age(spec_b, c, shared_j=shared_j, shared_blank=sb)
        run_age_pipeline(age_a, c)
        run_age_pipeline(age_b, c)
        names = ["A.f", "B.f", "A.age", "B.age", "A.age_w_j_err", "B.age_w_j_err"]
        vals = [age_a.uF, age_b.uF, age_a.uage, age_b.uage, age_a.uage_w_j_err,
                age_b.uage_w_j_err]
        expected = {n: uv(v) for n, v in zip(names, vals)}
        expected["cov"] = cov_entries(names, vals)
        inputs = {"function": "reduce_pair",
                  "analyses": {"A": pipeline_inputs_json(spec_a, None),
                               "B": pipeline_inputs_json(spec_b, None)},
                  "constants": c, "shared": shared}
        cases.append(case(name, "arar_age.py:443-689 with one shared ufloat (spec Q2, owner "
                          "requirement)", inputs, expected))
    return cases


# ----------------------------------------------------------------------------
# driver
# ----------------------------------------------------------------------------


def legacy_header(legacy):
    commit = subprocess.run(["git", "-C", legacy, "rev-parse", "HEAD"], check=True,
                            capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "-C", legacy, "status", "--porcelain"], check=True,
                                capture_output=True, text=True).stdout.strip())
    packages = {"python": platform.python_version()}
    for key, dist in LEGACY_PACKAGES:
        packages[key] = importlib.metadata.version(dist)
    return {"schema": SCHEMA, "legacy_commit": commit, "legacy_dirty": dirty,
            "packages": packages}


def check_case_set(fname, cases):
    names = [c["name"] for c in cases]
    if len(set(names)) != len(names):
        raise RuntimeError("duplicate case names in " + fname)
    for c in cases:
        if fname not in ("ufloat.json", "decay_factors.json"):
            consts = c["inputs"].get("constants")
            if not consts or set(consts) != set(CONSTANT_KEYS):
                raise RuntimeError("incomplete constants in {} {}".format(fname, c["name"]))
        warn_cancellation(fname, c)
        if fname != "ufloat.json":
            for x in walk_numbers(c["expected"]):
                if isinstance(x, str):
                    raise RuntimeError("non-finite expected value in {} {}".format(
                        fname, c["name"]))


def warn_cancellation(fname, c):
    """Report expected nominals that look like near cancellations without atol."""
    if fname in ("ufloat.json", "constants.json") or c["tol"]["atol"] > 0:
        return
    scale = cancellation_tol(c["inputs"], "")["atol"] / 1e-12
    for path, v in walk_values(c["expected"]):
        if isinstance(v, float) and v != 0 and abs(v) < 1e-9 * scale:
            print("warning: {} {} {}: |{}| < 1e-9 * {}".format(fname, c["name"], path, v, scale),
                  file=sys.stderr)


def walk_values(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == "v":
                yield path, v
            elif k != "e":
                yield from walk_values(v, path + "/" + k)


def walk_numbers(o):
    if isinstance(o, dict):
        for v in o.values():
            yield from walk_numbers(v)
    elif isinstance(o, list):
        for v in o:
            yield from walk_numbers(v)
    elif isinstance(o, (float, int)) and not isinstance(o, bool):
        yield o
    elif isinstance(o, str) and o in ("nan", "inf", "-inf"):
        yield o


def generate(legacy, out_dir):
    header = legacy_header(legacy)
    generators = {
        "ufloat.json": gen_ufloat,
        "constants.json": lambda: gen_constants(legacy),
        "isotope_arithmetic.json": gen_isotope_arithmetic,
        "decay_factors.json": gen_decay_factors,
        "interference.json": gen_interference,
        "atmospheric.json": gen_atmospheric,
        "calculate_f.json": gen_calculate_f,
        "age.json": gen_age,
        "pipeline.json": gen_pipeline,
        "chlorine.json": gen_chlorine,
        "correlation.json": gen_correlation,
    }
    os.makedirs(out_dir, exist_ok=True)
    for fname in FILES:
        cases = generators[fname]()
        check_case_set(fname, cases)
        doc = dict(header)
        doc["cases"] = cases
        text = json.dumps(doc, sort_keys=True, indent=1, allow_nan=False) + "\n"
        with open(os.path.join(out_dir, fname), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--legacy", required=True, help="legacy pychron checkout (read only)")
    p.add_argument("--out", required=True, help="golden directory (tests/reduction/golden)")
    p.add_argument("--check", action="store_true",
                   help="regenerate into a temporary directory; exit 1 if --out differs")
    args = p.parse_args(argv)
    legacy = os.path.abspath(args.legacy)
    bootstrap(legacy)
    if not args.check:
        generate(legacy, args.out)
        return 0
    tmp = tempfile.mkdtemp(prefix="reduction-golden-check-")
    try:
        generate(legacy, tmp)
        differ = []
        for fname in FILES:
            ours = os.path.join(args.out, fname)
            if not os.path.exists(ours):
                differ.append(fname + " (missing)")
                continue
            with open(ours, "rb") as a, open(os.path.join(tmp, fname), "rb") as b:
                if a.read() != b.read():
                    differ.append(fname)
        if differ:
            print("golden files differ: " + ", ".join(differ), file=sys.stderr)
            return 1
        print("golden files up to date ({} files)".format(len(FILES)))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
