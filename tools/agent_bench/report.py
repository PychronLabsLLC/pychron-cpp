#!/usr/bin/env python3
"""A table of the fixed set's runs: one label alone, or a variant against a baseline.

    python3 tools/agent_bench/report.py baseline
    python3 tools/agent_bench/report.py baseline split

For each task: runs that passed, and the median and the range of what a run
cost. With two labels each task gets a verdict. The variant "worked" on a
task when it passed at least as often as the baseline and its median cost is
under the baseline's by more than the baseline's own range, which is all one
run differs from the next with nothing changed; "worse" is the mirror of
that; anything between is "same". One run a label has no range, and says
nothing about cost.
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEASURES = [
    ("cost $", lambda r: (r.get("agent") or {}).get("total_cost_usd")),
    ("turns", lambda r: (r.get("agent") or {}).get("num_turns")),
    ("minutes", lambda r: ((r.get("agent") or {}).get("seconds") or 0) / 60 or None),
    ("lines", lambda r: (r["diff"]["insertions"] + r["diff"]["deletions"]) if r.get("diff") else None),
    ("findings", lambda r: next((c.get("findings") for c in r.get("checks", []) if "findings" in c), None)),
]


def load(directory: Path) -> dict[str, list[dict]]:
    runs: dict[str, list[dict]] = {}
    for path in sorted(directory.glob("*.json")):
        run = json.loads(path.read_text())
        runs.setdefault(run["task"], []).append(run)
    return runs


def spread(values: list[float]) -> tuple[float, float, float] | None:
    values = [v for v in values if v is not None]
    if not values:
        return None
    return statistics.median(values), min(values), max(values)


def cell(values: list[float]) -> str:
    found = spread(values)
    if found is None:
        return "-"
    median, low, high = found
    return f"{median:.3g}" if low == high else f"{median:.3g} ({low:.3g}-{high:.3g})"


def passes(runs: list[dict]) -> int:
    return sum(1 for run in runs if run.get("passed"))


def verdict(baseline: list[dict], variant: list[dict]) -> str:
    """How the variant did on one task, by pass rate first and then by cost."""
    if not baseline or not variant:
        return "no runs"
    base_rate = passes(baseline) / len(baseline)
    rate = passes(variant) / len(variant)
    if rate < base_rate:
        return "worse: passes less often"
    cost = MEASURES[0][1]
    base = spread([cost(run) for run in baseline])
    new = spread([cost(run) for run in variant])
    if base is None or new is None:
        return "passes more often" if rate > base_rate else "same (no cost recorded)"
    noise = base[2] - base[1]
    if len(baseline) < 2:
        return "passes more often" if rate > base_rate else "same (one baseline run: no range)"
    if new[0] < base[0] - noise:
        return "worked: cheaper by more than the baseline's range"
    if new[0] > base[0] + noise:
        return "worse: dearer by more than the baseline's range"
    return "passes more often" if rate > base_rate else "same"


def table(label: str, runs: dict[str, list[dict]]) -> list[str]:
    out = [f"{label}", f"{'task':28} {'passed':>7}  " + "  ".join(f"{name:>20}" for name, _ in MEASURES)]
    for task, task_runs in runs.items():
        cells = "  ".join(f"{cell([measure(run) for run in task_runs]):>20}" for _, measure in MEASURES)
        out.append(f"{task:28} {passes(task_runs):>3}/{len(task_runs):<3}  {cells}")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baseline", help="a label of run.py")
    parser.add_argument("variant", nargs="?", help="a second label, compared with the first")
    parser.add_argument("--results", default=str(ROOT / "build" / "agent_bench"))
    args = parser.parse_args()

    results = Path(args.results)
    baseline = load(results / args.baseline)
    if not baseline:
        print(f"report: no runs under {results / args.baseline}", file=sys.stderr)
        return 2
    lines = table(args.baseline, baseline)
    if args.variant:
        variant = load(results / args.variant)
        lines += [""] + table(args.variant, variant) + ["", f"{args.variant} against {args.baseline}"]
        for task in baseline:
            lines.append(f"{task:28} {verdict(baseline[task], variant.get(task, []))}")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
