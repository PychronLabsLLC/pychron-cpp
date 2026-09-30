"""Thin, cached wrapper around TypeSafe System One.

Everything the router asks the model goes through ``Judge.ask``. Answers are
normalized into plain dataclasses so planning code and tests never touch SDK
types, and every (state, questions) pair is cached on disk keyed by content
hash so re-runs are free and deterministic.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Protocol

from typesafe_sdk import Choice, Noul, Score

Question = Choice | Noul | Score


@dataclass(frozen=True)
class ChoiceAnswer:
    choice: str
    confidence: float
    probabilities: dict[str, float]


@dataclass(frozen=True)
class ScoreAnswer:
    score: float
    confidence: float
    probabilities: list[float]
    legend: list[str] = field(default_factory=list)

    @property
    def max_level(self) -> int:
        return len(self.probabilities) - 1


@dataclass
class Answers:
    choices: dict[str, ChoiceAnswer] = field(default_factory=dict)
    nouls: dict[str, float] = field(default_factory=dict)
    scores: dict[str, ScoreAnswer] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "choices": {k: asdict(v) for k, v in self.choices.items()},
            "nouls": dict(self.nouls),
            "scores": {k: asdict(v) for k, v in self.scores.items()},
        }

    @classmethod
    def from_dict(cls, d: Mapping[str, Any]) -> "Answers":
        return cls(
            choices={k: ChoiceAnswer(**v) for k, v in d.get("choices", {}).items()},
            nouls={k: float(v) for k, v in d.get("nouls", {}).items()},
            scores={k: ScoreAnswer(**v) for k, v in d.get("scores", {}).items()},
        )


class Judge(Protocol):
    def ask(self, state: Any, questions: Mapping[str, Question]) -> Answers: ...


def _question_dict(q: Question) -> dict[str, Any]:
    dump = getattr(q, "model_dump", None)
    return dump() if dump else dict(vars(q))


def request_key(state: Any, questions: Mapping[str, Question], model: str) -> str:
    payload = json.dumps(
        {"model": model, "state": state, "questions": {k: _question_dict(q) for k, q in questions.items()}},
        sort_keys=True,
        default=str,
    )
    return hashlib.sha256(payload.encode()).hexdigest()


def _normalize(response: Any) -> Answers:
    out = Answers()
    for key, ans in response.answers.items():
        kind = getattr(ans, "type", None)
        if kind == "choice":
            out.choices[key] = ChoiceAnswer(ans.choice, float(ans.confidence), dict(ans.probabilities))
        elif kind == "noul":
            out.nouls[key] = float(ans.noul)
        elif kind == "score":
            probs = ans.probabilities
            if isinstance(probs, Mapping):
                probs = [probs[k] for k in sorted(probs, key=lambda x: int(x))]
            legend = ans.legend
            if isinstance(legend, Mapping):
                legend = [legend[k] for k in sorted(legend, key=lambda x: int(x))]
            out.scores[key] = ScoreAnswer(float(ans.score), float(ans.confidence), list(probs), list(legend or []))
        else:
            raise ValueError(f"unknown answer type for {key!r}: {kind!r}")
    return out


class TypeSafeJudge:
    """Live judge with a JSON file cache. Client is created lazily so importing
    this module (and running dry runs) never needs an API key."""

    def __init__(self, cache_path: Path, *, model: str = "jev-latest", client: Any | None = None) -> None:
        self.cache_path = Path(cache_path)
        self.model = model
        self._client = client
        self._cache: dict[str, Any] = {}
        if self.cache_path.exists():
            self._cache = json.loads(self.cache_path.read_text())
        self.calls = 0
        self.hits = 0

    @property
    def client(self) -> Any:
        if self._client is None:
            from typesafe_sdk import TypeSafeClient

            self._client = TypeSafeClient(model=self.model)
        return self._client

    def ask(self, state: Any, questions: Mapping[str, Question]) -> Answers:
        key = request_key(state, questions, self.model)
        if key in self._cache:
            self.hits += 1
            return Answers.from_dict(self._cache[key])
        self.calls += 1
        answers = _normalize(self.client.system_one(state=state, questions=dict(questions), model=self.model))
        self._cache[key] = answers.to_dict()
        self.cache_path.parent.mkdir(parents=True, exist_ok=True)
        self.cache_path.write_text(json.dumps(self._cache, indent=1, sort_keys=True))
        return answers


class FakeJudge:
    """Test double: delegates to a function ``fn(state, questions) -> Answers``
    and records every request."""

    def __init__(self, fn: Callable[[Any, Mapping[str, Question]], Answers]) -> None:
        self.fn = fn
        self.requests: list[tuple[Any, dict[str, Question]]] = []

    def ask(self, state: Any, questions: Mapping[str, Question]) -> Answers:
        self.requests.append((state, dict(questions)))
        return self.fn(state, questions)
