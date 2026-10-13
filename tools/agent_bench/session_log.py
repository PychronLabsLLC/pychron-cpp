"""What one Claude Code session did, read from its log.

A session is a JSON-lines file under ~/.claude/projects/<project>/<id>.jsonl,
with the agents it started under <id>/subagents/. Only counts and names are
taken from it: no prompt, no file content, no command text is kept.
"""

import json
import re
from datetime import datetime
from pathlib import Path

TOKEN_KINDS = {
    "input": "input_tokens",
    "output": "output_tokens",
    "cache_read": "cache_read_input_tokens",
    "cache_write": "cache_creation_input_tokens",
}
CPP_RE = re.compile(r"\.(cpp|hpp|h|cc)$")
TEST_PATH_RE = re.compile(r"(^|/)tests/")
SOURCE_PATH_RE = re.compile(r"(^|/)(libs|apps)/")
CONVENTIONAL_RE = re.compile(r"^(feat|fix|perf|docs|test|refactor|chore|ci|build)(\([a-z0-9_-]+\))?!?: \S")
COMMIT_SUBJECT_RE = re.compile(r"""git\b[^|;&]*\bcommit\b[^|;&]*?-m\s+(?:"([^"\n]+)|'([^'\n]+))""")

# What a Bash command was for, by what it runs. The first that matches names it.
BASH_KINDS = [
    ("quality_check", re.compile(r"tools/quality_check\.py")),
    ("compiler_sweep", re.compile(r"tools/compiler_sweep\.py")),
    ("build", re.compile(r"\bcmake\s+--build\b")),
    ("configure", re.compile(r"\bcmake\s+--preset\b")),
    ("ctest", re.compile(r"\bctest\b")),
    ("test_binary", re.compile(r"pychron_\w+_tests?\b")),
    ("git_push", re.compile(r"\bgit\b[^|;&]*\bpush\b")),
    ("git_commit", re.compile(r"\bgit\b[^|;&]*\bcommit\b")),
    ("git_merge", re.compile(r"\bgit\b[^|;&]*\b(merge|fetch \. )")),
    ("gh", re.compile(r"\bgh\s")),
]


def bash_kinds(command: str) -> list[str]:
    """Every kind a command is, in the order of BASH_KINDS; one command often builds and tests."""
    return [name for name, pattern in BASH_KINDS if pattern.search(command)]


def _records(path: Path):
    with path.open(errors="replace") as lines:
        for line in lines:
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(record, dict):
                yield record


def _time(text):
    try:
        return datetime.fromisoformat(str(text).replace("Z", "+00:00"))
    except ValueError:
        return None


class _Counts:
    def __init__(self):
        self.tokens = dict.fromkeys(TOKEN_KINDS, 0)
        self.turns = 0
        self.models: dict[str, int] = {}
        self.tools: dict[str, int] = {}
        self.bash: dict[str, int] = {}
        self.first = None
        self.last = None
        self.seen_messages: set[str] = set()
        self.events: list[tuple[str, str]] = []  # (kind, path or ""), in order
        self.commit_subjects: list[str] = []
        self.no_verify = False

    def read(self, path: Path, *, follow_events: bool):
        for record in _records(path):
            when = _time(record.get("timestamp"))
            if when:
                self.first = when if self.first is None or when < self.first else self.first
                self.last = when if self.last is None or when > self.last else self.last
            message = record.get("message")
            if record.get("type") != "assistant" or not isinstance(message, dict):
                continue
            # One reply is written as a line per block, each with the reply's whole usage.
            reply = message.get("id")
            if reply not in self.seen_messages:
                self.seen_messages.add(reply)
                self.turns += 1
                usage = message.get("usage") or {}
                for kind, key in TOKEN_KINDS.items():
                    self.tokens[kind] += int(usage.get(key) or 0)
                model = message.get("model") or "unknown"
                self.models[model] = self.models.get(model, 0) + 1
            for block in message.get("content") or []:
                if isinstance(block, dict) and block.get("type") == "tool_use":
                    self._tool(block, follow_events)

    def _tool(self, block: dict, follow_events: bool):
        name = block.get("name") or "unknown"
        self.tools[name] = self.tools.get(name, 0) + 1
        given = block.get("input") or {}
        if name == "Bash":
            command = str(given.get("command") or "")
            for kind in bash_kinds(command):
                self.bash[kind] = self.bash.get(kind, 0) + 1
                if follow_events:
                    self.events.append((kind, ""))
            if "--no-verify" in command:
                self.no_verify = True
            for double, single in COMMIT_SUBJECT_RE.findall(command):
                self.commit_subjects.append(double or single)
        elif name in ("Edit", "Write", "NotebookEdit") and follow_events:
            self.events.append(("edit", str(given.get("file_path") or "")))


def _rules(counts: _Counts) -> dict:
    """The rules of AGENTS.md a log can show kept or not. None: the session gave no occasion."""
    events = counts.events
    cpp_edits = [i for i, (kind, path) in enumerate(events) if kind == "edit" and CPP_RE.search(path)]
    checks = [i for i, (kind, _) in enumerate(events) if kind == "quality_check"]
    quality_after_last_cpp_edit = None
    if cpp_edits:
        quality_after_last_cpp_edit = any(i > cpp_edits[-1] for i in checks)

    test_edits = [i for i, (kind, path) in enumerate(events) if kind == "edit" and TEST_PATH_RE.search(path)]
    source_edits = [
        i for i, (kind, path) in enumerate(events)
        if kind == "edit" and SOURCE_PATH_RE.search(path) and not TEST_PATH_RE.search(path) and CPP_RE.search(path)
    ]
    test_before_source = None
    if source_edits:
        test_before_source = bool(test_edits) and test_edits[0] < source_edits[0]

    conventional = None
    if counts.commit_subjects:
        conventional = all(CONVENTIONAL_RE.match(subject) for subject in counts.commit_subjects)

    return {
        "quality_check_after_last_cpp_edit": quality_after_last_cpp_edit,
        "test_edited_before_source": test_before_source,
        "commit_subjects_conventional": conventional,
        "pushed": counts.bash.get("git_push", 0) > 0,
        "merged": counts.bash.get("git_merge", 0) > 0,
        "no_verify": counts.no_verify,
    }


def summarize(session: Path) -> dict:
    """One line of the ledger for the session whose log is `session` (<id>.jsonl)."""
    main = _Counts()
    main.read(session, follow_events=True)
    agents = _Counts()
    agent_logs = sorted((session.parent / session.stem / "subagents").glob("*.jsonl"))
    for log in agent_logs:
        agents.read(log, follow_events=False)

    first = min((t for t in (main.first, agents.first) if t), default=None)
    last = max((t for t in (main.last, agents.last) if t), default=None)
    tokens = {kind: main.tokens[kind] + agents.tokens[kind] for kind in TOKEN_KINDS}
    return {
        "session": session.stem,
        "project": session.parent.name,
        "started": first.isoformat() if first else None,
        "ended": last.isoformat() if last else None,
        "wall_s": round((last - first).total_seconds()) if first and last else None,
        "turns": main.turns,
        "models": main.models,
        "tokens": tokens,
        "subagents": {"count": len(agent_logs), "turns": agents.turns, "tokens": agents.tokens},
        "tools": dict(sorted(main.tools.items(), key=lambda item: -item[1])),
        "bash": main.bash,
        "commits": len(main.commit_subjects),
        "rules": _rules(main),
    }


def project_slug(directory: Path) -> str:
    """The name Claude Code gives a directory's sessions under ~/.claude/projects."""
    return re.sub(r"[^A-Za-z0-9]", "-", str(directory))


def find_session(session_id: str, projects: Path) -> Path | None:
    for path in projects.glob(f"*/{session_id}.jsonl"):
        return path
    return None
