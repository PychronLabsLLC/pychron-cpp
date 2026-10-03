#!/usr/bin/env python3
"""Generate the SQLite DDL from the PostgreSQL migration.

DVC schema spec 2026-10-01, section 11.3. The PostgreSQL file under
libs/persistence/migrations/pg/ is the single source of the schema; this tool
writes the matching file under migrations/sqlite/ and the output is committed.

    python3 tools/ddl_sqlite.py            # regenerate every migration
    python3 tools/ddl_sqlite.py --check    # exit 1 if a committed file is stale

Mechanical mapping (SQLite tables are STRICT, so SQLite >= 3.37):

    uuid            TEXT    CHECK (length(x) = 36)
    timestamptz     TEXT    CHECK (x LIKE '____-__-__T__:__:__%Z')
    date            TEXT
    jsonb           TEXT    CHECK (json_valid(x))
    bytea           BLOB
    double precision REAL
    int / bigint    INTEGER
    boolean         INTEGER CHECK (x IN (0, 1))

Non-mechanical rules:

    * PRIMARY KEY columns get NOT NULL (PostgreSQL implies it; SQLite does not).
    * ALTER TABLE ... ADD FOREIGN KEY folds into the CREATE TABLE as a table
      constraint, DEFERRABLE INITIALLY DEFERRED.
    * CHECKs that call PostgreSQL-only functions (convert_to, sha256) are
      dropped; the access layer verifies them instead (signal_blob hash).
    * DEFAULT now() becomes the microsecond-free ISO-8601 UTC strftime.
    * ALTER TABLE ... ADD COLUMN stays an ALTER TABLE (the column is mapped like
      any other); the table itself comes from an earlier migration.
    * CREATE FUNCTION is dropped. CREATE TRIGGER ... EXECUTE FUNCTION
      forbid_mutation() becomes BEFORE UPDATE and BEFORE DELETE triggers that
      RAISE(ABORT). A '-- @sqlite guard <table> allow <cols...>' directive turns
      the matching trigger into: no DELETE, and no UPDATE that changes any
      column outside the allow list.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PG_DIR = ROOT / "libs/persistence/migrations/pg"
SQLITE_DIR = ROOT / "libs/persistence/migrations/sqlite"

NOW_SQLITE = "(strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
TABLE_CONSTRAINT = re.compile(r"^(PRIMARY\s+KEY|UNIQUE|FOREIGN\s+KEY|CHECK|CONSTRAINT)\b", re.I)
PG_ONLY_CALLS = ("convert_to(", "sha256(")


def strip_comments(sql: str) -> tuple[str, list[str]]:
    """Remove -- comments outside quotes; return the text and any @sqlite directives."""
    out, directives = [], []
    for line in sql.splitlines():
        result, in_quote, i = [], False, 0
        while i < len(line):
            c = line[i]
            if c == "'":
                in_quote = not in_quote
            if not in_quote and line.startswith("--", i):
                comment = line[i + 2 :].strip()
                if comment.startswith("@sqlite"):
                    directives.append(comment[len("@sqlite") :].strip())
                break
            result.append(c)
            i += 1
        out.append("".join(result).rstrip())
    return "\n".join(out), directives


def split_statements(sql: str) -> list[str]:
    """Split on ';' outside quotes and $$ bodies."""
    stmts, buf, in_quote, in_dollar, i = [], [], False, False, 0
    while i < len(sql):
        if not in_quote and sql.startswith("$$", i):
            in_dollar = not in_dollar
            buf.append("$$")
            i += 2
            continue
        c = sql[i]
        if c == "'" and not in_dollar:
            in_quote = not in_quote
        if c == ";" and not in_quote and not in_dollar:
            stmt = "".join(buf).strip()
            if stmt:
                stmts.append(stmt)
            buf = []
        else:
            buf.append(c)
        i += 1
    if "".join(buf).strip():
        raise SystemExit("ddl_sqlite: trailing statement without ';'")
    return stmts


def split_top_level(body: str) -> list[str]:
    parts, depth, buf, in_quote = [], 0, [], False
    for c in body:
        if c == "'":
            in_quote = not in_quote
        if not in_quote:
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == "," and depth == 0:
                parts.append(" ".join("".join(buf).split()))
                buf = []
                continue
        buf.append(c)
    if "".join(buf).strip():
        parts.append(" ".join("".join(buf).split()))
    return parts


def remove_pg_only_checks(text: str) -> str:
    """Drop CHECK (...) groups that call PostgreSQL-only functions."""
    out, i = [], 0
    while True:
        m = re.search(r"\bCHECK\s*\(", text[i:], re.I)
        if not m:
            out.append(text[i:])
            break
        start = i + m.start()
        j, depth = i + m.end(), 1
        while depth:
            depth += {"(": 1, ")": -1}.get(text[j], 0)
            j += 1
        group = text[start:j]
        out.append(text[i:start])
        if not any(call in group for call in PG_ONLY_CALLS):
            out.append(group)
        i = j
    return " ".join("".join(out).split())


TYPE_MAP = [
    # (pattern at start of the remainder, sqlite type, check template or None)
    (r"double precision\b", "REAL", None),
    (r"timestamptz\b", "TEXT", "{c} LIKE '____-__-__T__:__:__%Z'"),
    (r"uuid\b", "TEXT", "length({c}) = 36"),
    (r"jsonb\b", "TEXT", "json_valid({c})"),
    (r"bytea\b", "BLOB", None),
    (r"bigint\b", "INTEGER", None),
    (r"int\b", "INTEGER", None),
    (r"boolean\b", "INTEGER", "{c} IN (0, 1)"),
    (r"text\b", "TEXT", None),
    (r"date\b", "TEXT", None),
]


class Table:
    def __init__(self, name: str, items: list[str]):
        self.name = name
        self.columns: list[str] = []
        self.pk: list[str] = []
        self.items = items
        for item in items:
            m = re.match(r"PRIMARY\s+KEY\s*\(([^)]*)\)", item, re.I)
            if m:
                self.pk = [c.strip() for c in m.group(1).split(",")]
            elif not TABLE_CONSTRAINT.match(item):
                col = item.split()[0]
                self.columns.append(col)
                if re.search(r"\bPRIMARY\s+KEY\b", item, re.I):
                    self.pk = [col]
        self.extra_constraints: list[str] = []

    def render(self) -> str:
        lines = [r for r in (self.render_item(item) for item in self.items) if r] + self.extra_constraints
        return f"CREATE TABLE {self.name} (\n  " + ",\n  ".join(lines) + "\n) STRICT"

    def render_item(self, item: str) -> str:
        item = remove_pg_only_checks(item)
        if not item or TABLE_CONSTRAINT.match(item):
            return item
        col, rest = item.split(None, 1)
        for pattern, sqlite_type, check in TYPE_MAP:
            m = re.match(pattern, rest, re.I)
            if m:
                rest = rest[m.end() :].strip()
                break
        else:
            raise SystemExit(f"ddl_sqlite: {self.name}.{col}: unmapped type in '{item}'")
        rest = re.sub(r"DEFAULT now\(\)", f"DEFAULT {NOW_SQLITE}", rest, flags=re.I)
        rest = re.sub(r"DEFAULT false\b", "DEFAULT 0", rest, flags=re.I)
        rest = re.sub(r"DEFAULT true\b", "DEFAULT 1", rest, flags=re.I)
        parts = [col, sqlite_type]
        if col in self.pk and not re.search(r"\bNOT NULL\b", rest, re.I):
            parts.append("NOT NULL")
        if rest:
            parts.append(rest)
        if check and "GENERATED" not in rest.upper():
            parts.append(f"CHECK ({check.format(c=col)})")
        return " ".join(parts)


def convert(pg_sql: str) -> str:
    text, directives = strip_comments(pg_sql)
    guards: dict[str, list[str]] = {}
    for d in directives:
        m = re.match(r"guard\s+(\w+)\s+allow\s+(.+)$", d)
        if not m:
            raise SystemExit(f"ddl_sqlite: unknown directive '@sqlite {d}'")
        guards[m.group(1)] = m.group(2).split()

    tables: dict[str, Table] = {}
    ordered: list[object] = []  # Table or str
    for stmt in split_statements(text):
        flat = " ".join(stmt.split())
        if m := re.match(r"CREATE TABLE (\w+) \((.*)\)$", flat, re.S):
            t = Table(m.group(1), split_top_level(m.group(2)))
            tables[t.name] = t
            ordered.append(t)
        elif m := re.match(r"ALTER TABLE (\w+) (ADD FOREIGN KEY .*)$", flat):
            for clause in split_top_level(m.group(2)):
                fk = re.sub(r"^ADD ", "", clause)
                tables[m.group(1)].extra_constraints.append(f"{fk} DEFERRABLE INITIALLY DEFERRED")
        elif m := re.match(r"ALTER TABLE (\w+) ADD COLUMN (.*)$", flat):
            # The table was created by an earlier migration, so there is no
            # Table to fold into; render the column on its own. Added columns
            # cannot be primary keys.
            column = Table(m.group(1), []).render_item(m.group(2))
            ordered.append(f"ALTER TABLE {m.group(1)} ADD COLUMN {column}")
        elif flat.startswith("CREATE FUNCTION"):
            continue
        elif m := re.match(
            r"CREATE TRIGGER (\w+) BEFORE UPDATE OR DELETE ON (\w+) FOR EACH ROW EXECUTE FUNCTION (\w+)\(\)$", flat
        ):
            name, table, func = m.groups()
            if func == "forbid_mutation":
                ordered.append(append_only_triggers(table))
            elif table in guards:
                ordered.append(guard_triggers(tables[table], guards[table]))
            else:
                raise SystemExit(f"ddl_sqlite: no SQLite translation for trigger {name}")
        elif flat.startswith(("CREATE INDEX", "CREATE UNIQUE INDEX", "INSERT INTO")):
            ordered.append(flat)
        else:
            raise SystemExit(f"ddl_sqlite: unsupported statement: {flat[:80]}")

    out = []
    for item in ordered:
        out.append(item.render() if isinstance(item, Table) else item)
    return ";\n".join(out) + ";\n"


def append_only_triggers(table: str) -> str:
    msg = f"pychron: {table} is append-only"
    return (
        f"CREATE TRIGGER {table}_no_update BEFORE UPDATE ON {table} BEGIN SELECT RAISE(ABORT, '{msg}'); END;\n"
        f"CREATE TRIGGER {table}_no_delete BEFORE DELETE ON {table} BEGIN SELECT RAISE(ABORT, '{msg}'); END"
    )


def guard_triggers(table: Table, allow: list[str]) -> str:
    unknown = set(allow) - set(table.columns)
    if unknown:
        raise SystemExit(f"ddl_sqlite: guard {table.name}: unknown columns {sorted(unknown)}")
    frozen = [c for c in table.columns if c not in allow]
    cond = " OR ".join(f"NEW.{c} IS NOT OLD.{c}" for c in frozen)
    msg = f"pychron: {table.name} is append-only"
    return (
        f"CREATE TRIGGER {table.name}_guard_update BEFORE UPDATE ON {table.name} WHEN {cond} "
        f"BEGIN SELECT RAISE(ABORT, '{msg}'); END;\n"
        f"CREATE TRIGGER {table.name}_no_delete BEFORE DELETE ON {table.name} "
        f"BEGIN SELECT RAISE(ABORT, '{msg}'); END"
    )


HEADER = (
    "-- GENERATED by tools/ddl_sqlite.py from migrations/pg/{name}. Do not edit;\n"
    "-- edit the PostgreSQL migration and regenerate.\n"
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if a generated file is out of date")
    args = ap.parse_args()
    stale = []
    for pg in sorted(PG_DIR.glob("*.sql")):
        generated = HEADER.format(name=pg.name) + convert(pg.read_text())
        target = SQLITE_DIR / pg.name
        if args.check:
            if not target.exists() or target.read_text() != generated:
                stale.append(target)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(generated)
    for s in stale:
        print(f"stale: {s.relative_to(ROOT)} (run tools/ddl_sqlite.py)", file=sys.stderr)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
