# Legacy import (`libs/ingest`, `libs/dvc`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- `libs/ingest` and `libs/dvc` build only with persistence; their tests (and
  `elctl`'s import tests) need `git` >= 2.32 on PATH and `tzdata`. Real-data
  check: `tools/import_fixture_check.sh build/dev` (network, not in CI). User
  docs: `docs/legacy_import.md`.
- Importer rules easy to break (spec section 10 of
  `docs/superpowers/specs/2026-10-03-legacy-ingestion-design.md`): the store
  must end the same however a walk is cut, stopped, resumed or replayed, so
  an adapter decides from the walk (paths and commits), never from what an
  earlier run happened to leave in memory, and never orders anything by
  address. Each adapter's `OneHistoryOneResult` test holds one history with
  everything that has gone wrong at a batch boundary; a change to a walk or
  to the writer adds its case there (`same_at_every_cut` in
  `tests/dvc/verify_support.hpp` does same for a small history). Ids derive
  from normalized source url and from commit and path: changing
  `normalize_source_url` or an id recipe orphans every existing import. Words
  that make a conflict a warning live in
  `libs/ingest/include/pychron/ingest/conflict_markers.hpp`.
- `GitFixture` switches user's and machine's git configuration off for the
  test process (`GitReader::mirror` reads it); a test that wants one sets
  `GIT_CONFIG_GLOBAL` itself. No test may run git against this repository.
- One importer at a time per database. Windows CI builds without
  persistence, so `libs/ingest`, `libs/dvc` and `elctl import` are not built
  or tested there.
