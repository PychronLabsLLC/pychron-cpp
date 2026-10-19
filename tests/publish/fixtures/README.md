# Release format fixtures

`table-inputs.rows_sha256`: the canonical row hashes (data publishing spec
5.7) of the five tables built from the fixed in-memory inputs of
`tests/publish/table_inputs.hpp`, one line per table: name, row count,
SHA-256 hex. Written on macOS (Apple clang, arm64) with

    PYCHRON_WRITE_FIXTURE=1 build/dev/tests/publish/pychron_publish_tests --gtest_filter='Tables.RowHashesEqualTheCommittedFixture'

and compared on every platform by that test: a row hash that differs on
Windows or Linux means the canonical text, or a reduced value, is not the
same bytes there, which is what the manifest's `rows_sha256` promises
(root `AGENTS.md`: a new stored format is checked on Windows CI against a
fixture written elsewhere). Rewrite it only when the tables' columns or the
inputs change on purpose, and say so in the commit.
