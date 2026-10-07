# Install Defaults Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fresh instrument install has a database seeded with a references project, one sample per special identifier and the Triga production ratios, and its new blank/air/cocktail runs carry that sample and project.

**Architecture:** Shipped files in `profiles/instrument-common` (`seed.toml`, `identifiers.toml`, `defaults.toml` with sample tables). `libs/experiment` run defaults learn a `sample` table. `libs/entry` parses and applies the seed through the store's ensure-by-key calls. `libs/setup` only computes the instrument install's database url; `elctl init`, `elctl entry seed` and the UI wizard open the store and call the seed.

**Tech Stack:** C++20, toml++, nlohmann_json (entry only), GoogleTest, CMake preset `dev`.

**Spec:** `docs/superpowers/specs/2026-10-07-install-defaults-design.md`

## Global Constraints

- Read `AGENTS.md` first. Branch `feat/install-defaults`. Conventional Commits with the component as scope. Never push to `main`.
- Test first: each test is seen failing before the code that passes it.
- `libs/setup` must not depend on persistence. Qt appears in no `libs/` library but persistence.
- A build with `-DPYCHRON_PERSISTENCE=OFF` still builds and passes: store code in apps sits behind `PYCHRON_ELCTL_HAS_STORE` / `PYCHRON_UI_HAS_STORE`.
- The seed only ensures: it never changes or removes a row or a reactor that exists.
- Project `references`. Samples and materials: `bu` blank_unknown/blank, `ba` blank_air/blank, `bc` blank_cocktail/blank, `be` blank_extractionline/blank, `bg` background/blank, `a` air/air, `c` cocktail/cocktail, `ic` detector_ic/air. Sample name = analysis type name. No sample for `pa`, `dg`.
- Reactor `Triga`: K4039 [0.00873, 0.00017], K3839 [0.013, 0.0], K3739 [0.0, 0.0], Ca3937 [0.000758, 7e-06], Ca3837 [4e-05, 2e-05], Ca3637 [0.000286, 5e-07], Cl3638 [250.0, 0.0], Ca_K [1.96, 0.0], Cl_K [0.227, 0.0].
- Build and run: `cmake --build build/dev -j 10`, then `ctest --test-dir build/dev -j 8 --output-on-failure`. Full suite green before the last commit of every task.

## Review Focus

Each line has its test in the task named.

1. A shared store that already has a `references` project with a principal investigator, or the sample `air` under it: the seed keeps them as they are and reports them kept (Task 4).
2. A special identifier the legacy importer made, with no sample: kept sample-less, named in `kept`, no error (Task 4).
3. A `reactors.json` already stored as `content_text`, or one that is not valid JSON: the seed does not overwrite it; the invalid one is an error that says so and writes nothing (Task 4).
4. A lab-edited `seed.toml` that no longer parses: `elctl init --reconfigure` still writes the files, exits as it would have, and prints the file, the problem and `elctl entry seed` (Task 6).
5. An instrument install on a server whose schema is behind or which cannot be reached: the install succeeds, the seed is skipped with the reason (Task 6).

---

### Task 1: Run defaults carry a sample

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/factory/defaults.hpp`, `libs/experiment/src/factory/defaults.cpp`
- Test: `tests/experiment/test_run_factory.cpp`

**Interfaces:**
- Produces: `RunDefaults::sample` of type `std::optional<SampleInfo>`; `defaults.toml` table `[<type>.<device>.sample]` with keys `sample`, `material`, `project` only.

- [ ] **Step 1: Failing tests** in `test_run_factory.cpp`:

```cpp
TEST(RunDefaults, ASampleTableIsReadAndGivenToTheRun) {
  auto t = DefaultsTable::from_toml(R"(
[air."*"]
template = "mc"
[air."*".sample]
sample = "air"
material = "air"
project = "references"
)");
  ASSERT_TRUE(t) << t.error().what;
  auto run = make_special_run(AnalysisType::Air, IdentifierRules::defaults(), "", *t);
  ASSERT_TRUE(run) << run.error().what;
  EXPECT_EQ(run->sample.sample, "air");
  EXPECT_EQ(run->sample.material, "air");
  EXPECT_EQ(run->sample.project, "references");
}
TEST(RunDefaults, AnUnknownSampleKeyIsAnError) {
  auto t = DefaultsTable::from_toml("[air.\"*\"]\ntemplate = \"mc\"\n[air.\"*\".sample]\nirradiation = \"NM-1\"\n");
  ASSERT_FALSE(t);
  EXPECT_NE(t.error().what.find("unknown key 'irradiation'"), std::string::npos) << t.error().what;
}
TEST(RunDefaults, DefaultsWithoutASampleLeaveTheRunsSampleAlone) {
  // apply_defaults on a run that has a sample, with defaults that have none: unchanged.
}
```

Write the third test's body from `apply_defaults(RunSpec&, const RunDefaults&)`.

- [ ] **Step 2:** Build, run `pychron_experiment_tests --gtest_filter='RunDefaults.*'`. Expected: first two fail (unknown key `sample`).
- [ ] **Step 3:** Add the field; in `Parser::entry` allow `sample`, parse it as the `extraction` table is parsed (`'sample' must be a table` otherwise); `apply_defaults` sets `run.sample` when given. Update the header comment that documents the file's tables.
- [ ] **Step 4:** Tests pass; full suite.
- [ ] **Step 5:** Commit `feat(experiment): run defaults can give a new run its sample and project`.

### Task 2: The shipped files

**Files:**
- Create: `profiles/instrument-common/seed.toml`, `profiles/instrument-common/identifiers.toml`
- Modify: `profiles/instrument-common/profile.toml` (two `[[files]]`), `profiles/instrument-common/defaults.toml`, `profiles/instrument-common/experiment.toml`
- Test: `tests/setup/test_profiles.cpp`

**Interfaces:**
- Produces: `<root>/seed.toml` in the format of spec 3.1 (top-level `project`, `[[samples]]` with `identifier`, `analysis_type`, `sample`, `material`, `[reactors.<name>]` with `<key> = [value, error]`); `<root>/identifiers.toml`.

- [ ] **Step 1: Failing tests** in `test_profiles.cpp`, in the existing `InstrumentProfile` parameterized suite (it installs each of argus, helix, ngx into a scratch root; reuse its install helper):

```cpp
TEST_P(InstrumentProfile, ShipsIdentifiersThatAreTheBuiltInOnes) {
  // load <root>/identifiers.toml with IdentifierRules::load
  // for every AnalysisType but Unknown: rules.classify(defaults-prefix) == that type,
  // and make_special_run(type, loaded, "", {}) gives the identifier
  // make_special_run(type, IdentifierRules::defaults(), "", {}) gives.
}
TEST_P(InstrumentProfile, ShipsASeedThatTheDefaultsAndTheExampleQueueAgreeWith) {
  // parse <root>/seed.toml with toml++: project == "references"; 8 samples, exactly the
  // (identifier, analysis_type, sample, material) rows of Global Constraints.
  // DefaultsTable::load(<root>/defaults.toml): for each seed row, find(type, "*")->sample is
  // {sample, material, "references"}. Pause and Degas have no defaults sample.
  // load_queue_file(<root>/experiment.toml): the "bu" run's sample is {"blank_unknown","blank","references"}.
}
TEST_P(InstrumentProfile, ShipsTheTrigaProductionOfTheNm293Fixture) {
  // seed.toml [reactors.Triga]: the nine keys with the values of Global Constraints, compared exactly.
}
```

`defaults.toml` today has entries only for some types; the test requires one (with `template`) for each of the eight.

- [ ] **Step 2:** Run `pychron_setup_tests --gtest_filter='*InstrumentProfile*'`. Expected: the three fail (file missing).
- [ ] **Step 3:** Write the files. `identifiers.toml` in the format documented at `libs/experiment/include/pychron/experiment/model/identifiers.hpp:21-28`, with the prefixes of `libs/experiment/src/model/identifiers.cpp` `kTypes` and a comment saying what the file is. `seed.toml` opens with the comment of spec 3.1. New `defaults.toml` entries take the template and scripts their nearest existing neighbor uses (`blank_air`, `blank_cocktail`, `blank_extractionline`, `background` as `blank_unknown`; `cocktail` as `air`).
- [ ] **Step 4:** Tests pass; full suite (`InstallsLoadsAndPassesDoctor` and the elctl setup tests still pass with the two new files).
- [ ] **Step 5:** Commit `feat(setup): an instrument install ships its special identifiers, a seed file and samples for reference runs`.

### Task 3: Parsing the seed

**Files:**
- Create: `libs/entry/include/pychron/entry/seed.hpp`, `libs/entry/src/seed.cpp`, `tests/entry/test_seed.cpp`
- Modify: `libs/entry/CMakeLists.txt` (source; `tomlplusplus` PRIVATE, as `libs/experiment/CMakeLists.txt` links it), `tests/entry/CMakeLists.txt` if it lists files

**Interfaces:**
- Produces (namespace `pychron::entry`, public header std-only plus persistence types):

```cpp
struct SeedSample {
  std::string identifier, analysis_type, sample, material;
  friend bool operator==(const SeedSample&, const SeedSample&) = default;
};
struct Seed {
  std::string project;
  std::vector<SeedSample> samples;                              // file order
  std::map<std::string, persistence::ProductionValue> reactors; // value.reactor == key; ratios in production_keys() order
};
Result<Seed> parse_seed(std::string_view toml, std::string_view name = "seed.toml");
```

- [ ] **Step 1: Failing tests** `TEST(Seed, Parses)` (the spec 3.1 text with two samples and Triga: fields, order, nine ratios) and one `TEST(Seed, Refuses...)` per refusal, each asserting `ErrorKind::Config` and that the message holds `name` and the quoted offender:
  - unknown top-level key; unknown key in a sample; unknown key in a reactor (not one of the nine: use `"K4038"`)
  - missing `project`; project `"my refs"` (`valid_project_name`, `names.hpp`)
  - sample missing any of its four fields, or one empty
  - `analysis_type = "blank"` (not a name `experiment`'s `to_string(AnalysisType)` gives; entry does not link experiment, so keep the list of the eight seedable names plus the check in `seed.cpp`, and refuse `unknown`, `pause`, `degas`)
  - the same `identifier` twice (case-insensitive)
  - a ratio that is not an array of two numbers; a ratio holding `nan` or `inf`
  - text that is not TOML
  - `TEST(Seed, AFileWithNoSamplesAndNoReactorsIsASeedOfTheProjectAlone)`: parses.
- [ ] **Step 2:** Build fails (no header). Add the header and an empty-bodied `parse_seed` returning `fail(ErrorKind::Config, "not implemented")`; run `pychron_entry_tests --gtest_filter='Seed.*'`; expected: all fail.
- [ ] **Step 3:** Implement. The nine keys come from the same list `package_edit.cpp` uses (`production_keys()`, `package_edit.cpp:35-39`); move it to a private header in `libs/entry/src` rather than copy it.
- [ ] **Step 4:** Tests pass; full suite.
- [ ] **Step 5:** Commit `feat(entry): the seed file is read and checked`.

### Task 4: Applying the seed

**Files:**
- Modify: `libs/entry/include/pychron/entry/seed.hpp`, `libs/entry/src/seed.cpp`, `tests/entry/test_seed.cpp`

**Interfaces:**
- Consumes: `Seed`, `parse_seed`; `IStore::add_project / add_material / add_sample / add_identifier` (`store.hpp:586-592`, specs at `store.hpp:108-154`), `find_catalog_row`, `add_ref_object`, `begin`, `add_revision`, `commit` as `save_settings` uses them (`libs/entry/src/settings.cpp:96-116`); `load_reactors` (`package_edit.hpp`). Test store: `tests/entry/store_fixture.hpp`, as `test_package_edit.cpp` uses it (SQLite always, PostgreSQL with `PYCHRON_TEST_PG_URL`).
- Produces:

```cpp
struct SeedReport {
  int projects = 0, materials = 0, samples = 0, identifiers = 0, reactors = 0;  // made by this call
  std::vector<std::string> kept;  // "project references", "sample air", "identifier bu (no sample)", "reactor Triga"
  bool changed() const;           // any count non-zero
};
// The spec's `actor` is a persistence::Actor, as every other entry write takes.
Result<SeedReport> apply_seed(persistence::IStore& store, const Seed& seed, const persistence::Actor& actor,
                              bool dry_run = false);
std::string describe(const SeedReport& report);  // one line: "seeded 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor" / "seed: nothing to add (18 already there)"
```

`dry_run` reads only and reports what a real call would make.

- [ ] **Step 1: Failing tests** (fixture store; `seed()` helper = `parse_seed` of the shipped file read from `PYCHRON_PROFILES_DIR "/instrument-common/seed.toml"`, a compile definition added to the entry tests):

```cpp
TEST_P(SeedStore, AnEmptyStoreGetsEverything)
  // report: 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor; kept empty.
  // the identifier "a" row: kind special, analysis_type air, sample = the sample "air" of project references.
  // load_reactors(store): one reactor "Triga", nine ratios == Global Constraints.
TEST_P(SeedStore, ASecondRunWritesNothing)
  // report.changed() false, kept.size() == 18; the store's changeset count is the same before and after.
TEST_P(SeedStore, ADryRunReportsAndWritesNothing)
  // same counts as the first test; afterwards the store is still empty of project and reactors.json.
TEST_P(SeedStore, WhatIsThereIsKept)                       // Review Focus 1
  // before: add_principal_investigator "Ross", add_project{"references", pi}, sample "air" under it with material "air".
  // after: the project still has its PI; kept holds "project references" and "sample air"; samples == 7.
TEST_P(SeedStore, AnIdentifierWithoutASampleIsKeptAsItIs)  // Review Focus 2
  // before: add_identifier{"bu", "special", "blank_unknown"} with no sample.
  // after: its sample is still null; kept holds "identifier bu (no sample)"; identifiers == 7; no error.
TEST_P(SeedStore, AnEditedReactorIsNotChangedAndAMissingOneIsAdded)
  // before: reactors.json {"Triga": {"K4039": [0.01, 0.001]}} as content_json.
  // seed with Triga and a second reactor "Osu" (one ratio): after, Triga is still the one-ratio 0.01; Osu is there;
  // report.reactors == 1; kept holds "reactor Triga"; the document has exactly one more revision.
TEST_P(SeedStore, AReactorsDocumentKeptAsTextIsReadAndExtended)  // Review Focus 3
  // before: the same JSON in content_text. after: Triga unchanged, Osu added.
TEST_P(SeedStore, AReactorsDocumentThatIsNotJsonIsLeftAloneAndSaid)  // Review Focus 3
  // before: content_text "not json". apply_seed fails (ErrorKind::Config, message holds "reactors.json");
  // the document's head is the revision it had; project/samples of the same call: decide and assert one
  // behavior: reactors are written last, so the catalog rows exist. State that in the header comment.
```

- [ ] **Step 2:** Run `pychron_entry_tests --gtest_filter='*SeedStore*'`. Expected: fail (undefined `apply_seed`; add a stub that fails to see them run red).
- [ ] **Step 3:** Implement in the order of spec 4.3. "Made or kept" for an ensure call is decided by `find_catalog_row` before it (the ensure calls return the uuid either way). An added reactor keeps every other key of the existing JSON object untouched (parse, insert, serialize; do not rebuild it from `parse_reactors`, which drops what it does not know).
- [ ] **Step 4:** Tests pass; full suite; with PostgreSQL too if `PYCHRON_TEST_PG_URL` is set on this machine (say in the commit body whether it was).
- [ ] **Step 5:** Commit `feat(entry): a seed puts the reference project, samples, identifiers and reactors in a store`.

### Task 5: An instrument install has a database

**Files:**
- Create: `profiles/store-common/profile.toml`, move `profiles/data-reduction/credentials.toml` to `profiles/store-common/credentials.toml`
- Modify: `profiles/data-reduction/profile.toml` (includes `store-common`; its six questions and the credentials `[[files]]` leave), `profiles/instrument-common/profile.toml` (includes `store-common`; `groups` gains `"Data"` last), `libs/setup/src/doctor.cpp` (`site_install`)
- Test: `tests/setup/test_profiles.cpp`, `apps/elctl/tests/test_setup_commands.cpp`

**Interfaces:**
- Produces: for an instrument install, `SiteInstall::database == database_url_for(answers, root, false)` (`sqlite:<root>/data/pychron.db` by default). Question ids unchanged: `data_source`, `db_host`, `db_port`, `db_name`, `db_user`, `db_password`.

- [ ] **Step 1: Failing tests**
  - `test_profiles.cpp`: `TEST_P(InstrumentProfile, HasADatabaseOnThisComputerByDefault)`: `site_install(plan, "x").database == "sqlite:" + (root / "data" / "pychron.db").generic_string()`; with `data_source=server` and the db answers it is the `postgresql://` url and `.pychron/credentials.toml` is planned as a secret file.
  - Update `InstrumentPagesAreSimulationConnectionThenDetectors`: `Data` is the last page.
  - `DataReductionLocalAndServer` must pass unchanged (the move is invisible to it).
- [ ] **Step 2:** Run the setup tests. Expected: the new test and the pages test fail.
- [ ] **Step 3:** Make the fragment (`kind = "fragment"`, `version = 1`, group `Data`); move the questions verbatim; in `site_install` set `database` for both kinds. Check `doctor()` (`libs/setup/src/doctor.cpp`) on an instrument install with a database it cannot open yet: with no `open_database` hook it must not fail; with one, a missing SQLite file is reported the way a data-reduction install's is.
- [ ] **Step 4:** Fix what the new question breaks: `ElctlSetupTest.QuestionsAreAskedWithDefaultsInBrackets` feeds answers by position (one more Enter for `data_source`); the wizard self-test. Full suite.
- [ ] **Step 5:** Commit `feat(setup): an instrument install has a database, on this computer or the lab's server`.

### Task 6: `elctl init` seeds, and `elctl entry seed`

**Files:**
- Modify: `apps/elctl/src/setup.cpp` (the block at `:299-311`), `apps/elctl/src/entry.cpp` (usage text at `:56-81`, command table at `:589-598`), `apps/elctl/src/cli.cpp` (the entry summary line near `:107`)
- Test: `apps/elctl/tests/test_setup_commands.cpp`, `apps/elctl/tests/test_entry_cmd.cpp`

**Interfaces:**
- Consumes: `entry::parse_seed`, `entry::apply_seed`, `entry::describe`; `persistence::open_store(StoreConfig{url, migrate})`; the actor an entry command gets from its context in `entry.cpp`; `setup::database_url(const SiteInstall&)` (adds the server password from `.pychron/credentials.toml`).
- Produces: `elctl entry seed [<seed.toml>] [--dry-run]` (default file `<install root>/seed.toml`); exit 0 when applied or nothing to add, 1 on a file or store error, 2 on usage.

Behavior of `init` after `apply_install`, for both kinds as today plus:

| install | step |
|---|---|
| instrument, `sqlite:` | open with migrate (prints `database ... ready (...)` as data reduction does), then seed `<root>/seed.toml` |
| instrument, `postgresql://` | open without migrate; seed only if `schema_status()` is current |
| data reduction | unchanged, no seed |

A seed that cannot run prints one line to stderr and the install goes on to its normal exit code:
`seed skipped: <reason>. Run: elctl --install <name> entry seed`. Reasons: the file's parse error, `the database could not be opened: ...`, `the database schema is not current`. A missing `seed.toml` is silent (an install from before this work). Without `PYCHRON_ELCTL_HAS_STORE`: `skip  seed: built without the DVC store` on stdout.

- [ ] **Step 1: Failing tests**

```cpp
TEST_F(ElctlSetupTest, AnInstrumentInstallMakesAndSeedsItsDatabase)
  // init argus --yes: code as before; data/pychron.db exists; out holds "seeded 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor".
  // elctl --install lab entry samples list --project references: the eight sample names.
  // second init: out holds "seed: nothing to add".
TEST_F(ElctlSetupTest, ASeedFileThatDoesNotParseDoesNotStopTheInstall)   // Review Focus 4
  // init helix --yes; overwrite <root>/seed.toml with "project = \n"; init --reconfigure --yes:
  // same exit code as a reconfigure with a good file; err holds "seed skipped: " and "seed.toml" and "entry seed".
TEST_F(ElctlSetupTest, AServerThatCannotBeReachedSkipsTheSeed)           // Review Focus 5
  // init argus --yes --set data_source=server --set db_host=127.0.0.1 --set db_port=1 --set db_password=x:
  // files written, exit code as the doctor's would be without this work, err holds "seed skipped: the database could not be opened".
TEST_F(ElctlEntryTest, SeedAppliesAFileAndDryRunWritesNothing)
  // against the test's store: `entry seed <file> --dry-run` prints the counts and `samples list --project references` is empty;
  // `entry seed <file>` then lists them; again prints "seed: nothing to add"; a missing file: code 1; `entry seed a b`: code 2.
```

The "schema is not current" branch has no cheap fixture: cover it by a unit of the decision (a free function `Result<void> seedable(const std::vector<SchemaVersion>& status)` or the like beside the store helper, tested directly), not by a server.

- [ ] **Step 2:** Run the two elctl test binaries with the filters. Expected: fail.
- [ ] **Step 3:** Implement. One function in `apps/elctl/src` shared by `init` and `entry seed` does open, parse, apply and describe; `init` wraps it in the skip rule. Keep everything store-typed inside `#ifdef PYCHRON_ELCTL_HAS_STORE`; `entry_stub.cpp` needs no change beyond what `entry`'s usage already prints.
- [ ] **Step 4:** Tests pass; full suite. Configure and build once with `-DPYCHRON_PERSISTENCE=OFF` in a scratch build dir (`cmake -S . -B build/nopersist -DBUILD_TESTS=ON -DPYCHRON_PERSISTENCE=OFF && cmake --build build/nopersist -j 10 && ctest --test-dir build/nopersist -j 8`); expected: builds, passes, `init` prints the skip line.
- [ ] **Step 5:** Commit `feat(elctl): init seeds an instrument install's database; entry seed does it by hand`.

### Task 7: The setup wizard seeds

**Files:**
- Modify: `apps/pychron-ui/src/setup_wizard.hpp` (options, near `:50-54`), `apps/pychron-ui/src/setup_wizard.cpp` (`install()`, `:756-786`), `apps/pychron-ui/src/setup_support.cpp` / `.hpp`
- Test: the wizard's existing test file (find it: `grep -rl SetupWizard tests apps/pychron-ui/tests`)

**Interfaces:**
- Consumes: Task 4's functions.
- Produces: `SetupWizard::SeedDatabase = std::function<Result<std::string>(const std::string& url, const std::filesystem::path& seed_file, bool migrate)>` in the wizard's options, returning `describe(report)`; `ui::database_seeder()` in `setup_support` (empty without `PYCHRON_UI_HAS_STORE`), passed wherever `database_opener()` is.

Same table and skip rule as Task 6. A skipped seed does not fail `install()`: its line is shown with the checks on the wizard's last page (add it to what that page lists; name `seed`, status warning, hint `elctl --install <name> entry seed`).

- [ ] **Step 1: Failing tests** with fake hooks (no store needed): an instrument install calls `open_database(url, true)` then `seed_database(url, <root>/seed.toml, true)` for `sqlite:`; a failing seeder leaves `install()` true and the last page showing the warning; a data-reduction install never calls the seeder; with no seeder nothing is called and nothing is shown.
- [ ] **Step 2:** Build the `dev-ui` preset (`cmake --preset dev-ui && cmake --build build/dev-ui -j 10`), run the wizard tests. Expected: fail.
- [ ] **Step 3:** Implement; add a `database_seeder()` line to `self_test` (seed `sqlite::memory:` with the shipped file; `skip` line without the store).
- [ ] **Step 4:** `ctest --test-dir build/dev-ui -j 8 --output-on-failure` and the `dev` suite.
- [ ] **Step 5:** Commit `feat(ui): the setup wizard seeds an instrument install's database`.

### Task 8: Docs

**Files:**
- Modify: `docs/installation_runbook.md`, `docs/entry.md`, `AGENTS.md`, `profiles/README.md`, the spec (status line: implemented; section 4.3: `apply_seed` takes a `persistence::Actor` and `dry_run`)

- [ ] **Step 1:** Runbook: the Data questions of an instrument install; what setup puts in a new database (the table of Global Constraints, Triga); that existing rows are never changed; `seed.toml` may be edited and `elctl entry seed` run again; the skip messages and what to do.
- [ ] **Step 2:** `entry.md`: the `references` project and its samples; `elctl entry seed [--dry-run]`; a sample-less special identifier from a legacy import stays so.
- [ ] **Step 3:** `AGENTS.md`, under Build and test, one bullet: the seed (`libs/entry` `seed.hpp`) only ensures, never edits; `seed.toml`, `defaults.toml` and the example queue are held together by `tests/setup/test_profiles.cpp`; the Triga values are those of the NM-293 fixture.
- [ ] **Step 4:** Full `dev` suite once more. Commit `docs: install defaults (the seed, the references project, entry seed)`.
