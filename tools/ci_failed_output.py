#!/usr/bin/env python3
"""Prints the output of every test that failed in the last ctest run.

ctest keeps each test's output in Testing/Temporary/LastTest.log, and the
failed ones in LastTestsFailed.log. A CI test step calls this when ctest fails,
so the log shows why from the run that failed, not only from a rerun (a
test that fails once and then passes still says what went wrong).

    python3 tools/ci_failed_output.py <build dir>
"""

import re
import sys
from pathlib import Path


def failed_names(temporary: Path) -> list[str]:
    names = []
    for path in sorted(temporary.glob("LastTestsFailed*.log")):
        for line in path.read_text(errors="replace").splitlines():
            # "<number>:<name>"
            if ":" in line:
                names.append(line.split(":", 1)[1].strip())
    return names


def outputs(log: str) -> dict[str, str]:
    """Test name -> its section of LastTest.log."""
    found = {}
    # Each test's section: "<n>/<m> Testing: <name>" ... up to the next "<n>/<m> Testing:".
    starts = list(re.finditer(r"^\d+/\d+ Testing: (.+)$", log, re.M))
    for i, m in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(log)
        found[m.group(1).strip()] = log[m.start():end]
    return found


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    temporary = Path(sys.argv[1]) / "Testing" / "Temporary"
    names = failed_names(temporary)
    if not names:
        print(f"no failed tests recorded under {temporary}")
        return 0
    sections = {}
    for path in sorted(temporary.glob("LastTest*.log")):
        if "Failed" not in path.name:
            sections.update(outputs(path.read_text(errors="replace")))
    for name in names:
        print("=" * 100)
        print(f"FAILED: {name}")
        print("=" * 100)
        section = sections.get(name)
        print(section.rstrip() if section else "(no output recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
