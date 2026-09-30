"""Build agent prompts and run one headless Claude Code agent per unit in its
own git worktree. No judgments here; pure orchestration."""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Protocol

from .plan import UnitBrief

REPORT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "required": ["summary", "files_changed", "commands_run", "tests_run", "tests_passed", "blockers"],
    "properties": {
        "summary": {"type": "string", "description": "What was implemented, 3-8 sentences."},
        "files_changed": {"type": "array", "items": {"type": "string"}},
        "commands_run": {"type": "array", "items": {"type": "string"}, "description": "Build/test commands actually executed."},
        "tests_run": {"type": "boolean"},
        "tests_passed": {"type": ["boolean", "null"], "description": "null if tests could not be run."},
        "test_output_tail": {"type": "string", "description": "Last ~30 lines of the test run, verbatim."},
        "blockers": {"type": "array", "items": {"type": "string"}, "description": "Anything unfinished and why."},
        "out_of_scope_changes": {"type": "array", "items": {"type": "string"}, "description": "Edits outside the unit's targets, with reason."},
    },
}

CONVENTIONS = """\
Repository conventions (non-negotiable):
- C++20, CMake >= 3.25, dependencies via vcpkg.json manifest. Presets live in CMakePresets.json.
- libs/* must not depend on Qt. Dependency direction is strictly core <- transport <- codecs <- devices <- systems <- apps.
- No exceptions across library boundaries: return Result<T> (std::expected<T, Error>).
- Every new behavior gets a GoogleTest test under tests/<lib>/. Tests must not need hardware or network.
- Test-first: write a failing test, make it pass, then refactor. Keep files focused.
- Do not touch files outside your unit's target paths unless strictly required to build; if you must, list them in the report.
- Do not commit. Leave changes in the working tree. The router builds, runs ctest, and merges.
- Finish by emitting the JSON report matching the schema you were given. Be literal: tests_passed is true only if you ran ctest and it exited 0.
"""


def build_prompt(brief: UnitBrief, *, spec_path: str, completed_units: list[str]) -> str:
    sizing = brief.sizing
    sections = "\n\n".join(f"### [{s.id}] {s.title}\n\n{s.body.rstrip()}" for s in brief.sections)
    done = ", ".join(completed_units) if completed_units else "none (you are first)"
    turns = f"You have roughly {sizing.max_turns} tool turns." if sizing else ""
    return f"""You are implementing one work unit of a C++ project from its design spec.

## Unit `{brief.unit.id}` (layer: {brief.unit.layer})

Goal: {brief.unit.goal}

Target paths: {", ".join(brief.unit.targets)}
Units already merged on this branch: {done}
{turns}

## Spec sections that govern this unit

The full spec is at `{spec_path}` if you need surrounding context, but the sections below are your requirements.

{sections}

## {CONVENTIONS}
Start by reading the existing tree (CMakeLists.txt, libs/, tests/) so you extend rather than duplicate. Then implement the unit.
"""


@dataclass
class UnitResult:
    unit_id: str
    branch: str
    worktree: str
    exit_code: int
    report: dict[str, Any] | None
    raw_stdout: str = ""
    raw_stderr: str = ""
    cost_usd: float | None = None
    duration_ms: int | None = None


class Runner(Protocol):
    def __call__(self, cmd: list[str], *, cwd: Path, stdin: str | None = None, timeout: float | None = None) -> subprocess.CompletedProcess[str]: ...


def resolve_tool(name: str) -> str:
    """Prefer a tool installed next to this interpreter (the router venv), then PATH.
    Returns the bare name if nothing is found so the caller gets a clean 'not found'."""
    import shutil
    import sys

    local = Path(sys.executable).parent / name
    if local.exists():
        return str(local)
    return shutil.which(name) or name


def subprocess_runner(cmd: list[str], *, cwd: Path, stdin: str | None = None, timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, cwd=cwd, input=stdin, text=True, capture_output=True, timeout=timeout)
    except FileNotFoundError as e:
        # Missing executable: report as a failed process instead of crashing the wave.
        return subprocess.CompletedProcess(cmd, 127, stdout="", stderr=f"{cmd[0]}: not found ({e})")


@dataclass
class DispatchConfig:
    repo: Path
    claude_bin: str = "claude"
    worktree_root: str = ".worktrees"
    allowed_tools: list[str] = field(default_factory=lambda: ["Bash", "Read", "Edit", "Write", "Glob", "Grep"])
    permission_mode: str = "acceptEdits"
    agent_timeout_s: float = 3 * 60 * 60
    extra_args: list[str] = field(default_factory=list)


def claude_command(cfg: DispatchConfig, brief: UnitBrief) -> list[str]:
    sizing = brief.sizing
    cmd = [
        cfg.claude_bin, "-p",
        "--output-format", "json",
        "--permission-mode", cfg.permission_mode,
        "--allowedTools", *cfg.allowed_tools,
        "--json-schema", json.dumps(REPORT_SCHEMA),
        "--no-session-persistence",
    ]
    if sizing:
        cmd += ["--model", sizing.model, "--max-turns", str(sizing.max_turns)]
    cmd += cfg.extra_args
    return cmd


def parse_agent_output(stdout: str) -> tuple[dict[str, Any] | None, dict[str, Any]]:
    """Return (report, envelope). Tolerates plain-text or partial output."""
    try:
        env = json.loads(stdout)
    except json.JSONDecodeError:
        return None, {"raw": stdout[-4000:]}
    if not isinstance(env, dict):
        return None, {"raw": env}
    report = env.get("structured_output")
    if report is None:
        res = env.get("result")
        if isinstance(res, dict):
            report = res
        elif isinstance(res, str):
            try:
                report = json.loads(res)
            except json.JSONDecodeError:
                report = None
    return (report if isinstance(report, dict) else None), env


def ensure_worktree(cfg: DispatchConfig, unit_id: str, *, base: str = "HEAD", runner: Runner = subprocess_runner) -> tuple[Path, str]:
    branch = f"unit/{unit_id}"
    path = cfg.repo / cfg.worktree_root / unit_id
    if path.exists():
        return path, branch
    path.parent.mkdir(parents=True, exist_ok=True)
    exists = runner(["git", "rev-parse", "--verify", "--quiet", f"refs/heads/{branch}"], cwd=cfg.repo).returncode == 0
    if exists:
        # Never reset an existing unit branch (-B would discard unmerged work); check it out as-is.
        cmd = ["git", "worktree", "add", str(path), branch]
    else:
        cmd = ["git", "worktree", "add", "-b", branch, str(path), base]
    r = runner(cmd, cwd=cfg.repo)
    if r.returncode != 0:
        raise RuntimeError(f"git worktree add failed for {unit_id}: {r.stderr.strip()}")
    return path, branch


def merge_subject(unit_id: str) -> str:
    return f"merge unit/{unit_id}"


def branch_merged(repo: Path, unit_id: str, *, base: str = "main", runner: Runner = subprocess_runner) -> bool:
    """True when the router's merge commit for this unit is on `base`.

    Ancestry alone is not enough: a freshly created unit branch points at the
    commit it was cut from, which is trivially an ancestor of main while the
    agent's work still sits uncommitted in the worktree."""
    r = runner(["git", "log", base, "--fixed-strings", f"--grep={merge_subject(unit_id)}", "--format=%s", "-n", "20"], cwd=repo)
    if r.returncode != 0:
        return False
    return any(line.strip() == merge_subject(unit_id) for line in r.stdout.splitlines())


def remove_worktree(cfg: DispatchConfig, unit_id: str, *, runner: Runner = subprocess_runner) -> None:
    path = cfg.repo / cfg.worktree_root / unit_id
    runner(["git", "worktree", "remove", "--force", str(path)], cwd=cfg.repo)


def run_unit(cfg: DispatchConfig, brief: UnitBrief, prompt: str, *, base: str = "HEAD", runner: Runner = subprocess_runner) -> UnitResult:
    worktree, branch = ensure_worktree(cfg, brief.unit.id, base=base, runner=runner)
    cmd = claude_command(cfg, brief)
    try:
        proc = runner(cmd, cwd=worktree, stdin=prompt, timeout=cfg.agent_timeout_s)
    except subprocess.TimeoutExpired as e:
        return UnitResult(brief.unit.id, branch, str(worktree), -1, None, raw_stdout=str(e.stdout or ""), raw_stderr="timeout")
    report, env = parse_agent_output(proc.stdout)
    stderr = proc.stderr
    if env.get("is_error"):
        # CLI-level failure (auth, model, quota): surface its message as the error, drop any pseudo-report.
        stderr = f"claude cli error: {env.get('result')}\n{stderr}"
        report = None
    return UnitResult(
        brief.unit.id, branch, str(worktree), proc.returncode or (1 if env.get("is_error") else 0), report,
        raw_stdout=proc.stdout, raw_stderr=stderr,
        cost_usd=env.get("total_cost_usd"), duration_ms=env.get("duration_ms"),
    )


def dry_runner_factory(sink: Callable[[str], None]) -> Runner:
    """A Runner that prints commands instead of executing them and fakes success."""

    def _run(cmd: list[str], *, cwd: Path, stdin: str | None = None, timeout: float | None = None) -> subprocess.CompletedProcess[str]:
        shown = [c if len(c) < 120 else c[:117] + "..." for c in cmd]
        sink(f"[dry-run] (cwd={cwd}) {' '.join(shown)}")
        if stdin:
            sink(f"[dry-run] stdin: {len(stdin)} chars")
        return subprocess.CompletedProcess(cmd, 0, stdout="{}", stderr="")

    return _run
