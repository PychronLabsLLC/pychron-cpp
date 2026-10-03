#!/usr/bin/env python3
"""Convert a mysqldump file of the legacy pychron database to JSON lines.

Legacy ingestion spec (docs/superpowers/specs/2026-10-03-legacy-ingestion-design.md)
section 4.2. The importer does not talk to MySQL: this tool turns a dump into
one file per table, and the C++ catalog adapter (libs/dvc, catalog_adapter.hpp)
reads those.

    python3 tools/legacy_dump_to_jsonl.py pychrondvc.sql catalog/

Output, in <outdir>:

  <Table>.jsonl   One JSON object per row, in dump order, one row per line,
                  UTF-8. Keys are the column names: those of the INSERT's own
                  column list, else those of the table's CREATE TABLE, in that
                  order. A value with no column name gets "column_<n>" (n from
                  1). A table that is created but has no rows gets an empty
                  file.
                    NULL                  -> null
                    number                -> number (an integer stays one; a
                                             number JSON cannot hold, such as
                                             1e999, stays text)
                    'text'                -> string, escapes resolved
                    0x4142, X'4142',      -> the bytes in base64, under the key
                    _binary '...'            "<column>__base64"
                    b'0101'               -> integer
                    anything else         -> string, as written
                  Text is decoded as UTF-8; text that is not UTF-8 is decoded
                  as Windows-1252 (what MySQL calls latin1), and the few bytes
                  that has no character for as ISO-8859-1, so no text makes
                  the conversion fail. Nothing marks such a value;
                  MANIFEST.json counts them.
  MANIFEST.json   Written last, so a directory without it is not a complete
                  conversion:
                    "tables"           {table: row count}
                    "files"            {table: file name}; "<table>.jsonl"
                                       unless the name has characters outside
                                       A-Z a-z 0-9 _ $ -, which become %XX
                    "sha256"           of the dump file
                    "columns"          {table: {column: type}} for tables with
                                       a CREATE TABLE; type is the first word
                                       of the column type, lower case
                    "time_zone"        the session time zone the dump sets
                                       (mysqldump: "+00:00", in which TIMESTAMP
                                       columns are written), or null
                    "dump_completed"   whether mysqldump's closing
                                       "-- Dump completed" line was seen
                    "non_utf8_values"  how many text values were not UTF-8

The dump is read as a stream: memory use does not grow with the size of the
dump or of a statement, only with the size of a single value.

Exit status: 0 on success; 2 with a one-line message when the dump cannot be
read, is empty, holds no table or is malformed (for instance cut short); 1
when the output cannot be written.

Standard library only.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import sys
from pathlib import Path

CHUNK_SIZE = 1 << 20
BINARY_SUFFIX = "__base64"

_SPACE = re.compile(rb"[ \t\r\n\f\v]+")
_WORD = re.compile(rb"[A-Za-z0-9_$]+")
_BARE_NAME = re.compile(rb"[A-Za-z0-9_$\x80-\xff]+")
_TOKEN = re.compile(rb"[^,()\s;'\"`]+")
_HEX = re.compile(rb"[0-9A-Fa-f]+")
_NOT_NEWLINE = re.compile(rb"[^\n]+")
_NOT_STAR = re.compile(rb"[^*]+")
_NOT_BACKTICK = re.compile(rb"[^`]+")
_STRING_RUN = {0x27: re.compile(rb"[^'\\]+"), 0x22: re.compile(rb'[^"\\]+')}
_STATEMENT_RUN = re.compile(rb"[^;'\"`/#-]+")
_DEFINITION_RUN = re.compile(rb"[^,()'\"`]+")
_INT = re.compile(rb"[-+]?[0-9]+")
_FLOAT = re.compile(rb"[-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?")
_SET_TIME_ZONE = re.compile(rb"SET\s+(?:SESSION\s+)?TIME_ZONE\s*=\s*'([^']*)'", re.IGNORECASE)
_SAFE_FILE_BYTE = re.compile(rb"[A-Za-z0-9_$-]")

_QUOTE, _DOUBLE_QUOTE, _BACKTICK, _BACKSLASH = 0x27, 0x22, 0x60, 0x5C

# MySQL string escapes. \% and \_ keep their backslash; any other escaped
# character stands for itself.
_ESCAPES = {
    ord("0"): b"\0",
    ord("'"): b"'",
    ord('"'): b'"',
    ord("b"): b"\b",
    ord("n"): b"\n",
    ord("r"): b"\r",
    ord("t"): b"\t",
    ord("Z"): b"\x1a",
    ord("\\"): b"\\",
    ord("%"): b"\\%",
    ord("_"): b"\\_",
}

# First words of a CREATE TABLE entry that is not a column.
_NOT_A_COLUMN = {b"PRIMARY", b"KEY", b"UNIQUE", b"INDEX", b"CONSTRAINT", b"FOREIGN", b"FULLTEXT", b"SPATIAL", b"CHECK"}
_INSERT_MODIFIERS = {b"LOW_PRIORITY", b"DELAYED", b"HIGH_PRIORITY", b"IGNORE"}


class DumpError(Exception):
    """The dump cannot be converted; the message is one line."""


class _Malformed(Exception):
    pass


class _Binary:
    __slots__ = ("data",)

    def __init__(self, data):
        self.data = data


class _Source:
    """The dump as a byte stream with a small look-ahead buffer."""

    def __init__(self, stream, chunk_size):
        self.stream = stream
        self.chunk_size = chunk_size
        self.buf = b""
        self.pos = 0
        self.base = 0  # file offset of buf[0]
        self.eof = False
        self.sha256 = hashlib.sha256()

    def _more(self):
        if self.eof:
            return False
        data = self.stream.read(self.chunk_size)
        if not data:
            self.eof = True
            return False
        self.sha256.update(data)
        self.base += self.pos
        self.buf = self.buf[self.pos :] + data
        self.pos = 0
        return True

    def have(self, count=1):
        """Whether `count` bytes are left; reads on as needed."""
        while len(self.buf) - self.pos < count:
            if not self._more():
                return False
        return True

    def offset(self):
        return self.base + self.pos

    def peek(self):
        return self.buf[self.pos] if self.have(1) else -1

    def at(self, literal):
        return self.have(len(literal)) and self.buf.startswith(literal, self.pos)

    def run(self, regex, keep=True, limit=None):
        """Consumes the bytes `regex` (a character-class run) matches from here
        on, across buffer refills. Returns them, or at most the first `limit`."""
        parts = []
        kept = 0
        while self.have(1):
            m = regex.match(self.buf, self.pos)
            if m is None:
                break
            if keep and (limit is None or kept < limit):
                piece = m.group()
                parts.append(piece)
                kept += len(piece)
            self.pos = m.end()
            if self.pos < len(self.buf):
                break
        joined = b"".join(parts)
        return joined if limit is None else joined[:limit]

    def drain(self):
        while self._more():
            pass


def _decode_name(raw):
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("latin-1")


def _file_name(table):
    encoded = "".join(
        chr(b) if _SAFE_FILE_BYTE.match(bytes([b])) else "%%%02X" % b for b in table.encode("utf-8", "surrogateescape")
    )
    return encoded + ".jsonl"


class _Converter:
    def __init__(self, source, outdir):
        self.src = source
        self.outdir = outdir
        # name -> {"file", "rows", "columns": [names], "types": {name: type}, or None without a CREATE TABLE}
        self.tables = {}
        self.files = {}  # lower-case file name -> table
        self.open_table = None
        self.open_file = None
        self.time_zone = None
        self.dump_completed = False
        self.non_utf8 = 0

    # ------------------------------------------------------------ output

    def _table(self, name):
        entry = self.tables.get(name)
        if entry is None:
            if not name:
                raise _Malformed("a table has no name")
            file_name = _file_name(name)
            other = self.files.setdefault(file_name.lower(), name)
            if other != name:
                raise _Malformed(
                    "tables '%s' and '%s' would share one file on a case-insensitive file system" % (other, name)
                )
            entry = self.tables[name] = {"file": file_name, "rows": 0, "columns": [], "types": None}
            with open(self.outdir / file_name, "wb"):
                pass
        return entry

    def _created(self, name, columns):
        """CREATE TABLE: the table starts (again) empty with these columns."""
        entry = self._table(name)
        if self.open_table == name:
            self._close()
        if entry["rows"]:
            with open(self.outdir / entry["file"], "wb"):
                pass
            entry["rows"] = 0
        entry["columns"] = [column for column, _ in columns]
        entry["types"] = dict(columns)

    def _close(self):
        if self.open_file is not None:
            self.open_file.close()
        self.open_file = None
        self.open_table = None

    def _text(self, raw):
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            self.non_utf8 += 1
        try:
            return raw.decode("cp1252")
        except UnicodeDecodeError:
            return raw.decode("latin-1")

    def _emit(self, name, entry, columns, values):
        row = {}
        for i, value in enumerate(values):
            key = columns[i] if i < len(columns) else "column_%d" % (i + 1)
            if isinstance(value, _Binary):
                row[key + BINARY_SUFFIX] = base64.b64encode(value.data).decode("ascii")
            else:
                row[key] = value
        if self.open_table != name:
            self._close()
            # One table's file is open at a time: a dump may hold thousands.
            self.open_file = open(self.outdir / entry["file"], "ab")
            self.open_table = name
        line = json.dumps(row, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
        self.open_file.write(line.encode("utf-8") + b"\n")
        entry["rows"] += 1

    # ------------------------------------------------------------ lexing

    def _space(self):
        self.src.run(_SPACE, keep=False)

    def _line(self, limit=64):
        """Consumes the rest of the line; returns its first `limit` bytes."""
        head = self.src.run(_NOT_NEWLINE, limit=limit)
        if self.src.peek() == 0x0A:
            self.src.pos += 1
        return head

    def _block_comment(self):
        """At "/*": consumes the comment. A conditional comment that sets the
        session time zone is noted."""
        src = self.src
        src.pos += 2
        head = b""
        while True:
            piece = src.run(_NOT_STAR, limit=256)
            if len(head) < 256:
                head += piece
            if not src.have(1):
                raise _Malformed("comment not closed")
            if src.at(b"*/"):
                src.pos += 2
                break
            src.pos += 1
            if len(head) < 256:
                head += b"*"
        self._note_time_zone(head)

    def _note_time_zone(self, text):
        if self.time_zone is None:
            m = _SET_TIME_ZONE.search(text)
            if m:
                self.time_zone = _decode_name(m.group(1))

    def _line_comment(self):
        if self._line().startswith(b"-- Dump completed"):
            self.dump_completed = True

    def _space_and_comments(self):
        src = self.src
        while True:
            self._space()
            if src.at(b"/*"):
                self._block_comment()
            elif src.at(b"--") or src.peek() == 0x23:
                self._line_comment()
            else:
                return

    def _string(self, quote):
        """After an opening quote: consumes through the closing one and returns
        the bytes the literal stands for."""
        src = self.src
        plain = _STRING_RUN[quote]
        parts = []
        while True:
            parts.append(src.run(plain))
            if not src.have(1):
                raise _Malformed("string not closed")
            if src.buf[src.pos] == _BACKSLASH:
                if not src.have(2):
                    raise _Malformed("string not closed")
                escaped = src.buf[src.pos + 1]
                parts.append(_ESCAPES.get(escaped, bytes([escaped])))
                src.pos += 2
            elif src.have(2) and src.buf[src.pos + 1] == quote:  # a doubled quote
                parts.append(bytes([quote]))
                src.pos += 2
            else:
                src.pos += 1
                return b"".join(parts)

    def _quoted_name(self):
        """After an opening backtick."""
        src = self.src
        parts = []
        while True:
            parts.append(src.run(_NOT_BACKTICK))
            if not src.have(1):
                raise _Malformed("name not closed")
            if src.have(2) and src.buf[src.pos + 1] == _BACKTICK:
                parts.append(b"`")
                src.pos += 2
            else:
                src.pos += 1
                return b"".join(parts)

    def _name_part(self):
        if self.src.peek() == _BACKTICK:
            self.src.pos += 1
            return self._quoted_name()
        return self.src.run(_BARE_NAME)

    def _name(self, first=None):
        """A table name, with or without a database in front; the table.
        `first`: its first part, when that is read already."""
        part = self._name_part() if first is None else first
        self._space()
        while self.src.peek() == 0x2E:  # "."
            self.src.pos += 1
            self._space()
            part = self._name_part()
            self._space()
        return _decode_name(part)

    @staticmethod
    def _unhex(digits):
        if len(digits) % 2:
            digits = b"0" + digits
        try:
            return bytes.fromhex(digits.decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            raise _Malformed("not a hexadecimal value") from None

    def _value(self):
        src = self.src
        c = src.peek()
        if c < 0:
            raise _Malformed("row not closed")
        if c in (_QUOTE, _DOUBLE_QUOTE):
            src.pos += 1
            return self._text(self._string(c))
        following = src.buf[src.pos + 1] if src.have(2) else -1
        if c == 0x30 and following in (0x78, 0x58):  # 0x...
            src.pos += 2
            return _Binary(self._unhex(src.run(_HEX)))
        if c in (0x78, 0x58) and following == _QUOTE:  # X'...'
            src.pos += 2
            return _Binary(self._unhex(self._string(_QUOTE)))
        if c in (0x62, 0x42) and following == _QUOTE:  # b'...'
            src.pos += 2
            bits = self._string(_QUOTE)
            try:
                return int(bits or b"0", 2)
            except ValueError:
                raise _Malformed("not a bit value") from None
        if c == 0x5F:  # a character set introducer: _binary '...', _utf8'...'
            introducer = src.run(_WORD)
            self._space()
            quote = src.peek()
            if quote in (_QUOTE, _DOUBLE_QUOTE):
                src.pos += 1
                raw = self._string(quote)
                return _Binary(raw) if introducer.lower() == b"_binary" else self._text(raw)
            if src.at(b"0x") or src.at(b"0X"):
                src.pos += 2
                return _Binary(self._unhex(src.run(_HEX)))
            return self._text(introducer)
        token = src.run(_TOKEN)
        if not token:
            raise _Malformed("a value was expected")
        if token.upper() == b"NULL":
            return None
        if _INT.fullmatch(token):
            return int(token)
        if _FLOAT.fullmatch(token):
            number = float(token)
            if math.isfinite(number):
                return number
        return self._text(token)

    # ------------------------------------------------------------ statements

    def _skip_statement(self):
        """Consumes through the statement's closing ";" (or the end of the dump)."""
        src = self.src
        while True:
            src.run(_STATEMENT_RUN, keep=False)
            c = src.peek()
            if c < 0:
                return
            if c == 0x3B:
                src.pos += 1
                return
            if c in (_QUOTE, _DOUBLE_QUOTE):
                src.pos += 1
                self._string(c)
            elif c == _BACKTICK:
                src.pos += 1
                self._quoted_name()
            elif src.at(b"/*"):
                self._block_comment()
            elif c == 0x23 or (src.at(b"--") and src.have(3) and src.buf[src.pos + 2] in b" \t\r\n"):
                self._line_comment()
            else:
                src.pos += 1

    def _set(self):
        """SET ...: a statement that sets the session time zone is noted."""
        src = self.src
        self._space()
        words = [src.run(_WORD).upper()]
        if words[0] == b"SESSION":
            self._space()
            words.append(src.run(_WORD).upper())
        if words[-1] == b"TIME_ZONE":
            self._space()
            if src.peek() == 0x3D:
                src.pos += 1
                self._space()
                if src.peek() == _QUOTE:
                    src.pos += 1
                    zone = self._string(_QUOTE)
                    if self.time_zone is None:
                        self.time_zone = _decode_name(zone)
        self._skip_statement()

    def _delimiter(self):
        """DELIMITER <x>: what follows, up to "DELIMITER ;", is stored code."""
        if self._line().strip() == b";":
            return
        while self.src.have(1):
            line = self._line().strip()
            if line[:9].upper() == b"DELIMITER" and line[9:].strip() == b";":
                return

    def _skip_definition(self):
        """Consumes the rest of a CREATE TABLE entry and the "," or ")" that ends it."""
        src = self.src
        depth = 0
        while True:
            src.run(_DEFINITION_RUN, keep=False)
            c = src.peek()
            if c < 0:
                raise _Malformed("CREATE TABLE not closed")
            src.pos += 1
            if c in (_QUOTE, _DOUBLE_QUOTE):
                self._string(c)
            elif c == _BACKTICK:
                self._quoted_name()
            elif c == 0x28:
                depth += 1
            elif c == 0x29:
                if depth == 0:
                    return c
                depth -= 1
            elif depth == 0:  # ","
                return c

    def _create(self):
        src = self.src
        self._space()
        word = src.run(_WORD).upper()
        if word == b"TEMPORARY":
            self._space()
            word = src.run(_WORD).upper()
        if word != b"TABLE":
            self._skip_statement()
            return
        self._space()
        first = None
        if src.peek() != _BACKTICK:
            first = src.run(_BARE_NAME)
            if first.upper() == b"IF":  # IF NOT EXISTS
                for _ in range(2):
                    self._space()
                    src.run(_WORD, keep=False)
                self._space()
                first = None
        name = self._name(first)
        self._space_and_comments()
        columns = []
        if src.peek() == 0x28:
            src.pos += 1
            while True:
                self._space_and_comments()
                if src.peek() == _BACKTICK:
                    src.pos += 1
                    column = self._quoted_name()
                else:
                    column = src.run(_BARE_NAME)
                    if column.upper() in _NOT_A_COLUMN:
                        column = b""
                if column:
                    self._space()
                    columns.append((_decode_name(column), src.run(_WORD).lower().decode("ascii")))
                if self._skip_definition() == 0x29:
                    break
        self._created(name, columns)
        self._skip_statement()

    def _insert(self):
        src = self.src
        first = None
        while True:
            self._space()
            if src.peek() == _BACKTICK:
                break
            first = src.run(_BARE_NAME)
            if first.upper() == b"INTO":
                self._space()
                first = None
                break
            if first.upper() not in _INSERT_MODIFIERS:
                break  # INTO is optional: this is the table
        name = self._name(first)
        entry = self._table(name)
        columns = entry["columns"]
        if src.peek() == 0x28:
            src.pos += 1
            columns = []
            while True:
                self._space()
                columns.append(_decode_name(self._name_part()))
                self._space()
                c = src.peek()
                src.pos += 1
                if c == 0x29:
                    break
                if c != 0x2C:
                    raise _Malformed("column list not closed")
            self._space()
        if src.run(_WORD).upper() not in (b"VALUES", b"VALUE"):
            self._skip_statement()  # INSERT ... SELECT, INSERT ... SET
            return
        while True:
            self._space()
            if src.peek() != 0x28:
                raise _Malformed("a row was expected")
            src.pos += 1
            values = []
            self._space()
            if src.peek() == 0x29:
                src.pos += 1
            else:
                while True:
                    self._space()
                    values.append(self._value())
                    self._space()
                    c = src.peek()
                    src.pos += 1
                    if c == 0x29:
                        break
                    if c != 0x2C:
                        raise _Malformed("row not closed")
            self._emit(name, entry, columns, values)
            self._space()
            c = src.peek()
            src.pos += 1
            if c == 0x3B:
                return
            if c != 0x2C:
                raise _Malformed("INSERT not terminated")

    def run(self):
        src = self.src
        try:
            while True:
                self._space_and_comments()
                if not src.have(1):
                    break
                word = src.run(_WORD).upper()
                if word in (b"INSERT", b"REPLACE"):
                    self._insert()
                elif word == b"CREATE":
                    self._create()
                elif word == b"SET":
                    self._set()
                elif word == b"DELIMITER":
                    self._delimiter()
                elif word:
                    self._skip_statement()
                else:
                    src.pos += 1  # a stray ";" or a byte no statement starts with
        finally:
            self._close()
        src.drain()


def convert(dump, outdir, chunk_size=CHUNK_SIZE):
    """Converts the dump at `dump` into `outdir` and returns the manifest.
    Raises DumpError when the dump cannot be read or converted and OSError
    when the output cannot be written."""
    dump = Path(dump)
    outdir = Path(outdir)
    try:
        stream = open(dump, "rb")
    except OSError as e:
        raise DumpError("cannot read %s: %s" % (dump, e.strerror or e)) from None
    with stream:
        try:
            magic = stream.read(2)
            stream.seek(0)
        except OSError as e:
            raise DumpError("cannot read %s: %s" % (dump, e.strerror or e)) from None
        if not magic:
            raise DumpError("%s is empty" % dump)
        if magic == b"\x1f\x8b":
            raise DumpError("%s is gzip-compressed; decompress it first" % dump)

        outdir.mkdir(parents=True, exist_ok=True)
        manifest_path = outdir / "MANIFEST.json"
        partial_path = outdir / "MANIFEST.json.tmp"
        # From here on the directory is not a complete conversion until the
        # new manifest is in place.
        for stale in (manifest_path, partial_path):
            stale.unlink(missing_ok=True)

        source = _Source(stream, max(1, chunk_size))
        converter = _Converter(source, outdir)
        try:
            converter.run()
        except _Malformed as e:
            raise DumpError("malformed dump %s at byte %d: %s" % (dump, source.offset(), e)) from None
    if not converter.tables:
        raise DumpError("no tables in %s" % dump)

    manifest = {
        "tables": {name: entry["rows"] for name, entry in converter.tables.items()},
        "files": {name: entry["file"] for name, entry in converter.tables.items()},
        "sha256": source.sha256.hexdigest(),
        "columns": {name: entry["types"] for name, entry in converter.tables.items() if entry["types"] is not None},
        "time_zone": converter.time_zone,
        "dump_completed": converter.dump_completed,
        "non_utf8_values": converter.non_utf8,
    }
    with open(partial_path, "w", encoding="utf-8", newline="\n") as out:
        json.dump(manifest, out, indent=2, sort_keys=True, ensure_ascii=False)
        out.write("\n")
        out.flush()
        os.fsync(out.fileno())
    os.replace(partial_path, manifest_path)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description="Convert a mysqldump file to one JSON-lines file per table.")
    parser.add_argument("dump", help="the mysqldump file (plain SQL)")
    parser.add_argument("outdir", help="directory for <Table>.jsonl and MANIFEST.json; created when missing")
    args = parser.parse_args(argv)
    try:
        manifest = convert(args.dump, args.outdir)
    except DumpError as e:
        print("legacy_dump_to_jsonl: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("legacy_dump_to_jsonl: cannot write to %s: %s" % (args.outdir, e.strerror or e), file=sys.stderr)
        return 1
    tables = manifest["tables"]
    print("%s: %d tables, %d rows" % (args.outdir, len(tables), sum(tables.values())))
    if manifest["non_utf8_values"]:
        print("%d text values were not UTF-8 and were read as Windows-1252" % manifest["non_utf8_values"])
    if not manifest["dump_completed"]:
        print("note: the dump has no '-- Dump completed' line; check that it is whole")
    return 0


if __name__ == "__main__":
    sys.exit(main())
