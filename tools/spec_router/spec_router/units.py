"""Work-unit registry loaded from units.toml. Pure code; no judgments here."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from importlib import resources
from pathlib import Path


@dataclass(frozen=True)
class Unit:
    id: str
    wave: int
    layer: str
    goal: str
    targets: list[str] = field(default_factory=list)
    depends: list[str] = field(default_factory=list)
    manual: bool = False
    spec: str | None = None  # spec file (repo-relative) this unit is routed from; None = CLI --spec
    preset: str | None = None  # CMake configure preset the router verifies with (default: plain BUILD_UI=OFF)
    # Read-only ground-truth sources outside the repo (e.g. the production
    # Python pychron for a vendor wire protocol). Passed to the agent with
    # --add-dir and listed in its prompt; "~" is expanded.
    references: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class Wave:
    number: int
    units: list[Unit]


def load_units(path: Path | None = None) -> list[Unit]:
    if path is None:
        data = tomllib.loads(resources.files(__package__).joinpath("units.toml").read_text())
    else:
        data = tomllib.loads(Path(path).read_text())
    units = [Unit(**u) for u in data["unit"]]
    ids = [u.id for u in units]
    dupes = {i for i in ids if ids.count(i) > 1}
    if dupes:
        raise ValueError(f"duplicate unit ids: {sorted(dupes)}")
    by_id = {u.id: u for u in units}
    for u in units:
        for d in u.depends:
            if d not in by_id:
                raise ValueError(f"unit {u.id!r} depends on unknown unit {d!r}")
            if by_id[d].wave >= u.wave:
                raise ValueError(f"unit {u.id!r} (wave {u.wave}) depends on {d!r} in wave {by_id[d].wave}")
    return units


def waves(units: list[Unit], *, include_manual: bool = False) -> list[Wave]:
    grouped: dict[int, list[Unit]] = {}
    for u in units:
        if u.manual and not include_manual:
            continue
        grouped.setdefault(u.wave, []).append(u)
    return [Wave(n, grouped[n]) for n in sorted(grouped)]
