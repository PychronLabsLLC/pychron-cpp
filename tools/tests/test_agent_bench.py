"""tools/agent_bench: the session log reader, the ledger, the runner and the report.

No agent is started and nothing outside a temporary directory is touched: the
runner is given a small repository made here and a stand-in for `claude`.
"""

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

BENCH = Path(__file__).resolve().parents[1] / "agent_bench"
sys.path.insert(0, str(BENCH))

import report  # noqa: E402
import session_log  # noqa: E402


def assistant(message_id, *blocks, usage=None, when="2026-10-12T10:00:00Z", model="claude-test"):
    """A reply as the log holds it: one line a block, each with the reply's whole usage."""
    usage = usage or {"input_tokens": 10, "output_tokens": 5, "cache_read_input_tokens": 100,
                      "cache_creation_input_tokens": 20}
    return [json.dumps({"type": "assistant", "timestamp": when,
                        "message": {"id": message_id, "model": model, "usage": usage, "content": [block]}})
            for block in blocks]


def bash(command):
    return {"type": "tool_use", "name": "Bash", "input": {"command": command}}


def edit(path):
    return {"type": "tool_use", "name": "Edit", "input": {"file_path": path}}


class SessionLog(unittest.TestCase):
    def write(self, directory, lines, name="abc"):
        path = Path(directory) / f"{name}.jsonl"
        path.write_text("\n".join(lines) + "\n")
        return path

    def test_a_reply_written_as_several_lines_is_counted_once(self):
        with tempfile.TemporaryDirectory() as d:
            log = self.write(d, assistant("m1", bash("ls"), bash("pwd"), {"type": "text", "text": "x"})
                             + assistant("m2", edit("/r/libs/a/src/a.cpp"), when="2026-10-12T10:05:00Z")
                             + ["not json", json.dumps({"type": "user", "timestamp": "2026-10-12T10:06:00Z"})])
            line = session_log.summarize(log)
        self.assertEqual(line["turns"], 2)
        self.assertEqual(line["tokens"], {"input": 20, "output": 10, "cache_read": 200, "cache_write": 40})
        self.assertEqual(line["tools"], {"Bash": 2, "Edit": 1})
        self.assertEqual(line["wall_s"], 360)
        self.assertEqual(line["models"], {"claude-test": 2})

    def test_what_the_shell_was_used_for(self):
        self.assertEqual(session_log.bash_kinds("cmake --build --preset dev && ctest --preset dev"),
                         ["build", "ctest"])
        self.assertEqual(session_log.bash_kinds("build/dev/tests/core/pychron_core_tests --gtest_filter=X.*"),
                         ["test_binary"])
        self.assertEqual(session_log.bash_kinds("python3 tools/quality_check.py --json"), ["quality_check"])
        self.assertEqual(session_log.bash_kinds("git -C x push origin develop"), ["git_push"])
        self.assertEqual(session_log.bash_kinds("ls -la"), [])

    def test_rules_kept_and_not(self):
        with tempfile.TemporaryDirectory() as d:
            kept = self.write(d, assistant("m1", edit("/r/tests/a/test_a.cpp"), edit("/r/libs/a/src/a.cpp"),
                                           bash("python3 tools/quality_check.py"),
                                           bash('git commit -q -m "fix(a): the thing holds"')), "kept")
            broken = self.write(d, assistant("m1", edit("/r/libs/a/src/a.cpp"),
                                             bash("python3 tools/quality_check.py"),
                                             edit("/r/libs/a/src/a.cpp"),
                                             bash("git commit -m 'did stuff' --no-verify"),
                                             bash("git push origin develop")), "broken")
            none = self.write(d, assistant("m1", bash("ls")), "none")
            good, bad, idle = (session_log.summarize(p)["rules"] for p in (kept, broken, none))
        self.assertEqual(good, {"quality_check_after_last_cpp_edit": True, "test_edited_before_source": True,
                                "commit_subjects_conventional": True, "pushed": False, "merged": False,
                                "no_verify": False})
        self.assertFalse(bad["quality_check_after_last_cpp_edit"])
        self.assertFalse(bad["test_edited_before_source"])
        self.assertFalse(bad["commit_subjects_conventional"])
        self.assertTrue(bad["pushed"])
        self.assertTrue(bad["no_verify"])
        # No occasion: nothing was edited or committed.
        self.assertIsNone(idle["quality_check_after_last_cpp_edit"])
        self.assertIsNone(idle["commit_subjects_conventional"])

    def test_the_agents_a_session_started_are_added_to_its_tokens(self):
        with tempfile.TemporaryDirectory() as d:
            log = self.write(d, assistant("m1", bash("ls")))
            agents = Path(d) / "abc" / "subagents"
            agents.mkdir(parents=True)
            (agents / "agent-1.jsonl").write_text("\n".join(assistant("s1", bash("cmake --build x"))) + "\n")
            line = session_log.summarize(log)
        self.assertEqual(line["subagents"]["count"], 1)
        self.assertEqual(line["tokens"]["output"], 10)
        self.assertEqual(line["turns"], 1)  # the session's own
        self.assertEqual(line["bash"], {})  # and its own commands

    def test_a_directorys_project_name(self):
        self.assertEqual(session_log.project_slug(Path("/Users/a/pychron-cpp/.claude/x")),
                         "-Users-a-pychron-cpp--claude-x")


class Ledger(unittest.TestCase):
    def test_out_adds_only_the_sessions_it_does_not_have(self):
        with tempfile.TemporaryDirectory() as d:
            projects = Path(d) / "projects" / "p"
            projects.mkdir(parents=True)
            (projects / "one.jsonl").write_text("\n".join(assistant("m1", bash("ls"))) + "\n")
            out = Path(d) / "ledger.jsonl"
            command = [sys.executable, str(BENCH / "ledger.py"), "--projects", str(projects.parent),
                       "--session", "one", "--out", str(out)]
            subprocess.run(command, check=True, capture_output=True)
            subprocess.run(command, check=True, capture_output=True)
            rows = [json.loads(row) for row in out.read_text().splitlines()]
        self.assertEqual([row["session"] for row in rows], ["one"])
        self.assertNotIn("command", json.dumps(rows))


STAND_IN = textwrap.dedent('''\
    import json, pathlib, sys
    prompt = sys.stdin.read()
    assert "--output-format" in sys.argv and "-p" in sys.argv
    # The agent's work: the fix, and a note that the guide under test was there to read.
    pathlib.Path("calc.py").write_text("def double(x):\\n    return x * 2\\n")
    pathlib.Path("saw_guide.txt").write_text(pathlib.Path("AGENTS.md").read_text() + prompt[:5])
    print(json.dumps({"session_id": "s-1", "num_turns": 3, "total_cost_usd": 0.25, "is_error": False}))
''')


@unittest.skipIf(os.name == "nt", "the runner starts agents on a developer's Mac or Linux machine, not on Windows")
class Runner(unittest.TestCase):
    """A repository of two commits: a function that is wrong, then right with its test."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.repo = root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        (self.repo / "calc.py").write_text("def double(x):\n    return x + 2\n")
        (self.repo / "AGENTS.md").write_text("old guide\n")
        self.commit("start")
        (self.repo / "calc.py").write_text("def double(x):\n    return 2 * x\n")
        (self.repo / "test_calc.py").write_text(
            "import unittest, calc\n\nclass T(unittest.TestCase):\n"
            "    def test_double(self):\n        self.assertEqual(calc.double(5), 10)\n")
        self.commit("reference")
        self.reference = self.git("rev-parse", "HEAD").stdout.strip()
        (self.repo / "AGENTS.md").write_text("new guide\n")
        self.commit("the guide moves on")

        self.agent = root / "stand_in.py"
        self.agent.write_text(STAND_IN)
        self.tasks = root / "tasks.toml"
        self.tasks.write_text(textwrap.dedent(f'''\
            [[task]]
            id = "double"
            smoke = true
            reference = "{self.reference}"
            prompt = "make double double"
            hidden = ["test_calc.py"]

            [[task.check]]
            name = "before"
            argv = ["{{python}}", "-c", "import pathlib, sys; sys.exit(pathlib.Path('test_calc.py').exists())"]
            before_hidden = true
            required = false

            [[task.check]]
            name = "tests"
            argv = ["{{python}}", "-m", "unittest", "test_calc"]
        '''))
        self.results = root / "results"

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, text=True)

    def commit(self, subject):
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@localhost", "commit", "-q", "-m", subject)

    def run_bench(self, label, *extra):
        done = subprocess.run(
            [sys.executable, str(BENCH / "run.py"), "smoke", "--label", label, "--repo", str(self.repo),
             "--tasks-file", str(self.tasks), "--results", str(self.results), "--work", str(self.results / "work"),
             "--projects", str(Path(self.tmp.name) / "no-projects"),
             "--claude", f"{sys.executable} {self.agent}", *extra],
            capture_output=True, text=True, check=False)
        result = json.loads((self.results / label / "double-1.json").read_text())
        return done, result

    def test_an_agent_that_fixes_it_passes_and_its_own_work_is_what_is_counted(self):
        done, result = self.run_bench("agent")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertTrue(result["passed"])
        # The first check ran before the real commit's test was copied in.
        self.assertEqual([c["exit"] for c in result["checks"]], [0, 0])
        self.assertEqual(result["agent"]["total_cost_usd"], 0.25)
        self.assertEqual(result["agent"]["num_turns"], 3)
        # calc.py and the note: not the guide laid under it, not the hidden test.
        self.assertEqual(result["diff"]["files"], 2)
        # The worktree is gone, and the repository is as it was.
        self.assertEqual(list((self.results / "work").iterdir()), [])
        self.assertEqual(self.git("status", "--porcelain").stdout, "")
        self.assertEqual(self.git("worktree", "list").stdout.count("\n"), 1)

    def test_the_guide_under_test_is_the_variants_not_the_start_commits(self):
        _, result = self.run_bench("head", "--keep")
        work = self.results / "work" / "head-double-1"
        self.assertEqual((work / "saw_guide.txt").read_text(), "new guide\nmake ")
        self.git("worktree", "remove", "--force", str(work))

        variant = Path(self.tmp.name) / "variant"
        variant.mkdir()
        (variant / "AGENTS.md").write_text("another guide\n")
        (variant / "notes.txt").write_text("not a guide file\n")
        _, result = self.run_bench("dir", "--variant", str(variant), "--keep")
        work = self.results / "work" / "dir-double-1"
        self.assertEqual((work / "saw_guide.txt").read_text(), "another guide\nmake ")
        self.assertFalse((work / "notes.txt").exists())
        self.assertEqual(result["guide_files"], 1)
        self.git("worktree", "remove", "--force", str(work))

    def test_a_task_is_sound_when_the_real_commit_passes_and_no_change_fails(self):
        done, result = self.run_bench("sound", "--agent", "reference")
        self.assertTrue(result["passed"])
        self.assertEqual(done.returncode, 0)
        done, result = self.run_bench("empty", "--agent", "none")
        self.assertFalse(result["passed"])
        self.assertEqual(done.returncode, 1)
        self.assertEqual(result["checks"][-1]["name"], "tests")

    def test_a_failed_required_check_stops_the_rest(self):
        text = self.tasks.read_text().replace("sys.exit(pathlib.Path('test_calc.py').exists())", "sys.exit(3)")
        text = text.replace("required = false", "required = true")
        self.tasks.write_text(text)
        _, result = self.run_bench("stopped")
        self.assertFalse(result["passed"])
        self.assertEqual([c["name"] for c in result["checks"]], ["before"])

    def test_an_agent_that_cannot_be_started_is_a_failed_run_not_a_crash(self):
        done = subprocess.run(
            [sys.executable, str(BENCH / "run.py"), "double", "--label", "missing", "--repo", str(self.repo),
             "--tasks-file", str(self.tasks), "--results", str(self.results), "--work", str(self.results / "work"),
             "--claude", "no-such-program-here"], capture_output=True, text=True, check=False)
        result = json.loads((self.results / "missing" / "double-1.json").read_text())
        self.assertEqual(result["agent"]["exit"], 127)
        self.assertFalse(result["passed"])
        self.assertEqual(done.returncode, 1)


def run_of(passed, cost):
    return {"task": "t", "passed": passed, "agent": {"total_cost_usd": cost, "num_turns": 5, "seconds": 60},
            "diff": {"insertions": 3, "deletions": 1}, "checks": [{"name": "static analysis", "findings": 0}]}


class Report(unittest.TestCase):
    def test_passing_less_often_is_worse_whatever_it_costs(self):
        baseline = [run_of(True, 1.0), run_of(True, 1.2), run_of(True, 1.1)]
        self.assertEqual(report.verdict(baseline, [run_of(True, 0.1), run_of(False, 0.1), run_of(True, 0.1)]),
                         "worse: passes less often")

    def test_cheaper_counts_only_past_the_baselines_own_range(self):
        baseline = [run_of(True, 1.0), run_of(True, 1.2), run_of(True, 1.1)]  # median 1.1, range 0.2
        self.assertEqual(report.verdict(baseline, [run_of(True, 0.95)] * 3), "same")
        self.assertTrue(report.verdict(baseline, [run_of(True, 0.8)] * 3).startswith("worked"))
        self.assertTrue(report.verdict(baseline, [run_of(True, 1.4)] * 3).startswith("worse"))

    def test_one_baseline_run_has_no_range_to_judge_cost_by(self):
        self.assertEqual(report.verdict([run_of(True, 1.0)], [run_of(True, 0.1)]),
                         "same (one baseline run: no range)")
        self.assertEqual(report.verdict([run_of(True, 1.0)], []), "no runs")

    def test_a_cell_is_the_median_and_the_range(self):
        self.assertEqual(report.cell([1.0, 3.0, 2.0]), "2 (1-3)")
        self.assertEqual(report.cell([2.0, 2.0]), "2")
        self.assertEqual(report.cell([None]), "-")


if __name__ == "__main__":
    unittest.main()
