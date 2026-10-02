#!/usr/bin/env python3
"""Convert legacy pychron conditionals YAML to pychron-cpp TOML.

Conditionals spec (docs/superpowers/specs/2026-10-02-conditionals-design.md)
section 7. Handles system files (setupfiles/spectrometer/*conditionals.yaml),
queue files (queue_conditionals/<name>.yaml), run files
(scripts/conditionals/<name>.yaml) and the legacy inline run value
"<check>,<start>".

Nothing is dropped silently: every rewrite, default and skipped entry is
listed in the report. Action snippets become `run_hook` actions plus a hook
stub to fill in by hand.

    python tools/pychron_conditionals_import.py system_conditionals.yaml -o system.toml
    python tools/pychron_conditionals_import.py --inline "Ar40>1000,20" -o run.toml

Requires PyYAML.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml

# Legacy YAML key -> TOML table.
TABLES = {
    "truncations": "truncations",
    "terminations": "terminations",
    "cancelations": "cancelations",
    "actions": "actions",
    "equilibrations": "equilibrations",
    "modifications": "modifications",
    "pre_run_terminations": "pre_run",
    "post_run_terminations": "post_run",
    "post_run_actions": "post_run",
}

# QueueModificationConditional action names (conditional.py MODIFICATION_ACTIONS).
MODIFICATION_ACTIONS = {
    "skip next run": "skip_next",
    "skip n runs": "skip_n",
    "skip aliquot": "skip_aliquot",
    "skip to last in aliquot": "skip_to_last_in_aliquot",
    "set extract": "set_extract",
    "repeat run": "repeat",
    "run blank": "run_blank",
}

RATIO_TABLES = {"truncations", "equilibrations", "modifications"}
LEGACY_DEFAULT_START = 50  # BaseConditional.from_dict default

PRESSURE_RE = re.compile(r"\b(?!gauge\.)(\w+)\.(\w+)\.pressure\b")
CURRENT_RE = re.compile(r"\.current\b")
INTERP_BS_RE = re.compile(r"\b(Ar\d\d)bs\b")


@dataclass
class Result:
    toml: str
    report: list[str] = field(default_factory=list)
    hooks: dict[str, str] = field(default_factory=dict)  # hook name -> stub source
    converted: int = 0
    skipped: int = 0


def convert_check(teststr: str) -> tuple[str, list[str]]:
    """Legacy check string -> pychron-cpp grammar, with notes on each rewrite."""
    notes = []
    check = " ".join(str(teststr).split())

    def pressure(m: re.Match) -> str:
        notes.append(f"'{m.group(0)}' -> 'gauge.{m.group(2)}.pressure' (controller '{m.group(1)}' dropped; "
                     "check the gauge name in extraction_line.toml)")
        return f"gauge.{m.group(2)}.pressure"

    check = PRESSURE_RE.sub(pressure, check)
    if CURRENT_RE.search(check):
        check = CURRENT_RE.sub(".cur", check)
        notes.append("'.current' -> '.cur'")
    if INTERP_BS_RE.search(check):
        check = INTERP_BS_RE.sub(r"average(\1.bs)", check)
        notes.append("'<iso>bs' baseline value -> 'average(<iso>.bs)'")
    if check.count("(") != check.count(")"):
        notes.append("unbalanced parentheses: check by hand")
    return check, notes


def _start(entry: dict) -> tuple[int, str | None]:
    for key in ("start", "start_count"):
        if key in entry and entry[key] is not None:
            return int(entry[key]), None
    return LEGACY_DEFAULT_START, f"start defaulted to {LEGACY_DEFAULT_START} (legacy YAML default)"


def _teststr(entry: dict) -> str | None:
    # dictgetter precedence: teststr > comp > check; a present key wins even if empty.
    for key in ("teststr", "comp", "check"):
        if key in entry:
            return entry[key]
    return None


def _analysis_types(entry: dict) -> list[str]:
    return [str(a).lower().replace(" ", "_") for a in entry.get("analysis_types") or []]


def _hook_name(table: str, index: int) -> str:
    return f"{table}_{index}"


def _hook_stub(name: str, snippet: str, check: str) -> str:
    body = "\n".join(f"    # {line}" for line in str(snippet).splitlines()) or "    # (empty)"
    return (f'"""Measurement hook `{name}` imported from a pychron action conditional.\n\n'
            f"Called when this check trips: {check}\n"
            f'The legacy snippet is below; port it to the MeasurementAPI.\n"""\n\n\n'
            f"def {name}(api):\n{body}\n    api.log(\"{name} fired\")\n")


def convert_entry(table: str, legacy_key: str, index: int, entry: dict, res: Result) -> dict | None:
    where = f"{legacy_key}[{index}]"
    if not isinstance(entry, dict):
        res.report.append(f"{where}: skipped: not a mapping")
        return None
    teststr = _teststr(entry)
    if not teststr:
        res.report.append(f"{where}: skipped: no teststr/comp/check (pychron drops these too)")
        return None
    check, notes = convert_check(teststr)
    out: dict = {"check": check}
    for n in notes:
        res.report.append(f"{where}: {n}")

    start, note = _start(entry)
    if note and table not in ("pre_run", "post_run"):
        res.report.append(f"{where}: {note}")
    if table not in ("pre_run", "post_run"):
        out["start"] = start
        for key in ("frequency", "ntrips"):
            if key in entry and entry[key] not in (None, 1):
                v = int(entry[key])
                if v < 1:
                    res.report.append(f"{where}: {key} {v} is invalid, using 1")
                else:
                    out[key] = v
    elif entry.get("ntrips") not in (None, 1):
        out["ntrips"] = int(entry["ntrips"])

    window = int(entry.get("window") or 0)
    if window > 0:
        out["window"] = window
    if entry.get("mapper"):
        out["mapper"] = str(entry["mapper"])
    types = _analysis_types(entry)
    if types:
        out["analysis_types"] = types

    ratio = entry.get("abbreviated_count_ratio")
    if ratio is not None and float(ratio) != 1.0:
        if table not in RATIO_TABLES:
            res.report.append(f"{where}: abbreviated_count_ratio ignored ({table} do not take it)")
        elif not 0 < float(ratio) <= 1:
            res.report.append(f"{where}: abbreviated_count_ratio {ratio} out of (0, 1], dropped")
        else:
            out["abbreviated_count_ratio"] = float(ratio)

    if table == "actions":
        snippet = entry.get("action")
        if not snippet:
            res.report.append(f"{where}: skipped: action conditional without an action")
            return None
        name = _hook_name("action", index)
        out["action"] = f"run_hook {name}"
        res.hooks[name] = _hook_stub(name, snippet, check)
        res.report.append(f"{where}: action snippet -> 'run_hook {name}' (stub written; port it by hand)")
        if entry.get("resume"):
            out["resume"] = True
    elif table == "modifications":
        raw = str(entry.get("action") or "Skip Next Run")
        action = MODIFICATION_ACTIONS.get(raw.strip().lower())
        if action is None:
            res.report.append(f"{where}: skipped: unknown modification action '{raw}'")
            return None
        if action == "skip_n":
            n = int(entry.get("nskip") or 0)
            if n < 1:
                res.report.append(f"{where}: 'Skip N Runs' with nskip={n}: using 1")
                n = 1
            action = f"skip_n {n}"
        elif action == "set_extract":
            steps = str(entry.get("extraction_str") or "").strip()
            if not steps:
                res.report.append(f"{where}: skipped: 'Set Extract' without extraction_str")
                return None
            action = f"set_extract {steps}"
            res.report.append(f"{where}: 'Set Extract' was broken in pychron; now applies '{steps}'")
        out["action"] = action
        if entry.get("use_truncation"):
            out["truncate"] = True
            if entry.get("use_termination"):
                res.report.append(f"{where}: both use_truncation and use_termination set; keeping truncate")
        elif entry.get("use_termination"):
            out["terminate"] = True
    elif legacy_key == "post_run_actions":
        raw = str(entry.get("action") or "").strip()
        if raw == "repeat":
            out["action"] = "repeat"
        elif raw == "cancel":
            out["action"] = "cancel"
        else:
            res.report.append(f"{where}: skipped: post-run action '{raw}' has no equivalent "
                              "(cancel or a queue action)")
            return None
    return out


def _unique_names(entries: list[dict], table: str) -> None:
    seen: dict[str, int] = {}
    for e in entries:
        seen[e["check"]] = seen.get(e["check"], 0) + 1
    counts: dict[str, int] = {}
    for e in entries:
        if seen[e["check"]] > 1:
            counts[e["check"]] = counts.get(e["check"], 0) + 1
            e["name"] = f"{table}:{e['check']} #{counts[e['check']]}"


def _toml_value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, list):
        return "[" + ", ".join(_toml_value(x) for x in v) + "]"
    return json.dumps(str(v))


KEY_ORDER = ["name", "check", "start", "frequency", "ntrips", "window", "mapper", "analysis_types",
             "abbreviated_count_ratio", "action", "resume", "truncate", "terminate"]


def to_toml(tables: dict[str, list[dict]], header: str) -> str:
    lines = [f"# {line}" if line else "#" for line in header.splitlines()]
    for table in ("pre_run", "modifications", "truncations", "actions", "terminations", "cancelations",
                  "equilibrations", "post_run"):
        for entry in tables.get(table, []):
            lines.append("")
            lines.append(f"[[{table}]]")
            for key in KEY_ORDER:
                if key in entry:
                    lines.append(f"{key} = {_toml_value(entry[key])}")
    return "\n".join(lines) + "\n"


def convert_yaml(text: str, source: str = "conditionals.yaml") -> Result:
    data = yaml.safe_load(text) or {}
    res = Result(toml="")
    if not isinstance(data, dict):
        res.report.append(f"{source}: not a mapping of conditional kinds; nothing converted")
        res.toml = to_toml({}, f"Imported from {source}: nothing converted")
        return res
    tables: dict[str, list[dict]] = {}
    for key, entries in data.items():
        table = TABLES.get(key)
        if table is None:
            res.report.append(f"{key}: skipped: unknown kind")
            continue
        if not entries:
            continue
        if not isinstance(entries, list):
            res.report.append(f"{key}: skipped: expected a list")
            continue
        for i, entry in enumerate(entries):
            out = convert_entry(table, key, i, entry, res)
            if out is None:
                res.skipped += 1
            else:
                tables.setdefault(table, []).append(out)
                res.converted += 1
    for table, entries in tables.items():
        _unique_names(entries, table)
    res.toml = to_toml(tables, f"Imported from {source} by tools/pychron_conditionals_import.py.\n"
                               "See the import report for every rewrite.")
    return res


def convert_inline(value: str) -> Result:
    """Legacy run value "<check>,<start>": one truncation with ratio 0.5."""
    res = Result(toml="")
    if "," not in value:
        res.report.append(f"'{value}': skipped: legacy inline conditionals need '<check>,<start>' "
                          "(pychron ignored this value)")
        res.skipped = 1
        res.toml = to_toml({}, "Imported legacy inline conditional: nothing converted")
        return res
    teststr, start = value.rsplit(",", 1)
    check, notes = convert_check(teststr)
    res.report.extend(f"inline: {n}" for n in notes)
    res.report.append("inline: legacy inline value -> one truncation, abbreviated_count_ratio 0.5")
    entry = {"check": check, "start": int(start), "abbreviated_count_ratio": 0.5}
    res.converted = 1
    res.toml = to_toml({"truncations": [entry]}, f"Imported legacy inline conditional '{value}'.")
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", nargs="?", help="legacy conditionals YAML file")
    ap.add_argument("--inline", help="legacy run value '<check>,<start>'")
    ap.add_argument("-o", "--output", help="TOML file to write (default: stdout)")
    ap.add_argument("--hooks-dir", help="where to write hook stubs (default: next to the output)")
    args = ap.parse_args(argv)
    if bool(args.input) == bool(args.inline):
        ap.error("give exactly one of INPUT or --inline")

    if args.inline:
        res = convert_inline(args.inline)
    else:
        path = Path(args.input)
        res = convert_yaml(path.read_text(), path.name)

    if args.output:
        out = Path(args.output)
        out.write_text(res.toml)
        hooks_dir = Path(args.hooks_dir) if args.hooks_dir else out.parent / "measurement_hooks"
    else:
        sys.stdout.write(res.toml)
        hooks_dir = Path(args.hooks_dir) if args.hooks_dir else None
    if res.hooks:
        if hooks_dir is None:
            res.report.append("hook stubs not written: pass -o or --hooks-dir")
        else:
            hooks_dir.mkdir(parents=True, exist_ok=True)
            for name, src in res.hooks.items():
                (hooks_dir / f"{name}.py").write_text(src)
    print(f"converted {res.converted}, skipped {res.skipped}", file=sys.stderr)
    for line in res.report:
        print(f"  {line}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
