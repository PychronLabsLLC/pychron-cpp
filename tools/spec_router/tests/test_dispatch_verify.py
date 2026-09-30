import json
import subprocess
from pathlib import Path

from spec_router import dispatch, verify
from spec_router.judge import Answers, ChoiceAnswer, FakeJudge
from spec_router.plan import Sizing, UnitBrief
from spec_router.spec import Section
from spec_router.units import Unit

UNIT = Unit(id="core", wave=1, layer="core", goal="core lib", targets=["libs/core"])
BRIEF = UnitBrief(UNIT, [Section("4.1", 3, "Errors", "Result<T>\n", ["Core types", "Errors"])], Sizing(1.0, 0.9, "haiku", 30, False))


def test_prompt_contains_goal_sections_and_conventions():
    p = dispatch.build_prompt(BRIEF, spec_path="docs/spec.md", completed_units=[])
    assert "core lib" in p and "[4.1] Core types / Errors" in p and "Result<T>" in p
    assert "Do not commit" in p and "you are first" in p and "30 tool turns" in p


def test_claude_command_shape():
    cfg = dispatch.DispatchConfig(repo=Path("/r"), claude_bin="/bin/claude")
    cmd = dispatch.claude_command(cfg, BRIEF)
    assert cmd[:2] == ["/bin/claude", "-p"]
    assert "--json-schema" in cmd and "--model" in cmd and cmd[cmd.index("--model") + 1] == "haiku"
    assert cmd[cmd.index("--max-turns") + 1] == "30"
    json.loads(cmd[cmd.index("--json-schema") + 1])


def test_parse_agent_output_variants():
    rep = {"summary": "x", "files_changed": [], "commands_run": [], "tests_run": True, "tests_passed": True, "blockers": []}
    r, env = dispatch.parse_agent_output(json.dumps({"structured_output": rep, "total_cost_usd": 0.5}))
    assert r == rep and env["total_cost_usd"] == 0.5
    r, _ = dispatch.parse_agent_output(json.dumps({"result": json.dumps(rep)}))
    assert r == rep
    r, env = dispatch.parse_agent_output("not json")
    assert r is None and "raw" in env


def test_run_unit_dry_run_creates_worktree_command_and_feeds_prompt(tmp_path):
    seen = []
    runner = dispatch.dry_runner_factory(seen.append)
    cfg = dispatch.DispatchConfig(repo=tmp_path, claude_bin="claude")
    res = dispatch.run_unit(cfg, BRIEF, "PROMPT", runner=runner)
    assert res.unit_id == "core" and res.branch == "unit/core"
    assert any("git worktree add" in s and "unit/core" in s and "-B" not in s for s in seen)
    assert any("stdin: 6 chars" in s for s in seen)


def _cp(rc=0, out="", err=""):
    return subprocess.CompletedProcess([], rc, stdout=out, stderr=err)


def test_build_and_test_without_cmake(tmp_path):
    b = verify.build_and_test(tmp_path, runner=lambda *a, **k: _cp())
    assert not b.has_cmake and not b.green


def test_build_and_test_green(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text("")
    calls = []

    def runner(cmd, *, cwd, stdin=None, timeout=None):
        calls.append([Path(cmd[0]).name, *cmd[1:2]])
        return _cp(0, out="100% tests passed")

    b = verify.build_and_test(tmp_path, runner=runner)
    assert b.green and [c[0] for c in calls] == ["cmake", "cmake", "ctest"]


def test_build_fail_stops_before_ctest(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text("")
    calls = []

    def runner(cmd, *, cwd, stdin=None, timeout=None):
        calls.append(Path(cmd[0]).name)
        return _cp(1 if [Path(cmd[0]).name, *cmd[1:2]] == ["cmake", "--build"] else 0, err="boom")

    b = verify.build_and_test(tmp_path, runner=runner)
    assert b.configured and not b.built and b.tests_passed is None and "ctest" not in calls


def _judgment(outcome="complete", conf=0.9, scope=0.9, unfinished=0.05):
    return verify.ReportJudgment(outcome, conf, 0.9, scope, unfinished)


def _green():
    return verify.BuildResult(True, True, True, True)


def _result(rc=0, report=None):
    return dispatch.UnitResult("core", "unit/core", "/wt", rc, report)


def test_gate_policy():
    ok = verify.gate(_result(report={}), _green(), _judgment(), changed=True)
    assert ok.merge
    assert not verify.gate(_result(report={}), _green(), _judgment(), changed=False).merge
    assert not verify.gate(_result(rc=2, report={}), _green(), _judgment(), changed=True).merge
    red = verify.BuildResult(True, True, True, False)
    assert "ctest" in verify.gate(_result(report={}), red, _judgment(), changed=True).reason
    assert not verify.gate(_result(report={}), _green(), None, changed=True).merge
    assert not verify.gate(_result(report={}), _green(), _judgment("partial"), changed=True).merge
    # uncertain complete (caveats) with no unfinished work is mergeable
    assert verify.gate(_result(report={}), _green(), _judgment(conf=0.3), changed=True).merge
    assert not verify.gate(_result(report={}), _green(), _judgment(conf=0.3, unfinished=0.7), changed=True).merge
    assert not verify.gate(_result(report={}), _green(), _judgment("blocked", conf=0.4), changed=True).merge
    assert not verify.gate(_result(report={}), _green(), _judgment("off_track", conf=0.4), changed=True).merge
    # low-confidence partial with no unfinished work: allowed (tests are the ground truth)
    assert verify.gate(_result(report={}), _green(), _judgment("partial", conf=0.3), changed=True).merge
    assert not verify.gate(_result(report={}), _green(), _judgment(scope=0.2), changed=True).merge


def test_judge_report_builds_state_and_reads_answers():
    def fn(state, questions):
        assert state["unit"]["id"] == "core" and "report" in state
        assert set(questions) == {"outcome", "evidence", "in_scope", "unfinished"}
        a = Answers()
        a.choices["outcome"] = ChoiceAnswer("partial", 0.8, {"partial": 0.8})
        a.nouls = {"evidence": 0.3, "in_scope": 0.95}
        return a

    j = verify.judge_report(BRIEF, {"summary": "did half"}, FakeJudge(fn))
    assert j.outcome == "partial" and j.evidence == 0.3 and j.in_scope == 0.95


def test_run_unit_surfaces_cli_error_envelope(tmp_path):
    env = {"is_error": True, "result": "Not logged in · Please run /login", "total_cost_usd": 0}

    def runner(cmd, *, cwd, stdin=None, timeout=None):
        return subprocess.CompletedProcess(cmd, 0 if cmd[0] == "git" else 1, stdout=json.dumps(env) if cmd[0] != "git" else "", stderr="")

    cfg = dispatch.DispatchConfig(repo=tmp_path)
    res = dispatch.run_unit(cfg, BRIEF, "P", runner=runner)
    assert res.exit_code == 1 and res.report is None
    assert "Not logged in" in res.raw_stderr


def test_subprocess_runner_reports_missing_tool_instead_of_raising(tmp_path):
    r = dispatch.subprocess_runner(["definitely-not-a-real-tool-xyz"], cwd=tmp_path)
    assert r.returncode == 127 and "not found" in r.stderr


def test_resolve_tool_prefers_interpreter_sibling(monkeypatch, tmp_path):
    import sys
    fake = tmp_path / "bin"; fake.mkdir(); (fake / "cmake").write_text("")
    monkeypatch.setattr(sys, "executable", str(fake / "python"))
    assert dispatch.resolve_tool("cmake") == str(fake / "cmake")
    assert dispatch.resolve_tool("no-such-tool-abc") == "no-such-tool-abc"


def test_ensure_worktree_never_resets_existing_branch(tmp_path):
    seen = []

    def runner(cmd, *, cwd, stdin=None, timeout=None):
        seen.append(cmd)
        if cmd[:3] == ["git", "rev-parse", "--verify"]:
            return _cp(0)  # branch exists
        return _cp(0)

    cfg = dispatch.DispatchConfig(repo=tmp_path)
    dispatch.ensure_worktree(cfg, "core", runner=runner)
    add = next(c for c in seen if c[:3] == ["git", "worktree", "add"])
    assert "-B" not in add and "-b" not in add and add[-1] == "unit/core"


def test_ensure_worktree_creates_branch_when_missing(tmp_path):
    def runner(cmd, *, cwd, stdin=None, timeout=None):
        if cmd[:3] == ["git", "rev-parse", "--verify"]:
            return _cp(1)
        return _cp(0)

    seen = []
    def rec(cmd, **kw):
        seen.append(cmd); return runner(cmd, **kw)
    dispatch.ensure_worktree(dispatch.DispatchConfig(repo=tmp_path), "core", runner=rec)
    add = next(c for c in seen if c[:3] == ["git", "worktree", "add"])
    assert "-b" in add and "-B" not in add


def test_branch_merged_requires_router_merge_commit(tmp_path):
    def runner(cmd, *, cwd, stdin=None, timeout=None):
        if cmd[:2] == ["git", "log"]:
            grep = next(a for a in cmd if a.startswith("--grep="))
            return _cp(0, out="merge unit/core\n" if grep.endswith("unit/core") else "")
        return _cp(0)

    assert dispatch.branch_merged(tmp_path, "core", runner=runner)
    # a fresh branch that is merely an ancestor of main is NOT merged
    assert not dispatch.branch_merged(tmp_path, "transport", runner=runner)
