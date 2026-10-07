# Install defaults: reference samples, special identifiers and reactor productions

Status: design, awaiting review. 2026-10-07.

## 1. Goal

A fresh experiment (instrument) install can run blanks, airs and cocktails
that are recorded under a real project and sample, and can make an
irradiation package whose reactor has production ratios, with nothing
entered by hand first.

Today:

- An instrument install is files only. It has no database
  (`libs/setup/src/doctor.cpp` `site_install`: only `data_reduction` gets one).
- No `identifiers.toml` is shipped; the special identifiers come from the
  code's defaults (`libs/experiment/src/model/identifiers.cpp`).
- A blank or air run has no sample and no project: `defaults.toml` cannot
  carry them, and the experiment takes them only from the queue file.
- A new store is empty. `elctl entry package add --reactor Triga` warns that
  the reactor is not in `reactors.json` and leaves the production empty.

## 2. Decisions (owner, 2026-10-07)

1. The defaults are seeded into the store.
2. An instrument install gets a store, and new blank, air and cocktail runs
   are also stamped with their sample and project by the run defaults (the
   experiment does not read the store).
3. The default reactor is `Triga`, with the nine ratios of
   `tests/dvc/fixtures/meta/NM-293/productions/Triga_PR.json`.

## 3. What is shipped

### 3.1 `seed.toml` (new, `profiles/instrument-common/seed.toml`)

Installed as `<root>/seed.toml`. It is the one source for what setup puts in
the store, and the lab may edit it before running setup again.

```toml
# What pychron setup puts in a new database. Rows already there are left
# as they are; setup never changes or removes one.

project = "references"

[[samples]]
identifier = "bu"            # the special identifier, as in identifiers.toml
analysis_type = "blank_unknown"
sample = "blank_unknown"
material = "blank"
# ... ba blank_air, bc blank_cocktail, be blank_extractionline,
#     bg background (material "blank"); a air (material "air");
#     c cocktail (material "cocktail"); ic detector_ic (material "air")

[reactors.Triga]
K4039  = [0.00873, 0.00017]
K3839  = [0.013, 0.0]
K3739  = [0.0, 0.0]
Ca3937 = [0.000758, 7e-06]
Ca3837 = [4e-05, 2e-05]
Ca3637 = [0.000286, 5e-07]
Cl3638 = [250.0, 0.0]
Ca_K   = [1.96, 0.0]
Cl_K   = [0.227, 0.0]
```

`pa` (pause) and `dg` (degas) make no analysis and get no sample.

### 3.2 `identifiers.toml` (new, `profiles/instrument-common/identifiers.toml`)

The prefixes the code already defaults to, written out so a lab can see and
change them. Loading it must give exactly `IdentifierRules::defaults()`.

### 3.3 `defaults.toml` (changed)

Each of `blank_unknown`, `blank_air`, `blank_cocktail`,
`blank_extractionline`, `background`, `air`, `cocktail`, `detector_ic` gets

```toml
[air."*".sample]
sample = "air"
material = "air"
project = "references"
```

The example queue's `bu` run gets the same `sample = { ... }`.

### 3.4 Data-source questions

`instrument-common` gains the `data_source`, `db_host`, `db_port`, `db_name`,
`db_user`, `db_password` questions and the `credentials.toml` file of
`data-reduction`, moved into a new fragment `store-common` that both include,
so there is one copy. Default: a local SQLite file, `data/pychron.db`.

## 4. Mechanics

### 4.1 Run defaults (`libs/experiment`)

`RunDefaults` gains `std::optional<SampleInfo> sample`, read from
`[<type>.<device>.sample]` (`sample`, `material`, `project`; an unknown key is
an error, as elsewhere in the file). `apply_defaults` sets `run.sample` when
it is given. `strip_for_type` is unchanged; if the rules for a type forbid a
sample, that type gets none and the profile does not give it one.

### 4.2 The install has a database (`libs/setup`)

`site_install` gives an instrument install `database =
database_url_for(...)` as it does for data reduction. `libs/setup` stays free
of persistence: it only computes the url.

### 4.3 The seed (`libs/entry`, new `seed.hpp` / `seed.cpp`)

```cpp
struct Seed { std::string project; std::vector<SeedSample> samples;
              std::map<std::string, ProductionValue> reactors; };
Result<Seed> parse_seed(std::string_view toml, std::string_view name = "seed.toml");

struct SeedReport { int projects, materials, samples, identifiers, reactors;  // made
                    std::vector<std::string> kept; };                        // already there, said by name
Result<SeedReport> apply_seed(persistence::IStore& store, const Seed& seed, std::string_view actor);
```

`parse_seed` refuses: an unknown key, a project name `valid_project_name`
refuses, an `analysis_type` that is not one, a ratio key outside the nine
the reduction knows, a ratio that is not two finite numbers, the same
identifier twice.

`apply_seed`, in this order, through the store's ensure-by-natural-key calls
only (`add_project`, `add_material`, `add_sample`, `add_identifier` with kind
`special` and the sample), which keep what is there:

1. the project (no principal investigator),
2. each material,
3. each sample,
4. each special identifier, linked to its sample. No mass spectrometer: the
   identifier is unique in the store and shared by every instrument on it.
5. `reactors.json`: when the document does not exist it is written with the
   seed's reactors (`add_ref_object` Document, one revision, one Reference
   changeset, as `save_settings` does). When it exists, each seed reactor
   whose name is not in it is added in a new revision; a reactor already in
   it is never changed.

Run again it writes nothing and reports everything as kept. An identifier row
that exists without a sample (one the legacy importer made) is kept as it is
and named in `kept`: the store has no edit for an identifier's sample, and
adding one is outside this work.

### 4.4 Where it runs (apps)

`elctl init` / `init --reconfigure` and the UI setup wizard, after the files
are written:

- instrument install, local SQLite: create and migrate, then seed.
- instrument install, server: the schema is not migrated by setup (as now);
  when the store opens and its schema is current, seed; otherwise say why the
  seed was skipped and how to run it later.
- data-reduction install: unchanged (no seed; its database is somebody's
  data).
- New `elctl entry seed [<seed.toml>] [--dry-run]` runs the same thing by
  hand (default `<root>/seed.toml`), for the skipped case and for a lab that
  edits the file.

A failed seed does not fail the install: the files are written, the error is
printed with the command to run again. A build without persistence prints
`skip  seed: built without the DVC store`, as the database step does.

## 5. Tests

- `tests/experiment`: `defaults.toml` with a `sample` table; `make_run` and
  `make_special_run` give the run its sample; an unknown key is an error.
- `tests/entry/test_seed.cpp`: parse (each refusal above); apply on an empty
  store (rows, the identifier's sample, `load_reactors` finds Triga with nine
  ratios); apply twice (second writes nothing, no new changeset); a
  pre-existing `reactors.json` with an edited Triga is not changed and a
  second seed reactor is added; a pre-existing sample-less `bu` is kept.
  SQLite always, PostgreSQL when `PYCHRON_TEST_PG_URL` is set.
- `tests/setup/test_profiles.cpp`: every instrument profile renders
  `seed.toml`, `identifiers.toml` and `defaults.toml`; the shipped
  `identifiers.toml` equals `IdentifierRules::defaults()`; every sample and
  project in `defaults.toml` and the example queue is in `seed.toml` (the two
  files cannot drift); the shipped Triga equals the NM-293 fixture.
- `apps/elctl/tests/test_setup_commands.cpp`: `init` of an instrument profile
  makes `data/pychron.db` holding the seed; `--reconfigure` reports nothing
  new; `entry seed --dry-run` writes nothing.

## 6. Docs

`docs/installation_runbook.md` (the new questions, what is seeded),
`docs/entry.md` (`entry seed`, the references project), `AGENTS.md` (one
bullet: the seed is ensure-only and `seed.toml` / `defaults.toml` agreement
is tested).

## 7. Not in this work

- The experiment reading sample information from the store by identifier.
- Analyses written to the store from the experiment (records stay files).
- Editing an existing identifier's sample.
- More reactors than Triga.
- Seeding a data-reduction install.
