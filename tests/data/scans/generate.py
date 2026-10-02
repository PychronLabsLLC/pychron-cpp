#!/usr/bin/env python3
"""Regenerate the peak-center fixture scans and their expected results.

The expected values come from legacy pychron's own code: this script loads
pychron/core/stats/peak_detection.py from a pychron checkout and runs
calculate_peak_center, calculate_resolution and calculate_resolving_power on
each scan. The C++ port (libs/systems jobs/peak_center) must reproduce them.

    python tests/data/scans/generate.py ~/Programming/pychron

Needs numpy. Writes <name>.csv (x,y per line) and expected.toml here
(absent keys: not computable).
"""

import math
import random
import sys
from pathlib import Path

import numpy

HERE = Path(__file__).resolve().parent


def load_legacy(pychron_root):
    src = (Path(pychron_root) / "pychron" / "core" / "stats" / "peak_detection.py").read_text()
    src = src.replace("from pychron.core.time_series.time_series import smooth\n", "")
    src = src.replace("from pychron.pychron_constants import NULL_STR\n", "")
    ns = {"smooth": lambda y, **kw: y, "NULL_STR": "---", "__name__": "legacy_peak_detection"}
    exec(compile(src, "peak_detection.py", "exec"), ns)
    return ns


def trapezoid(x, center, height, flat=0.0020, edge=0.0015, background=0.0):
    d = abs(x - center)
    if d <= flat:
        return background + height
    if d <= flat + edge:
        return background + height * (1 - (d - flat) / edge)
    return background


def scan(center=5.0, window=0.015, step=0.0005):
    n = int(round(2 * window / step)) + 1
    return [center - window + i * step for i in range(n)]


def scans():
    rng = random.Random(1234)
    out = {}
    xs = scan()
    out["flat_top"] = [(x, trapezoid(x, 5.0, 1000)) for x in xs]
    out["noisy_flat_top"] = [(x, trapezoid(x, 5.0, 1000) + rng.gauss(0, 3)) for x in xs]
    out["shifted"] = [(x, trapezoid(x, 5.006, 2.5e5, background=20)) for x in xs]
    out["asymmetric_shoulder"] = [
        (x, trapezoid(x, 5.0, 800) + trapezoid(x, 4.9945, 150, flat=0.001, edge=0.001)) for x in xs
    ]
    out["counter_poisson"] = [(x, float(numpy.random.default_rng(7).poisson(trapezoid(x, 5.001, 3000) + 5)))
                              for x in xs]
    out["descending_order"] = list(reversed(out["flat_top"]))
    out["high_left_background"] = [
        (x, trapezoid(x, 5.0, 1000) + (900 if x < 4.997 else 0)) for x in xs
    ]
    out["edge_peak"] = [(x, trapezoid(x, 5.0165, 1000)) for x in xs]
    out["too_small"] = [(x, trapezoid(x, 5.0, 0.5)) for x in xs]
    # A plateau with a notch at its center: the plateau test rejects it.
    notched = [(x, trapezoid(x, 5.0, 1000)) for x in xs]
    for i, (x, _) in enumerate(notched):
        if abs(x - 4.9995) < 1e-9 or abs(x - 5.0) < 1e-9:
            notched[i] = (x, 600.0)
    out["notched_plateau"] = notched
    # Two separated peaks: legacy centers on the first (lower x) one.
    out["double_peak"] = [(x, trapezoid(x, 4.9975, 1000, flat=0.0010, edge=0.0005) +
                          trapezoid(x, 5.0025, 1000, flat=0.0010, edge=0.0005)) for x in xs]
    out["gaussian_no_plateau"] = [(x, 1e6 * math.exp(-((x - 5.0) / 0.003) ** 2 / 2)) for x in xs]
    return out


def main():
    legacy = load_legacy(sys.argv[1] if len(sys.argv) > 1 else Path.home() / "Programming" / "pychron")
    expected = {}
    for name, pts in scans().items():
        with open(HERE / f"{name}.csv", "w") as f:
            f.write("# x,y\n")
            for x, y in pts:
                f.write(f"{x!r},{y!r}\n")
        x = [p[0] for p in pts]
        y = [p[1] for p in pts]
        entry = {}
        try:
            (lx, cx, hx), (ly, cy, hy), mx, my = legacy["calculate_peak_center"](x, y, min_peak_height=1.0,
                                                                                 percent=80)
            entry["peak"] = {k: float(v) for k, v in dict(low_x=lx, center_x=cx, high_x=hx, low_y=ly,
                                                          center_y=cy, high_y=hy, max_x=mx, max_y=my).items()}
        except legacy["PeakCenterError"] as e:
            entry["error"] = str(e)
        res = legacy["calculate_resolution"](x, y)
        lrp, hrp = legacy["calculate_resolving_power"](x, y)
        entry["resolution"] = None if res == "---" else float(res)
        # Legacy divides by zero for a vertical edge; the port reports "not computable".
        finite = lambda v: None if v == "---" or not math.isfinite(float(v)) else float(v)
        entry["resolving_power_low"] = finite(lrp)
        entry["resolving_power_high"] = finite(hrp)
        expected[name] = entry
    lines = ["# Produced by generate.py from legacy pychron's peak_detection.py. Do not edit."]
    for name in sorted(expected):
        e = expected[name]
        lines.append("")
        lines.append(f"[{name}]")
        if "error" in e:
            lines.append(f"error = {e['error']!r}".replace("'", '"'))
        for k in ("resolution", "resolving_power_low", "resolving_power_high"):
            if e[k] is not None:
                lines.append(f"{k} = {e[k]!r}")
        if "peak" in e:
            for k in sorted(e["peak"]):
                lines.append(f"peak.{k} = {e['peak'][k]!r}")
    (HERE / "expected.toml").write_text("\n".join(lines) + "\n")
    for k, v in expected.items():
        print(k, v.get("error") or v["peak"]["center_x"], v["resolution"])


if __name__ == "__main__":
    main()
