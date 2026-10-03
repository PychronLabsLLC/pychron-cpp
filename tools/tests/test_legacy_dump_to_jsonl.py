"""Tests for tools/legacy_dump_to_jsonl.py.

fixtures/legacy_dump.sql is a hand-written dump in mysqldump's own shape.
fixtures/legacy_catalog.sql is the dump the C++ catalog adapter's fixture
(tests/dvc/fixtures/catalog) is converted from; one test here checks that the
checked-in fixture is what the converter writes.
"""

import base64
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import legacy_dump_to_jsonl as conv  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures"
CATALOG_FIXTURE = TOOLS.parent / "tests" / "dvc" / "fixtures" / "catalog"


def rows_of(outdir, table):
    text = (Path(outdir) / f"{table}.jsonl").read_text(encoding="utf-8")
    return [json.loads(line) for line in text.splitlines()]


class ConvertCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.out = self.tmp / "out"

    def dump(self, data, name="dump.sql"):
        path = self.tmp / name
        path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
        return path

    def convert(self, data):
        """Converts a dump given as text or bytes; returns the manifest."""
        return conv.convert(self.dump(data), self.out)

    def run_tool(self, *args):
        return subprocess.run(
            [sys.executable, str(TOOLS / "legacy_dump_to_jsonl.py"), *map(str, args)],
            capture_output=True,
            text=True,
        )


class FixtureDump(ConvertCase):
    def setUp(self):
        super().setUp()
        self.manifest = conv.convert(FIXTURES / "legacy_dump.sql", self.out)

    def test_row_counts_and_manifest(self):
        self.assertEqual(self.manifest["tables"], {"MaterialTbl": 4, "SampleTbl": 3})
        stored = json.loads((self.out / "MANIFEST.json").read_text(encoding="utf-8"))
        self.assertEqual(stored, self.manifest)
        digest = hashlib.sha256((FIXTURES / "legacy_dump.sql").read_bytes()).hexdigest()
        self.assertEqual(stored["sha256"], digest)
        self.assertEqual(len(rows_of(self.out, "MaterialTbl")), 4)
        self.assertEqual(len(rows_of(self.out, "SampleTbl")), 3)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["MANIFEST.json", "MaterialTbl.jsonl", "SampleTbl.jsonl"])

    def test_keys_are_the_columns_of_create_table_in_order(self):
        first = rows_of(self.out, "SampleTbl")[0]
        self.assertEqual(list(first), ["id", "name", "materialID", "lat", "lon", "note", "create_date", "kind"])
        self.assertEqual(
            self.manifest["columns"]["SampleTbl"],
            {
                "id": "int",
                "name": "varchar",
                "materialID": "int",
                "lat": "float",
                "lon": "double",
                "note": "varchar",
                "create_date": "datetime",
                "kind": "enum",
            },
        )

    def test_escaped_strings_round_trip(self):
        materials = rows_of(self.out, "MaterialTbl")
        self.assertEqual(materials[1]["name"], "Groundmass, 'fine' (sieved)")
        self.assertEqual(materials[2]["name"], 'It\'s; a "test"\\path\n2nd line\ttab')
        self.assertEqual(materials[2]["grainsize"], "")
        self.assertEqual(rows_of(self.out, "SampleTbl")[0]["note"], "Fish Canyon; (sanidine), 'neutron' fluence monitor")

    def test_null_and_numbers(self):
        materials = rows_of(self.out, "MaterialTbl")
        self.assertIsNone(materials[0]["grainsize"])
        samples = rows_of(self.out, "SampleTbl")
        self.assertEqual(samples[0]["id"], 1)
        self.assertIsInstance(samples[0]["id"], int)
        self.assertEqual(samples[0]["lon"], -106.93)
        self.assertEqual(samples[1]["lat"], -1.5e-3)
        self.assertEqual(samples[1]["lon"], 100.0)
        self.assertIsInstance(samples[1]["lon"], float)
        self.assertIsNone(samples[1]["materialID"])
        self.assertIsNone(samples[1]["note"])
        self.assertEqual(samples[2]["lon"], -0.25)
        self.assertEqual(samples[0]["create_date"], "2015-03-04 10:11:12")

    def test_session_time_zone_and_completion_are_recorded(self):
        self.assertEqual(self.manifest["time_zone"], "+00:00")
        self.assertTrue(self.manifest["dump_completed"])
        self.assertEqual(self.manifest["non_utf8_values"], 0)


class Literals(ConvertCase):
    def test_every_escape(self):
        self.convert(
            b"CREATE TABLE `t` (`a` text);\n"
            b"INSERT INTO `t` VALUES ('q\\'q'),('d\\\"d'),('b\\\\b'),('n\\nn'),('r\\rr'),('t\\tt'),('z\\0z'),"
            b"('s\\Zs'),('p\\%p\\_p'),('o\\xo'),('two''quotes'),(''),('\\b');\n"
        )
        got = [r["a"] for r in rows_of(self.out, "t")]
        self.assertEqual(
            got,
            ["q'q", 'd"d', "b\\b", "n\nn", "r\rr", "t\tt", "z\0z", "s\x1as", "p\\%p\\_p", "oxo", "two'quotes", "", "\b"],
        )

    def test_numbers(self):
        self.convert(
            "CREATE TABLE t (a int, b double, c double, d double, e bigint, f decimal(10,2), g double);\n"
            "INSERT INTO t VALUES (-7,-1.25,3e-10,-4.5E+3,18446744073709551615,12.50,.5);\n"
        )
        row = rows_of(self.out, "t")[0]
        self.assertEqual(row, {"a": -7, "b": -1.25, "c": 3e-10, "d": -4500.0, "e": 18446744073709551615, "f": 12.5, "g": 0.5})
        self.assertIsInstance(row["a"], int)
        self.assertIsInstance(row["d"], float)
        self.assertIsInstance(row["e"], int)

    def test_a_number_json_cannot_hold_stays_text(self):
        self.convert("CREATE TABLE t (a double, b double);\nINSERT INTO t VALUES (1e999,-1e999);\n")
        self.assertEqual(rows_of(self.out, "t")[0], {"a": "1e999", "b": "-1e999"})

    def test_binary_values_are_base64_under_a_suffixed_key(self):
        payload = bytes(range(256))
        self.convert(
            b"CREATE TABLE `t` (`id` int, `a` blob, `b` blob, `c` blob, `d` blob, `e` varchar(8));\n"
            b"INSERT INTO `t` VALUES (1,0x" + payload.hex().upper().encode() + b",_binary 'ab\\0\\'c',X'00ff',NULL,_utf8'text'),"
            b"(2,0xabc,_binary '',x'',0x00,_latin1 'x');\n"
        )
        first, second = rows_of(self.out, "t")
        self.assertEqual(list(first), ["id", "a__base64", "b__base64", "c__base64", "d", "e"])
        self.assertEqual(base64.b64decode(first["a__base64"]), payload)
        self.assertEqual(base64.b64decode(first["b__base64"]), b"ab\0'c")
        self.assertEqual(base64.b64decode(first["c__base64"]), b"\x00\xff")
        self.assertIsNone(first["d"])
        self.assertEqual(first["e"], "text")
        self.assertEqual(base64.b64decode(second["a__base64"]), b"\x0a\xbc")  # an odd digit count is left-padded
        self.assertEqual(second["b__base64"], "")
        self.assertEqual(second["c__base64"], "")
        self.assertEqual(base64.b64decode(second["d__base64"]), b"\x00")
        self.assertEqual(second["e"], "x")

    def test_bit_literals_are_integers(self):
        self.convert("CREATE TABLE t (a bit(4), b bit(1));\nINSERT INTO t VALUES (b'1010',B'0');\n")
        self.assertEqual(rows_of(self.out, "t")[0], {"a": 10, "b": 0})

    def test_utf8_and_latin1_text(self):
        manifest = self.convert(
            b"CREATE TABLE `t` (`a` varchar(40));\n"
            b"INSERT INTO `t` VALUES ('caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d'),('caf\xe9'),('\x93smart\x94'),('odd \x81 byte');\n"
        )
        got = [r["a"] for r in rows_of(self.out, "t")]
        self.assertEqual(got, ["café \u201cq\u201d", "café", "\u201csmart\u201d", "odd \x81 byte"])
        self.assertEqual(manifest["non_utf8_values"], 3)
        # The output itself is UTF-8.
        (self.out / "t.jsonl").read_bytes().decode("utf-8")

    def test_unknown_bare_token_stays_text(self):
        self.convert("CREATE TABLE t (a int, b int);\nINSERT INTO t VALUES (CURRENT_TIMESTAMP,null);\n")
        self.assertEqual(rows_of(self.out, "t")[0], {"a": "CURRENT_TIMESTAMP", "b": None})


class Statements(ConvertCase):
    def test_explicit_column_list_wins_over_create_table(self):
        manifest = self.convert(
            "CREATE TABLE `t` (`a` int, `b` varchar(8), `c` int);\n"
            "INSERT INTO `t` (`c`, `a`) VALUES (3,1),(30,10);\n"
            "insert  ignore into t(b) values ('x') ;\n"
            "REPLACE INTO `t` VALUES (7,'y',9);\n"
        )
        self.assertEqual(
            rows_of(self.out, "t"),
            [{"c": 3, "a": 1}, {"c": 30, "a": 10}, {"b": "x"}, {"a": 7, "b": "y", "c": 9}],
        )
        self.assertEqual(manifest["tables"], {"t": 4})

    def test_insert_without_any_column_names(self):
        manifest = self.convert("INSERT INTO `t` VALUES (1,'a');\n")
        self.assertEqual(rows_of(self.out, "t"), [{"column_1": 1, "column_2": "a"}])
        self.assertEqual(manifest["columns"], {})

    def test_more_values_than_columns(self):
        self.convert("CREATE TABLE t (a int);\nINSERT INTO t VALUES (1,2,3);\n")
        self.assertEqual(rows_of(self.out, "t"), [{"a": 1, "column_2": 2, "column_3": 3}])

    def test_table_without_rows_gets_an_empty_file(self):
        manifest = self.convert("CREATE TABLE `empty` (`id` int);\nCREATE TABLE `t` (`id` int);\nINSERT INTO `t` VALUES (1);\n")
        self.assertEqual(manifest["tables"], {"empty": 0, "t": 1})
        self.assertEqual((self.out / "empty.jsonl").read_bytes(), b"")

    def test_database_qualified_and_unquoted_names(self):
        self.convert(
            "CREATE TABLE IF NOT EXISTS `db`.`t` (id int NOT NULL, `na``me` varchar(8), PRIMARY KEY (id));\n"
            "INSERT INTO db.t VALUES (1,'a');\n"
        )
        self.assertEqual(rows_of(self.out, "t"), [{"id": 1, "na`me": "a"}])

    def test_other_statements_and_comments_are_skipped(self):
        manifest = self.convert(
            "# a comment; with a semicolon\n"
            "-- another; 'unbalanced\n"
            "/* block ; ' comment */\n"
            "SET @x = 'a;b';\n"
            "USE `pychron`;\n"
            "DROP TABLE IF EXISTS `t`;\n"
            "CREATE TABLE `t` (`id` int, `s` varchar(9) DEFAULT ');' COMMENT 'x, `y` int');\n"
            "LOCK TABLES `t` WRITE;\n"
            "/*!40000 ALTER TABLE `t` DISABLE KEYS */;\n"
            "INSERT INTO `t` VALUES (1,'-- not a comment'),(2,'/* nor this */') , (3 , '#' ) ;\n"
            "UNLOCK TABLES;\n"
            "DELIMITER ;;\n"
            "CREATE TRIGGER trg BEFORE INSERT ON t FOR EACH ROW BEGIN INSERT INTO `t` VALUES (99,'no'); END ;;\n"
            "DELIMITER ;\n"
            "CREATE VIEW v AS SELECT 1;\n"
        )
        self.assertEqual(manifest["tables"], {"t": 3})
        self.assertEqual([r["s"] for r in rows_of(self.out, "t")], ["-- not a comment", "/* nor this */", "#"])
        self.assertEqual(manifest["columns"]["t"], {"id": "int", "s": "varchar"})
        self.assertIsNone(manifest["time_zone"])
        self.assertFalse(manifest["dump_completed"])

    def test_windows_line_endings(self):
        self.convert(b"CREATE TABLE `t` (\r\n  `id` int\r\n);\r\nINSERT INTO `t` VALUES (1),\r\n(2);\r\n")
        self.assertEqual(rows_of(self.out, "t"), [{"id": 1}, {"id": 2}])

    def test_table_created_again_starts_over(self):
        manifest = self.convert(
            "CREATE TABLE t (a int);\nINSERT INTO t VALUES (1),(2);\n"
            "DROP TABLE IF EXISTS t;\nCREATE TABLE t (b int);\nINSERT INTO t VALUES (3);\n"
        )
        self.assertEqual(manifest["tables"], {"t": 1})
        self.assertEqual(rows_of(self.out, "t"), [{"b": 3}])

    def test_table_names_that_are_not_file_names(self):
        manifest = self.convert("CREATE TABLE `../up` (a int);\nINSERT INTO `../up` VALUES (1);\n")
        self.assertEqual(manifest["tables"], {"../up": 1})
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["%2E%2E%2Fup.jsonl", "MANIFEST.json"])
        self.assertFalse((self.tmp / "up.jsonl").exists())


class Streaming(ConvertCase):
    def test_one_long_insert_line_across_buffer_boundaries(self):
        # Every construct is cut by a buffer boundary somewhere: the buffer is
        # far smaller than the statement.
        rows = 5000
        values = ",".join(
            "(%d,'row \\'%d\\', (x)',%s,-%d.5e-3,0x%s)" % (i, i, "NULL" if i % 3 else "'it''s'", i, ("%04x" % i))
            for i in range(rows)
        )
        dump = "CREATE TABLE `t` (`id` int, `s` text, `n` text, `f` double, `b` blob);\nINSERT INTO `t` VALUES " + values + ";\n"
        for chunk in (7, 64, 1 << 20):
            with self.subTest(chunk=chunk):
                manifest = conv.convert(self.dump(dump), self.out, chunk_size=chunk)
                self.assertEqual(manifest["tables"], {"t": rows})
                got = rows_of(self.out, "t")
                self.assertEqual(len(got), rows)
                for i in (0, 1, 2, 4998, 4999):
                    self.assertEqual(got[i]["id"], i)
                    self.assertEqual(got[i]["s"], "row '%d', (x)" % i)
                    self.assertEqual(got[i]["n"], None if i % 3 else "it's")
                    self.assertEqual(got[i]["f"], float("-%d.5e-3" % i))
                    self.assertEqual(base64.b64decode(got[i]["b__base64"]), bytes.fromhex("%04x" % i))

    def test_the_buffer_does_not_grow_with_the_statement(self):
        values = ",".join("(%d,'%s')" % (i, "x" * 50) for i in range(20000))
        path = self.dump("CREATE TABLE t (id int, s text);\nINSERT INTO t VALUES " + values + ";\n")
        self.assertGreater(path.stat().st_size, 1_000_000)
        source_sizes = []
        original = conv._Source._more

        def spy(source):
            source_sizes.append(len(source.buf) - source.pos)
            return original(source)

        conv._Source._more = spy
        try:
            conv.convert(path, self.out, chunk_size=4096)
        finally:
            conv._Source._more = original
        self.assertLess(max(source_sizes), 4096)


class Failures(ConvertCase):
    def assert_exit_2(self, result, needle):
        self.assertEqual(result.returncode, 2, result.stderr)
        lines = result.stderr.strip().splitlines()
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertIn(needle, lines[0])
        self.assertFalse((self.out / "MANIFEST.json").exists())

    def test_empty_dump_is_exit_2(self):
        self.assert_exit_2(self.run_tool(self.dump(b""), self.out), "empty")

    def test_missing_dump_is_exit_2(self):
        self.assert_exit_2(self.run_tool(self.tmp / "nope.sql", self.out), "cannot read")

    def test_dump_without_tables_is_exit_2(self):
        self.assert_exit_2(self.run_tool(self.dump("-- nothing here\nSET NAMES utf8;\n"), self.out), "no tables")

    def test_truncated_dump_is_exit_2_and_leaves_no_manifest(self):
        whole = (FIXTURES / "legacy_dump.sql").read_bytes()
        cut = whole[: whole.index(b"'Biotite'") + 4]
        self.assert_exit_2(self.run_tool(self.dump(cut), self.out), "malformed")

    def test_compressed_dump_is_exit_2(self):
        import gzip

        self.assert_exit_2(self.run_tool(self.dump(gzip.compress(b"CREATE TABLE t (a int);")), self.out), "gzip")

    def test_a_failed_conversion_removes_the_manifest_of_an_earlier_one(self):
        self.assertEqual(self.run_tool(FIXTURES / "legacy_dump.sql", self.out).returncode, 0)
        self.assertTrue((self.out / "MANIFEST.json").exists())
        self.assert_exit_2(self.run_tool(self.dump("CREATE TABLE t (a int);\nINSERT INTO t VALUES (1,'open"), self.out), "malformed")

    def test_command_line_reports_what_it_wrote(self):
        result = self.run_tool(FIXTURES / "legacy_dump.sql", self.out)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 tables, 7 rows", result.stdout)
        self.assertEqual(json.loads((self.out / "MANIFEST.json").read_text())["tables"], {"MaterialTbl": 4, "SampleTbl": 3})


class CatalogFixture(ConvertCase):
    """tests/dvc/fixtures/catalog is the converter's output for fixtures/legacy_catalog.sql."""

    def test_checked_in_fixture_is_current(self):
        conv.convert(FIXTURES / "legacy_catalog.sql", self.out)
        written = sorted(p.name for p in self.out.iterdir())
        self.assertEqual(written, sorted(p.name for p in CATALOG_FIXTURE.iterdir() if p.name != "README.md"))
        for name in written:
            self.assertEqual((self.out / name).read_bytes(), (CATALOG_FIXTURE / name).read_bytes(), name)


if __name__ == "__main__":
    unittest.main()
