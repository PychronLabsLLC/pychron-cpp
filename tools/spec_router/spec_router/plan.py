"""Turn spec sections + unit registry into a per-unit brief using Jev judgments.

Code decides the candidates (sections, units) and the policy (thresholds,
tier mapping). Jev decides the semantic mapping: which unit a section
primarily specifies, which other units still need to read it, and how large
each unit's job is.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any, Mapping

from typesafe_sdk import Choice, Noul, Score

from .judge import Judge
from .spec import Section
from .units import Unit

BACKGROUND = "background"

SIZE_LEVELS: list[dict[str, Any]] = [
    {"what": "A single header or one small file; no tests beyond a compile check.",
     "examples": ["add an enum and a struct", "write one CMake option"]},
    {"what": "One library with a few source files and unit tests; no interaction with other libraries.",
     "examples": ["a pure byte codec with fixture tests", "a config struct + TOML parser"]},
    {"what": "Several source files plus tests inside one library, with threads, timing, or I/O to get right.",
     "examples": ["a scheduler with non-overlap guarantees", "a serial transport on asio"]},
    {"what": "Spans two or more libraries or an app plus libraries, needing integration tests end to end.",
     "examples": ["a facade wiring config -> transports -> drivers -> managers", "a CLI over multiple managers"]},
    {"what": "Requires physical hardware, vendor SDKs, or contains unknowns that cannot be resolved from the spec.",
     "examples": ["capturing real device traces", "wrapping a proprietary DLL"]},
]

# (max score exclusive, model alias, max_turns)
# Calibrated on the extraction-line waves: sonnet units that had to port
# algorithms or fixtures needed the whole budget; haiku is only for trivial
# header/config work.
TIER_TABLE: list[tuple[float, str, int]] = [
    (1.0, "haiku", 40),
    (2.25, "sonnet", 80),
    (3.5, "opus", 100),
    (float("inf"), "opus", 150),
]
SPLIT_AT = 3.5


@dataclass
class SectionRouting:
    section_id: str
    primary: str
    confidence: float
    probabilities: dict[str, float]
    needed_by: dict[str, float]  # unit id -> P(required reading)


@dataclass
class Sizing:
    score: float
    confidence: float
    model: str
    max_turns: int
    split: bool


@dataclass
class UnitBrief:
    unit: Unit
    sections: list[Section]
    sizing: Sizing | None = None


@dataclass
class Plan:
    routings: list[SectionRouting]
    briefs: dict[str, UnitBrief]
    background: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "routings": [asdict(r) for r in self.routings],
            "background": list(self.background),
            "briefs": {
                uid: {
                    "unit": asdict(b.unit),
                    "sections": [s.id for s in b.sections],
                    "sizing": asdict(b.sizing) if b.sizing else None,
                }
                for uid, b in self.briefs.items()
            },
        }


def _unit_catalog(units: list[Unit]) -> dict[str, str]:
    return {u.id: u.goal for u in units if not u.manual}


def routing_questions(units: list[Unit]) -> dict[str, Choice | Noul]:
    catalog = _unit_catalog(units)
    criteria: dict[str, Any] = dict(catalog)
    criteria[BACKGROUND] = "Rationale, intent, history, or process guidance; no unit implements it directly."
    qs: dict[str, Choice | Noul] = {
        "primary": Choice(
            instructions=(
                "The `section` is one part of a software design spec. `units` lists the work units the spec "
                "will be implemented as. Which single unit does this section primarily specify, meaning the "
                "agent building that unit would treat this section as its requirements?"
            ),
            criteria=criteria,
        )
    }
    for uid in catalog:
        qs[f"need:{uid}"] = Noul(
            instructions=(
                f"Must the agent implementing unit `units.{uid}` read `section.body` to build that unit "
                "correctly? Yes if it defines an interface, type, rule, file layout, or behavior that unit "
                "consumes, produces, or must not violate. No if the unit could be built correctly without it."
            )
        )
    return qs


def route_sections(
    sections: list[Section],
    units: list[Unit],
    judge: Judge,
    *,
    attach_threshold: float = 0.5,
    background_confidence: float = 0.6,
) -> Plan:
    catalog = _unit_catalog(units)
    qs = routing_questions(units)
    by_unit: dict[str, list[Section]] = {uid: [] for uid in catalog}
    routings: list[SectionRouting] = []
    background: list[str] = []

    for sec in sections:
        state = {
            "section": {"id": sec.id, "title": sec.title, "body": sec.body},
            "units": catalog,
        }
        ans = judge.ask(state, qs)
        primary = ans.choices["primary"]
        needed = {uid: ans.nouls.get(f"need:{uid}", 0.0) for uid in catalog}
        routings.append(SectionRouting(sec.id, primary.choice, primary.confidence, primary.probabilities, needed))

        attached: set[str] = set()
        if primary.choice == BACKGROUND and primary.confidence >= background_confidence:
            background.append(sec.id)
        elif primary.choice != BACKGROUND:
            attached.add(primary.choice)
        else:
            # Uncertain background call: attach to any unit with non-trivial primary probability.
            attached.update(u for u, p in primary.probabilities.items() if u != BACKGROUND and p >= 0.2)
        attached.update(u for u, p in needed.items() if p >= attach_threshold)
        for uid in attached:
            by_unit[uid].append(sec)

    briefs = {u.id: UnitBrief(u, by_unit[u.id]) for u in units if not u.manual}
    return Plan(routings, briefs, background)


def size_question() -> Score:
    return Score(
        instructions=(
            "`unit.goal` is a work unit and `sections` are the spec sections it must implement or respect. "
            "How large is the implementation job for one engineer or agent?"
        ),
        criteria=SIZE_LEVELS,
    )


def size_units(plan: Plan, judge: Judge, *, tiers: list[tuple[float, str, int]] = TIER_TABLE, split_at: float = SPLIT_AT) -> Plan:
    q = {"size": size_question()}
    for brief in plan.briefs.values():
        state = {
            "unit": {"id": brief.unit.id, "goal": brief.unit.goal, "layer": brief.unit.layer, "targets": brief.unit.targets},
            "sections": [{"id": s.id, "title": s.title, "body": s.body} for s in brief.sections],
        }
        s = judge.ask(state, q).scores["size"]
        model, turns = next((m, t) for lim, m, t in tiers if s.score < lim)
        brief.sizing = Sizing(s.score, s.confidence, model, turns, split=s.score >= split_at)
    return plan
