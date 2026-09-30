"""spec-router CLI.

  spec-router plan                 route + size, write .spec_router/plan.json
  spec-router prompt <unit>        print the agent prompt for one unit
  spec-router run [--wave N|--unit U] [--dry-run] [--no-merge]
  spec-router verify <unit> [--without-report]   re-verify existing worktree, no agent
  spec-router status
"""

from __future__ import annotations

import argparse
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
from pathlib import Path
from typing import Any

from . import dispatch, verify
from .judge import Judge, TypeSafeJudge
from .plan import Plan, Sizing, UnitBrief, route_sections, size_units
from .spec import parse_sections
from .units import Unit, load_units, waves

STATE_DIR = ".spec_router"


def _paths(args: argparse.Namespace) -> dict[str, Path]:
    repo = Path(args.repo).resolve()
    sd = repo / STATE_DIR
    return {"repo": repo, "state_dir": sd, "plan": sd / "plan.json", "state": sd / "state.json", "cache": sd / "judgments.json", "spec": (repo / args.spec).resolve()}


def _judge(args: argparse.Namespace, p: dict[str, Path]) -> Judge:
    return TypeSafeJudge(p["cache"], model=args.jev_model)


def _spec_path(p: dict[str, Path], unit: Unit) -> Path:
    return (p["repo"] / unit.spec).resolve() if unit.spec else p["spec"]


def _spec_rel(p: dict[str, Path], unit: Unit) -> str:
    return str(_spec_path(p, unit).relative_to(p["repo"]))


def _load_plan(p: dict[str, Path], units: list[Unit]) -> Plan:
    if not p["plan"].exists():
        sys.exit("no plan yet: run `spec-router plan` first")
    d = json.loads(p["plan"].read_text())
    by_id = {u.id: u for u in units}
    sec_cache: dict[Path, dict[str, Any]] = {}
    briefs = {}
    for uid, b in d["briefs"].items():
        if uid not in by_id:
            continue  # unit removed from units.toml since planning
        unit = by_id[uid]
        sp = _spec_path(p, unit)
        if sp not in sec_cache:
            sec_cache[sp] = {s.id: s for s in parse_sections(sp.read_text())}
        secs = sec_cache[sp]
        sizing = Sizing(**b["sizing"]) if b.get("sizing") else None
        briefs[uid] = UnitBrief(unit, [secs[s] for s in b["sections"] if s in secs], sizing)
    missing = [u.id for u in units if not u.manual and u.id not in briefs]
    if missing:
        sys.exit(f"plan is stale; units without a brief: {missing}. Run `spec-router plan`.")
    return Plan([], briefs, d.get("background", []))


def _load_state(p: dict[str, Path]) -> dict[str, Any]:
    return json.loads(p["state"].read_text()) if p["state"].exists() else {"units": {}}


def _save_state(p: dict[str, Path], st: dict[str, Any]) -> None:
    p["state_dir"].mkdir(exist_ok=True)
    p["state"].write_text(json.dumps(st, indent=1, sort_keys=True))


def cmd_plan(args: argparse.Namespace) -> int:
    p = _paths(args)
    units = load_units(Path(args.units) if args.units else None)
    judge = _judge(args, p)
    # Route each spec's sections only among the units that spec defines.
    by_spec: dict[Path, list[Unit]] = {}
    for u in units:
        by_spec.setdefault(_spec_path(p, u), []).append(u)
    plan = Plan([], {}, [])
    total_sections = 0
    for sp, us in by_spec.items():
        sections = parse_sections(sp.read_text())
        total_sections += len(sections)
        sub = size_units(route_sections(sections, us, judge, attach_threshold=args.attach_threshold), judge)
        plan.routings += sub.routings
        plan.briefs.update(sub.briefs)
        plan.background += [f"{sp.name}:{sid}" for sid in sub.background]
    p["state_dir"].mkdir(exist_ok=True)
    p["plan"].write_text(json.dumps(plan.to_dict(), indent=1))
    print(f"{total_sections} sections across {len(by_spec)} specs -> {len(plan.briefs)} units; background: {plan.background}")
    print(f"jev calls: {judge.calls}, cache hits: {judge.hits}\n")
    print(f"{'unit':26} {'wave':>4} {'#sec':>4} {'size':>5} {'conf':>5} {'model':7} {'turns':>5} split  spec")
    for uid, b in sorted(plan.briefs.items(), key=lambda kv: (kv[1].unit.wave, kv[0])):
        s = b.sizing
        print(f"{uid:26} {b.unit.wave:>4} {len(b.sections):>4} {s.score:>5.2f} {s.confidence:>5.2f} {s.model:7} {s.max_turns:>5} {'YES' if s.split else '   '}  {_spec_rel(p, b.unit).split('/')[-1]}")
    low = [r for r in plan.routings if r.confidence < 0.5 and r.primary != "background"]
    if low:
        print("\nlow-confidence routings (review):")
        for r in low:
            top = sorted(r.probabilities.items(), key=lambda kv: -kv[1])[:3]
            print(f"  [{r.section_id}] -> {r.primary} ({r.confidence:.2f}); top: {top}")
    return 0


def cmd_prompt(args: argparse.Namespace) -> int:
    p = _paths(args)
    units = load_units(Path(args.units) if args.units else None)
    plan = _load_plan(p, units)
    if args.unit not in plan.briefs:
        sys.exit(f"unknown unit {args.unit!r}; known: {sorted(plan.briefs)}")
    brief = plan.briefs[args.unit]
    print(dispatch.build_prompt(brief, spec_path=_spec_rel(p, brief.unit), completed_units=_done(_load_state(p))))
    return 0


def _done(st: dict[str, Any]) -> list[str]:
    return [u for u, s in st["units"].items() if s.get("status") == "merged"]


def _reconcile_with_git(p: dict[str, Path], st: dict[str, Any], unit_ids: list[str], log) -> None:
    """Git is the source of truth for 'merged'. If a unit branch is already an
    ancestor of main but state disagrees (crash, manual merge, stale rerun),
    correct the state instead of re-running the agent."""
    changed = False
    for uid in unit_ids:
        rec = st["units"].get(uid, {})
        if rec.get("status") != "merged" and dispatch.branch_merged(p["repo"], uid):
            note = f"reconciled from git: unit/{uid} already merged into main"
            st["units"][uid] = {**rec, "status": "merged", "reason": note}
            log(f"    {uid}: {note}")
            changed = True
    if changed:
        _save_state(p, st)


def _result_path(p: dict[str, Path], unit_id: str) -> Path:
    return p["state_dir"] / "results" / f"{unit_id}.json"


def _save_result(p: dict[str, Path], result: dispatch.UnitResult) -> None:
    """Persist the agent's raw outcome the moment it returns, before any
    verification step that could fail, so a router crash never loses a report."""
    path = _result_path(p, result.unit_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(asdict(result), indent=1))


def _load_result(p: dict[str, Path], unit_id: str) -> dispatch.UnitResult | None:
    path = _result_path(p, unit_id)
    return dispatch.UnitResult(**json.loads(path.read_text())) if path.exists() else None


def _verify_one(p: dict[str, Path], brief: UnitBrief, result: dispatch.UnitResult, judge: Judge | None, runner: dispatch.Runner, log, *, without_report: bool = False, accept_judgment: bool = False) -> dict[str, Any]:
    wt = Path(result.worktree)
    changed = verify.diff_is_nonempty(wt, runner=runner)
    build = verify.build_and_test(wt, runner=runner)
    judgment = verify.judge_report(brief, result.report, judge) if (result.report and judge) else None
    decision = verify.gate(result, build, judgment, changed=changed)
    hard_ok = changed and build.green and result.exit_code == 0
    if without_report and result.report is None and hard_ok:
        decision = verify.Decision(True, "ACCEPTED WITHOUT AGENT REPORT (operator override): green build + tests", build, None, changed)
    elif accept_judgment and not decision.merge and hard_ok and judgment is not None and judgment.outcome not in ("blocked", "off_track"):
        # Operator has read the report and accepts its caveats; the router's own build+ctest is green.
        decision = verify.Decision(True, f"OPERATOR OVERRIDE (judgment accepted): {decision.reason}", build, judgment, changed)
    rec: dict[str, Any] = {
        "status": "verified" if decision.merge else "failed", "reason": decision.reason,
        "branch": result.branch, "worktree": result.worktree, "exit_code": result.exit_code,
        "cost_usd": result.cost_usd, "duration_ms": result.duration_ms,
        "report": result.report, "decision": decision.to_dict(),
    }
    if not result.report:
        rec["stderr_tail"] = result.raw_stderr[-2000:]
        rec["stdout_tail"] = result.raw_stdout[-2000:]
        log(f"    agent stdout: {result.raw_stdout[-600:].strip() or '<empty>'}")
        log(f"    agent stderr: {result.raw_stderr[-600:].strip() or '<empty>'}")
    if not build.green:
        log(f"    build log tail:\n{build.log_tail[-1500:]}")
    log(f"<== {brief.unit.id}: {rec['status']} - {decision.reason}")
    return rec


def _run_one(args: argparse.Namespace, p: dict[str, Path], brief: UnitBrief, judge: Judge | None, runner: dispatch.Runner, log) -> dict[str, Any]:
    cfg = dispatch.DispatchConfig(repo=p["repo"], claude_bin=args.claude_bin)
    st = _load_state(p)
    prompt = dispatch.build_prompt(brief, spec_path=_spec_rel(p, brief.unit), completed_units=_done(st))
    log(f"==> {brief.unit.id}: model={brief.sizing.model if brief.sizing else '?'} sections={[s.id for s in brief.sections]}")
    result = dispatch.run_unit(cfg, brief, prompt, runner=runner)
    if args.dry_run:
        return {"status": "dry-run", "unit": brief.unit.id}
    _save_result(p, result)
    return _verify_one(p, brief, result, judge, runner, log)


def _merge_records(args: argparse.Namespace, p: dict[str, Path], st: dict[str, Any], results: list[tuple[str, dict[str, Any]]]) -> list[str]:
    """Merge verified units sequentially; returns ids that did not end merged."""
    for uid, rec in results:
        if rec["status"] == "verified" and not args.no_merge:
            ok, msg = verify.commit_and_merge(p["repo"], Path(rec["worktree"]), rec["branch"], uid)
            rec["status"] = "merged" if ok else "failed"
            rec["reason"] = msg if not ok else rec["reason"]
            if ok:
                dispatch.remove_worktree(dispatch.DispatchConfig(repo=p["repo"]), uid)
                rec.pop("worktree", None)
        st["units"][uid] = rec
        _save_state(p, st)
    return [uid for uid, rec in results if rec["status"] != "merged"]


def cmd_verify(args: argparse.Namespace) -> int:
    """Re-run verification (build, ctest, report judgment, merge) on an existing
    worktree without re-spawning the agent. Recovery path after a router or
    toolchain failure."""
    p = _paths(args)
    units = load_units(Path(args.units) if args.units else None)
    plan = _load_plan(p, units)
    brief = plan.briefs.get(args.unit) or sys.exit(f"unknown unit {args.unit!r}")
    result = _load_result(p, args.unit)
    wt = p["repo"] / ".worktrees" / args.unit
    if result is None:
        if not wt.exists():
            sys.exit(f"no saved result and no worktree for {args.unit}")
        result = dispatch.UnitResult(args.unit, f"unit/{args.unit}", str(wt), 0, None, raw_stderr="(no saved agent result; pre-persistence run)")
    judge = _judge(args, p)
    rec = _verify_one(p, brief, result, judge, dispatch.subprocess_runner, print, without_report=args.without_report, accept_judgment=args.accept_judgment)
    st = _load_state(p)
    failed = _merge_records(args, p, st, [(args.unit, rec)])
    return 1 if failed else 0


def cmd_run(args: argparse.Namespace) -> int:
    p = _paths(args)
    units = load_units(Path(args.units) if args.units else None)
    plan = _load_plan(p, units)
    st = _load_state(p)
    judge = None if args.dry_run else _judge(args, p)
    runner: dispatch.Runner = dispatch.dry_runner_factory(print) if args.dry_run else dispatch.subprocess_runner
    log = print

    selected = waves(units)
    if args.wave:
        selected = [w for w in selected if w.number == args.wave]
    if args.unit:
        selected = [type(w)(w.number, [u for u in w.units if u.id == args.unit]) for w in selected]
        selected = [w for w in selected if w.units]
    if not selected:
        sys.exit("nothing selected")

    _reconcile_with_git(p, st, [u.id for u in units], log)
    for wave in selected:
        todo = [u for u in wave.units if st["units"].get(u.id, {}).get("status") != "merged"]
        if not todo:
            log(f"wave {wave.number}: already merged")
            continue
        missing = [d for u in todo for d in u.depends if st["units"].get(d, {}).get("status") != "merged"]
        if missing and not args.dry_run and not args.force:
            sys.exit(f"wave {wave.number}: dependencies not merged: {sorted(set(missing))} (use --force to override)")
        split = [u.id for u in todo if plan.briefs[u.id].sizing and plan.briefs[u.id].sizing.split]
        if split and not args.force:
            sys.exit(f"wave {wave.number}: units flagged for manual split: {split} (edit units.toml or use --force)")

        log(f"\n=== wave {wave.number}: {[u.id for u in todo]} ===")
        with ThreadPoolExecutor(max_workers=args.parallel) as ex:
            results = list(ex.map(lambda u: (u.id, _run_one(args, p, plan.briefs[u.id], judge, runner, log)), todo))

        if args.dry_run:
            continue
        # Merge sequentially, in unit order, so the next wave sees all of this wave.
        failed = _merge_records(args, p, st, results)
        if failed:
            log(f"\nwave {wave.number} incomplete: {failed}. Stopping; fix or re-run with --wave {wave.number}.")
            return 1
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    p = _paths(args)
    st = _load_state(p)
    if not st["units"]:
        print("no runs recorded")
        return 0
    for uid, rec in st["units"].items():
        cost = f"${rec['cost_usd']:.2f}" if rec.get("cost_usd") else ""
        print(f"{uid:24} {rec['status']:9} {cost:>8}  {rec.get('reason','')}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="spec-router", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".", help="repository root (default: cwd)")
    ap.add_argument("--spec", default="docs/superpowers/specs/2026-09-29-instrument-control-design.md")
    ap.add_argument("--units", default=None, help="units.toml override")
    ap.add_argument("--jev-model", default="jev-latest")
    ap.add_argument("--claude-bin", default="claude")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("plan"); s.add_argument("--attach-threshold", type=float, default=0.5); s.set_defaults(fn=cmd_plan)
    s = sub.add_parser("prompt"); s.add_argument("unit"); s.set_defaults(fn=cmd_prompt)
    s = sub.add_parser("run")
    s.add_argument("--wave", type=int); s.add_argument("--unit")
    s.add_argument("--dry-run", action="store_true"); s.add_argument("--no-merge", action="store_true")
    s.add_argument("--force", action="store_true"); s.add_argument("--parallel", type=int, default=3)
    s.set_defaults(fn=cmd_run)
    s = sub.add_parser("verify", help="re-verify an existing worktree without re-running the agent")
    s.add_argument("unit"); s.add_argument("--no-merge", action="store_true")
    s.add_argument("--without-report", action="store_true", help="accept on green build+tests when the agent report was lost (logged as operator override)")
    s.add_argument("--accept-judgment", action="store_true", help="accept a partial/unfinished report judgment when the router's own build+ctest is green (logged as operator override)")
    s.set_defaults(fn=cmd_verify)
    s = sub.add_parser("status"); s.set_defaults(fn=cmd_status)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
