"""Deterministic markdown sectioning. No model involvement.

A spec is split into ``##`` and ``###`` sections. Each section's body is the
text under its heading up to the next heading of level <= 3, so a parent
section's body excludes its children. Fenced code blocks are opaque: a ``#``
inside a fence is never a heading.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

_HEADING = re.compile(r"^(#{2,3})\s+(.*?)\s*$")
_NUMBERED = re.compile(r"^(\d+(?:\.\d+)*)\.?\s+(.*)$")


@dataclass(frozen=True)
class Section:
    id: str
    level: int
    heading: str
    body: str
    path: list[str] = field(default_factory=list)

    @property
    def title(self) -> str:
        return " / ".join(self.path)


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def parse_sections(text: str) -> list[Section]:
    sections: list[Section] = []
    in_fence = False
    current: dict | None = None
    parent_heading: str | None = None
    parent_id: str | None = None

    def flush() -> None:
        if current is not None:
            sections.append(
                Section(
                    id=current["id"],
                    level=current["level"],
                    heading=current["heading"],
                    body="\n".join(current["lines"]).strip("\n") + ("\n" if current["lines"] else ""),
                    path=current["path"],
                )
            )

    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            if current is not None:
                current["lines"].append(line)
            continue
        m = None if in_fence else _HEADING.match(line)
        if not m:
            if current is not None:
                current["lines"].append(line)
            continue

        flush()
        level = len(m.group(1))
        raw = m.group(2)
        nm = _NUMBERED.match(raw)
        if nm:
            sec_id, heading = nm.group(1), nm.group(2)
        else:
            heading = raw
            sec_id = _slug(raw) if level == 2 else f"{parent_id}.{_slug(raw)}"

        if level == 2:
            parent_heading, parent_id = heading, sec_id
            path = [heading]
        else:
            path = [parent_heading, heading] if parent_heading else [heading]
        current = {"id": sec_id, "level": level, "heading": heading, "lines": [], "path": path}

    flush()
    return sections
