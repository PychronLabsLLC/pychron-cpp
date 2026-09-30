import json
from types import SimpleNamespace

from typesafe_sdk import Choice, Noul, Score

from spec_router.judge import TypeSafeJudge, request_key


class StubClient:
    def __init__(self):
        self.calls = 0

    def system_one(self, *, state, questions, model):
        self.calls += 1
        return SimpleNamespace(
            answers={
                "c": SimpleNamespace(type="choice", choice="a", confidence=0.7, probabilities={"a": 0.7, "b": 0.3}),
                "n": SimpleNamespace(type="noul", noul=0.42),
                "s": SimpleNamespace(type="score", score=1.5, confidence=0.5, probabilities=[0.0, 0.5, 0.5], legend=["x", "y", "z"]),
            }
        )


QS = {
    "c": Choice(instructions="pick", criteria={"a": None, "b": None}),
    "n": Noul(instructions="is it?"),
    "s": Score(instructions="how much", criteria=["x", "y", "z"]),
}


def test_normalizes_and_caches(tmp_path):
    cache = tmp_path / "cache.json"
    stub = StubClient()
    j = TypeSafeJudge(cache, client=stub)
    a1 = j.ask({"k": 1}, QS)
    assert a1.choices["c"].choice == "a"
    assert a1.nouls["n"] == 0.42
    assert a1.scores["s"].max_level == 2
    assert cache.exists()

    j2 = TypeSafeJudge(cache, client=stub)
    a2 = j2.ask({"k": 1}, QS)
    assert stub.calls == 1
    assert a2.to_dict() == a1.to_dict()
    assert j2.hits == 1


def test_key_changes_with_state_questions_and_model():
    k = request_key({"a": 1}, QS, "m")
    assert k != request_key({"a": 2}, QS, "m")
    assert k != request_key({"a": 1}, QS, "m2")
    assert k != request_key({"a": 1}, {"c": QS["c"]}, "m")


def test_cache_file_is_plain_json(tmp_path):
    j = TypeSafeJudge(tmp_path / "c.json", client=StubClient())
    j.ask("s", QS)
    json.loads((tmp_path / "c.json").read_text())
