"""Tests for tools/pychron_conditionals_import.py.

Fixtures are legacy pychron YAML (docs and pychron's own test config) plus a
file exercising every key. When ELCTL points at a built elctl binary, every
converted file is also checked by `elctl conditionals-check`, i.e. parsed by
the real C++ loader.
"""

import os
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pychron_conditionals_import as imp  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "conditionals"


def convert(name):
    res = imp.convert_yaml((FIXTURES / name).read_text(), name)
    return res, tomllib.loads(res.toml)


def test_docs_example_pre_and_post_run():
    res, doc = convert("docs_example.yaml")
    pre = [e["check"] for e in doc["pre_run"]]
    assert pre == [
        "CDD.inactive",
        "CDD.deflection==2000",
        "gauge.ig.pressure > 1e-8",
        "gauge.ig.pressure > 1e-8",
        "device.pneumatics < 10",
    ]
    # Two checks became identical once the controllers were dropped: unique names.
    assert doc["pre_run"][2]["name"] == "pre_run:gauge.ig.pressure > 1e-8 #1"
    assert doc["pre_run"][3]["name"] == "pre_run:gauge.ig.pressure > 1e-8 #2"
    assert "start" not in doc["pre_run"][0]  # between-run checks are not gated
    post = doc["post_run"][0]
    assert post == {"check": "Ar40 < $MIN_INTENSITY", "analysis_types": ["air", "cocktail"]}
    assert any("controller 'bone' dropped" in line for line in res.report)
    assert res.converted == 6 and res.skipped == 0


def test_pychron_test_configuration():
    res, doc = convert("test_configuration.yaml")
    assert doc["modifications"] == [{"check": "device.pneumatics<80", "start": 50, "action": "run_blank"}]
    assert doc["equilibrations"] == [
        {"check": "device.pneumatics<80", "start": 50, "abbreviated_count_ratio": 0.5}
    ]
    assert any("start defaulted to 50" in line for line in res.report)


def test_every_key():
    res, doc = convert("system_full.yaml")
    t = doc["truncations"]
    assert t[0] == {
        "check": "Ar40.cur>8e5",
        "start": 20,
        "analysis_types": ["unknown", "blank_air"],
        "abbreviated_count_ratio": 0.5,
    }
    assert t[1] == {"check": "average(Ar40)>1e6", "start": 50, "frequency": 5, "window": 10}  # comp beats check

    term = doc["terminations"]
    assert term[0] == {"check": "L2(CDD).deflection==3250", "start": 5, "frequency": 10}
    assert term[1]["ntrips"] == 3 and term[1]["mapper"] == "x+1000"
    assert term[1]["name"] == "terminations:Ar36<0 #1" and term[2]["name"] == "terminations:Ar36<0 #2"

    assert doc["cancelations"][0]["check"] == "average(Ar40.bs)>100"

    actions = doc["actions"]
    assert actions == [{"check": "slope(Ar40)>100", "start": 50, "action": "run_hook action_0", "resume": True}]
    assert "sleep(10)" in res.hooks["action_0"] and "def action_0(api):" in res.hooks["action_0"]

    mods = doc["modifications"]
    assert mods[0] == {
        "check": "Ar40<1000",
        "start": 50,
        "abbreviated_count_ratio": 0.25,
        "action": "skip_n 2",
        "truncate": True,
    }
    assert mods[1]["action"] == "set_extract 10%,20%"
    assert len(mods) == 2

    assert doc["post_run"] == [{"check": "Ar40<100", "action": "repeat"}]

    report = "\n".join(res.report)
    for expected in [
        "actions[1]: skipped: action conditional without an action",
        "modifications[2]: skipped: unknown modification action 'Explode'",
        "post_run_actions[1]: skipped: post-run action 'notify_lab()'",
        "bogus_kind: skipped: unknown kind",
        "'.current' -> '.cur'",
        "'Set Extract' was broken in pychron",
    ]:
        assert expected in report, expected
    assert res.skipped == 3


def test_inline_run_value():
    res = imp.convert_inline("Ar40>1000,20")
    doc = tomllib.loads(res.toml)
    assert doc["truncations"] == [{"check": "Ar40>1000", "start": 20, "abbreviated_count_ratio": 0.5}]
    bad = imp.convert_inline("Ar40>1000")
    assert bad.skipped == 1 and "nothing converted" in bad.toml


def test_check_rewrites():
    assert imp.convert_check("bone.ig.pressure  >  1e-8")[0] == "gauge.ig.pressure > 1e-8"
    assert imp.convert_check("gauge.IG1.pressure>1")[0] == "gauge.IG1.pressure>1"  # already new form
    assert imp.convert_check("Ar40.current>1 and not Ar36.cur<0")[0] == "Ar40.cur>1 and not Ar36.cur<0"
    assert imp.convert_check("Ar40bs>1")[0] == "average(Ar40.bs)>1"
    assert imp.convert_check("max(Ar40>1")[1] == ["unbalanced parentheses: check by hand"]


def test_cli_writes_toml_and_hooks(tmp_path):
    out = tmp_path / "system.toml"
    assert imp.main([str(FIXTURES / "system_full.yaml"), "-o", str(out)]) == 0
    assert tomllib.loads(out.read_text())["truncations"]
    assert (tmp_path / "measurement_hooks" / "action_0.py").exists()
    with pytest.raises(SystemExit):
        imp.main([])


@pytest.mark.parametrize("name", ["docs_example.yaml", "test_configuration.yaml", "system_full.yaml"])
def test_converted_files_load_in_cpp(tmp_path, name):
    elctl = os.environ.get("ELCTL")
    if not elctl:
        pytest.skip("set ELCTL to a built elctl to check with the C++ loader")
    out = tmp_path / "out.toml"
    imp.main([str(FIXTURES / name), "-o", str(out)])
    proc = subprocess.run([elctl, "-c", str(tmp_path / "none.toml"), "conditionals-check", str(out)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
