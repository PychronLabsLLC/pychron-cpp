#!/usr/bin/env python3
"""Static analysis of the C++ a branch changes: cppcheck, then clang-tidy.

    python3 tools/quality_check.py                  # lines changed since origin/develop
    python3 tools/quality_check.py --json           # the same, for a program to read
    python3 tools/quality_check.py libs/core/src/number.cpp   # these files, every line
    python3 tools/quality_check.py --fix            # let clang-tidy apply its fix-its

Without paths it takes the first-party C++ files that differ from the merge
base with --base (committed, staged, unstaged and untracked) and reports only
findings on the lines that differ, so the result is about the change and not
about the file's history. A new file is checked whole.

clang-tidy needs the compile database a configure writes
(build/<preset>/compile_commands.json) and, for the UI and its tests, a build
(the moc files). A changed header is analysed through up to two sources that
include it. cppcheck reads the files alone, first-party headers only.

Exit status: 0 nothing found, 1 findings, 2 the check could not run.
Checks are chosen in .clang-tidy and cmake/cppcheck.supp. To silence one
finding, `// NOLINT(<check>): <why>` or `// cppcheck-suppress <id>` on the
line, with the reason. Setup: docs/dev_setup.md, "Static analysis".
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRST_PARTY = ("libs/", "apps/", "tests/")
SOURCE_SUFFIXES = (".cpp",)
HEADER_SUFFIXES = (".hpp", ".h")
PRESET_DIRS = ("dev-ui", "dev", "mac-debug", "mac-release")
BREW_LLVM = ("/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin")
HEADER_SOURCES = 2  # sources analysed for one changed header

FINDING_RE = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<column>\d+): "
    r"(?P<severity>warning|error|performance|portability|style): "
    r"(?P<message>.*) \[(?P<check>[^\]\s]+)\]$"
)
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


class CannotRun(Exception):
    """The check itself could not be made (exit status 2)."""


@dataclass(frozen=True, order=True)
class Finding:
    file: str
    line: int
    column: int
    severity: str
    tool: str
    check: str
    message: str

    def text(self) -> str:
        return f"{self.file}:{self.line}:{self.column}: {self.severity}: {self.message} [{self.check}]"


def git(*args: str) -> str:
    done = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    if done.returncode != 0:
        raise CannotRun(f"git {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout


def is_first_party(path: str) -> bool:
    return path.startswith(FIRST_PARTY) and path.endswith(SOURCE_SUFFIXES + HEADER_SUFFIXES)


def parse_changed_lines(diff: str) -> dict[str, list[tuple[int, int]]]:
    """Line ranges added or changed, by file, from `git diff -U0`."""
    changed: dict[str, list[tuple[int, int]]] = {}
    current = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            name = line[4:]
            current = None if name == "/dev/null" else name.removeprefix("b/")
            if current is not None:
                changed.setdefault(current, [])
            continue
        hunk = HUNK_RE.match(line)
        if hunk and current is not None:
            start = int(hunk.group(1))
            count = 1 if hunk.group(2) is None else int(hunk.group(2))
            if count > 0:  # count 0 is a pure deletion: no line of the new file
                changed[current].append((start, start + count - 1))
    return changed


def changed_since(base: str) -> dict[str, list[tuple[int, int]] | None]:
    """First-party files that differ from the merge base; None means every line."""
    merge_base = git("merge-base", base, "HEAD").strip()
    diff = git("diff", "-U0", "--no-color", "--no-ext-diff", "--diff-filter=d", merge_base, "--")
    files: dict[str, list[tuple[int, int]] | None] = {
        path: ranges for path, ranges in parse_changed_lines(diff).items() if is_first_party(path)
    }
    for path in git("ls-files", "--others", "--exclude-standard").splitlines():
        if is_first_party(path):
            files[path] = None
    return files


def in_ranges(line: int, ranges: list[tuple[int, int]] | None) -> bool:
    return ranges is None or any(first <= line <= last for first, last in ranges)


def parse_findings(output: str, tool: str) -> list[Finding]:
    found = []
    for line in output.splitlines():
        match = FINDING_RE.match(line)
        if not match:
            continue
        path = Path(match["file"])
        if not path.is_absolute():
            path = ROOT / path
        try:
            file = path.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            file = str(path)
        found.append(
            Finding(file, int(match["line"]), int(match["column"]), match["severity"], tool,
                    match["check"], match["message"])
        )
    return found


def find_tool(name: str, env: str, extra_dirs: tuple[str, ...] = ()) -> str:
    override = os.environ.get(env)
    if override:
        return override
    candidates = [name] + [f"{name}-{version}" for version in range(22, 16, -1)]
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return path
    for directory in extra_dirs:
        path = Path(directory) / name
        if path.exists():
            return str(path)
    raise CannotRun(f"{name} not found (set {env}, or see docs/dev_setup.md, 'Static analysis')")


def find_build_dir(given: str | None) -> Path:
    if given:
        build = Path(given)
        if not (build / "compile_commands.json").exists():
            raise CannotRun(f"no compile_commands.json in {build}: configure it first")
        return build
    found = [ROOT / "build" / name for name in PRESET_DIRS
             if (ROOT / "build" / name / "compile_commands.json").exists()]
    if not found:
        raise CannotRun("no build/<preset>/compile_commands.json: run `cmake --preset dev-ui` "
                        "(or dev), or pass --build-dir")
    return max(found, key=lambda build: (build / "compile_commands.json").stat().st_mtime)


def database_sources(build: Path) -> list[str]:
    """First-party sources the build compiles, relative to the repository."""
    sources = []
    for entry in json.loads((build / "compile_commands.json").read_text()):
        path = Path(entry["directory"], entry["file"]).resolve()
        try:
            relative = path.relative_to(ROOT).as_posix()
        except ValueError:
            continue
        if relative.startswith(FIRST_PARTY):
            sources.append(relative)
    return sorted(set(sources))


def include_spelling(header: str) -> str:
    """How a source names the header: below include/ for a public one, else the file name."""
    marker = "/include/"
    if marker in header:
        return header.split(marker, 1)[1]
    return Path(header).name


def sources_including(header: str, sources: list[str], texts: dict[str, str]) -> list[str]:
    pattern = re.compile(r'#\s*include\s*[<"](?:[^">]*/)?' + re.escape(include_spelling(header)) + r'[">]')
    component = "/".join(header.split("/")[:2])
    users = []
    for source in sources:
        if source not in texts:
            try:
                texts[source] = (ROOT / source).read_text(errors="replace")
            except OSError:
                texts[source] = ""
        if pattern.search(texts[source]):
            users.append(source)
    # The header's own component first: its own source is the likeliest to use all of it.
    users.sort(key=lambda source: (not source.startswith(component + "/"), source))
    return users[:HEADER_SOURCES]


def run_cppcheck(files: list[str], jobs: int) -> list[Finding]:
    if not files:
        return []
    cppcheck = find_tool("cppcheck", "CPPCHECK")
    command = [
        cppcheck, "--std=c++20", "--language=c++", "--quiet", f"-j{jobs}",
        "--enable=warning,performance,portability", "--inline-suppr",
        "--library=googletest", "--library=qt",
        f"--suppressions-list={ROOT / 'cmake' / 'cppcheck.supp'}",
        "--template={file}:{line}:{column}: {severity}: {message} [{id}]",
    ]
    command += [f"-I{include.relative_to(ROOT)}" for include in sorted(ROOT.glob("libs/*/include"))]
    done = subprocess.run(command + files, cwd=ROOT, capture_output=True, text=True)
    findings = parse_findings(done.stderr, "cppcheck")
    if done.returncode != 0 and not findings:
        raise CannotRun(f"cppcheck failed:\n{done.stderr.strip()}")
    return findings


def sysroot_args() -> list[str]:
    # Homebrew's clang-tidy does not know where Apple's SDK is; Apple's compiler,
    # which wrote the compile commands, does not need to be told.
    if sys.platform != "darwin":
        return []
    done = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    sdk = done.stdout.strip()
    return [f"--extra-arg=-isysroot{sdk}"] if done.returncode == 0 and sdk else []


def run_clang_tidy(sources: list[str], line_filter: list[dict], build: Path, jobs: int,
                   fix: bool) -> list[Finding]:
    if not sources:
        return []
    clang_tidy = find_tool("clang-tidy", "CLANG_TIDY", BREW_LLVM)
    command = [clang_tidy, "-p", str(build), "--quiet", f"--line-filter={json.dumps(line_filter)}"]
    command += sysroot_args()
    if fix:
        command.append("--fix")

    def one(source: str) -> str:
        done = subprocess.run(command + [source], cwd=ROOT, capture_output=True, text=True)
        if done.returncode != 0 and not FINDING_RE.search(done.stdout + done.stderr):
            raise CannotRun(f"clang-tidy failed on {source}:\n{done.stderr.strip() or done.stdout.strip()}")
        return done.stdout

    # Two fixes to one header from two processes would collide.
    with ThreadPoolExecutor(max_workers=1 if fix else jobs) as pool:
        outputs = list(pool.map(one, sources))
    return [finding for output in outputs for finding in parse_findings(output, "clang-tidy")]


def check(files: dict[str, list[tuple[int, int]] | None], build_dir: str | None, stages: set[str],
          jobs: int, fix: bool) -> tuple[list[Finding], list[str]]:
    findings: list[Finding] = []
    notes: list[str] = []
    if not files:
        return findings, notes

    if "cppcheck" in stages:
        for finding in run_cppcheck(sorted(files), jobs):
            if finding.file in files and in_ranges(finding.line, files[finding.file]):
                findings.append(finding)

    if "clang-tidy" in stages:
        build = find_build_dir(build_dir)
        compiled = database_sources(build)
        texts: dict[str, str] = {}
        sources: set[str] = set()
        for path in sorted(files):
            if path.endswith(SOURCE_SUFFIXES):
                if path in compiled:
                    sources.add(path)
                else:
                    notes.append(f"{path}: not compiled by {build.relative_to(ROOT)} (new file: "
                                 "configure again; otherwise an option leaves it out): clang-tidy skipped")
            else:
                users = sources_including(path, compiled, texts)
                if users:
                    sources.update(users)
                else:
                    notes.append(f"{path}: no compiled source includes it: clang-tidy skipped")
        line_filter = []
        for path, ranges in sorted(files.items()):
            entry: dict = {"name": str(ROOT / path)}
            if ranges is not None:
                entry["lines"] = [list(pair) for pair in ranges]
            line_filter.append(entry)
        for finding in run_clang_tidy(sorted(sources), line_filter, build, jobs, fix):
            if finding.check == "clang-diagnostic-error":
                # Not a finding about the change: the source did not parse, so
                # nothing else clang-tidy said about it can be trusted.
                raise CannotRun(f"{finding.text()}\nclang-tidy could not parse the source. Build "
                                f"{build.relative_to(ROOT)} first (generated files), and fix any compile error.")
            findings.append(finding)

    return sorted(set(findings)), notes


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", help="files to check whole (default: what differs from --base)")
    parser.add_argument("--base", default="origin/develop", help="branch the change is measured from")
    parser.add_argument("--build-dir", help="directory holding compile_commands.json")
    parser.add_argument("--stage", action="append", choices=("cppcheck", "clang-tidy"),
                        help="run only this stage (repeatable)")
    parser.add_argument("--fix", action="store_true", help="apply clang-tidy's fix-its, then report what is left")
    parser.add_argument("--json", action="store_true", help="print one JSON object instead of text")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args(argv)

    try:
        if args.paths:
            files: dict[str, list[tuple[int, int]] | None] = {}
            for given in args.paths:
                path = Path(given).resolve()
                if not path.is_file():
                    raise CannotRun(f"{given}: no such file")
                files[path.relative_to(ROOT).as_posix()] = None
        else:
            files = changed_since(args.base)
        stages = set(args.stage or ("cppcheck", "clang-tidy"))
        findings, notes = check(files, args.build_dir, stages, max(1, args.jobs), args.fix)
    except CannotRun as error:
        if args.json:
            print(json.dumps({"ok": False, "error": str(error), "findings": []}, indent=2))
        else:
            print(f"quality_check: {error}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps({"ok": not findings, "files": sorted(files), "notes": notes,
                          "findings": [asdict(finding) for finding in findings]}, indent=2))
    else:
        for note in notes:
            print(f"note: {note}", file=sys.stderr)
        for finding in findings:
            print(finding.text())
        print(f"quality_check: {len(files)} file(s), {len(findings)} finding(s)", file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
