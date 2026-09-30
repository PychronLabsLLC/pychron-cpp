"""Regenerate the fit regression fixtures in this directory.

    uv run --with numpy --with scipy --with statsmodels python generate_fixtures.py

Reference values are computed the way pychron.core.regression does:
  * average      MeanRegressor: mean, std(ddof=1), sem = std/sqrt(n); SD error = std.
  * polynomial   OLSRegressor (statsmodels OLS): SEM = sef*sqrt(X0 C X0'),
                 SD = sef*sqrt(1 + X0 C X0'), C = normalized_cov_params, X0 = [1, 0, ...],
                 sef = sqrt(SSR/(n-q)).
  * exponential  ExponentialRegressor: scipy curve_fit of a*exp(-b*x)+c from pychron's
                 data-driven initial guess. Deviation: pychron's predict_error for this
                 regressor builds Xk=[x,x,x] and so reports 0 error at x=0; here the
                 intercept error is the delta method on a+c: SEM = sqrt(g' pcov g),
                 SD = sqrt(sef^2 + g' pcov g), g = [1, 0, 1].
  * outliers     BaseRegressor.calculate_filtered_data: per iteration, fit the kept
                 points, flag every point (kept or not) with |y - f(x)| >= std_devs*sef,
                 union into the excluded set. Deviations: an iteration whose bound is 0
                 (perfect fit) flags nothing, and an iteration that would leave fewer
                 than q points is discarded and filtering stops.

Series files that start with "pychron_" or "draper_smith" are the data sets from
pychron/core/regression/tests/standard_data.py; "evo_" files are synthetic
isotope evolutions whose y values are stored verbatim (the RNG is not needed to
reproduce them).
"""

import csv
import math
import os

import numpy as np
import statsmodels.api as sm
from scipy import optimize

HERE = os.path.dirname(os.path.abspath(__file__))

DRAPER_X = [35.3, 29.7, 30.8, 58.8, 61.4, 71.3, 74.4, 76.7, 70.7, 57.5, 46.4, 28.9, 28.1,
            39.1, 46.8, 48.5, 59.3, 70, 70, 74.5, 72.1, 58.1, 44.6, 33.4, 28.6]
DRAPER_Y = [10.98, 11.13, 12.51, 8.4, 9.27, 8.73, 6.36, 8.50, 7.82, 9.14, 8.24, 12.19, 11.88,
            9.57, 10.94, 9.58, 10.09, 8.11, 6.83, 8.88, 7.68, 8.47, 8.86, 10.36, 11.08]

DEGREE = {"linear": 1, "parabolic": 2, "cubic": 3}


def n_params(kind, degree):
    if kind == "average":
        return 1
    if kind == "exponential":
        return 3
    if kind == "custom_poly":
        return degree + 1
    return DEGREE[kind] + 1


def expo_guess(xs, ys):
    # pychron ExponentialRegressor._calculate_initial_guess
    y0, yn = ys[0], ys[-1]
    dx = xs[-1] - xs[0]
    if dx == 0:
        return 1.0, 0.0, float(y0)
    decay = y0 > yn
    c = min(yn, 0.0) if decay else min(y0, 0.0)
    a = y0 - c
    num = yn - c
    if a == 0 or num <= 0 or (num / a) <= 0:
        return (1.0, 0.1 if decay else -0.1, float(c))
    b = -math.log(num / a) / dx
    return float(a), float(b), float(c)


def expo_func(x, a, b, c):
    return a * np.exp(-b * x) + c


def solve(kind, degree, xs, ys, guess_xy):
    """Return (predict, value_at_0, var_hat_unscaled, sef, n)."""
    n = len(xs)
    q = n_params(kind, degree)
    if kind == "average":
        m = ys.mean()
        sef = ys.std(ddof=1) if n > 1 else 0.0
        return (lambda x: np.full_like(x, m)), m, 1.0 / n, sef
    if kind == "exponential":
        p, pcov = optimize.curve_fit(expo_func, xs, ys, p0=expo_guess(*guess_xy), maxfev=20000)
        resid = ys - expo_func(xs, *p)
        sef = math.sqrt((resid ** 2).sum() / (n - q)) if n > q else 0.0
        g = np.array([1.0, 0.0, 1.0])
        var = float(g @ pcov @ g)
        # store var in "unscaled" form so both branches share sef*sqrt(...)
        return (lambda x: expo_func(x, *p)), p[0] + p[2], (var / sef ** 2 if sef else 0.0), sef
    d = degree if kind == "custom_poly" else DEGREE[kind]
    X = np.vander(xs, d + 1, increasing=True)
    res = sm.OLS(ys, X).fit()
    sef = math.sqrt((res.resid ** 2).sum() / (n - q)) if n > q else 0.0
    beta = res.params
    C = res.normalized_cov_params
    return ((lambda x: np.vander(x, d + 1, increasing=True) @ beta), beta[0], float(C[0, 0]), sef)


def run(kind, degree, error, flt, iterations, std_devs, xs, ys):
    q = n_params(kind, degree)
    excluded = set()
    if flt:
        for _ in range(iterations):
            keep = np.array([i for i in range(len(xs)) if i not in excluded])
            pred, _, _, sef = solve(kind, degree, xs[keep], ys[keep], (xs, ys))
            bound = sef * std_devs
            if bound <= 0:
                break
            flagged = set(np.where(np.abs(ys - pred(xs)) >= bound)[0].tolist())
            if len(xs) - len(excluded | flagged) < q:
                break
            excluded |= flagged
    keep = np.array([i for i in range(len(xs)) if i not in excluded])
    _, value, var_hat, sef = solve(kind, degree, xs[keep], ys[keep], (xs, ys))
    if kind == "average":
        err = sef * math.sqrt(var_hat) if error == "sem" else sef
    else:
        err = sef * math.sqrt(var_hat) if error == "sem" else sef * math.sqrt(1.0 + var_hat)
    return value, err, sef, len(keep), sorted(excluded)


def write_series(name, xs, ys, source):
    with open(os.path.join(HERE, name + ".csv"), "w", newline="") as f:
        f.write("# {}\n".format(source))
        f.write("x,y\n")
        for x, y in zip(xs, ys):
            f.write("{!r},{!r}\n".format(float(x), float(y)))


def main():
    series = {}

    series["draper_smith"] = (np.array(DRAPER_X, float), np.array(DRAPER_Y, float),
                              "pychron standard_data.ols_data (Draper & Smith p.8)")
    series["draper_smith_outlier"] = (np.array(DRAPER_X + [10], float),
                                      np.array(DRAPER_Y + [1000], float),
                                      "pychron standard_data.filter_data (Draper & Smith + planted outlier)")
    xs = np.linspace(0, 100, 100)
    series["pychron_parabolic"] = (xs, np.polyval([2.12, 1.13, 5.14], xs),
                                   "pychron OLSRegressionTest2: polyval([2.12, 1.13, 5.14])")
    xs = np.linspace(1, 100)
    series["pychron_expo_growth"] = (xs, 100 * np.exp(0.05 * xs) + 1,
                                     "pychron standard_data.expo_data: 100*exp(0.05x)+1")
    xs = np.linspace(1, 20)
    series["pychron_expo_quadratic"] = (xs, 0.012 * (xs - 80) ** 2,
                                        "pychron standard_data.expo_data_linear: 0.012*(x-80)^2")

    rs = np.random.RandomState(20260930)
    xs = np.round(np.linspace(4.2, 400.0, 60), 3)
    ys = 12.0 * np.exp(-0.006 * xs) + 50.0 + rs.normal(0, 0.05, xs.size)
    ys[17] += 1.5
    ys[41] -= 1.2
    series["evo_decay_noisy"] = (xs, np.round(ys, 6),
                                 "synthetic Ar40-like decay 12*exp(-0.006x)+50, sd 0.05, outliers at 17,41")
    xs = np.round(np.linspace(2.0, 200.0, 50), 3)
    ys = 3.0 + 0.02 * xs - 1.5e-4 * xs ** 2 + 4e-7 * xs ** 3 + rs.normal(0, 0.01, xs.size)
    series["evo_cubic_noisy"] = (xs, np.round(ys, 6), "synthetic cubic evolution, sd 0.01")

    # (series, kind, degree, error, filter, iterations, std_devs, rtol, atol)
    cases = [
        ("draper_smith", "average", 0, "sem", 0, 1, 2.0, 1e-9, 1e-9),
        ("draper_smith", "average", 0, "sd", 0, 1, 2.0, 1e-9, 1e-9),
        ("draper_smith", "linear", 0, "sem", 0, 1, 2.0, 1e-9, 1e-9),
        ("draper_smith", "linear", 0, "sd", 0, 1, 2.0, 1e-9, 1e-9),
        ("draper_smith", "parabolic", 0, "sem", 0, 1, 2.0, 1e-8, 1e-8),
        ("draper_smith", "cubic", 0, "sem", 0, 1, 2.0, 1e-7, 1e-7),
        ("draper_smith", "custom_poly", 1, "sem", 0, 1, 2.0, 1e-9, 1e-9),
        ("draper_smith_outlier", "linear", 0, "sem", 1, 1, 2.0, 1e-9, 1e-9),
        ("draper_smith_outlier", "average", 0, "sem", 1, 3, 2.0, 1e-9, 1e-9),
        ("pychron_parabolic", "parabolic", 0, "sem", 0, 1, 2.0, 1e-9, 1e-6),
        ("pychron_expo_growth", "exponential", 0, "sem", 0, 1, 2.0, 1e-6, 1e-6),
        ("pychron_expo_quadratic", "exponential", 0, "sem", 0, 1, 2.0, 1e-5, 1e-6),
        ("pychron_expo_quadratic", "exponential", 0, "sd", 0, 1, 2.0, 1e-5, 1e-6),
        ("evo_decay_noisy", "average", 0, "sem", 0, 1, 2.0, 1e-9, 1e-9),
        ("evo_decay_noisy", "linear", 0, "sem", 1, 2, 2.5, 1e-9, 1e-9),
        ("evo_decay_noisy", "parabolic", 0, "sd", 1, 2, 2.5, 1e-9, 1e-9),
        ("evo_decay_noisy", "exponential", 0, "sem", 0, 1, 2.0, 1e-5, 1e-6),
        ("evo_decay_noisy", "exponential", 0, "sem", 1, 2, 2.5, 1e-5, 1e-6),
        ("evo_cubic_noisy", "cubic", 0, "sem", 0, 1, 2.0, 1e-8, 1e-8),
        ("evo_cubic_noisy", "cubic", 0, "sd", 0, 1, 2.0, 1e-8, 1e-8),
        ("evo_cubic_noisy", "custom_poly", 4, "sem", 0, 1, 2.0, 1e-7, 1e-7),
    ]

    for name, (xs, ys, source) in series.items():
        write_series(name, xs, ys, source)

    with open(os.path.join(HERE, "expected.csv"), "w", newline="") as f:
        f.write("# generated by generate_fixtures.py; |got-want| <= atol + rtol*|want|\n")
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["series", "kind", "degree", "error", "filter", "iterations", "std_devs",
                    "value", "error_value", "residual_sd", "n_used", "filtered", "rtol", "atol"])
        for name, kind, degree, error, flt, it, sd, rtol, atol in cases:
            xs, ys, _ = series[name]
            # fixtures store the rounded text; recompute from what the C++ side reads
            xs = np.array([float(repr(float(v))) for v in xs])
            ys = np.array([float(repr(float(v))) for v in ys])
            value, err, sef, n_used, filtered = run(kind, degree, error, flt, it, sd, xs, ys)
            w.writerow([name, kind, degree, error, flt, it, sd, repr(float(value)), repr(float(err)),
                        repr(float(sef)), n_used, ";".join(str(i) for i in filtered), rtol, atol])


if __name__ == "__main__":
    main()
