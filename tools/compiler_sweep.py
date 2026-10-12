#!/usr/bin/env python3
"""Compiles every source of the tree with another compiler, for its warnings only.

CI builds with gcc 14, clang 18 and MSVC; a Mac builds with Apple clang, which
does not give all of their warnings, and CI stops at the first error of each
job. This runs the commands of a configured build directory through a second
compiler with -fsyntax-only (no objects, no linking) and lists every warning
the project's -Werror would have stopped on, all of them at once.

    python3 tools/compiler_sweep.py                      # Homebrew's newest g++
    python3 tools/compiler_sweep.py --compiler g++-14
    python3 tools/compiler_sweep.py --flag=-Wmissing-designated-field-initializers --compiler c++

Exit status: 0 nothing found, 1 warnings found, 2 the sweep could not run.
It is the nearest thing to CI's compilers a Mac has, not CI: a clean sweep
with gcc 16 has still met a gcc 14 warning (-Wrange-loop-construct).
"""

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WARNING_RE = re.compile(r"error: .*\[-Werror")
# Arguments that are another compiler's, or that only matter when an object is written.
DROP_WITH_VALUE = {"-o", "-arch", "-MF", "-MT", "-MQ"}
DROP = {"-MD", "-MMD"}


def find_gxx() -> str | None:
    for version in range(20, 12, -1):
        path = shutil.which(f"g++-{version}")
        if path:
            return path
    return None


def sweep_command(command: str, compiler: str, flags: list[str]) -> list[str]:
    args = shlex.split(command)[1:]
    kept = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in DROP_WITH_VALUE:
            skip = True
            continue
        if arg in DROP or arg.startswith("-fsanitize"):
            continue
        kept.append(arg)
    return [compiler, "-fsyntax-only", "-fdiagnostics-color=never", *flags, *kept]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=str(ROOT / "build" / "dev"), help="a configured build directory")
    parser.add_argument("--compiler", help="the compiler to run (default: the newest g++-NN on PATH)")
    parser.add_argument("--flag", action="append", default=[], help="an extra flag, e.g. --flag=-Wextra-semi")
    parser.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = parser.parse_args()

    database = Path(args.build_dir) / "compile_commands.json"
    if not database.is_file():
        print(f"compiler_sweep: no {database}: configure the build first", file=sys.stderr)
        return 2
    compiler = shutil.which(args.compiler) if args.compiler else find_gxx()
    if not compiler:
        print("compiler_sweep: no compiler found (brew install gcc, or --compiler)", file=sys.stderr)
        return 2

    entries = [
        e for e in json.loads(database.read_text())
        if "/_deps/" not in e["file"] and "-Werror" in e["command"]
    ]

    def run(entry):
        done = subprocess.run(sweep_command(entry["command"], compiler, args.flag), cwd=entry["directory"],
                              capture_output=True, text=True, check=False)
        return entry["file"], done.returncode, done.stderr

    warnings: set[str] = set()
    broken: list[tuple[str, str]] = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for file, status, stderr in pool.map(run, entries):
            if status == 0:
                continue
            lines = stderr.splitlines()
            hard = [line for line in lines if " error: " in line and "[-Werror" not in line]
            if hard:
                broken.append((file, hard[0]))
            warnings.update(line for line in lines if WARNING_RE.search(line))

    for line in sorted(warnings):
        print(line)
    # A source the other compiler cannot read at all (a system header of the first one's) is not a finding.
    for file, why in broken:
        print(f"compiler_sweep: not checked: {file}: {why}", file=sys.stderr)
    print(f"compiler_sweep: {compiler}: {len(entries)} sources, {len(warnings)} warning(s), "
          f"{len(broken)} not checked", file=sys.stderr)
    return 1 if warnings else 0


if __name__ == "__main__":
    sys.exit(main())
