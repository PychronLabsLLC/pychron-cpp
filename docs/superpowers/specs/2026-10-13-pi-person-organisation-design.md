# A PI is a person or an organisation: design

Status: draft, 2026-10-13. Owner decided 2026-10-13 (section 9): way B
(clean), shown as "Jake Ross", browser filters by uuid, no allowed-names
list, ORCID optional, a namesake in a sheet import stops and asks. T039's
relaxed parser folds in here. Section 10 settled the same day; what
EarthBank takes is left for the EarthBank work.
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
- Two PIs may have same name. Entry asks which, does not refuse.
- No allowed-names list: an organisation is a kind of PI, not an exception
  to a name pattern.
- Browser tells namesakes apart: filter and facet by the PI's uuid.
- One migration, clean (way B). What legacy import and stored data have
  keeps working.

## 3. Out of scope

- Looking an ORCID or ROR up on the network. Format and checksum only.
- `app_user` (operators): unchanged.
- Sending ORCID / ROR to EarthBank: with EarthBank work, after point 1.
- Merging duplicate PIs already in a store (a tool, later).
- Email export rules: email stays optional and is not exported, as now.

## 4. Schema: migration `0006_pi_identity.sql` (way B, clean)

```sql
CREATE TABLE principal_investigator (uuid uuid PRIMARY KEY,
  kind text NOT NULL DEFAULT 'person' CHECK (kind IN ('person','organisation')),
  family_name text NOT NULL,           -- person: family name in full; organisation: its name
  given_name text NOT NULL DEFAULT '', -- person: given name(s), or what is known (an initial)
  orcid text,                          -- 0000-0002-1825-0097 form, no URL; persons only
  ror text,                            -- 0xxxxxxnn, no URL; the affiliation's, or the organisation's own
  affiliation text, email text,
  display_name text GENERATED ALWAYS AS (...) STORED,   -- 5.1
  created_utc timestamptz NOT NULL,
  CHECK (kind = 'person' OR (orcid IS NULL AND given_name = '')));
CREATE UNIQUE INDEX principal_investigator_orcid ON principal_investigator (orcid) WHERE orcid IS NOT NULL;
```

- No uniqueness on the name. Old `UNIQUE (last_name, first_initial)`,
  `last_name`, `first_initial` gone.
- Data: `family_name = last_name`, `given_name = first_initial`, `kind =
  'person'`, uuids kept (projects point at them unchanged).
- PostgreSQL: `ALTER TABLE` (drop generated `display_name` and the
  `UNIQUE`, rename `last_name`/`first_initial`, add columns, re-add
  generated `display_name`). SQLite cannot drop a constraint: rows copied
  aside, old table dropped, new table created under same name, rows
  inserted back, copy dropped, inside the migration's transaction with
  `PRAGMA defer_foreign_keys = ON` (`-- @sqlite exec` lines). No `ALTER
  TABLE ... RENAME`: with foreign keys on (store opens SQLite so),
  references of `project` and `repository` follow a renamed table. Drop
  leaves each child row a deferred violation; reinsert of same uuids
  resolves them before commit. `project.pi_uuid`, `repository.pi_uuid`
  keep pointing at same uuids. Test proves `project` rows and their PI
  survive on both engines.
- Old builds cannot open a migrated store: accepted (owner, way B). Commit
  carries `BREAKING CHANGE:` footer naming `elctl db migrate --db <url>`.
- `python3 tools/ddl_sqlite.py`; `SchemaTest.MigrateIsIdempotentAndRecords
  Checksums` gains `0006`; plan test in `SchemaIndexes` for the ORCID index.

## 5. Store API (`libs/persistence`)

```cpp
enum class PiKind { Person, Organisation };

struct PrincipalInvestigatorRow {
  Uuid uuid;
  PiKind kind = PiKind::Person;
  std::string family_name, given_name;
  std::string display_name;                     // 5.1
  std::optional<std::string> orcid, ror, affiliation, email;
};
```

- `PrincipalInvestigatorSpec`: same less `display_name`. `last_name` /
  `first_initial` leave the C++ API; callers (`libs/entry`, `libs/ingest`,
  `libs/dvc`, `apps/pychron-ui`, about 17 files) move in same commit.
- Catalog key of a PI (`catalog.hpp:26`, `apply_catalog_edits`): its `uuid`.
  A name no longer finds one row.
- `principal_investigators_named(family_name, given_initial)`: candidates for
  a namesake question and for legacy import; by `lower(family_name)`. A lab
  has tens to hundreds of PIs: a scan accepted, no index; said in a comment
  on the statement.
- `find_principal_investigator_by_orcid(orcid)`: by the partial index.
- Edit fields through `apply_catalog_edits` only (`libs/entry/AGENTS.md`):
  `kind`, `family_name`, `given_name`, `orcid`, `ror`, `affiliation`,
  `email`.

### 5.1 How a PI is shown (owner: "Jake Ross")

- Person: `given_name family_name` ("Jake Ross", "María José Ruiz García").
  Given name of one letter (legacy's initial) as it is: "J Ross" (owner).
  No given name: family name alone ("Ross").
- Organisation: its name ("NMGRL").
- Lists sorted by `family_name`, then `given_name`, not by display.
- Two PIs showing same text (namesakes): listings add affiliation, else the
  ORCID's last four, in brackets.
- Everywhere a PI is shown or exported (browser column, data report's PI
  column, entry sheets written back, EarthBank sample rows) this text. A
  migrated store's "Ross, J" becomes "J Ross": a visible change, said in
  release notes.

## 6. Entry (`libs/entry`, UI)

- Forms (samples window, project editor) ask for kind, then family and
  given name, ORCID, affiliation, ROR, each its own field. Nothing parsed.
- Sheet import, columns:
  - `principal_investigator`: a person as "Family, Given" (T039's relaxed
    rule: any letters, particles, spaces; the given name kept in full); an
    organisation's name when `principal_investigator_kind` says so.
  - `principal_investigator_kind` (optional): `person` (default) or
    `organisation`.
  - `principal_investigator_orcid` (optional): matched first; a row with it
    is that PI whatever the name cell says; a family name that differs from
    that row's is an error naming both.
  - `principal_investigator_uuid` (optional, written by the sheet export):
    the PI itself, no matching at all.
  - A person's cell without a comma: a family name alone, as legacy took
    "Ross" (owner). "Jake Ross" so written is a family name of two words;
    the template's header note says to write "Family, Given".
- ORCID optional everywhere (owner). Check when given: 16 digits in fours,
  last may be `X`, ISO 7064 mod 11-2 checksum; URL prefix stripped. ROR: `0`
  + 6 of `[a-hj-km-np-tv-z0-9]` + 2 digits; URL prefix stripped. Bad form
  refused with the value quoted.
- Namesake (owner: stop and ask). A person whose family name and given
  initial match an existing row, given without ORCID or uuid, is not
  resolved by guessing:
  - form: dialog listing the rows (name, affiliation, ORCID, projects) with
    "this one" or "another person";
  - sheet import in the UI: the import plan stops at those rows and the
    dialog asks once a distinct name, the answer used for every row of it;
  - `elctl entry samples import`: fails with the rows listed and how to
    say which (`principal_investigator_orcid` or `_uuid` column); nothing
    written.
- Organisation with an ORCID or a given name: refused.

### 6.1 No allowed-names list

- It existed because the parser took only `Last` / `Last, F`: a lab or a
  group could only be a PI as a listed exception. With `kind =
  organisation` it has no use.
- `EntrySettings::pi_names_allowed` removed; Entry Settings field,
  `elctl entry settings` key, docs (`docs/entry.md`) with it. A stored
  settings value of it stays in `other_json`, unread.
- PIs the list admitted are migrated as persons with a family name only.
  `elctl db migrate` lists, after 0006, PI rows with no given name, as
  candidates to mark `organisation` in the PI editor; nothing is guessed.

## 7. Browser: filter and facet by uuid (owner)

- `persistence::BrowseFilter::principal_investigators` and
  `processing::BrowseQuery::principal_investigators`: PI uuids, matched on
  `p.pi_uuid` (`browse.cpp:79`, `sql/browse.hpp` `kColPrincipalInvestigator`
  becomes `p.pi_uuid`; display still selected for the column).
- Facets: `facet()` returns `FacetValue{value, label}` (`source.hpp`); for
  every facet but the PI's, label is value. PI: value the uuid, label the
  display text of 5.1. `MemorySource` and the record source have no PI
  rows: their value is the name, as now.
- `AnalysisSummary` gains `principal_investigator_uuid` (empty without one).
- Facet box shows labels, keeps values; a saved browser search stores
  values. A saved search with PI names (before 0006) is read as names and
  matched by display text once, then saved as uuids.
- Plan test: the browse filtered by PI uses an index on `project.pi_uuid`
  or says why none is needed (projects are few). `libs/persistence/AGENTS.md`,
  "Indexes".

## 8. Legacy import (`libs/dvc`, `libs/ingest`)

- Legacy gives `Last, F`: `family_name = Last`, `given_name = F`. Resolved:
  rows with that family name and a given name starting with that letter.
  One: that row. None: new person. Several: the oldest by `created_utc`,
  and a note in the import report naming the others (owner: nobody is
  there to ask).
- Re-import of same legacy data creates no second row (idempotent).

## 9. Owner, 2026-10-13

1. Way B, clean.
2. Shown "Jake Ross" (5.1).
3. Browser filters by uuid (7).
4. No allowed-names list (6.1).
5. ORCID optional.
6. Namesake in a sheet import: stop and ask (6).
7. T039 folds in: its parser is the sheet's "Family, Given" rule.

## 10. Settled 2026-10-13 (owner)

1. Legacy import, several matching PIs: the oldest, reported (8).
2. Sheet cell without a comma: a family name alone, as legacy (6).
3. EarthBank: what its API takes for a PI is decided with the EarthBank
   work. Until then ROR is stored and shown, not sent; nothing here sends
   ORCID or ROR anywhere.
4. One-letter given names: "J Ross" (5.1).

## 11. Rules and tests

- ORCID unique when present; two rows without one never conflict.
- Migration keeps every PI uuid and every project's PI; display changes
  only as 5.1 says.
- Nothing reaches the table but through `apply_catalog_edits` / `ensure_*`.
- No guess between namesakes anywhere a person can answer.
- Tests: `tests/persistence` (0006 on a 0005 fixture on both engines: rows,
  uuids, projects kept; namesakes allowed; ORCID unique; plan tests),
  `tests/entry` (sheet columns, kinds, ORCID and ROR checks with known-good
  and known-bad values, namesake stops, no allowed list), `tests/ingest`
  and `tests/dvc` (three resolutions of section 8; fixtures' catalog files
  updated), `tests/processing` (facet values and labels, filter by uuid),
  `tests/ui` (entry forms, namesake dialog, facet box), `apps/elctl/tests`
  (import stops on a namesake; settings without `pi_names_allowed`).
