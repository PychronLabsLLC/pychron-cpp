"""Tests for tools/quality_check.py (stdlib unittest; run by ctest).

Only what the script decides itself: which lines a diff changed, what a tool's
output says, which sources stand in for a header. Neither cppcheck nor
clang-tidy is run.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import quality_check as qc  # noqa: E402

DIFF = """\
diff --git a/libs/core/src/a.cpp b/libs/core/src/a.cpp
--- a/libs/core/src/a.cpp
+++ b/libs/core/src/a.cpp
@@ -10,0 +11,3 @@ void f()
+one
+two
+three
@@ -20 +23 @@ void g()
-old
+new
@@ -30,2 +32,0 @@ void h()
-gone
-gone
diff --git a/libs/core/src/removed.cpp b/libs/core/src/removed.cpp
--- a/libs/core/src/removed.cpp
+++ /dev/null
@@ -1,2 +0,0 @@
-x
-y
diff --git a/libs/core/include/pychron/core/b.hpp b/libs/core/include/pychron/core/b.hpp
--- /dev/null
+++ b/libs/core/include/pychron/core/b.hpp
@@ -0,0 +1,4 @@
+a
+b
+c
+d
"""


class ChangedLines(unittest.TestCase):
    def test_added_and_changed_lines_are_ranges_of_the_new_file(self):
        changed = qc.parse_changed_lines(DIFF)
        self.assertEqual(changed["libs/core/src/a.cpp"], [(11, 13), (23, 23)])
        self.assertEqual(changed["libs/core/include/pychron/core/b.hpp"], [(1, 4)])

    def test_a_deleted_file_is_not_listed(self):
        self.assertNotIn("libs/core/src/removed.cpp", qc.parse_changed_lines(DIFF))
        self.assertNotIn("/dev/null", qc.parse_changed_lines(DIFF))

    def test_in_ranges(self):
        self.assertTrue(qc.in_ranges(12, [(11, 13)]))
        self.assertFalse(qc.in_ranges(14, [(11, 13)]))
        self.assertTrue(qc.in_ranges(999, None))  # a new file: every line

    def test_first_party_is_cpp_under_libs_apps_tests(self):
        self.assertTrue(qc.is_first_party("libs/core/src/a.cpp"))
        self.assertTrue(qc.is_first_party("tests/core/fake.hpp"))
        self.assertFalse(qc.is_first_party("build/dev/_deps/x/a.cpp"))
        self.assertFalse(qc.is_first_party("tools/quality_check.py"))


class Findings(unittest.TestCase):
    def test_clang_tidy_line(self):
        output = (
            f"{qc.ROOT}/libs/core/src/a.cpp:57:14: warning: use nullptr [modernize-use-nullptr]\n"
            "   57 |   if (out == NULL) return 1;\n"
            "      |              ^~~~\n"
            f"{qc.ROOT}/libs/core/src/a.cpp:9:1: note: declared here\n"
        )
        self.assertEqual(
            qc.parse_findings(output, "clang-tidy"),
            [qc.Finding("libs/core/src/a.cpp", 57, 14, "warning", "clang-tidy", "modernize-use-nullptr", "use nullptr")],
        )

    def test_cppcheck_line_with_brackets_in_the_message(self):
        output = "libs/core/src/a.cpp:57:33: error: Array 'buffer[8]' accessed at index 9. [arrayIndexOutOfBounds]\n"
        (finding,) = qc.parse_findings(output, "cppcheck")
        self.assertEqual(finding.check, "arrayIndexOutOfBounds")
        self.assertEqual(finding.message, "Array 'buffer[8]' accessed at index 9.")
        self.assertEqual(finding.text(), output.strip())

    def test_aliased_checks_are_one_finding(self):
        output = "libs/a.cpp:1:2: warning: narrowing [bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions]\n"
        (finding,) = qc.parse_findings(output, "clang-tidy")
        self.assertEqual(finding.check, "bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions")


class HeaderSources(unittest.TestCase):
    SOURCES = ["libs/core/src/b.cpp", "libs/devices/src/uses_b.cpp", "tests/core/test_b.cpp", "tests/core/other.cpp"]
    TEXTS = {
        "libs/core/src/b.cpp": '#include "pychron/core/b.hpp"\n',
        "libs/devices/src/uses_b.cpp": '#include <pychron/core/b.hpp>\n',
        "tests/core/test_b.cpp": '#  include "pychron/core/b.hpp"\n#include "fake.hpp"\n',
        "tests/core/other.cpp": '#include "pychron/core/bb.hpp"\n#include "not_fake.hpp"\n',
    }

    def test_a_public_header_is_named_from_below_include(self):
        self.assertEqual(qc.include_spelling("libs/core/include/pychron/core/b.hpp"), "pychron/core/b.hpp")
        self.assertEqual(qc.include_spelling("tests/core/fake.hpp"), "fake.hpp")

    def test_the_headers_own_component_comes_first(self):
        users = qc.sources_including("libs/core/include/pychron/core/b.hpp", self.SOURCES, dict(self.TEXTS))
        self.assertEqual(users, ["libs/core/src/b.cpp", "libs/devices/src/uses_b.cpp"])

    def test_a_longer_name_ending_the_same_is_not_a_match(self):
        users = qc.sources_including("tests/core/fake.hpp", self.SOURCES, dict(self.TEXTS))
        self.assertEqual(users, ["tests/core/test_b.cpp"])


if __name__ == "__main__":
    unittest.main()
