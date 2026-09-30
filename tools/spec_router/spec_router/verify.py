"""Ground-truth verification (build + ctest run by the router) combined with a
Jev judgment of the agent's own report. Policy is explicit in ``gate``."""

from __future__ import annotations

import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from typesafe_sdk import Choice, Noul

from .dispatch import Runner, UnitResult, subprocess_runner
from .judge import Judge
from .plan import UnitBrief


@dataclass
class BuildResult:
    has_cmake: bool
    configured: bool
    built: bool
    tests_passed: bool | None  # None = no tests discovered / not run
    log_tail: str = ""

    @property
    def green(self) -> bool:
        return self.has_cmake and self.configured and self.built and self.tests_passed is True


def _tail(s: str, n: int = 40) -> str:
    return "\n".join(s.splitlines()[-n:])


def build_and_test(worktree: Path, *, runner: Runner = subprocess_runner, build_dir: str = "build", preset: str | None = None, timeout: float = 1800) -> BuildResult:
    if not (worktree / "CMakeLists.txt").exists():
        return BuildResult(False, False, False, None, "no CMakeLists.txt")
    log: list[str] = []

    def step(cmd: list[str]) -> bool:
        r = runner(cmd, cwd=worktree, timeout=timeout)
        log.append(f"$ {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
        return r.returncode == 0

    cfg_cmd = ["cmake", "--preset", preset] if preset else ["cmake", "-S", ".", "-B", build_dir, "-DBUILD_UI=OFF", "-DBUILD_TESTS=ON"]
    configured = step(cfg_cmd)
    if not configured:
        return BuildResult(True, False, False, None, _tail("\n".join(log)))
    built = step(["cmake", "--build", build_dir, "--parallel"])
    if not built:
        return BuildResult(True, True, False, None, _tail("\n".join(log)))
    r = runner(["ctest", "--test-dir", build_dir, "--output-on-failure"], cwd=worktree, timeout=timeout)
    log.append(f"$ ctest\n{r.stdout}\n{r.stderr}")
    if "No tests were found" in r.stdout:
        return BuildResult(True, True, True, None, _tail("\n".join(log)))
    return BuildResult(True, True, True, r.returncode == 0, _tail("\n".join(log)))


def diff_is_nonempty(worktree: Path, *, runner: Runner = subprocess_runner) -> bool:
    r = runner(["git", "status", "--porcelain"], cwd=worktree)
    return bool(r.stdout.strip())


@dataclass
class ReportJudgment:
    outcome: str
    outcome_confidence: float
    evidence: float
    in_scope: float


REPORT_QUESTIONS = {
    "outcome": Choice(
        instructions=(
            "`unit` describes a work unit and `report` is the implementing agent's own account. Judging only "
            "from what the report states and shows, what is the state of the unit?"
        ),
        criteria={
            "complete": "Every part of the goal is implemented and the report shows tests were run and passed.",
            "partial": "Real progress on the goal, but the report names unfinished pieces or tests were not all run/passing.",
            "blocked": "Little or nothing implemented because of a stated obstacle (missing dependency, unclear spec, tooling failure).",
            "off_track": "The work described does not match the goal, or mostly touches things outside the unit's targets.",
        },
    ),
    "evidence": Noul(
        instructions=(
            "Does `report` contain concrete evidence of a test run (an actual command in `report.commands_run` "
            "and output in `report.test_output_tail` consistent with `report.tests_passed`), rather than a bare claim?"
        )
    ),
    "in_scope": Noul(
        instructions=(
            "Are the changes in `report.files_changed` confined to `unit.targets` (or clearly necessary build-glue "
            "such as the top-level CMakeLists), with any exceptions justified in `report.out_of_scope_changes`?"
        )
    ),
}


def judge_report(brief: UnitBrief, report: dict[str, Any], judge: Judge) -> ReportJudgment:
    state = {
        "unit": {"id": brief.unit.id, "goal": brief.unit.goal, "targets": brief.unit.targets},
        "report": report,
    }
    a = judge.ask(state, REPORT_QUESTIONS)
    c = a.choices["outcome"]
    return ReportJudgment(c.choice, c.confidence, a.nouls["evidence"], a.nouls["in_scope"])


@dataclass
class Decision:
    merge: bool
    reason: str
    build: BuildResult
    judgment: ReportJudgment | None
    changed: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "merge": self.merge, "reason": self.reason, "changed": self.changed,
            "build": asdict(self.build), "judgment": asdict(self.judgment) if self.judgment else None,
        }


def gate(result: UnitResult, build: BuildResult, judgment: ReportJudgment | None, *, changed: bool, min_confidence: float = 0.5, min_scope: float = 0.5) -> Decision:
    if not changed:
        return Decision(False, "agent produced no changes", build, judgment, changed)
    if result.exit_code != 0:
        return Decision(False, f"agent exited {result.exit_code}", build, judgment, changed)
    if not build.green:
        why = "no CMakeLists" if not build.has_cmake else "configure failed" if not build.configured else "build failed" if not build.built else "ctest failed or no tests"
        return Decision(False, f"router verification: {why}", build, judgment, changed)
    if judgment is None:
        return Decision(False, "agent emitted no structured report", build, judgment, changed)
    if judgment.outcome != "complete" or judgment.outcome_confidence < min_confidence:
        return Decision(False, f"report judged {judgment.outcome} (conf {judgment.outcome_confidence:.2f})", build, judgment, changed)
    if judgment.in_scope < min_scope:
        return Decision(False, f"report judged out of scope (P={judgment.in_scope:.2f})", build, judgment, changed)
    return Decision(True, "green build, tests pass, report complete and in scope", build, judgment, changed)


def commit_and_merge(repo: Path, worktree: Path, branch: str, unit_id: str, *, runner: Runner = subprocess_runner) -> tuple[bool, str]:
    r = runner(["git", "add", "-A"], cwd=worktree)
    if r.returncode != 0:
        return False, r.stderr
    r = runner(["git", "commit", "-q", "-m", f"feat({unit_id}): implement unit via spec-router agent"], cwd=worktree)
    if r.returncode != 0:
        return False, r.stderr
    r = runner(["git", "merge", "--no-ff", "-m", f"merge unit/{unit_id}", branch], cwd=repo)
    if r.returncode != 0:
        runner(["git", "merge", "--abort"], cwd=repo)
        return False, f"merge conflict: {r.stderr.strip() or r.stdout.strip()}"
    return True, "merged"
