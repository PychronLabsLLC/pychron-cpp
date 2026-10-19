#!/usr/bin/env python3
"""Convert legacy pychron measurement scripts and hop files to v2 measurement plans.

Measurement plan v2 spec (docs/superpowers/specs/2026-10-05-measurement-plan-v2-design.md)
section 4.2. Reads:

- a measurement script (`.py`): its `mx` YAML docstring, module constants and
  the calls in `main()`;
- a hops file, as Python tuples (`hops.txt`: `('Ar40:H1, Ar36:CDD:110', 15, 3)`,
  `iso:det[:deflection], counts, settle`) or as YAML (a list of `counts`,
  `settle`, `cup_configuration`, `positioning`);
- a `syn_extraction/<name>.yaml` (`baseline` and `modifier` tables), which
  becomes a baseline segment measured during extraction plus
  `[baseline.modifier.<detector>]`.

Nothing is dropped silently: every guess and every skipped command is in the
report. A command with no plan equivalent goes into a measurement hook stub
(`before_main` or `after_main`, by where it was called), with the original
command in a comment, and the report says `line <n>: <command>: hook stub`.

    python tools/pychron_measurement_import.py unknown.py --syn-extraction syn.yaml -o plan.toml
    python tools/pychron_measurement_import.py jan_ic.py --hops ic_hops.txt -o ic.toml
    python tools/pychron_measurement_import.py argonH.yaml --reference H2 --cycles 10 -o argonH.toml

Requires PyYAML.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml

# Cup order of an ARGUS VI, high mass to low; a label derived from the
# positioned isotope steps one mass unit per cup along it (--cups).
ARGUS_CUPS = ("H2", "H1", "AX", "L1", "L2", "CDD")

# Phases in run order; a segment without `when` is in the main phase (None).
PHASE_ORDER = {"before_inlet": 0, "during_extraction": 1, None: 2, "after_main": 3, "after_run": 4}

# Calls that carry nothing a plan needs, with the reason given in the report.
IGNORED = {
    "info": None,
    "set_fits": "fits come from the plan's [fits]",
    "set_baseline_fits": "fits come from the plan's [fits]",
    "sniff": "the plan sniffs during equilibration by default",
    "load_hops": "hops come from --hops",
    "define_hops": "hops come from --hops",
}

ISOTOPE_RE = re.compile(r"^([A-Za-z]+)(\d+)$")
BARE_KEY_RE = re.compile(r"^[A-Za-z0-9_-]+$")


@dataclass
class Result:
    toml: str
    report: list[str] = field(default_factory=list)
    hooks: dict[str, str] = field(default_factory=dict)  # hook name -> stub source


class Unknown(Exception):
    """An expression the converter cannot evaluate."""


@dataclass(frozen=True)
class Ref:
    """A run parameter the plan names with '@' (the script's context value)."""

    text: str


CONTEXT = {"eqtime": Ref("@extraction.eqtime")}


# ---------------------------------------------------------------- TOML out


def _key(k: str) -> str:
    return k if BARE_KEY_RE.match(k) else json.dumps(k)


def _value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, Ref):
        return json.dumps(v.text)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_value(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{_key(k)} = {_value(x)}" for k, x in v.items()) + " }"
    raise TypeError(f"cannot write {v!r}")


def _collect_value(entries: list[dict]) -> str:
    """The short form when every entry is a plain label = detector and labels
    differ; otherwise one inline table an entry."""
    labels = [e["label"] for e in entries]
    if all(set(e) == {"label", "detector"} for e in entries) and len(set(labels)) == len(labels):
        return _value({e["label"]: e["detector"] for e in entries})
    return "[\n  " + ",\n  ".join(_value(e) for e in entries) + " ]"


def to_toml(plan: dict, header: str) -> str:
    lines = [f"# {line}" if line else "#" for line in header.splitlines()]
    if "hook" in plan:
        lines += ["", f"hook = {_value(plan['hook'])}"]
    for table in ("plan", "equilibration", "detectors"):
        if table in plan:
            lines += ["", f"[{table}]"] + [f"{_key(k)} = {_value(v)}" for k, v in plan[table].items()]
    for seg in plan.get("segment", []):
        lines += ["", "[[segment]]"]
        lines += [f"{_key(k)} = {_value(v)}" for k, v in seg.items() if k != "positions"]
        for pos in seg.get("positions", []):
            lines.append("[[segment.positions]]")
            for k, v in pos.items():
                lines.append(f"collect = {_collect_value(v)}" if k == "collect" else f"{_key(k)} = {_value(v)}")
    for det, mod in plan.get("baseline", {}).get("modifier", {}).items():
        lines += ["", f"[baseline.modifier.{_key(det)}]"] + [f"{_key(k)} = {_value(v)}" for k, v in mod.items()]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- hops


def _number(text: str):
    try:
        return float(text)
    except ValueError:
        return None


def _mass_label(mass: float) -> str:
    return f"mass{int(mass)}" if float(mass).is_integer() else "mass" + str(mass).replace(".", "_")


@dataclass
class Hop:
    entries: list[dict]       # collect entries: label, detector and per-entry extras
    counts: int
    settle: float
    at: dict | None = None    # positioning when it is not the first entry
    baseline: bool = False    # a 'bs:<mass>:<det>' hop


def _settle(v):
    return int(v) if float(v).is_integer() else float(v)


def read_hops_txt(text: str, source: str, report: list[str]) -> list[Hop]:
    hops = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            spec, counts, settle = ast.literal_eval(line)
        except (ValueError, SyntaxError, TypeError):
            report.append(f"{source} line {n}: skipped: not a ('iso:det, ...', counts, settle) tuple")
            continue
        parts = [p.strip() for p in str(spec).split(",") if p.strip()]
        if parts and parts[0].startswith("bs:"):
            _, mass, det = parts[0].split(":")[:3]
            hops.append(Hop([], int(counts), _settle(settle), {"mass": float(mass), "detector": det}, True))
            continue
        entries, at = [], None
        for i, part in enumerate(parts):
            fields = part.split(":")
            iso, det = fields[0], fields[1]
            entry = {"label": iso, "detector": det}
            mass = _number(iso)
            if mass is not None:
                entry["label"] = _mass_label(mass)
                if i == 0:
                    at = {"mass": mass, "detector": det}
            if len(fields) > 2 and fields[2]:
                d = float(fields[2])
                entry["deflection"] = int(d) if d.is_integer() else d
            entries.append(entry)
        hops.append(Hop(entries, int(counts), _settle(settle), at))
    return hops


def read_hops_yaml(text: str, source: str, report: list[str]) -> list[Hop]:
    hops = []
    for n, h in enumerate(yaml.safe_load(text) or [], 1):
        entries = []
        for c in h.get("cup_configuration") or []:
            entry = {"label": str(c["isotope"]), "detector": str(c["detector"])}
            if c.get("active") is False:
                entry["active"] = False
            if c.get("protect"):
                entry["protect"] = True
            if c.get("deflection") is not None:
                entry["deflection"] = c["deflection"]
            if c.get("is_baseline"):
                report.append(f"{source} hop {n}: {c['detector']}: is_baseline ignored "
                              "(baselines are their own segment)")
            entries.append(entry)
        at = None
        pos = h.get("positioning") or {}
        if "dac" in pos:
            at = {"dac": float(pos["dac"])}
        elif "isotope" in pos:
            mass = _number(str(pos["isotope"]))
            det = str(pos.get("detector", entries[0]["detector"] if entries else ""))
            if mass is not None:
                at = {"mass": mass, "detector": det}
            elif not entries or (str(pos["isotope"]), det) != (entries[0]["label"], entries[0]["detector"]):
                at = {"label": str(pos["isotope"]), "detector": det}
        for k in pos:
            if k not in ("dac", "isotope", "detector"):
                report.append(f"{source} hop {n}: positioning.{k} ignored")
        hops.append(Hop(entries, int(h.get("counts", 1)), _settle(h.get("settle", 0)), at))
    return hops


def read_hops(path: Path, report: list[str]) -> list[Hop]:
    text = path.read_text()
    if path.suffix in (".yaml", ".yml"):
        return read_hops_yaml(text, path.name, report)
    return read_hops_txt(text, path.name, report)


def _position(entries: list[dict], counts: int, settle, at: dict | None) -> dict:
    pos = {"collect": entries}
    if at:
        pos["at"] = at
    pos["counts"] = counts
    if settle:
        pos["settle_s"] = settle
    return pos


def hop_segments(hops: list[Hop], cycles: int, integration, table: str | None) -> list[dict]:
    """A signal segment of the hops, and a baseline segment after the main
    measurement for each 'bs' hop, collecting every detector the hops read
    (its first label)."""
    signal = {"kind": "signal", "cycles": cycles}
    if integration is not None:
        signal["integration_s"] = integration
    if table:
        signal["table"] = table
    signal["positions"] = [_position(h.entries, h.counts, h.settle, h.at) for h in hops if not h.baseline]
    out = [signal]
    seen: dict[str, str] = {}
    for h in hops:
        for e in h.entries:
            seen.setdefault(e["detector"], e["label"])
    for h in hops:
        if h.baseline:
            collect = [{"label": lab, "detector": det} for det, lab in seen.items()]
            out.append({"kind": "baseline", "when": "after_main",
                        "positions": [_position(collect, h.counts, h.settle, h.at)]})
    return out


# ---------------------------------------------------------------- scripts


def _mx_lookup(node: ast.AST, mx: dict):
    path = []
    while isinstance(node, ast.Attribute):
        path.append(node.attr)
        node = node.value
    if not (isinstance(node, ast.Name) and node.id == "mx"):
        raise Unknown()
    v = mx
    for k in reversed(path):
        if not isinstance(v, dict) or k not in v:
            raise Unknown()
        v = v[k]
    return v


class ScriptConverter:
    def __init__(self, source: str, name: str, hops: list[Hop] | None, cups: tuple[str, ...], report: list[str]):
        self.source = source
        self.name = name
        self.hops = hops
        self.cups = cups
        self.report = report
        self.mx: dict = {}
        self.consts: dict = {}
        self.locals: dict = {}
        self.segments: list[dict] = []
        self.equilibration: dict = {}
        self.reference: str | None = None
        self.active: list[str] = []
        self.peak_center_dets: list[str] = []
        self.positioned: tuple[str, str] | None = None  # (isotope, detector)
        self.inlet = False          # equilibrate() called
        self.measured = False       # a main measurement call made
        self.integration = None     # set before the first collection segment
        self.time_zero_at_inlet = False
        self.hook_lines: dict[str, list[str]] = {"before_main": [], "after_main": []}

    # -- evaluation

    def ev(self, node: ast.AST):
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, (ast.Tuple, ast.List)):
            return [self.ev(e) for e in node.elts]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            return -self.ev(node.operand)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return not self.ev(node.operand)
        if isinstance(node, ast.Attribute):
            return _mx_lookup(node, self.mx)
        if isinstance(node, ast.Name):
            for scope in (self.locals, self.consts, CONTEXT):
                if node.id in scope:
                    return scope[node.id]
        raise Unknown()

    def args(self, call: ast.Call) -> tuple[list, dict]:
        pos = []
        for a in call.args:
            if isinstance(a, ast.Starred):
                pos.extend(self.ev(a.value))
            else:
                pos.append(self.ev(a))
        kw = {}
        for k in call.keywords:
            if k.arg is None:
                kw.update(self.ev(k.value) if not isinstance(k.value, ast.Dict)
                          else {self.ev(a): self.ev(b) for a, b in zip(k.value.keys, k.value.values)})
            else:
                kw[k.arg] = self.ev(k.value)
        return pos, kw

    # -- walking

    def convert(self) -> dict:
        tree = ast.parse(self.source)
        doc = ast.get_docstring(tree)
        if doc and doc.strip():
            self.mx = yaml.safe_load(doc) or {}
        for st in tree.body:
            if isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name):
                try:
                    self.consts[st.targets[0].id] = ast.literal_eval(st.value)
                except ValueError:
                    pass
        main = next((st for st in tree.body if isinstance(st, ast.FunctionDef) and st.name == "main"), None)
        if main is None:
            self.report.append(f"{self.name}: no main(); nothing converted")
            return {}
        self.walk(main.body)
        return self.plan()

    def walk(self, body: list[ast.stmt]):
        for st in body:
            if isinstance(st, ast.If):
                try:
                    taken = bool(self.ev(st.test))
                except Unknown:
                    self.unmapped(st)
                    continue
                self.walk(st.body if taken else st.orelse)
            elif isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name):
                try:
                    self.locals[st.targets[0].id] = self.ev(st.value)
                except Unknown:
                    self.unmapped(st)
            elif isinstance(st, ast.Expr) and isinstance(st.value, ast.Constant):
                continue  # a string statement
            elif isinstance(st, ast.Expr) and isinstance(st.value, ast.Call) and isinstance(st.value.func, ast.Name):
                self.call(st, st.value.func.id, st.value)
            elif isinstance(st, ast.Pass):
                continue
            else:
                self.unmapped(st)

    def unmapped(self, st: ast.stmt):
        text = " ".join((ast.get_source_segment(self.source, st) or "").split())
        entry = "after_main" if self.measured else "before_main"
        self.hook_lines[entry].append(f"line {st.lineno}: {text}")
        self.report.append(f"line {st.lineno}: {text}: hook stub")

    def when(self, after: str) -> str | None:
        if self.measured:
            return after
        return None if self.inlet else "before_inlet"

    def step(self, kind: str, body: dict):
        """A step where it was called: before the inlet, or in the main phase
        in file order."""
        seg = {"kind": kind}
        w = None if self.inlet else "before_inlet"
        if w:
            seg["when"] = w
        seg.update(body)
        self.segments.append(seg)

    def label_for(self, det: str) -> str:
        """The label `det` reads with the magnet where position_magnet put it."""
        if self.positioned is None:
            raise Unknown()
        iso, ref = self.positioned
        m = ISOTOPE_RE.match(iso)
        if det == ref:
            return iso
        if not m or det not in self.cups or ref not in self.cups:
            raise Unknown()
        return f"{m.group(1)}{int(m.group(2)) + self.cups.index(ref) - self.cups.index(det)}"

    def active_collect(self) -> list[dict]:
        return [{"label": self.label_for(d), "detector": d} for d in self.active]

    def collection(self, seg: dict):
        if "integration_s" not in seg and self.integration is not None:
            seg = {**{k: seg[k] for k in ("kind",) if k in seg}, "integration_s": self.integration,
                   **{k: v for k, v in seg.items() if k != "kind"}}
        self.segments.append(seg)

    def call(self, st: ast.stmt, fn: str, node: ast.Call):
        try:
            a, kw = self.args(node)
        except Unknown:
            self.unmapped(st)
            return
        if fn in IGNORED:
            if IGNORED[fn]:
                self.report.append(f"line {st.lineno}: {fn}: dropped ({IGNORED[fn]})")
            return
        handler = getattr(self, f"do_{fn}", None)
        if handler is None:
            self.unmapped(st)
            return
        try:
            handler(st, a, kw)
        except (Unknown, KeyError, IndexError, TypeError, ValueError):
            self.unmapped(st)

    # -- commands

    def do_activate_detectors(self, st, a, kw):
        if kw.get("peak_center"):
            self.peak_center_dets = [str(d) for d in a]
        else:
            self.active = [str(d) for d in a]

    def do_position_magnet(self, st, a, kw):
        iso = a[0] if a else kw["isotope"]
        det = kw.get("detector", a[1] if len(a) > 1 else None)
        if det is None or kw.get("dac"):
            raise Unknown()
        self.positioned = (str(iso), str(det))
        if self.reference is None:
            self.reference = str(det)

    def do_equilibrate(self, st, a, kw):
        names = ("eqtime", "inlet", "outlet", "delay")
        given = dict(zip(names, a)) | kw
        eq = self.mx.get("equilibration", {}) if isinstance(self.mx.get("equilibration"), dict) else {}
        inlet = given.get("inlet", eq.get("inlet"))
        outlet = given.get("outlet", eq.get("outlet"))
        if inlet is not None:
            self.equilibration["inlet"] = str(inlet)
        if outlet is not None:
            self.equilibration["outlet"] = str(outlet)
        t = given.get("eqtime", eq.get("eqtime"))
        if eq.get("use_extraction_eqtime"):
            t = CONTEXT["eqtime"]
        if t is not None:
            self.equilibration["time_s"] = t
        d = given.get("delay", eq.get("inlet_delay"))
        if d is not None:
            self.equilibration["inlet_delay_s"] = d
        self.equilibration["close_inlet"] = True
        self.inlet = True

    def do_set_time_zero(self, st, a, kw):
        if self.inlet and not self.measured:
            self.time_zero_at_inlet = True
            self.report.append(f"line {st.lineno}: set_time_zero at the inlet -> time_zero = \"on_inlet_close\" "
                               "on the first signal segment")
        else:
            self.step("time_zero", {"time_zero": "now"})

    def do_set_deflection(self, st, a, kw):
        det, v = (a + [None, None])[:2]
        det = kw.get("detector", det)
        v = kw.get("value", v)
        self.step("set", {"set": {"deflection": {str(det): v}}})

    def do_set_integration_time(self, st, a, kw):
        v = a[0] if a else kw["v"]
        if any(s["kind"] in ("signal", "baseline") for s in self.segments):
            self.step("set", {"set": {"integration_s": v}})
        else:
            self.integration = v

    def do_sleep(self, st, a, kw):
        self.step("delay", {"delay": {"seconds": a[0] if a else kw["seconds"]}})

    def _peak_center(self, body: dict):
        seg = {"kind": "peak_center"}
        w = "after_run" if self.measured else (None if self.inlet else "before_inlet")
        if w:
            seg["when"] = w
        seg["peak_center"] = body
        self.segments.append(seg)

    def do_peak_center(self, st, a, kw):
        det = str(kw.get("detector", a[0] if a else ""))
        iso = str(kw.get("isotope", a[1] if len(a) > 1 else "Ar40"))
        body = {"label": iso, "detector": det}
        extra = [d for d in self.peak_center_dets if d != det]
        if extra:
            body["extra_detectors"] = extra
        if "integration_time" in kw:
            self.report.append(f"line {st.lineno}: peak_center integration_time ignored "
                               "(the spectrometer's peak-center config sets it)")
        self._peak_center(body)

    def do_generate_ic_mftable(self, st, a, kw):
        dets = [str(d) for d in (a[0] if a else kw["detectors"])]
        table = kw.get("name")
        if table is None:
            table = "ic_run"
            self.report.append(f"line {st.lineno}: generate_ic_mftable: table named 'ic_run'")
        self._peak_center({"label": str(kw.get("refiso", "Ar40")), "detectors": dets, "write_table": str(table)})

    def do_multicollect(self, st, a, kw):
        counts = kw.get("ncounts", a[0] if a else None)
        seg = {"kind": "signal"}
        if "integration_time" in kw:
            seg["integration_s"] = kw["integration_time"]
        if self.time_zero_at_inlet:
            seg["time_zero"] = "on_inlet_close"
            self.time_zero_at_inlet = False
        at = None
        iso, det = self.positioned or (None, None)
        collect = self.active_collect()
        if collect and (collect[0]["label"], collect[0]["detector"]) != (iso, det):
            at = {"label": iso, "detector": det}
        self.report.append(f"line {st.lineno}: multicollect labels from the cup order {' '.join(self.cups)} "
                           "(check them against the field table)")
        seg["positions"] = [_position(collect, int(counts), 0, at)]
        self.collection(seg)
        self.measured = True

    def do_peak_hop(self, st, a, kw):
        if self.hops is None:
            raise Unknown()
        cycles = int(kw.get("ncycles", a[0] if a else 1))
        segs = hop_segments(self.hops, cycles, None, kw.get("mftable"))
        if self.time_zero_at_inlet:
            segs[0]["time_zero"] = "on_inlet_close"
            self.time_zero_at_inlet = False
        self.collection(segs[0])
        self.measured = True
        self.segments.extend(segs[1:])

    def do_baselines(self, st, a, kw):
        counts = int(kw.get("ncounts", a[0] if a else 1))
        mass = kw.get("mass", a[1] if len(a) > 1 else None)
        det = kw.get("detector", a[2] if len(a) > 2 else self.reference)
        seg = {"kind": "baseline"}
        w = self.when("after_main")
        if w:
            seg["when"] = w
        if "integration_time" in kw:
            seg["integration_s"] = kw["integration_time"]
        settle = kw.get("settling_time", 0)
        at = {"mass": float(mass), "detector": str(det)} if isinstance(mass, (int, float)) else None
        if at is None:
            raise Unknown()
        seg["positions"] = [_position(self.active_collect(), counts, _settle(settle), at)]
        self.segments.append(seg)

    # -- result

    def plan(self) -> dict:
        out: dict = {}
        if self.equilibration:
            out["equilibration"] = self.equilibration
        if self.reference:
            out["detectors"] = {"reference": self.reference}
        out["segment"] = self.segments
        return out


def syn_extraction(path: Path, collect: list[dict], report: list[str]) -> tuple[dict, dict]:
    """A baseline segment during extraction and the baseline modifiers."""
    doc = yaml.safe_load(path.read_text()) or {}
    b = doc.get("baseline") or {}
    seg = {"kind": "baseline", "when": "during_extraction"}
    if "integration_time" in b:
        seg["integration_s"] = b["integration_time"]
    at = {"mass": float(b["mass"]), "detector": str(b["detector"])}
    seg["positions"] = [_position(collect, int(b.get("counts", 1)), _settle(b.get("settling_time", 0)), at)]
    modifiers = {}
    for det, m in (doc.get("modifier") or {}).items():
        modifiers[str(det)] = {k: m[k] for k in ("variable", "model", "error") if k in m}
    for k in doc:
        if k not in ("baseline", "modifier"):
            report.append(f"{path.name}: {k}: ignored")
    return seg, modifiers


# ---------------------------------------------------------------- entry


def _plan_name(stem: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]+", "_", stem).strip("_") or "imported"


def _hook_stub(name: str, source: str, lines: dict[str, list[str]]) -> str:
    out = [f'"""Measurement hook `{name}`, imported from {source}.\n',
           "The legacy commands below have no plan equivalent; port them to the api.", '"""', ""]
    for entry, cmds in lines.items():
        if not cmds:
            continue
        out += ["", f"def {entry}(api):"] + [f"    # {c}" for c in cmds] + [f'    api.log("{entry}: port the legacy commands")']
    return "\n".join(out) + "\n"


def convert(path: str | Path, hops: str | Path | None = None, syn: str | Path | None = None,
            reference: str | None = None, cycles: int | None = None, integration: float | None = None,
            family: str = "thermo_argus", analysis_types: tuple[str, ...] = ("unknown",),
            name: str | None = None, cups: tuple[str, ...] = ARGUS_CUPS) -> Result:
    path = Path(path)
    report: list[str] = []
    res = Result(toml="", report=report)
    name = name or _plan_name(path.stem)
    hop_list = read_hops(Path(hops), report) if hops else None
    plan: dict = {"plan": {"name": name, "instrument_family": family, "analysis_types": list(analysis_types)}}
    hook_lines: dict[str, list[str]] = {}
    collect_for_syn: list[dict] = []

    if path.suffix == ".py":
        conv = ScriptConverter(path.read_text(), path.name, hop_list, cups, report)
        body = conv.convert()
        plan.update(body)
        hook_lines = conv.hook_lines
        try:
            collect_for_syn = conv.active_collect()
        except Unknown:
            collect_for_syn = []
    else:
        hop_list = read_hops(path, report)
        if cycles is None:
            report.append(f"{path.name}: cycles = 1 (pass --cycles)")
        plan["segment"] = hop_segments(hop_list, cycles or 1, integration, None)
        collect_for_syn = hop_list[0].entries if hop_list else []
    if reference:
        plan["detectors"] = {"reference": reference}

    if syn:
        seg, modifiers = syn_extraction(Path(syn), collect_for_syn, report)
        plan.setdefault("segment", []).append(seg)
        if modifiers:
            plan["baseline"] = {"modifier": modifiers}

    # Segments in the order their phases run (the engine runs them so anyway).
    plan["segment"] = sorted(plan.get("segment", []), key=lambda s: PHASE_ORDER.get(s.get("when"), 2))

    if any(hook_lines.values()):
        hook = f"{name}_hook"
        plan["hook"] = hook
        res.hooks[hook] = _hook_stub(hook, path.name, hook_lines)

    sources = ", ".join(str(Path(p).name) for p in (path, hops, syn) if p)
    res.toml = to_toml(plan, f"Imported from {sources} by tools/pychron_measurement_import.py.\n"
                             "Check the conversion report before using it.")
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", help="measurement script (.py) or hops file (.txt, .yaml)")
    ap.add_argument("--hops", help="the hops file a script's peak_hop reads")
    ap.add_argument("--syn-extraction", help="syn_extraction YAML: baseline during extraction")
    ap.add_argument("--reference", help="reference detector ([detectors] reference)")
    ap.add_argument("--cycles", type=int, help="cycles of a hops file given alone")
    ap.add_argument("--integration", type=float, help="integration time (s) of a hops file given alone")
    ap.add_argument("--family", default="thermo_argus", help="the plan's instrument_family")
    ap.add_argument("--name", help="the plan's name (default: the input's stem)")
    ap.add_argument("--cups", default=" ".join(ARGUS_CUPS), help="cup order, high mass to low")
    ap.add_argument("-o", "--output", help="TOML file to write (default: stdout)")
    ap.add_argument("--report", help="write the conversion report here too")
    ap.add_argument("--hooks-dir", help="where to write hook stubs (default: measurement_hooks/ next to the output)")
    args = ap.parse_args(argv)

    res = convert(args.input, hops=args.hops, syn=args.syn_extraction, reference=args.reference,
                  cycles=args.cycles, integration=args.integration, family=args.family, name=args.name,
                  cups=tuple(args.cups.split()))
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
            for hook, src in res.hooks.items():
                (hooks_dir / f"{hook}.py").write_text(src)
    if args.report:
        Path(args.report).write_text("\n".join(res.report) + "\n")
    for line in res.report:
        print(f"  {line}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
