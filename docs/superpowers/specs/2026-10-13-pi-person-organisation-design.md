# A PI is a person or an organisation: design

Status: draft, 2026-10-13 (nightly). Not agreed. Open points: section 9.
Task T084 (from T039). Owner's row: store a PI as a person or an
organisation: family and given name in full, optional ORCID (unique when
present), optional ROR for the affiliation, no uniqueness on the name (warn
on a namesake at entry). One migration. Do it with the EarthBank work, which
is what asks for ORCID.

## 1. Today

```sql
-- libs/persistence/migrations/pg/0001_init.sql:12-16
CREATE TABLE principal_investigator (uuid uuid PRIMARY KEY, last_name text NOT NULL,
  first_initial text NOT NULL DEFAULT '', affiliation text, email text,
  display_name text GENERATED ALWAYS AS
    (CASE WHEN first_initial = '' THEN last_name ELSE last_name || ', ' || first_initial END) STORED,
  created_utc timestamptz NOT NULL, UNIQUE (last_name, first_initial));
```

- `PrincipalInvestigatorRow{uuid, last_name, first_initial, display_name,
  affiliation, email}` (`catalog.hpp:68-74`); `PrincipalInvestigatorSpec`
  (`store.hpp:120-125`). Catalog natural key: `last_name, first_initial`
  (`catalog.hpp:26`).
- Entry: `parse_pi(text, allowed)` (`libs/entry/src/names.cpp:38`) takes
  `Last` or `Last, F`, or a name of the lab's allowed list. T039 (separate,
  no migration) relaxes what letters it takes.
- Legacy import: `libs/dvc/src/catalog_adapter.cpp:750,931` and
  `libs/ingest/src/catalog.cpp:253-255` find or make a PI by
  `(last_name, first_initial)`.
- What it cannot hold: two people with same last name and initial; a full
  given name; an organisation without the allow-list; any persistent
  identifier.
- EarthBank: nothing in `libs/earthbank` or its spec names ORCID or ROR
  today (grep, 2026-10-13). What its API takes for a person is not
  established here: section 9, point 1.

## 2. Goals

- A PI row says person or organisation.
- Person: family name and given name as written, any letters.
- Optional ORCID, unique among rows that have one. Optional ROR for the
  affiliation.
- Two PIs may have same name. Entry warns, does not refuse.
- One migration, and what legacy import and stored data have keeps working.

## 3. Out of scope

- Looking an ORCID or ROR up on the network. Format and checksum only.
- `app_user` (operators): unchanged.
- Sending ORCID / ROR to EarthBank: with EarthBank work, after point 1.
- Merging duplicate PIs already in a store (a tool, later).
- Email export rules: email stays optional and is not exported, as now.

## 4. Schema: migration `0006_pi_identity.sql`

Added columns:

| Column | Type | Meaning |
|---|---|---|
| `kind` | `text NOT NULL DEFAULT 'person' CHECK (kind IN ('person','organisation'))` | |
| `family_name` | `text` | person: family name in full. organisation: its name |
| `given_name` | `text NOT NULL DEFAULT ''` | person: given name(s) in full, or what is known (an initial). organisation: `''` |
| `orcid` | `text` | `0000-0002-1825-0097` form, no URL; persons only |
| `ror` | `text` | 9 characters, `0xxxxxxnn` form, no URL; of the affiliation (person) or of itself (organisation) |

- Backfill: `family_name = last_name`, `given_name = first_initial`, `kind =
  'person'`.
- Indexes: `CREATE UNIQUE INDEX principal_investigator_orcid ON
  principal_investigator (orcid) WHERE orcid IS NOT NULL` (lookup by ORCID at
  entry and import; plan test in `SchemaIndexes`). No index on `ror` (never
  looked up by).
- `CHECK (kind = 'person' OR (orcid IS NULL AND given_name = ''))`.
- Name uniqueness: `UNIQUE (last_name, first_initial)` has to go for
  namesakes. Two ways, owner chooses (section 9, point 2):
  - **A, additive (draft).** `last_name`, `first_initial`, `display_name`
    and the `UNIQUE` stay as columns old builds read. New builds read and
    write `family_name`, `given_name`, and keep `last_name` /
    `first_initial` filled as a unique short key: `last_name = family_name`,
    `first_initial =` first letter of `given_name`, and for a namesake of an
    existing pair, that letter followed by a counter (`"R"`, `"R2"`).
    Migration only adds: a migrated store stays usable by the build before
    it (`libs/persistence/AGENTS.md`, "Prefer a change old builds can live
    with"). Cost: two representations kept in step, in one place
    (`ensure_principal_investigator`).
  - **B, clean.** Drop the `UNIQUE`, `last_name`, `first_initial`; regenerate
    `display_name` from new columns. SQLite cannot drop a table constraint:
    table rebuilt there (`-- @sqlite exec`), with `project`'s foreign key.
    Old builds cannot open the store again.
- Either way one migration; commit carries `BREAKING CHANGE:` footer naming
  `elctl db migrate --db <url>`.
- `python3 tools/ddl_sqlite.py`; `SchemaTest.MigrateIsIdempotentAndRecords
  Checksums` gains `0006`.

## 5. Store API (`libs/persistence`)

```cpp
enum class PiKind { Person, Organisation };

struct PrincipalInvestigatorRow {
  Uuid uuid;
  PiKind kind = PiKind::Person;
  std::string family_name, given_name;
  std::string display_name;            // "Family, G." person; name, organisation
  std::string full_name;               // "Given Family" person; name, organisation
  std::optional<std::string> orcid, ror, affiliation, email;
};
```

- `PrincipalInvestigatorSpec` same fields less the derived two. `last_name`
  / `first_initial` leave the C++ API in both ways A and B; callers
  (`libs/entry`, `libs/ingest`, `libs/dvc`, `apps/pychron-ui`, 17 files) move
  to the new names in the same commit as the API.
- Display form derived, not stored by callers: person `Family, G.` from
  first letter of each given name (`"Ruiz García, M. J."`); with no given
  name, `Family`. Listings where two rows show the same display form add
  the ORCID's last four, or affiliation, in brackets.
- Catalog key of a PI (`catalog.hpp:26`, used by `apply_catalog_edits`):
  becomes its `uuid`. A name no longer finds one row.
- New read: `principal_investigators_named(family_name, given_name_prefix)`
  for the namesake warning and for legacy import; rows by `lower(family_
  name)`. Table is small (a lab has tens to hundreds of PIs): a scan is
  accepted and no index added; stated in a comment on the statement.
- `find_principal_investigator_by_orcid(orcid)`: one row, by the partial
  index.
- New edit fields through `apply_catalog_edits` only (`libs/entry/AGENTS.md`):
  `kind`, `family_name`, `given_name`, `orcid`, `ror`.

## 6. Entry (`libs/entry`, UI)

- `parse_pi` gives `{kind, family_name, given_name}`:
  - `Family, Given` and `Family` : person.
  - Lab's allowed list entry: organisation (what the list was standing in
    for). List stays readable; its entries create organisation rows.
  - A sheet column `principal_investigator_orcid` (optional): when present
    and it matches a row, that row, whatever the name cell says; mismatch
    of family name with that row is an error naming both.
- ORCID check: 16 digits in fours, last may be `X`, ISO 7064 mod 11-2
  checksum; URL prefix stripped. ROR: `0` + 6 of `[a-hj-km-np-tv-z0-9]` + 2
  digits; URL prefix stripped. Bad form refused with the value quoted.
- Namesake: adding a person whose family name and given-name initial match
  an existing row and who has no matching ORCID gives a warning listing the
  rows (name, affiliation, ORCID) and the choice "same person" (use that
  row) or "another person" (new row). In a sheet import the default with no
  answer is "same person" when exactly one row matches, and a stop with the
  list when several do.
- Organisation with an ORCID: refused.

## 7. Legacy import (`libs/dvc`, `libs/ingest`)

- Legacy gives `last, initial`. Resolved as: rows with that family name and
  a given name starting with that initial. One: that row. None: new person,
  `given_name` = the initial. Several: oldest by `created_utc`, and a note
  in the import report naming the others.
- Re-import of same legacy data creates no second row (idempotent, as now).

## 8. Rules and tests

- ORCID unique when present; two rows without one never conflict.
- A store migrated from 0005 shows every PI as before, but for the period
  after the initial if section 9, point 3 keeps it.
- Nothing reaches the table but through `apply_catalog_edits` /
  `ensure_*`.
- Tests: `tests/persistence` (migration on a 0005 fixture keeps rows and
  display names; ORCID uniqueness; namesakes allowed; plan test for the
  ORCID index; both engines when `PYCHRON_TEST_PG_URL` is set),
  `tests/entry` (parser forms, ORCID and ROR checks with known-good and
  known-bad values, namesake outcomes, organisation rules), `tests/ingest`
  and `tests/dvc` (three resolutions of section 7; fixtures' catalog files
  updated), `tests/ui` entry page.

## 9. Open points for owner

1. What does EarthBank's API take for a person and an organisation (ORCID,
   ROR, both, a name only)? Not found in tree. Decides whether ROR on a
   person means the affiliation's, as drafted.
2. Schema way A (additive, old builds keep working, short key kept in
   step) or B (clean, table rebuilt on SQLite, no going back)? Draft: A.
3. Display form of a person: `Family, G.` with a period (draft), or today's
   `Family, G`? Today's keeps migrated stores' text identical everywhere,
   including exports.
4. Allowed-list names become organisations (draft): right for every lab's
   list, or are some of them people written without a comma?
5. Sheet import with one namesake and no ORCID: assume same person (draft),
   or always stop and ask?
6. T039 (parser relaxed, no migration) first and separately, as its notes
   say, or folded into this?
