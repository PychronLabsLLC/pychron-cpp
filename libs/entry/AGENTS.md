# Entry and seed (`libs/entry`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- `libs/entry` (sample and package entry) builds only with persistence, like
  `libs/ingest`. Entry writes catalog rows only through
  `IStore::apply_catalog_edits` (field-value compare-and-swap, one
  transaction) and identifiers only through `allocate_identifiers`; never
  through ad hoc UPDATEs. User guide: `docs/entry.md`.
- Instrument install has a database, and setup seeds it from the install's
  `seed.toml` (`libs/entry` `seed.hpp`; `elctl entry seed` by hand): the
  `references` project, a sample and special identifier for each kind of
  reference run, Triga production ratios (those of the NM-293 fixture). The
  seed only ensures: it never edits or removes a row or a reactor that
  exists, and a failed seed never fails an install. `seed.toml`,
  `defaults.toml` and the example queue name the same samples;
  `tests/setup/test_profiles.cpp` holds them together. `libs/setup` only
  computes the database url; apps open the store.
