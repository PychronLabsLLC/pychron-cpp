"""Tests for tools/compiler_sweep.py (stdlib unittest; run by ctest).

Only what the script decides itself: which sources a branch's changes reach, read from Ninja's dependency
log. No compiler is run, and git and ninja are stood in for.
"""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import compiler_sweep as cs  # noqa: E402

BUILD = Path("/repo/build/dev")

DEPS = """\
libs/core/CMakeFiles/pychron_core.dir/src/a.cpp.o: #deps 3, deps mtime 1 (VALID)
    ../../libs/core/src/a.cpp
    ../../libs/core/include/pychron/core/a.hpp
    ../../libs/core/include/pychron/core/base.hpp

libs/core/CMakeFiles/pychron_core.dir/src/b.cpp.o: #deps 2, deps mtime 1 (VALID)
    ../../libs/core/src/b.cpp
    /usr/include/c++/v1/string

libs/core/CMakeFiles/pychron_core.dir/src/c.cpp.o: #deps 2, deps mtime 1 (STALE)
    ../../libs/core/src/c.cpp
    ../../libs/core/include/pychron/core/a.hpp

libs/core/CMakeFiles/pychron_core.dir/src/e.cpp.o: #deps 0, deps mtime 0 (VALID)

"""


def entry(name: str, output: bool = True) -> dict:
    e = {"directory": str(BUILD), "file": f"/repo/libs/core/src/{name}.cpp",
         "command": f"c++ -Werror -c /repo/libs/core/src/{name}.cpp"}
    if output:
        e["output"] = f"libs/core/CMakeFiles/pychron_core.dir/src/{name}.cpp.o"
    return e


ENTRIES = [entry(n) for n in ("a", "b", "c", "d", "e")]


def names(selected: list[dict]) -> list[str]:
    return sorted(Path(e["file"]).stem for e in selected)


class ParseDeps(unittest.TestCase):
    def test_records_by_object(self):
        deps = cs.parse_ninja_deps(DEPS, BUILD)
        a = deps["libs/core/CMakeFiles/pychron_core.dir/src/a.cpp.o"]
        self.assertIn(Path("/repo/libs/core/include/pychron/core/base.hpp"), a)
        self.assertIn(Path("/repo/libs/core/src/a.cpp"), a)

    def test_stale_and_empty_records_are_unknown(self):
        deps = cs.parse_ninja_deps(DEPS, BUILD)
        self.assertNotIn("libs/core/CMakeFiles/pychron_core.dir/src/c.cpp.o", deps)
        self.assertNotIn("libs/core/CMakeFiles/pychron_core.dir/src/e.cpp.o", deps)


class Select(unittest.TestCase):
    def select(self, changed: set[str], entries=ENTRIES, deps_text=DEPS):
        deps = cs.parse_ninja_deps(deps_text, BUILD) if deps_text is not None else None
        return cs.select_entries(entries, {Path("/repo") / c for c in changed}, deps)

    def test_own_file_changed(self):
        selected, why = self.select({"libs/core/src/b.cpp"})
        self.assertEqual(names(selected), ["b", "c", "d", "e"])   # c stale, d and e unknown
        self.assertIsNone(why)

    def test_header_reached_through_another_header(self):
        selected, _ = self.select({"libs/core/include/pychron/core/base.hpp"})
        self.assertEqual(names(selected), ["a", "c", "d", "e"])

    def test_unrelated_change_selects_only_unknowns(self):
        selected, _ = self.select({"docs/user/01.md"})
        self.assertEqual(names(selected), ["c", "d", "e"])

    def test_nothing_unknown_and_nothing_changed(self):
        selected, _ = self.select(set(), entries=[entry("a"), entry("b")])
        self.assertEqual(selected, [])

    def test_cmake_change_sweeps_everything(self):
        for changed in ("libs/core/CMakeLists.txt", "cmake/PychronWarnings.cmake", "CMakePresets.json"):
            selected, why = self.select({changed}, entries=[entry("a"), entry("b")])
            self.assertEqual(names(selected), ["a", "b"])
            self.assertIn(changed, why)

    def test_no_dependency_log_sweeps_everything(self):
        selected, why = self.select({"libs/core/src/b.cpp"}, entries=[entry("a"), entry("b")], deps_text=None)
        self.assertEqual(names(selected), ["a", "b"])
        self.assertIn("dependency log", why)

    def test_entries_without_output_sweep_everything(self):
        selected, why = self.select({"libs/core/src/b.cpp"}, entries=[entry("a", output=False), entry("b")])
        self.assertEqual(names(selected), ["a", "b"])
        self.assertIn("output", why)


class NinjaDeps(unittest.TestCase):
    def test_failing_ninja_is_none(self):
        failed = mock.Mock(returncode=1, stdout="", stderr="ninja: error: loading 'build.ninja'")
        with mock.patch.object(cs.shutil, "which", return_value="/usr/bin/ninja"), \
                mock.patch.object(cs.subprocess, "run", return_value=failed):
            self.assertIsNone(cs.ninja_deps(BUILD))

    def test_no_ninja_is_none(self):
        with mock.patch.object(cs.shutil, "which", return_value=None):
            self.assertIsNone(cs.ninja_deps(BUILD))


class Main(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.build = Path(self.tmp.name)
        (self.build / "compile_commands.json").write_text(json.dumps(
            [{**entry(n), "directory": str(self.build)} for n in ("a", "b")]))

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        ok = mock.Mock(returncode=0, stdout="", stderr="")
        deps = DEPS.replace("../../", "")
        with mock.patch.object(sys, "argv", ["compiler_sweep.py", "--build-dir", str(self.build),
                                             "--compiler", "true", *argv]), \
                mock.patch.object(cs, "ROOT", Path("/repo")), \
                mock.patch.object(cs, "changed_files", return_value={Path("/repo/libs/core/src/b.cpp")}), \
                mock.patch.object(cs, "ninja_deps", return_value=cs.parse_ninja_deps(deps, Path("/repo"))), \
                mock.patch.object(cs.subprocess, "run", return_value=ok), \
                redirect_stdout(out), redirect_stderr(err):
            status = cs.main()
        return status, err.getvalue()

    def test_changed_summary(self):
        status, err = self.run_main("--changed")
        self.assertEqual(status, 0)
        self.assertRegex(err, r": 1 of 2 sources \(changed since origin/develop\), 0 warning\(s\), 0 not checked")

    def test_whole_tree_summary_unchanged(self):
        status, err = self.run_main()
        self.assertEqual(status, 0)
        self.assertRegex(err, r": 2 sources, 0 warning\(s\), 0 not checked")

    def test_git_failure_cannot_run(self):
        with mock.patch.object(cs, "changed_files", side_effect=cs.CannotRun("git: unknown base")):
            out, err = io.StringIO(), io.StringIO()
            with mock.patch.object(sys, "argv", ["compiler_sweep.py", "--build-dir", str(self.build),
                                                 "--compiler", "true", "--changed", "--base", "nope"]), \
                    redirect_stdout(out), redirect_stderr(err):
                self.assertEqual(cs.main(), 2)
            self.assertIn("unknown base", err.getvalue())


if __name__ == "__main__":
    unittest.main()
