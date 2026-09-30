from spec_router.judge import Answers, ChoiceAnswer, FakeJudge, ScoreAnswer
from spec_router.plan import BACKGROUND, route_sections, size_units
from spec_router.spec import Section
from spec_router.units import Unit

UNITS = [
    Unit(id="core", wave=1, layer="core", goal="core lib"),
    Unit(id="transport", wave=2, layer="transport", goal="transport lib", depends=["core"]),
    Unit(id="manual", wave=9, layer="manual", goal="hands", manual=True),
]
SECS = [
    Section(id="1", level=2, heading="Intent", body="why we do this", path=["Intent"]),
    Section(id="4.1", level=2, heading="Errors", body="Result<T>", path=["Errors"]),
    Section(id="4.2", level=2, heading="Transport", body="exchange()", path=["Transport"]),
]


def _choice(pick, conf=1.0, probs=None):
    keys = ["core", "transport", BACKGROUND]
    probs = probs or {k: (1.0 if k == pick else 0.0) for k in keys}
    return ChoiceAnswer(pick, conf, probs)


def keyword_judge(state, questions):
    body = state["section"]["body"]
    a = Answers()
    if "why" in body:
        a.choices["primary"] = _choice(BACKGROUND)
        a.nouls = {"need:core": 0.1, "need:transport": 0.1}
    elif "Result" in body:
        a.choices["primary"] = _choice("core")
        a.nouls = {"need:core": 0.9, "need:transport": 0.8}  # transport consumes Result too
    else:
        a.choices["primary"] = _choice("transport")
        a.nouls = {"need:core": 0.05, "need:transport": 0.95}
    return a


def test_route_attaches_primary_and_required_reading():
    judge = FakeJudge(keyword_judge)
    plan = route_sections(SECS, UNITS, judge)
    assert plan.background == ["1"]
    assert [s.id for s in plan.briefs["core"].sections] == ["4.1"]
    assert [s.id for s in plan.briefs["transport"].sections] == ["4.1", "4.2"]
    assert "manual" not in plan.briefs


def test_route_questions_exclude_manual_units_and_include_background():
    judge = FakeJudge(keyword_judge)
    route_sections(SECS[:1], UNITS, judge)
    _, qs = judge.requests[0]
    assert set(qs) == {"primary", "need:core", "need:transport"}
    assert BACKGROUND in qs["primary"].criteria
    assert "manual" not in qs["primary"].criteria


def test_uncertain_background_attaches_to_plausible_units():
    def unsure(state, questions):
        a = Answers()
        a.choices["primary"] = _choice(BACKGROUND, conf=0.3, probs={"core": 0.35, "transport": 0.1, BACKGROUND: 0.55})
        a.nouls = {"need:core": 0.2, "need:transport": 0.2}
        return a

    plan = route_sections(SECS[:1], UNITS, FakeJudge(unsure))
    assert plan.background == []
    assert [s.id for s in plan.briefs["core"].sections] == ["1"]
    assert plan.briefs["transport"].sections == []


def test_size_maps_score_to_tier_and_split_flag():
    scores = iter([0.8, 3.7])

    def sizer(state, questions):
        a = Answers()
        a.scores["size"] = ScoreAnswer(next(scores), 0.9, [0, 0, 0, 0, 0], [])
        return a

    plan = route_sections(SECS, UNITS, FakeJudge(keyword_judge))
    plan = size_units(plan, FakeJudge(sizer))
    core, transport = plan.briefs["core"].sizing, plan.briefs["transport"].sizing
    assert (core.model, core.max_turns, core.split) == ("haiku", 30, False)
    assert (transport.model, transport.max_turns, transport.split) == ("opus", 150, True)


def test_plan_round_trips_to_dict():
    plan = route_sections(SECS, UNITS, FakeJudge(keyword_judge))
    d = plan.to_dict()
    assert d["briefs"]["transport"]["sections"] == ["4.1", "4.2"]
    assert d["routings"][0]["primary"] == BACKGROUND
