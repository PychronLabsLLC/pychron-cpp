# Changing an installation's data source: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Point an existing install at another database (local SQLite file or the lab's PostgreSQL server) and another data folder, from `elctl data-source set` and from a "Data source" page in pychron-ui's Preferences, without re-running setup.

**Architecture:** One Qt-free, persistence-free engine in `libs/setup` (`data_source.hpp/.cpp`): read the current source, plan a change (refusals, notes), check it through injected openers, write exactly three files (credentials, install record, site entry), seed. `elctl` and pychron-ui only parse input, inject the store openers they already have, and show the result. The `store-common` profile gains `db_file` and `data_folder` so a later reconfigure keeps what was set.

**Tech Stack:** C++20, toml++, GoogleTest (`pychron_setup_tests`, `pychron_elctl_tests`), Qt 6 Widgets and QtTest (`tests/ui`, offscreen).

**Spec:** `docs/superpowers/specs/2026-10-18-data-source-editing-design.md`

## Global Constraints

- `libs/setup` stays Qt-free and persistence-free; the store is reached only through injected `std::function` openers, as `DoctorOptions::open_database`.
- Errors are `pychron::Result<T>`; refusals are `ErrorKind::Config` and write nothing.
- Secrets never in `install.toml` or `site.toml`. `credentials.toml` is owner-only (0600), written by the same `write_file(..., secret = true)` path as an install.
- `apply_data_source` writes in this order and nothing else under the root: `<root>/.pychron/credentials.toml` (server with a new password only), `<root>/.pychron/install.toml`, `site.toml`.
- Nothing is copied or moved (old database, `records/`). A local file that exists is never migrated from here; a server is never migrated from here.
- Paths stored as given; relative ones resolve against the root (`SiteInstall::path`). Outside the root allowed.
- `store-common` version 1 → 2; defaults `db_file = "data/pychron.db"`, `data_folder = "data"` keep every first install as today.
- Password never on the command line: `--password-stdin`, or a no-echo prompt (`read_password`, `apps/elctl/src/earthbank_prompt.cpp`).
- `elctl data-source set` exit codes: 0 written (or nothing to change), 1 refused or check failed, 2 usage.
- The Data source page uses neither QSettings nor the line's local override file.
- Commit messages: Conventional Commits with component scope: `feat(setup)`, `feat(elctl)`, `feat(ui)`, `refactor(ui)`, `docs`.

## Review Focus

- A password holding `@`, `:`, `/`, `"` or `\`: percent-encoded in the URL that opens the store, TOML-escaped in `credentials.toml`, and the file reads back the same password. Test in Task 4 (`ApplyKeepsAPasswordWithSpecialCharacters`).
- Server → local, then `plan_install` reconfigure: the stale `credentials.toml` (still in the record's `files`) gives no conflict and is not removed. Test in Task 4 (`AReconfigureAfterServerToLocalHasNoConflicts`).
- A change to exactly what the install already uses: nothing written, said so, exit 0. Tests in Task 3 (`APlanToTheCurrentSourceChangesNothing`) and Task 6 (`SetToTheCurrentSourceWritesNothing`).
- A data folder or database file with spaces, or absolute outside the root: stored as given, URL and doctor resolve it. Test in Task 3 (`PathsOutsideTheRootAndWithSpacesAreKeptAsGiven`).
- A site file holding other installs and a default: after apply, the other entries and `default` unchanged. Covered by Task 4's site test (`ApplyLocalToServerWritesTheThreeFiles` asserts the second entry and `default`).

---

### Task 1: Install record writer and kept password, shared

Refactor so later tasks can rewrite `install.toml` and read the stored password without duplicating `apply_install` and `doctor.cpp`.

**Files:**
- Modify: `libs/setup/include/pychron/setup/install.hpp`, `libs/setup/src/install.cpp` (record serialization now inline in `apply_install`, around line 300-337)
- Modify: `libs/setup/include/pychron/setup/doctor.hpp`, `libs/setup/src/doctor.cpp` (credentials read in `database_url`, around line 271)
- Test: `tests/setup/test_install.cpp`

**Interfaces:**
- Produces:
  - `Result<void> write_install_record(const std::filesystem::path& root, const InstallRecord& record);` (install.hpp) writes `record_path(root)`, not secret, same TOML `apply_install` writes today.
  - `Result<std::optional<std::string>> read_kept_password(const std::filesystem::path& root);` (doctor.hpp): `[database] password` of `<root>/.pychron/credentials.toml`; `nullopt` when the file or key is missing; `Config` when the file does not parse.
  - `apply_install` and `database_url(const SiteInstall&)` call these.

- [ ] **Step 1: Write the failing tests** in `tests/setup/test_install.cpp`:
  - `TEST_F(Fixture, AnInstallRecordWrittenAgainReadsBackTheSame)`: install a profile, `read_install_record`, change one answer, `write_install_record`, read again: `profile`, `versions`, `files` equal, the changed answer is the new value, every other answer equal.
  - `TEST(Installer, TheKeptPasswordIsReadFromTheCredentialsFile)`: temp root without `.pychron` → `nullopt`; with `[database]\npassword = "p@ss:w/rd"` → `"p@ss:w/rd"`; with `[database` (bad TOML) → error kind `Config`.
- [ ] **Step 2: Run, expect compile failure** (`write_install_record`, `read_kept_password` undeclared):
  `cmake --build --preset dev --target pychron_setup_tests`
- [ ] **Step 3: Implement** both functions, move `apply_install`'s record serialization into `write_install_record`, and make `database_url` use `read_kept_password`.
- [ ] **Step 4: Run** `build/dev/tests/setup/pychron_setup_tests --gtest_filter='Fixture.*:Installer.*:Site.*'`; expect PASS, existing cases included.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 6: Commit** `refactor(setup): the install record writer and the kept password are shared`

### Task 2: `store-common` asks for the database file and the data folder

**Files:**
- Modify: `profiles/store-common/profile.toml`
- Modify: `libs/setup/src/installer.cpp` (`database_url_for`), `libs/setup/include/pychron/setup/installer.hpp` (its comment)
- Modify: `libs/setup/src/doctor.cpp` (`site_install`)
- Test: `tests/setup/test_install.cpp`, `tests/setup/test_profiles.cpp`

**Interfaces:**
- Produces: answers `db_file` (path), `data_folder` (path). `database_url_for(answers, root, with_password)` for local returns `"sqlite:" + (file is absolute ? file : root / file).generic_string()` with `file = db_file` or `"data/pychron.db"` when absent. `site_install(plan, name).data` = `data_folder` or `"data"` when absent.

- [ ] **Step 1: Write the failing tests:**
  - `tests/setup/test_install.cpp`, extend `TEST(Installer, DatabaseUrlsFromTheAnswers)`: answers without `db_file` → `"sqlite:" + (root / "data" / "pychron.db").generic_string()` (unchanged); `db_file = "lab/copy.db"` → `root / "lab/copy.db"`; `db_file` absolute `/shared/pychron.db` (built from a temp dir, so it is absolute on Windows too) → that path.
  - `tests/setup/test_profiles.cpp`, `TEST(Profiles, StoreCommonAsksForTheFileAndTheDataFolder)`: library's `store-common` has version 2; `db_file` type `Path`, default `"data/pychron.db"`, group `Data`, `when` `data_source == "local"`; `data_folder` type `Path`, default `"data"`, group `Data`, no `when`; prompts "Database file", "Data folder"; helps "Relative to the install folder, or a full path", "Where run records are kept".
  - Same file, `TEST(Profiles, ADataFolderAnswerIsTheSiteEntrysData)`: data-reduction plan with `data_folder = "/x/records-disk"` (temp absolute) → `site_install(...).data` is that string; without the answer → `"data"`.
- [ ] **Step 2: Run, expect FAIL** `build/dev/tests/setup/pychron_setup_tests --gtest_filter='Installer.DatabaseUrlsFromTheAnswers:Profiles.*'` after `cmake --build --preset dev --target pychron_setup_tests`.
- [ ] **Step 3: Implement:** add the two `[[questions]]` after `db_password` in `profile.toml` (fields exactly as spec 3.2), `version = 2`; `database_url_for` and `site_install` as in Interfaces.
- [ ] **Step 4: Run** the whole `pychron_setup_tests` binary (every profile still renders with defaults, `DataReductionLocalAndServer` and `HasADatabaseOnThisComputerByDefault` unchanged). Expect PASS. A test that pinned `store-common` at version 1 is edited to 2, nothing else.
- [ ] **Step 5: Run** the wizard UI test, since the Data page is generated from the profile: `cmake --build --preset dev-ui --target pychron_ui_test_setup_wizard` then `build/dev-ui/tests/ui/pychron_ui_test_setup_wizard`. Expect PASS (fix any count of Data-page fields the test pins, and only that).
- [ ] **Step 6: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 7: Commit** `feat(setup): an install can name its database file and its data folder`

### Task 3: Data source model: current source, URL, plan

**Files:**
- Create: `libs/setup/include/pychron/setup/data_source.hpp`, `libs/setup/src/data_source.cpp` (globbed by `libs/setup/CMakeLists.txt`)
- Create: `tests/setup/test_data_source.cpp` (globbed by `tests/setup/CMakeLists.txt`)

**Interfaces:**
- Consumes: `read_install_record` (install.hpp), `database_url_for` semantics of Task 2.
- Produces (namespace `pychron::setup`), exactly the spec 3.1 declarations: `LocalDatabase`, `ServerDatabase`, `DataSource`, `current_data_source`, `database_url(const DataSource&, const std::filesystem::path& root)`, `DataSourceChange`, `plan_data_source`. Plus:
  - `bool operator==` (defaulted) on `LocalDatabase`, `ServerDatabase`, `DataSource`.
  - `Result<std::string> database_url_with_password(const DataSource& source, const std::filesystem::path& root);` server: password from `source.password`, else `read_kept_password(root)`, percent-encoded as `database_url_for(..., true)`; local: as `database_url`.
  - `std::string describe(const DataSource& source, const std::filesystem::path& root);` two lines, `database <url>` and `data folder <path as given> (<resolved>)`, used by both apps' before → after.
  - `DataSourceChange::notes` text pinned here: records note `"records in <old>/records are not moved; <new> starts without them"`; no record `"no install record in <root>/.pychron: a later elctl init --reconfigure would not know the new data source"`; unchanged `"nothing to change"` (and `install` equal to the input).

- [ ] **Step 1: Write the failing tests** in `tests/setup/test_data_source.cpp` (temp root and site per test, `PYCHRON_SITE_CONFIG` not needed: paths passed in):
  - `CurrentSourceComesFromTheRecord`: record answers `data_source=server, db_host=h, db_port=5433, db_name=n, db_user=u, data_folder=runs`, site entry saying local → `ServerDatabase{"h",5433,"n","u",nullopt}`, `data == "runs"`.
  - `CurrentSourceFallsBackToTheSiteEntry`: no record, site `database = "sqlite:<root>/x.db"`, `data = "d"` → `LocalDatabase{<root>/x.db}`, `data == "d"`; site `postgresql://u@h:5432/n` → server fields parsed.
  - `AVersionOneRecordIsTheDefaultFileAndFolder`: record with `data_source=local`, no `db_file`, `data_folder` → `LocalDatabase{"data/pychron.db"}`, `"data"`.
  - `DatabaseUrlIsTheSiteEntrysUrl`: relative, absolute file; server `postgresql://u@h:5433/n`, no password in it even when `password` set.
  - `RefusalsWriteNothing`: for each of empty host, empty name, empty user, port 0, port 65536, empty file, empty data, file naming an existing directory, root missing on disk: `plan_data_source` error kind `Config`, and a hash of every file under the temp dir equal before and after.
  - `APlanSetsTheSiteFieldsAndSaysWhatIsNotMoved`: old data folder with `records/a.toml`, new data `other` → `change.install.data == "other"`, `change.install.database == database_url(to, root)`, notes contain the records note; same with an empty `records/` → no records note.
  - `AnInstallWithoutARecordIsToldSo`: notes contain the no-record note, no error.
  - `APlanToTheCurrentSourceChangesNothing`: `to == current_data_source(install)` → notes `{"nothing to change"}`.
  - `PathsOutsideTheRootAndWithSpacesAreKeptAsGiven`: data `"<tmp>/shared disk/runs"` and file `"my data/p.db"` → `install.data` exactly that string, URL `sqlite:<root>/my data/p.db`.
- [ ] **Step 2: Run, expect compile failure** (header missing): `cmake --build --preset dev --target pychron_setup_tests`.
- [ ] **Step 3: Implement** in `data_source.cpp`. Postgres URL parse for the site fallback: `postgresql://user[:pw]@host:port/db`, password part ignored. A file "names an existing directory" when `fs::is_directory(install.path(file))`.
- [ ] **Step 4: Run** `build/dev/tests/setup/pychron_setup_tests --gtest_filter='DataSource*.*'`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 6: Commit** `feat(setup): a data source change is planned and refused before anything is written`

### Task 4: Apply a data source change

**Files:**
- Modify: `libs/setup/include/pychron/setup/data_source.hpp`, `libs/setup/src/data_source.cpp`
- Test: `tests/setup/test_data_source.cpp`

**Interfaces:**
- Consumes: `write_install_record`, `read_install_record`, `sha256_hex`, `render` (template.hpp), `load_site`/`save_site`, Task 3's types.
- Produces: `apply_data_source(change, to, site_path)` per spec 3.1. Credentials content is `render` of the constant `kCredentialsTemplate` in `data_source.cpp`, byte-equal to `profiles/store-common/credentials.toml`, with answer `db_password`; written by the owner-only path (extract `write_file` from `install.cpp` into an internal header `libs/setup/src/file_io.hpp` if needed, no public API). Record answers replaced: `data_source`, `db_host`, `db_port` (int64), `db_name`, `db_user`, `db_file`, `data_folder`; server keys erased for a local source, `db_file` erased for a server one.

- [ ] **Step 1: Write the failing tests:**
  - `ApplyLocalToServerWritesTheThreeFiles`: site with this install, a second install `other`, `default = "other"`; apply server with password `"s3cret"`: credentials 0600 (skip the mode assert on Windows, as `SecretsAreOwnerOnlyAndNeverRecorded` does) reading back `"s3cret"`; record answers updated, `files[".pychron/credentials.toml"] == sha256_hex(file bytes)`; site entry `database == "postgresql://u@h:5432/n"`, `data` set; other fields of the entry, entry `other` and `default` equal to before; `"s3cret"` absent from `install.toml` and `site.toml` bytes.
  - `AKeptPasswordLeavesTheCredentialsFileAlone`: existing credentials file; apply server with `password = nullopt`: file bytes and mtime-independent hash unchanged, record `files` entry unchanged.
  - `ApplyKeepsAPasswordWithSpecialCharacters`: password `p@ss:w/"r\d`; `read_kept_password` returns it; `database_url_with_password` contains `p%40ss%3Aw%2F%22r%5Cd`.
  - `TheCredentialsFileIsWhatTheProfileRenders`: `kCredentialsTemplate` output for password `x` equals `render` of `profiles/store-common/credentials.toml` (`PYCHRON_PROFILES_DIR`).
  - `AReconfigureKeepsTheNewSource`: install data-reduction profile local, apply server with password, then `plan_install(..., record answers, root, {.reconfigure = true, .skip_secret_files = true})` after `keep_unanswered_secrets`: `site_install(plan, name).database` and `.data` equal the applied ones; no file with action `Conflict`. Same with the password given again: credentials action `Update` or `Same`, not `Conflict`.
  - `AReconfigureAfterServerToLocalHasNoConflicts`: server install, apply local; reconfigure plan: no `Conflict`, `credentials.toml` still on disk.
  - `OnlyTheThreeFilesChange`: hash every file under root and the site dir before and after apply: only `.pychron/credentials.toml`, `.pychron/install.toml`, `site.toml` differ.
  - `WithoutARecordOnlyTheSiteAndCredentialsAreWritten`: no `install.toml` before, none after; site updated.
- [ ] **Step 2: Run, expect FAIL** (`apply_data_source` undefined) after building `pychron_setup_tests`.
- [ ] **Step 3: Implement** `apply_data_source` in the spec's order; it re-reads the record (missing record: step 2 skipped).
- [ ] **Step 4: Run** `build/dev/tests/setup/pychron_setup_tests --gtest_filter='DataSource*.*:Fixture.*'`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 6: Commit** `feat(setup): a data source change writes the credentials, the install record and the site entry`

### Task 5: Checks before writing, and the seed after

**Files:**
- Modify: `libs/setup/include/pychron/setup/data_source.hpp`, `libs/setup/src/data_source.cpp`
- Test: `tests/setup/test_data_source.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct DataSourceOpeners {
    // Opens url (with password); migrate: made and migrated. Result describes it
    // ("schema version 12"). Empty: database not checked (built without the store).
    std::function<Result<std::string>(const std::string& url, bool migrate)> open_database;
    // As SetupWizard::SeedDatabase. Empty: not seeded.
    std::function<Result<std::string>(const std::string& url, const std::filesystem::path& seed_file, bool migrate)>
        seed_database;
  };
  // Spec 3.3. `create`: a missing local file is made (folder too) and migrated;
  // false (Test connection): said as "will be made", nothing made. Lines said on
  // success; Config error with the reason on refusal.
  Result<std::vector<std::string>> check_data_source(const DataSourceChange& change, const DataSource& to,
                                                     const DataSourceOpeners& openers, bool create);
  // After apply: instrument install with <root>/seed.toml seeded (ensure-only);
  // what was done or why skipped, never an error.
  std::vector<std::string> seed_data_source(const DataSourceChange& change, const DataSource& to,
                                            const DataSourceOpeners& openers);
  ```
- Pinned text: no opener → `"skip  check: built without the DVC store"`; existing local file of size 0 → refusal `"<file> is empty: not a pychron store"`; existing local file whose open fails with kind `Config` → refusal `"<url>: <first line of error>; run `elctl db migrate --db <url>` first"`; server failure → `"<url without password>: <first line>"` plus doctor's hint `"check the server, the user and the password"`; data folder not writable → `"data folder <path> is not writable"`; seed skipped → `"seed skipped: <first line>"`.

- [ ] **Step 1: Write the failing tests** with a fake opener recording `(url, migrate)` calls:
  - `ANewLocalFileIsMadeAndMigrated`: missing `sub/new.db`, `create = true` → opener called once with `migrate = true`, folder `sub` exists; `create = false` → opener not called, said contains `"will be made"`.
  - `AnExistingLocalFileIsNeverMigrated`: existing non-empty file; opener returns `Config` error → refusal with `elctl db migrate --db sqlite:`; opener called with `migrate = false` only.
  - `AnEmptyFileIsNotAStore`: zero-byte file → refusal `"is empty: not a pychron store"`, opener not called.
  - `AServerIsOpenedWithThePasswordGivenOrKept`: password given → opener url contains it; `nullopt` with kept `k` → contains `k`; opener failure → refusal text without the password; `migrate == false` always.
  - `WithoutAnOpenerTheDatabaseIsNotChecked`: said contains `"skip  check: built without the DVC store"`.
  - `TheDataFolderIsMadeOrRefused`: missing folder made; folder without write permission (POSIX only, `GTEST_SKIP` on Windows, as other permission tests) → refusal.
  - `AnInstrumentInstallIsSeeded`: kind `instrument`, `seed.toml` present → seeder called with `<root>/seed.toml`; seeder error → said `"seed skipped: ..."`; data-reduction install → seeder not called.
- [ ] **Step 2: Run, expect FAIL** after building `pychron_setup_tests`.
- [ ] **Step 3: Implement.** Writability: create the folder (`create_directories`), then make and remove a probe file, as doctor's `check_writable`; reuse it if it can be made non-static without widening the public API.
- [ ] **Step 4: Run** `build/dev/tests/setup/pychron_setup_tests --gtest_filter='DataSource*.*'`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 6: Commit** `feat(setup): a data source is checked before it is written and seeded after`

### Task 6: `elctl data-source` show and `set --local` / `--data`

**Files:**
- Create: `apps/elctl/src/data_source.hpp`, `apps/elctl/src/data_source.cpp` (globbed; store parts under `#ifdef PYCHRON_ELCTL_HAS_STORE`)
- Modify: `apps/elctl/src/cli.cpp` (dispatch beside `doctor`, around line 774; usage text), `apps/elctl/src/setup.cpp` (move `open_store` out of the anonymous namespace into `setup.hpp` so `data_source.cpp` uses it)
- Test: `apps/elctl/tests/test_setup_commands.cpp`

**Interfaces:**
- Consumes: Tasks 3-5; `resolve_install`, `yes(...)` (setup.cpp line 39, exposed in `setup.hpp`), `open_store(url, migrate)`, `seed_database(url, seed, migrate, false)` (`entry.hpp`), `report_doctor`'s database check (expose `report_doctor` in `setup.hpp`).
- Produces: `int data_source_command(const std::vector<std::string>& args, std::optional<std::string> install, Io io);`. Show prints `database <url>`, `data folder <data>`, `password kept` / `no password kept` (server only). `set` prints `describe` before → after, notes, `Write them? [Y/n]` unless `--yes`, then check lines, `written`, then `seed_data_source` lines and doctor's output.

- [ ] **Step 1: Write the failing tests** (`ElctlSetupTest`, installs made with `elctl init` as the file's other tests):
  - `DataSourceShowsTheDatabaseAndTheDataFolder`: exit 0, output has `database sqlite:` and `data folder data`.
  - `SetLocalMakesAndMigratesTheFileAndSeedsAnInstrument` (store builds only, `#ifdef PYCHRON_ELCTL_HAS_STORE`): `data-source set --local new.db --yes` → exit 0; `<root>/new.db` exists and `db status --db sqlite:<root>/new.db` exits 0; site entry database is that URL; seed line printed.
  - `SetLocalRefusesAnOutdatedFile` (store): a SQLite file created with an older schema (open with migrations up to an earlier version via the store fixture's helper, or a file whose `schema_version` is behind) → exit 1, output has `elctl db migrate --db`, site unchanged.
  - `SetDataAloneKeepsTheDatabase`: `set --data runs --yes` → database URL unchanged, `data = "runs"`, `<root>/runs` exists.
  - `SetToTheCurrentSourceWritesNothing`: `set --local --yes` on a default install → exit 0, `nothing to change`, site bytes unchanged.
  - `SetAsksBeforeWriting`: input `n\n` → exit 1, site unchanged.
  - `SetUsageMistakes`: `--local` with `--server`, unknown flag, nothing to set → exit 2.
  - Without the store (`#ifndef`): `set --local new.db --yes` prints `skip  check: built without the DVC store`.
- [ ] **Step 2: Run, expect FAIL** `cmake --build --preset dev --target pychron_elctl_tests && build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='ElctlSetupTest.*DataSource*:ElctlSetupTest.Set*'` (path of the binary as `apps/elctl/tests/CMakeLists.txt` puts it).
- [ ] **Step 3: Implement** the command; openers: `open_database = open_store`, `seed_database = [](url, seed, migrate){ return seed_database(url, seed, migrate, false); }`.
- [ ] **Step 4: Run** the filter above, then all of `ElctlSetupTest.*`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 6: Commit** `feat(elctl): data-source shows and sets an install's database file and data folder`

### Task 7: `elctl data-source set --server`, password, `--no-check`; `db --install`

**Files:**
- Modify: `apps/elctl/src/data_source.cpp`, `apps/elctl/src/db.cpp`, `apps/elctl/src/db.hpp`, `apps/elctl/src/db_stub.cpp`, `apps/elctl/src/cli.cpp` (pass `--install` to `db_command`)
- Test: `apps/elctl/tests/test_setup_commands.cpp`, `apps/elctl/tests/test_db_cmd.cpp`

**Interfaces:**
- Consumes: `read_password(Io, std::string_view)`, `at_terminal(Io)` (`earthbank_impl.hpp`; move both to a header both commands include if `earthbank_impl.hpp` is not built without the store), `database_url(const SiteInstall&)`.
- Produces: `--server HOST[:PORT]/DB` (port default 5432; no `/DB` → usage 2), `--user USER` required with `--server`, `--password-stdin` (one line, trailing `\r\n` stripped), prompt without echo only when terminal attached and server new or different from current; `--no-check` skips `check_data_source` and says `skip  check: --no-check`. `int db_command(const std::vector<std::string>& args, std::optional<std::string> install, Io io);`: `--db` absent and an install given (or a default one) → `setup::database_url(install)`.

- [ ] **Step 1: Write the failing tests:**
  - `SetServerUnreachableIsRefused` (store): `--server 127.0.0.1:1/pychron --user u --password-stdin --yes`, input `pw\n` → exit 1, site unchanged, no credentials file.
  - `SetServerWithNoCheckIsWritten`: same with `--no-check` → exit 0; site `postgresql://u@127.0.0.1:1/pychron`; credentials holds `pw`; `pw` not in site nor record.
  - `SetServerWithoutPasswordStdinKeepsThePassword`: second `set --server ... --no-check --yes` with no stdin, not a terminal → credentials unchanged.
  - `SetServerAgainstPostgres`: `PYCHRON_TEST_PG_URL` set (else `GTEST_SKIP`): parsed host/port/db/user/password from it → exit 0, doctor's database line `[OK]`.
  - `test_db_cmd.cpp`: `DbStatusUsesTheInstallsDatabase` (store): install made, `elctl --install NAME db status` → exit 0, same output as with `--db <its url>`; no install and no `--db` → exit 2 as today.
- [ ] **Step 2: Run, expect FAIL** after building `pychron_elctl_tests`, filter `'ElctlSetupTest.SetServer*:*DbStatusUsesTheInstallsDatabase*'`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the filter, then `ElctlSetupTest.*:*Db*`; expect PASS (with `PYCHRON_TEST_PG_URL` exported once, per memory "Postgres for tests").
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed`.
- [ ] **Step 6: Commit** `feat(elctl): data-source set points an install at the lab's server; db commands take --install`

### Task 8: Shared question editors (wizard refactor)

The page must use the wizard's widgets, not copies (spec 3.5).

**Files:**
- Create: `apps/pychron-ui/src/question_editor.hpp`, `apps/pychron-ui/src/question_editor.cpp` (add both to `apps/pychron-ui/CMakeLists.txt` source and header lists)
- Modify: `apps/pychron-ui/src/setup_wizard.cpp` (`make_editor`, `set_value`, `value_of`, lines ~278-380), `apps/pychron-ui/src/setup_wizard.hpp`
- Test: `tests/ui/test_setup_wizard.cpp` (unchanged; must pass), new `tests/ui/test_question_editor.cpp`

**Interfaces:**
- Produces (namespace `pychron::ui`):
  ```cpp
  QWidget* make_question_editor(const setup::Question& q, QWidget* parent);   // Choice, Host, Port, String, Int,
                                                                              // Float, Bool, Path, Folder, Secret
  void set_question_value(QWidget* editor, const setup::Question& q, const setup::Value& v);
  Result<setup::Value> question_value(const QWidget* editor, const setup::Question& q);
  ```
  Same object names as today (`text` line edit, `Browse…` button, radio buttons in a `QButtonGroup`). `List` and `Table` stay in the wizard if they need `Field`'s rows.

- [ ] **Step 1: Write the failing test** `tests/ui/test_question_editor.cpp` (QtTest, `QTEST_MAIN`): for a `Path` question, editor has a `QLineEdit` named `text` and a `Browse…` button, `set_question_value("data/x.db")` then `question_value` returns it; `Choice` with `local`/`server` round-trips; `Port` refuses `70000` with an error; `Secret` line edit echo mode `Password`.
- [ ] **Step 2: Run, expect compile failure:** `cmake --build --preset dev-ui --target pychron_ui_test_question_editor`.
- [ ] **Step 3: Move** the three wizard members' type switches into the free functions; wizard members call them.
- [ ] **Step 4: Run** `build/dev-ui/tests/ui/pychron_ui_test_question_editor` and `build/dev-ui/tests/ui/pychron_ui_test_setup_wizard`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
- [ ] **Step 6: Commit** `refactor(ui): the setup wizard's question editors are shared`

### Task 9: Data source page widget

**Files:**
- Create: `apps/pychron-ui/src/data_source_page.hpp`, `apps/pychron-ui/src/data_source_page.cpp` (CMake lists)
- Test: `tests/ui/test_data_source_page.cpp`

**Interfaces:**
- Consumes: Task 8 editors; `setup::current_data_source`, `plan_data_source`, `check_data_source`, `read_kept_password`; `database_opener()`, `database_seeder()` (`setup_support.hpp`, same signatures as `DataSourceOpeners` members).
- Produces:
  ```cpp
  struct DataSourcePageDeps {
    setup::SiteInstall install;
    std::filesystem::path site_path;            // default_site_path() in the app
    std::vector<setup::Question> questions;     // store-common's, from the profile library
    setup::DataSourceOpeners openers;
    std::function<bool()> queue_running;        // empty: never
    std::optional<std::string> session_database;  // --db given
  };
  class DataSourcePage : public QWidget {
   public:
    explicit DataSourcePage(DataSourcePageDeps deps, QWidget* parent = nullptr);
    Result<setup::DataSource> source() const;   // what the fields hold; password nullopt when blank
    bool changed() const;                       // differs from current_data_source, or a password typed
    void test_connection();                     // check_data_source(..., create = false) into result()
    // OK/Apply: empty when written or unchanged, else the reason; `check` false skips the checks.
    QString apply(bool check = true);
    void refresh_read_only();                   // queue_running() → read-only with the note
    // For tests.
    QWidget* editor(const QString& id) const;   // "data_source", "db_host", ..., "db_file", "data_folder"
    QPushButton* test_button() const; QLabel* result() const; QLabel* note() const;
  };
  ```
  Pinned text: password placeholder `"kept"` when `read_kept_password` has one; read-only note `"Stop the queue to change the data source"`; `--db` note `"This session uses --db <url without password>; changes here are for the install"`.

- [ ] **Step 1: Write the failing tests** `tests/ui/test_data_source_page.cpp` (temp root, temp site file, install made with `setup::plan_install`/`apply_install` of `data-reduction`):
  - `loadsTheCurrentSource`: `data_source` editor on `local`, `db_file` `data/pychron.db`, `data_folder` `data`; server fields hidden.
  - `aKeptPasswordIsShownAsKept`: server install with credentials → password editor empty, placeholder `kept`.
  - `testConnectionOnATempSqlite` (store builds): existing migrated temp file chosen → `result()` text contains `schema`; nothing on disk changed.
  - `readOnlyWhileAQueueRuns`: `queue_running` returns true → editors disabled, note text pinned above.
  - `aSessionDatabaseIsNoted`: `session_database = "sqlite:/x.db"` → note contains `--db`.
- [ ] **Step 2: Run, expect compile failure:** `cmake --build --preset dev-ui --target pychron_ui_test_data_source_page`.
- [ ] **Step 3: Implement** the page: editors from `deps.questions` filtered to `data_source`, `db_host`, `db_port`, `db_name`, `db_user`, `db_password`, `db_file`, `data_folder`; visibility from each question's `when` (`setup::is_asked`); "New file…" button beside `db_file` (`QFileDialog::getSaveFileName`).
- [ ] **Step 4: Run** `build/dev-ui/tests/ui/pychron_ui_test_data_source_page`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
- [ ] **Step 6: Commit** `feat(ui): a data source page shows and checks an install's database and data folder`

### Task 10: Preferences > Data source, Apply, reopen

**Files:**
- Modify: `apps/pychron-ui/src/preferences_dialog.hpp`, `apps/pychron-ui/src/preferences_dialog.cpp` (`commit()` line ~257, `complain()` ~282)
- Modify: `apps/pychron-ui/src/main_window.cpp` (`open_preferences`, ~668), `apps/pychron-ui/src/data_main_window.cpp` (`open_preferences`, ~62), `apps/pychron-ui/src/main.cpp` (pass install, site path, questions, `--db`, and a reopen action built like `installations_handler`: `window->close()` then `start_install(name)`; `ExperimentBridge::running()` as `queue_running` for the instrument window)
- Test: `tests/ui/test_data_source_page.cpp`, `tests/ui/test_preferences.cpp`

**Interfaces:**
- Consumes: Task 9's `DataSourcePage`.
- Produces on `PreferencesDialog`:
  ```cpp
  using Reopen = std::function<void()>;       // close the window, start pychron-ui on the install
  using AskReopen = std::function<bool()>;    // default: QMessageBox "Reopen now" / "Later"
  void add_data_source_page(DataSourcePageDeps deps, Reopen reopen, AskReopen ask = {});
  DataSourcePage* data_source_page() const noexcept;   // null when not added
  QPushButton* save_without_checking() const noexcept; // shown after a failed check
  ```
  Page added only when the window runs on an install (callers skip it for `--sim` examples without one). `commit()`: page `changed()` → `apply(true)`; failure → `complain(reason, page)` and `save_without_checking` shown, which calls `apply(false)`; success → `ask()` true → `reopen()`. Text: `"Pychron must reopen to use the new data source."`, buttons `Reopen now`, `Later`.

- [ ] **Step 1: Write the failing tests:**
  - `test_data_source_page.cpp` `applyWritesSiteAndRecordAndOffersReopen`: dialog with page (temp site, `AskReopen` returning true, `Reopen` counting calls), set `data_folder` to `runs`, press Apply → site entry `data == "runs"`, record answer `data_folder == "runs"`, reopen called once; unchanged page → Apply does not ask.
  - `aFailedCheckOffersSaveWithoutChecking`: opener returning an error → `problem()` visible with the reason, site unchanged, `save_without_checking()` visible; pressing it → site written.
  - `test_preferences.cpp` `noDataSourcePageWithoutAnInstall`: `show_for` without `add_data_source_page` → page list has no "Data source".
- [ ] **Step 2: Run, expect FAIL:** build and run `pychron_ui_test_data_source_page`, `pychron_ui_test_preferences` (dev-ui).
- [ ] **Step 3: Implement** dialog and the three callers.
- [ ] **Step 4: Run** those binaries plus `pychron_ui_test_setup_wizard`, `pychron_ui_test_database_choice`; expect PASS.
- [ ] **Step 5: Checks** `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed --build-dir build/dev-ui`.
- [ ] **Step 6: Commit** `feat(ui): Preferences changes the install's data source and reopens on it`

### Task 11: Docs

**Files:**
- Modify: `docs/installation_runbook.md` (2.5 table, line ~451), `docs/user/08-elctl-reference.md` (Contents, a `### data-source` under "Setup" after `doctor`; `db status`/`db migrate` `--install`), `docs/user/01-getting-started.md` (section 9, a Data source paragraph), `libs/setup/AGENTS.md`

- [ ] **Step 1:** Runbook row `| Change where data is read and written | Preferences > Data source, or elctl data-source set |`.
- [ ] **Step 2:** elctl reference: synopsis from spec 3.4, exit codes, password rule, example switching to a server and back.
- [ ] **Step 3:** Getting started section 9: the page, Test connection, reopen, read-only while a queue runs.
- [ ] **Step 4:** `libs/setup/AGENTS.md` bullet, compressed: "Data-source change (`data_source.cpp`) writes credentials, install record, site entry; nothing else under root. Reconfigure must keep it: `DataSource*.AReconfigureKeepsTheNewSource`."
- [ ] **Step 5: Commit** `docs: changing an installation's data source`

### Task 12: Final run

- [ ] **Step 1:** `git fetch && git rebase origin/develop`.
- [ ] **Step 2:** `python3 tools/quality_check.py`; expect exit 0.
- [ ] **Step 3:** `cmake --build --preset dev-ui` and `ctest --preset dev-ui` (covers `dev` and UI); expect all pass. With `PYCHRON_TEST_PG_URL` set so `SetServerAgainstPostgres` runs.
- [ ] **Step 4:** `python3 tools/compiler_sweep.py` and `python3 tools/compiler_sweep.py --build-dir build/dev-ui`; expect no warnings.
- [ ] **Step 5:** Spec and code agree: any interface that departed (for example `DataSourceOpeners`, `describe`, `database_url_with_password`, added here beyond spec 3.1) is written into the spec in this branch; commit `docs(setup): the data source spec says what was built`.

## Open questions

None.
