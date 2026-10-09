#!/usr/bin/env python3
"""Static analysis of the C++ a branch changes: cppcheck, then clang-tidy.

    python3 tools/quality_check.py                  # lines changed since origin/develop
    python3 tools/quality_check.py --json           # the same, for a program to read
    python3 tools/quality_check.py libs/core/src/number.cpp   # these files, every line
    python3 tools/quality_check.py libs/core                  # a directory, every line
    python3 tools/quality_check.py --fix            # let clang-tidy apply its fix-its

Without paths it takes the first-party C++ files that differ from the merge
base with --base (committed, staged, unstaged and untracked) and reports only
findings on the lines that differ, so the result is about the change and not
about the file's history. A new file is checked whole.

A path may be a directory: every first-party C++ file git knows below it.

clang-tidy needs the compile database a configure writes
(build/<dir>/compile_commands.json) and, for the UI and its tests, the moc
files of a build. Every configured directory under build/ is read, the newest
first, so a source one configuration leaves out (a *_stub.cpp) is analysed
with another that compiles it. A header is analysed through up to two sources
that include it, directly or through one other header. cppcheck reads the
files alone, first-party headers only.

Exit status: 0 nothing found, 1 findings, 2 the check could not run, or could
not run on every file (the findings it did make are still printed).
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


def find_build_dirs(given: str | None) -> list[Path]:
    """Configured build directories, the one to prefer first."""
    if given:
        build = Path(given).resolve()
        if not (build / "compile_commands.json").exists():
            raise CannotRun(f"no compile_commands.json in {given}: configure it first")
        return [build]
    found = [database.parent for database in (ROOT / "build").glob("*/compile_commands.json")]
    if not found:
        raise CannotRun("no build/<dir>/compile_commands.json: run `cmake --preset dev-ui` "
                        "(or dev), or pass --build-dir")
    return sorted(found, key=lambda build: (build / "compile_commands.json").stat().st_mtime, reverse=True)


def database_sources(builds: list[Path]) -> dict[str, Path]:
    """First-party sources and the build that compiles each (the first that does)."""
    sources: dict[str, Path] = {}
    for build in builds:
        for entry in json.loads((build / "compile_commands.json").read_text()):
            path = Path(entry["directory"], entry["file"]).resolve()
            try:
                relative = path.relative_to(ROOT).as_posix()
            except ValueError:
                continue
            if relative.startswith(FIRST_PARTY):
                sources.setdefault(relative, build)
    return dict(sorted(sources.items()))


def first_party_headers() -> list[str]:
    return sorted(path.relative_to(ROOT).as_posix()
                  for top in FIRST_PARTY for suffix in HEADER_SUFFIXES
                  for path in (ROOT / top).rglob(f"*{suffix}"))


def include_spelling(header: str) -> str:
    """How a source names the header: below include/ for a public one, else the file name."""
    marker = "/include/"
    if marker in header:
        return header.split(marker, 1)[1]
    return Path(header).name


def files_including(header: str, candidates: list[str], texts: dict[str, str]) -> list[str]:
    pattern = re.compile(r'#\s*include\s*[<"](?:[^">]*/)?' + re.escape(include_spelling(header)) + r'[">]')
    users = []
    for candidate in candidates:
        if candidate not in texts:
            try:
                texts[candidate] = (ROOT / candidate).read_text(errors="replace")
            except OSError:
                texts[candidate] = ""
        if candidate != header and pattern.search(texts[candidate]):
            users.append(candidate)
    return users


def sources_including(header: str, sources: list[str], headers: list[str],
                      texts: dict[str, str]) -> list[str]:
    """Up to HEADER_SOURCES sources that see the header, the header's own component first."""
    users = files_including(header, sources, texts)
    if not users:  # reached only through another header: one level, no further
        for via in files_including(header, headers, texts):
            users += files_including(via, sources, texts)
    component = "/".join(header.split("/")[:2])
    # Its own component's source is the likeliest to use all of it.
    users = sorted(set(users), key=lambda source: (not source.startswith(component + "/"), source))
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


def run_clang_tidy(sources: dict[str, Path], line_filter: list[dict], jobs: int,
                   fix: bool) -> tuple[list[Finding], list[str]]:
    """Findings, and what stopped a source from being analysed."""
    if not sources:
        return [], []
    clang_tidy = find_tool("clang-tidy", "CLANG_TIDY", BREW_LLVM)
    common = ["--quiet", f"--line-filter={json.dumps(line_filter)}"] + sysroot_args()
    if fix:
        common.append("--fix")

    def one(item: tuple[str, Path]) -> tuple[str, str]:
        source, build = item
        done = subprocess.run([clang_tidy, "-p", str(build), *common, source],
                              cwd=ROOT, capture_output=True, text=True)
        said = (done.stdout + done.stderr).splitlines()
        if done.returncode != 0 and not any(FINDING_RE.match(line) for line in said):
            return "", f"clang-tidy failed on {source}: {(done.stderr.strip() or done.stdout.strip())[:400]}"
        return done.stdout, ""

    # Two fixes to one header from two processes would collide.
    with ThreadPoolExecutor(max_workers=1 if fix else jobs) as pool:
        results = list(pool.map(one, sources.items()))
    findings: list[Finding] = []
    errors = [error for _, error in results if error]
    for output, _ in results:
        for finding in parse_findings(output, "clang-tidy"):
            if finding.check == "clang-diagnostic-error":
                # Not a finding about the code: the source did not parse, so
                # nothing else clang-tidy said about it can be trusted.
                errors.append(f"{finding.text()} (did not parse: build first for generated "
                              "files, and fix any compile error)")
            else:
                findings.append(finding)
    return findings, sorted(set(errors))


def check(files: dict[str, list[tuple[int, int]] | None], build_dir: str | None, stages: set[str],
          jobs: int, fix: bool) -> tuple[list[Finding], list[str], list[str]]:
    """Findings, notes, and errors (what could not be checked)."""
    findings: list[Finding] = []
    notes: list[str] = []
    errors: list[str] = []
    if not files:
        return findings, notes, errors

    if "cppcheck" in stages:
        for finding in run_cppcheck(sorted(files), jobs):
            if finding.file in files and in_ranges(finding.line, files[finding.file]):
                findings.append(finding)

    if "clang-tidy" in stages:
        compiled = database_sources(find_build_dirs(build_dir))
        headers = first_party_headers()
        texts: dict[str, str] = {}
        sources: dict[str, Path] = {}
        for path in sorted(files):
            if path.endswith(SOURCE_SUFFIXES):
                if path in compiled:
                    sources[path] = compiled[path]
                else:
                    notes.append(f"{path}: no configured build compiles it (new file: configure "
                                 "again; otherwise an option leaves it out): clang-tidy skipped")
            else:
                users = sources_including(path, list(compiled), headers, texts)
                if not users:
                    notes.append(f"{path}: no compiled source includes it: clang-tidy skipped")
                for user in users:
                    sources[user] = compiled[user]
        line_filter = []
        for path, ranges in sorted(files.items()):
            entry: dict = {"name": str(ROOT / path)}
            if ranges is not None:
                entry["lines"] = [list(pair) for pair in ranges]
            line_filter.append(entry)
        found, errors = run_clang_tidy(dict(sorted(sources.items())), line_filter, jobs, fix)
        findings += found

    return sorted(set(findings)), notes, errors


def files_below(directory: Path) -> list[str]:
    """First-party C++ files git knows below a directory, tracked or not yet."""
    relative = directory.relative_to(ROOT).as_posix()
    listed = git("ls-files", "--cached", "--others", "--exclude-standard", "--", relative).splitlines()
    return [path for path in listed if is_first_party(path) and (ROOT / path).is_file()]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*",
                        help="files or directories to check whole (default: what differs from --base)")
    parser.add_argument("--base", default="origin/develop", help="branch the change is measured from")
    parser.add_argument("--build-dir", help="the one directory to read compile_commands.json from")
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
                if path.is_dir():
                    files.update(dict.fromkeys(files_below(path)))
                elif path.is_file():
                    files[path.relative_to(ROOT).as_posix()] = None
                else:
                    raise CannotRun(f"{given}: no such file or directory")
        else:
            files = changed_since(args.base)
        stages = set(args.stage or ("cppcheck", "clang-tidy"))
        findings, notes, errors = check(files, args.build_dir, stages, max(1, args.jobs), args.fix)
    except CannotRun as error:
        if args.json:
            print(json.dumps({"ok": False, "errors": [str(error)], "findings": []}, indent=2))
        else:
            print(f"quality_check: {error}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps({"ok": not findings and not errors, "files": sorted(files), "notes": notes,
                          "errors": errors, "findings": [asdict(finding) for finding in findings]}, indent=2))
    else:
        for note in notes:
            print(f"note: {note}", file=sys.stderr)
        for finding in findings:
            print(finding.text())
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"quality_check: {len(files)} file(s), {len(findings)} finding(s), "
              f"{len(errors)} not checked", file=sys.stderr)
    if errors:
        return 2
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
