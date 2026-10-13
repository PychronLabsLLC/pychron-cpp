#!/usr/bin/env python3
"""The fixed set: tasks this repository has already done, done again by an agent.

Each task of tasks.toml names a commit. A run starts a worktree at the
commit's parent, puts the instruction files under test over it (the variant),
gives the task to a headless Claude Code session there, then copies in the
tests the real commit added and runs the task's checks. One JSON file a run
goes under build/agent_bench/<label>/.

    python3 tools/agent_bench/run.py --list
    python3 tools/agent_bench/run.py smoke --label baseline
    python3 tools/agent_bench/run.py full --label split --variant HEAD --repeats 3
    python3 tools/agent_bench/run.py sim-transport-bounded --label try --model sonnet

To check a task itself, without an agent and without usage:

    python3 tools/agent_bench/run.py smoke --label sound --agent reference   # every task passes
    python3 tools/agent_bench/run.py smoke --label empty --agent none        # every task fails

A run costs usage and takes as long as the task and a build do. It starts an
agent that edits files and runs commands in its own worktree with nobody
watching: it is never started by a test, and it does not push.

The variant is a commit (default HEAD: the guide as it is now) or a
directory; every AGENTS.md and CLAUDE.md in it is laid over the start
commit, so that what differs between two labels is the guide and nothing
else. report.py compares labels.
"""

import argparse
import json
import shlex
import shutil
import subprocess
import sys
import time
import tomllib
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import session_log  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GUIDE_NAMES = ("AGENTS.md", "CLAUDE.md")
TAIL = 4000  # characters of a check's output kept in the result


def git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=check)


def load_tasks(path: Path) -> dict[str, dict]:
    with path.open("rb") as handle:
        data = tomllib.load(handle)
    tasks = {}
    for task in data.get("task", []):
        for key in ("id", "reference", "prompt"):
            if not task.get(key):
                raise SystemExit(f"{path}: a task without '{key}'")
        if task["id"] in tasks:
            raise SystemExit(f"{path}: task '{task['id']}' twice")
        task.setdefault("start", task["reference"] + "^")
        task.setdefault("hidden", [])
        task.setdefault("check", [])
        task.setdefault("smoke", False)
        for check in task["check"]:
            if not check.get("name") or not isinstance(check.get("argv"), list):
                raise SystemExit(f"{path}: task '{task['id']}': a check needs a name and an argv list")
            check.setdefault("required", True)
            check.setdefault("timeout_s", 3600)
        tasks[task["id"]] = task
    return tasks


def chosen(tasks: dict[str, dict], names: list[str]) -> list[dict]:
    picked = []
    for name in names:
        if name == "full":
            picked += list(tasks.values())
        elif name == "smoke":
            picked += [task for task in tasks.values() if task["smoke"]]
        elif name in tasks:
            picked.append(tasks[name])
        else:
            raise SystemExit(f"run: no task '{name}' (--list shows them)")
    seen = set()
    return [task for task in picked if not (task["id"] in seen or seen.add(task["id"]))]


def guide_files_of_commit(repo: Path, rev: str) -> list[str]:
    listed = git(repo, "ls-tree", "-r", "--name-only", rev).stdout.splitlines()
    return [path for path in listed if Path(path).name in GUIDE_NAMES]


def lay_variant(repo: Path, work: Path, variant: str) -> list[str]:
    """Puts the variant's guide files over the worktree and commits them: the agent's diff starts after."""
    laid = []
    directory = Path(variant)
    if directory.is_dir():
        for source in sorted(directory.rglob("*")):
            if source.is_file() and source.name in GUIDE_NAMES:
                relative = source.relative_to(directory)
                (work / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, work / relative)
                laid.append(relative.as_posix())
    else:
        for path in guide_files_of_commit(repo, variant):
            (work / path).parent.mkdir(parents=True, exist_ok=True)
            (work / path).write_text(git(repo, "show", f"{variant}:{path}").stdout)
            laid.append(path)
    git(work, "add", "-A")
    git(work, "-c", "user.name=agent-bench", "-c", "user.email=agent-bench@localhost", "commit", "-q",
        "--allow-empty", "--no-verify", "-m", "bench: the guide under test")
    return laid


def claude_command(claude: list[str], model: str | None, permission_mode: str, tools: list[str],
                   extra: list[str]) -> list[str]:
    command = [*claude, "-p", "--output-format", "json", "--permission-mode", permission_mode]
    if tools:
        command += ["--allowedTools", *tools]
    if model:
        command += ["--model", model]
    return command + extra


def run_agent(command: list[str], prompt: str, work: Path, timeout_s: float) -> dict:
    began = time.monotonic()
    try:
        done = subprocess.run(command, input=prompt, cwd=work, capture_output=True, text=True,
                              timeout=timeout_s, check=False)
        status, out, err = done.returncode, done.stdout, done.stderr
    except subprocess.TimeoutExpired:
        status, out, err = -1, "", "timed out"
    except OSError as missing:
        status, out, err = 127, "", str(missing)
    seconds = round(time.monotonic() - began, 1)
    envelope = {}
    try:
        parsed = json.loads(out)
        if isinstance(parsed, dict):
            envelope = parsed
    except json.JSONDecodeError:
        pass
    kept = {key: envelope.get(key) for key in
            ("session_id", "is_error", "num_turns", "duration_ms", "total_cost_usd", "usage", "subtype")}
    return {"exit": status, "seconds": seconds, "stderr_tail": err[-TAIL:], **kept}


def diff_stat(work: Path, since: str) -> dict:
    git(work, "add", "-A")
    files = insertions = deletions = 0
    for line in git(work, "diff", "--cached", "--numstat", since).stdout.splitlines():
        added, removed, _ = line.split("\t", 2)
        files += 1
        insertions += int(added) if added.isdigit() else 0
        deletions += int(removed) if removed.isdigit() else 0
    return {"files": files, "insertions": insertions, "deletions": deletions}


def run_check(check: dict, work: Path, values: dict) -> dict:
    argv = [part.format(**values) for part in check["argv"]]
    began = time.monotonic()
    printed = ""
    try:
        done = subprocess.run(argv, cwd=work, capture_output=True, text=True, timeout=check["timeout_s"],
                              check=False)
        status, printed, out = done.returncode, done.stdout, done.stdout + done.stderr
    except subprocess.TimeoutExpired:
        status, out = -1, "timed out"
    except OSError as missing:
        status, out = 127, str(missing)
    result = {"name": check["name"], "required": check["required"], "exit": status,
              "seconds": round(time.monotonic() - began, 1), "tail": out[-TAIL:]}
    if check.get("findings_json"):
        # A check that prints {"findings": [...]}: tools/quality_check.py --json.
        try:
            result["findings"] = len(json.loads(printed).get("findings", []))
        except (json.JSONDecodeError, AttributeError):
            result["findings"] = None
    return result


def one_run(repo: Path, task: dict, label: str, number: int, options) -> dict:
    out_dir = Path(options.results) / label
    out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(options.work) / f"{label}-{task['id']}-{number}"
    if work.exists():
        git(repo, "worktree", "remove", "--force", str(work), check=False)
        shutil.rmtree(work, ignore_errors=True)
    work.parent.mkdir(parents=True, exist_ok=True)
    start = git(repo, "rev-parse", task["start"]).stdout.strip()
    git(repo, "worktree", "add", "--detach", str(work), start)

    result = {"task": task["id"], "kind": task.get("kind", ""), "label": label, "run": number,
              "variant": options.variant, "model": options.model, "agent_kind": options.agent, "start": start,
              "reference": git(repo, "rev-parse", task["reference"]).stdout.strip(),
              "when": datetime.now(timezone.utc).isoformat(timespec="seconds")}
    try:
        result["guide_files"] = len(lay_variant(repo, work, options.variant))
        base = git(work, "rev-parse", "HEAD").stdout.strip()
        if options.agent == "claude":
            command = claude_command(shlex.split(options.claude), options.model, options.permission_mode,
                                     options.allowed_tools, options.claude_arg)
            result["agent"] = run_agent(command, task["prompt"], work, options.agent_timeout_s)
        else:
            # No agent: the real commit's own change, or none. A task is sound
            # when the first passes its checks and the second does not.
            if options.agent == "reference":
                changed = git(repo, "diff", "--name-only", start, result["reference"]).stdout.split()
                git(work, "checkout", result["reference"], "--", *changed)
            result["agent"] = {"exit": 0, "seconds": 0.0, "stand_in": options.agent}
        result["diff"] = diff_stat(work, base)

        session = result["agent"].get("session_id")
        log = session_log.find_session(session, Path(options.projects)) if session else None
        result["session"] = session_log.summarize(log) if log else None

        # Checks marked before_hidden see the agent's work alone (the static
        # analysis of its lines); the rest run with the real commit's tests in.
        values = {"start": start, "base": base, "python": sys.executable}
        ordered = ([c for c in task["check"] if c.get("before_hidden")]
                   + [None]
                   + [c for c in task["check"] if not c.get("before_hidden")])
        checks = []
        for check in ordered:
            if check is None:
                if task["hidden"]:
                    git(work, "add", "-A")
                    git(work, "checkout", result["reference"], "--", *task["hidden"])
                continue
            checks.append(run_check(check, work, values))
            if checks[-1]["required"] and checks[-1]["exit"] != 0:
                break  # what follows needs what failed
        result["checks"] = checks
        ran_all = len(checks) == len(task["check"])
        result["passed"] = ran_all and all(c["exit"] == 0 for c in checks if c["required"])
    finally:
        if not options.keep:
            git(repo, "worktree", "remove", "--force", str(work), check=False)
            shutil.rmtree(work, ignore_errors=True)
            git(repo, "worktree", "prune", check=False)
    (out_dir / f"{task['id']}-{number}.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tasks", nargs="*", help="task ids, 'smoke' or 'full'")
    parser.add_argument("--list", action="store_true", help="show the tasks and stop")
    parser.add_argument("--tasks-file", default=str(HERE / "tasks.toml"))
    parser.add_argument("--repo", default=str(ROOT), help="the repository the tasks are commits of")
    parser.add_argument("--label", help="names this set of runs; results go under <results>/<label>")
    parser.add_argument("--variant", default="HEAD",
                        help="a commit or a directory whose AGENTS.md and CLAUDE.md files are the guide under test")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--model", help="passed to claude --model; left out, the session's default")
    parser.add_argument("--agent", choices=["claude", "reference", "none"], default="claude",
                        help="'reference' applies the real commit and 'none' nothing, in place of an agent: "
                             "a task is sound when the first passes and the second fails; neither costs usage")
    parser.add_argument("--claude", default="claude", help="the program to run, with any leading arguments")
    parser.add_argument("--claude-arg", action="append", default=[], help="an extra argument for it; may repeat")
    parser.add_argument("--permission-mode", default="acceptEdits")
    parser.add_argument("--allowed-tools", nargs="*", default=["Bash", "Read", "Edit", "Write", "Glob", "Grep"])
    parser.add_argument("--agent-timeout-s", type=float, default=2 * 60 * 60)
    parser.add_argument("--results", default=str(ROOT / "build" / "agent_bench"))
    parser.add_argument("--work", default=str(ROOT / "build" / "agent_bench" / "work"))
    parser.add_argument("--projects", default=str(Path.home() / ".claude" / "projects"))
    parser.add_argument("--keep", action="store_true", help="leave each run's worktree in place")
    options = parser.parse_args()

    tasks = load_tasks(Path(options.tasks_file))
    if options.list:
        for task in tasks.values():
            print(f"{task['id']:28} {'smoke' if task['smoke'] else '     '} {task['reference'][:9]} "
                  f"{task.get('kind', '')}")
        return 0
    if not options.tasks or not options.label:
        parser.error("name the tasks to run and give the runs a --label")

    repo = Path(options.repo)
    failed = 0
    for task in chosen(tasks, options.tasks):
        for number in range(1, options.repeats + 1):
            result = one_run(repo, task, options.label, number, options)
            agent = result.get("agent", {})
            print(f"{task['id']} run {number}: {'pass' if result.get('passed') else 'FAIL'}, "
                  f"{agent.get('num_turns')} turns, ${agent.get('total_cost_usd')}, {agent.get('seconds')} s",
                  flush=True)
            failed += 0 if result.get("passed") else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
