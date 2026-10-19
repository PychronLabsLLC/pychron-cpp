"""Tests for tools/pychron_measurement_import.py.

Fixtures are written here in the legacy formats (hops tuples, hops YAML,
measurement scripts with an `mx` docstring, a syn_extraction YAML). Each
conversion is compared with the worked plan of the measurement plan v2 spec
it stands for (tests/experiment/fixtures/plans/valid/v2_7*.toml), below the
[plan] and [equilibration] tables. When ELCTL points at a built elctl, every
converted plan is also loaded by `elctl exp validate`, i.e. by the real C++
plan parser.
"""

import os
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pychron_measurement_import as imp  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "measurement"
ROOT = Path(__file__).resolve().parents[2]
WORKED = ROOT / "tests" / "experiment" / "fixtures" / "plans" / "valid"


def body(doc: dict) -> dict:
    return {k: v for k, v in doc.items() if k not in ("plan", "equilibration")}


def worked(name: str) -> dict:
    return body(tomllib.loads((WORKED / f"{name}.toml").read_text()))


def convert(name, **kw):
    res = imp.convert(FIXTURES / name, **kw)
    return res, tomllib.loads(res.toml)


def test_hops_with_mass_35_is_worked_plan_7_2():
    res, doc = convert("hops_with_mass_35.txt", cycles=20, integration=1)
    assert body(doc) == worked("v2_72_argus_hop_fifth")
    assert doc["plan"]["instrument_family"] == "thermo_argus"


def test_hops_file_alone_reports_its_cycles():
    res, doc = convert("hops_with_mass_35.txt")
    assert doc["segment"][0]["cycles"] == 1
    assert "hops_with_mass_35.txt: cycles = 1 (pass --cycles)" in res.report


def test_ic_script_and_hops_are_worked_plan_7_3():
    res, doc = convert("jan_ic_peak_hop3.py", hops=FIXTURES / "ic_hops.txt")
    assert body(doc) == worked("v2_73_argus_ic_table")
    assert doc["equilibration"] == {"inlet": "R", "outlet": "O", "time_s": 15, "inlet_delay_s": 3,
                                    "close_inlet": True}
    assert "hook" not in doc


def test_hops_yaml_is_worked_plan_7_4():
    res, doc = convert("argonH.yaml", reference="H2", cycles=10, family="thermo_helix")
    assert body(doc) == worked("v2_74_helix_hydride_hcl")
    assert doc["plan"]["instrument_family"] == "thermo_helix"


def test_argus_script_with_syn_extraction_is_worked_plan_7_1():
    res, doc = convert("argus_multicollect.py", syn=FIXTURES / "syn_extraction" / "default.yaml")
    assert body(doc) == worked("v2_71_argus_syn_baseline")
    assert doc["equilibration"] == {"inlet": "R", "outlet": "O", "time_s": "@extraction.eqtime",
                                    "inlet_delay_s": 3, "close_inlet": True}
    # Every guess is in the report.
    assert any("multicollect labels from the cup order" in line for line in res.report)
    assert any("on_inlet_close" in line for line in res.report)
    assert "hook" not in doc


def test_unmapped_command_becomes_hook_stub_and_report_line():
    res, doc = convert("unmapped.py")
    assert "line 17: gosub('warm_cdd'): hook stub" in res.report
    assert "line 21: set_accelerating_voltage(4500): hook stub" in res.report
    assert doc["hook"] == "unmapped_hook"
    stub = res.hooks["unmapped_hook"]
    assert "def before_main(api):\n    # line 17: gosub('warm_cdd')" in stub
    assert "def after_main(api):\n    # line 21: set_accelerating_voltage(4500)" in stub
    compile(stub, "unmapped_hook.py", "exec")  # the stub is Python
    # What was mapped still is.
    assert [s["kind"] for s in doc["segment"]] == ["signal"]
    assert doc["segment"][0]["positions"][0]["collect"] == {"Ar40": "H1", "Ar39": "AX"}


def test_cli_writes_plan_report_and_hooks(tmp_path):
    out = tmp_path / "plans" / "unmapped.toml"
    out.parent.mkdir()
    report = tmp_path / "report.txt"
    assert imp.main([str(FIXTURES / "unmapped.py"), "-o", str(out), "--report", str(report)]) == 0
    assert tomllib.loads(out.read_text())["hook"] == "unmapped_hook"
    assert (tmp_path / "plans" / "measurement_hooks" / "unmapped_hook.py").exists()
    assert "gosub('warm_cdd'): hook stub" in report.read_text()


CASES = [
    ("hops_with_mass_35.txt", dict(cycles=20, integration=1)),
    ("jan_ic_peak_hop3.py", dict(hops=FIXTURES / "ic_hops.txt")),
    ("argonH.yaml", dict(reference="H2", cycles=10, family="thermo_helix")),
    ("argus_multicollect.py", dict(syn=FIXTURES / "syn_extraction" / "default.yaml")),
    ("unmapped.py", {}),
]


@pytest.mark.skipif(not os.environ.get("ELCTL"), reason="set ELCTL to a built elctl to check with the C++ loader")
@pytest.mark.parametrize("name,kw", CASES)
def test_elctl_loads_every_conversion(tmp_path, name, kw):
    lab = tmp_path / "lab"
    shutil.copytree(ROOT / "configs" / "examples", lab)
    res = imp.convert(FIXTURES / name, **kw)
    plan = tomllib.loads(res.toml)["plan"]["name"]
    (lab / "plans" / f"{plan}.toml").write_text(res.toml)
    hooks = lab / "scripts" / "measurement_hooks"
    hooks.mkdir(parents=True, exist_ok=True)
    for hook, src in res.hooks.items():
        (hooks / f"{hook}.py").write_text(src)
    queue = lab / "queues" / "imported.toml"
    queue.write_text(
        '[queue]\nname = "imported"\nmass_spectrometer = "sim"\nusername = "test"\n\n'
        f'[[runs]]\nidentifier = "66001"\nextraction = {{ script = "sim_extract", duration = 1 }}\n'
        f'measurement = {{ plan = "{plan}" }}\n')
    # The line resolves the plan's aliases ('@extraction.eqtime').
    p = subprocess.run([os.environ["ELCTL"], "-c", str(lab / "extraction_line.toml"), "exp", "validate", str(queue),
                        "--lab", str(lab)],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
