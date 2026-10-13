#!/usr/bin/env python3
"""The ledger: one line for each Claude Code session on this repository.

Read from the sessions' own logs (tools/agent_bench/session_log.py): time,
turns, tokens by kind, tool calls by name, what the shell was used for, and
which rules of AGENTS.md the log shows kept. It shows where the cost of the
work goes and how that moves from week to week; it cannot say why, because
the tasks differ. The fixed set (run.py) is for that.

    python3 tools/agent_bench/ledger.py --summary              # a table, every session
    python3 tools/agent_bench/ledger.py --out ledger.jsonl     # a line a session, to keep
    python3 tools/agent_bench/ledger.py --session <id or path>

--out adds the sessions the file does not have yet and leaves the others as
they are: run it when the sessions it should hold have ended.

Sessions are found under ~/.claude/projects, in the directories of this
checkout and of its worktrees. Nothing of what was said or written in a
session is copied: counts and names only.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import session_log  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def main_checkout(root: Path) -> Path:
    """The checkout a worktree belongs to: a worktree's sessions are filed under their own name."""
    marker = "/.claude/worktrees/"
    text = str(root)
    return Path(text.split(marker)[0]) if marker in text else root


def session_files(projects: Path, root: Path, wanted: list[str]) -> list[Path]:
    if wanted:
        found = []
        for name in wanted:
            path = Path(name)
            if not path.is_file():
                path = session_log.find_session(name, projects)
            if path is None:
                raise SystemExit(f"ledger: no session '{name}'")
            found.append(path)
        return found
    slug = session_log.project_slug(main_checkout(root))
    return [path for directory in projects.glob(f"{slug}*") for path in directory.glob("*.jsonl")]


def total_tokens(line: dict) -> int:
    return sum(line["tokens"].values())


def summary(lines: list[dict]) -> str:
    out = [f"{'started':16} {'min':>5} {'turns':>5} {'Mtok':>7} {'out k':>6} {'agents':>6} "
           f"{'build':>5} {'ctest':>5} {'bin':>4} {'qc':>3}  rules not kept"]
    for line in lines:
        not_kept = [name for name, kept in line["rules"].items()
                    if (kept is False and name not in ("pushed", "merged", "no_verify"))
                    or (kept is True and name == "no_verify")]
        bash = line["bash"]
        out.append(
            f"{(line['started'] or '')[:16]:16} {(line['wall_s'] or 0) // 60:>5} {line['turns']:>5} "
            f"{total_tokens(line) / 1e6:>7.1f} {line['tokens']['output'] / 1e3:>6.0f} {line['subagents']['count']:>6} "
            f"{bash.get('build', 0):>5} {bash.get('ctest', 0):>5} {bash.get('test_binary', 0):>4} "
            f"{bash.get('quality_check', 0):>3}  {', '.join(not_kept)}")
    tools: dict[str, int] = {}
    for line in lines:
        for name, count in line["tools"].items():
            tools[name] = tools.get(name, 0) + count
    calls = sum(tools.values()) or 1
    top = sorted(tools.items(), key=lambda item: -item[1])[:8]
    out.append("")
    out.append(f"{len(lines)} sessions, {sum(line['turns'] for line in lines)} turns, "
               f"{sum(total_tokens(line) for line in lines) / 1e6:.0f} M tokens "
               f"({sum(line['tokens']['cache_read'] for line in lines) / 1e6:.0f} M of them read from cache), "
               f"{sum(line['tokens']['output'] for line in lines) / 1e6:.1f} M written")
    out.append("tool calls: " + ", ".join(f"{name} {100 * count // calls}%" for name, count in top))
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--projects", default=str(Path.home() / ".claude" / "projects"),
                        help="where Claude Code keeps session logs")
    parser.add_argument("--session", action="append", default=[], help="a session id or a log file; may repeat")
    parser.add_argument("--out", help="append the lines to this file instead of printing them")
    parser.add_argument("--summary", action="store_true", help="a table instead of JSON lines")
    args = parser.parse_args()

    files = session_files(Path(args.projects), ROOT, args.session)
    lines = [session_log.summarize(path) for path in files]
    lines = sorted((line for line in lines if line["turns"] > 0), key=lambda line: line["started"] or "")
    if args.summary:
        print(summary(lines))
        return 0
    text = "".join(json.dumps(line, sort_keys=True) + "\n" for line in lines)
    if args.out:
        known = set()
        out = Path(args.out)
        if out.is_file():
            known = {json.loads(row)["session"] for row in out.read_text().splitlines() if row.strip()}
        fresh = [line for line in lines if line["session"] not in known]
        with out.open("a") as handle:
            handle.writelines(json.dumps(line, sort_keys=True) + "\n" for line in fresh)
        print(f"ledger: {len(fresh)} new session(s) written to {out}, {len(lines) - len(fresh)} already there",
              file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
