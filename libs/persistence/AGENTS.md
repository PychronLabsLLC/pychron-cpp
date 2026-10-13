# The store (`libs/persistence`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- Persistence tests always run on SQLite. Set
  `PYCHRON_TEST_PG_URL=postgresql://user:pw@host/db` to run them on
  PostgreSQL as well (each test uses throwaway schema).
- Schema source: `libs/persistence/migrations/pg/`. After editing it, run
  `python3 tools/ddl_sqlite.py` and commit regenerated SQLite file. Never
  edit an applied migration; add `NNNN_<name>.sql`. A statement only
  PostgreSQL understands is preceded by `-- @sqlite skip`; its SQLite
  counterpart, when one is needed, is given as
  `-- @sqlite exec <statement>`.
- Sample location: one PostGIS `geometry(Point, 4326)` column, `geom`
  (migration 0004); SQLite keeps same point as EWKT text. Catalog API still
  speaks `lat` and `lon` (`SampleFields`, the `lat`/`lon` edit fields):
  `libs/persistence/src/sql/geometry.hpp` converts, and every read of the
  column goes through `geom_read()` (`ST_AsEWKT` on PostgreSQL). Half a point
  is refused. PostgreSQL needs PostGIS; tests on it do too.
- A program that registers a client with the store names its machine with
  `pychron::host_name()` (`libs/core` `host_name.hpp`), never the `HOSTNAME`
  variable.
- ctest lists `pg` cases only when `PYCHRON_TEST_PG_URL` was set when the
  tests were built (`gtest_discover_tests` runs at build time): set it,
  rebuild, then run.

## Schema and queries

Store (`libs/persistence`) is read one analysis at a time: a figure of 2000
analyses runs every per-analysis statement 2000 times. October 2026: ideogram
of 24 analyses took 12 s, none of it calculation (reduction 3 ms, figure
1 ms): it was four mistakes in how rows were found. Rules below are those
mistakes, generalised. Commits: `57621f0` and `1786fa8`.

Sizes to think with, from one lab's imported store: 8.7 thousand analyses,
312 thousand revisions (about 36 an analysis), 21 thousand reference objects,
19 thousand irradiation positions, 15 thousand identifiers, 6.5 thousand
samples. Lab that has run for a decade is ten to a hundred times that. Test
fixture has five rows of each, where every plan is instant: a query that
passes its test has told you nothing about its cost.

### Before adding a migration

- Every migration locks every existing store out of `pychron-ui`, `export`,
  `flux` and `entry` until someone runs `elctl db migrate --db <url>` on it
  (they open with `migrate = false` and refuse a schema that is behind). So:
  add one when it earns that step, put what belongs together into one
  migration rather than three in a week, and give the commit a
  `BREAKING CHANGE:` footer that says to run the command.
- Prefer a change old builds can live with. A build checks only the
  migrations it carries and ignores later ones, so a migration that only
  adds (index, nullable column, table) leaves a migrated store usable by the
  build before it. One that drops, renames or tightens does not: labs then
  cannot go back.
- Can the query be written so it needs no schema change? Ask first. Sample
  counts were going to get two indexes; rewritten to count once instead of
  per row they needed none, and were quicker than with them.
- Mechanics: source is `libs/persistence/migrations/pg/NNNN_<name>.sql`;
  run `python3 tools/ddl_sqlite.py` and commit the SQLite file it writes;
  never edit an applied migration; `-- @sqlite skip` and `-- @sqlite exec`
  for what only one engine understands. `SchemaTest.MigrateIsIdempotentAndRecordsChecksums`
  lists migrations by number and name: add yours.

### Indexes

- Foreign key is not an index. Neither PostgreSQL nor SQLite makes one for
  the referring column: `REFERENCES sample` on `identifier.sample_uuid` gives
  no way to find the identifiers of a sample but to read them all.
- `UNIQUE (a, b)` finds rows by `a`, or by `a` and `b`. Not by `b`.
  `ref_object` had `UNIQUE (ref_type, key)`, and looking a reference up by
  its position read every object of the type.
- So: every column a query filters or joins on, when that query runs per
  analysis or over a listing, needs an index that starts with it. When you
  add a column that will be looked up by, add its index in the same
  migration.
- Column null for most rows gets a partial index
  (`... (position_uuid) WHERE position_uuid IS NOT NULL`): smaller, and both
  engines use it for an equality, which implies not null.
- Index on an expression (`lower(name)`) serves only a query that writes the
  same expression.
- Index is not free: each one is written on every insert. Add the ones a
  query needs and can be shown to use, not one per column.
- New index gets a test that reads the plan: `SchemaIndexes` in
  `tests/persistence/test_schema.cpp` runs `EXPLAIN QUERY PLAN` on the real
  statement and requires the index by name and no `SCAN`. That test fails
  when a later rewrite of the query stops using it.

### Queries

- Fetch a row by its uuid. Name is not a key: a monitor's sample exists once
  per project (97 times in that store), and matching by name and project
  name is a guess. If the query that found the analysis already joined the
  row you want, carry its uuid out (`BrowseRow::sample_uuid`) instead of
  looking it up again by what it is called.
- Do not use a search to do a lookup. `samples({.text = name})` is
  `LIKE '%name%'`: no index can serve a pattern that starts with `%`, it
  returns every name containing the text, and it stops at its `LIMIT`, so
  the row you wanted may not be among them.
- Never count in a subquery per row
  (`(SELECT count(*) FROM ... WHERE x = outer.uuid) AS n`). It runs once for
  every row listed. Aggregate once for all rows in a `WITH` clause and
  `LEFT JOIN` it, `coalesce(n, 0)`: `kSampleCounts` in `sql/catalog.hpp` is
  the model. Listing 500 samples went from 1.3 s to 0.02 s. A count per row
  that an index answers (one seek each) is bearable for a short listing;
  `kIrradiations` and `kSheetPositions` in that file are still written that
  way and have not been measured on a large store. Measure before copying
  them.
- No `OR` between columns of different tables, least of all across an outer
  join (`WHERE ip.sample_uuid = ? OR i.sample_uuid = ?`): no index serves it
  and the tables are read whole. Write the two cases as a `UNION`. An `OR`
  between columns of one table is fine when each side has its index (plan
  says `MULTI-INDEX OR`).
- What costs and is not always wanted is asked for, not given: a listing's
  counts are behind `SampleQuery::counts`, and the loader, which wants the
  row, turns them off.
- `ORDER BY ... LIMIT` over a large table needs an index in that order, or
  everything is sorted to return the first page (`USE TEMP B-TREE FOR ORDER BY`
  in the plan). Fine for a few hundred rows that a filter already chose; not
  for `analysis` or `revision`.
- A new read that happens for every analysis is multiplied by every
  analysis. Before adding one to `StoreSource::load`, ask whether it is the
  same for every analysis of a level or an irradiation (a production, a
  chronology) and should be read once and shared, and whether it can be one
  statement for many analyses (`WHERE analysis_uuid IN (...)`) rather than
  one each. Loading is still one analysis at a time, several small
  statements each (the row, its heads, a payload per head, its references, a
  payload per reference); that is the next thing to change, not a pattern to
  copy.
- Both engines run every statement. Standard SQL, and where they differ, the
  `Dialect` switch (`sql::ts`, `geom_read`), not two copies of a query. Times
  are read as text through `sql::ts`. TinyORM's `return_qdatetime` stays off
  for SQLite (`tiny/db.cpp`): on, every text value read is tried as a date,
  which was most of the time of a load and reworded any text that looked
  like one.

### How to know

Reading a query does not tell its cost; the plan and a clock do.

- Plan, on a store of real size (ask for a copy of a lab's; work on the
  copy):

      sqlite3 store.db
      .timer on
      EXPLAIN QUERY PLAN <the statement, with real values>;

  Bad signs: `SCAN <a large table>`; `CORRELATED SCALAR SUBQUERY` under a
  listing; `USE TEMP B-TREE` over many rows; a `SEARCH` by an index's first
  column only when you filter on more. On PostgreSQL:
  `EXPLAIN (ANALYZE, BUFFERS)`, and look for `Seq Scan` on a large table.
- Clock: `tests/processing/test_store_load_timing.cpp` runs the pipeline a
  figure window runs and prints the time of each node. It does nothing
  unless told which store:

      PYCHRON_BENCH_DB=sqlite:/path/to/store.db PYCHRON_BENCH_N=400 \
        build/dev/tests/processing/pychron_processing_store_tests --gtest_filter='StoreLoadTiming.*'

  Run it before and after any change to loading, to a query the loader uses,
  or to the schema, at 24, 400 and 2000 analyses, and put the numbers in the
  commit message. Time that grows faster than the count is a query that
  reads a table per analysis.
- When it is slow and the plan looks right, sample the process (`sample <pid> 5`
  on macOS, `perf` on Linux) before changing anything. Costliest of the four
  causes was not in a query: it was the library parsing dates.
- What goes in CI is the plan test, not the clock: a time limit fails on a
  loaded runner and passes on a fast one. Slow query has a shape (a scan, a
  subquery per row); assert the shape.
- Persistence tests run on SQLite. Set `PYCHRON_TEST_PG_URL` and run them on
  PostgreSQL as well before a release that changes a query or adds a
  migration: the two planners do not make the same choices.
