# Changing an installation's data source after install

Date: 2026-10-18
Status: Draft (row T088, "installations need to be editable")
Owner: Jake Ross

## 1. Goal

Owner (2026-10-13): "mainly want to be able to change the data source the
app is writing and reading from. should be able to be done via preferences
page and elctl".

After install, lab points an install at another data source without
re-running setup: local SQLite file ↔ lab's PostgreSQL server, another
server or database, another SQLite file (copy of a lab's store), other
records folder. From pychron-ui (Preferences page) and from `elctl`. Same
engine under both (`libs/setup`), as wizard and `elctl init` share one.

## 2. What exists (2026-10-18)

- Data source = two things of `SiteInstall` (`libs/setup` `site.hpp`):
  - `database`: store URL (`sqlite:<path>` or
    `postgresql://user@host:port/db`, no password). Computed once by
    `database_url_for(answers, root, ...)` (`installer.cpp`) from
    `store-common` answers `data_source` (`local` | `server`), `db_host`,
    `db_port`, `db_name`, `db_user`; local always
    `<root>/data/pychron.db`. Password: `db_password` (secret) rendered into
    `<root>/.pychron/credentials.toml` (`[database] password`), joined into
    URL only on open (`setup::database_url`, `doctor.cpp`).
  - `data`: data folder relative to root (default `data`). Holds
    `records/` (file records: `RecordDirectorySource`, simulated runs unless
    `--sim-save`, `references.toml`). Read by pychron-ui
    (`main.cpp` `run_data_reduction`, instrument path) and `elctl`
    (`cli.cpp`: `globals.exp.data`).
- "DVC location": in this port DVC store is the database (`libs/persistence`);
  legacy git repositories are only an import source (`elctl import`), not
  something app reads or writes day to day. So DVC location = `database`;
  `data` is the one folder app also writes. No third location exists.
- Only way to change either today: `elctl init --reconfigure --install NAME
  --set data_source=server --set db_host=...` or `pychron-ui --setup`. Both
  re-plan whole profile (every managed file re-rendered, edited ones get
  `.new`), no way to name a SQLite file other than `<root>/data/pychron.db`,
  no way to change `data` at all (not a question), undocumented for this use
  (`docs/installation_runbook.md` 2.5: "Change an answer").
- Store URL fixed for process life: `choose_database` at startup
  (`database_choice.cpp`), `DataMainWindow` holds one `IAnalysisSource&`,
  `EntryActions`, `FitActions` take URL at construction. File > Installations
  switch already closes window and starts pychron-ui again
  (`installations_handler`, `start_install`).
- Preferences (`preferences_dialog.cpp`): Appearance, Data (browser page
  size, time break, type colours), Plots, Spectrometer, Logging, Metrics,
  EarthBank. No data-source page. Site and install record are not written by
  it.
- Migrations: UI never migrates (`StoreConfig{url, false}`); `elctl init`
  migrates new local SQLite; `elctl db migrate --db <url>` by hand; doctor
  warns only. Instrument install seeds its database
  (`seed_instrument_database`, ensure-only, `libs/entry` `apply_seed`).

## 3. What is built

### 3.1 Model (`libs/setup`, new `data_source.hpp` / `data_source.cpp`)

Qt-free, persistence-free (as rest of `libs/setup`).

```cpp
namespace pychron::setup {

struct LocalDatabase {
  std::filesystem::path file;     // relative to root, or absolute; default "data/pychron.db"
};
struct ServerDatabase {
  std::string host;
  int port = 5432;
  std::string name, user;
  std::optional<std::string> password;  // nullopt: keep the one the install has
};
struct DataSource {
  std::variant<LocalDatabase, ServerDatabase> database;
  std::string data = "data";      // data folder, relative to root, or absolute
};

// What the install uses now: from its install record's answers, its site entry
// as fallback (an install without a record, or made before `db_file`).
Result<DataSource> current_data_source(const SiteInstall& install);

// Store URL the source stands for (no password), as `SiteInstall::database`.
std::string database_url(const DataSource& source, const std::filesystem::path& root);

// Checked, then written. Nothing is written when any check fails.
struct DataSourceChange {
  SiteInstall install;            // after: database, data set from the source
  std::vector<std::string> notes; // said to the user (records not moved, ...)
};
Result<DataSourceChange> plan_data_source(const SiteInstall& install, const DataSource& to);
Result<void> apply_data_source(const DataSourceChange& change, const DataSource& to,
                               const std::filesystem::path& site_path);

}  // namespace pychron::setup
```

`apply_data_source` writes, in this order:

1. `<root>/.pychron/credentials.toml`: server with a new password: written
   (owner-only, as `secret` file of a profile). Server with password
   `nullopt`: left alone. Local: left alone (an old password does no harm;
   a switch back to the server keeps it).
2. `<root>/.pychron/install.toml`: answers `data_source`, `db_host`,
   `db_port`, `db_name`, `db_user`, `db_file`, `data_folder` replaced; when
   credentials file was written, its sha256 in `files` replaced by the new
   file's. Secrets never in it (rule of 2026-10-03 spec, section 3.2).
3. `site.toml`: install's entry `database` and `data` replaced, every other
   field and every other entry as they were (load, change one entry, save).

Rules that must hold:

- A later `elctl init --reconfigure` of the install keeps the new source: it
  re-renders from recorded answers (step 2), and the credentials file is not
  treated as edited (its sha recorded in step 2), so it gets no `.new`.
- Nothing else under the root is rendered, written or removed. Profile files
  are not re-planned: this is not a reconfigure.
- Nothing is copied or moved: not the old database, not `records/`. Change
  says so in `notes` when old data folder holds records and the new one is
  another folder.
- Paths stored as given; relative ones resolve against root
  (`SiteInstall::path`). Data folder or database file outside root is
  allowed (lab's shared disk).
- Refused (`ErrorKind::Config`, nothing written): empty host, name or user;
  port outside 1 to 65535; empty file or data path; a file path that names
  an existing directory; install with no root on disk.
- Install record missing (install made before records, or by hand): step 2
  skipped, notes say a reconfigure would not know the new source. Not an
  error.

### 3.2 Profile (`profiles/store-common`)

Two questions added, version 1 → 2:

- `db_file`: type `path`, default `"data/pychron.db"`, group `Data`, `when =
  "data_source == \"local\""`, prompt "Database file", help "Relative to the
  install folder, or a full path".
- `data_folder`: type `path`, default `"data"`, group `Data`, prompt "Data
  folder", help "Where run records are kept".

`database_url_for` uses `db_file` (absent: `data/pychron.db`, so an install
made at version 1 gives same URL as before). `site_install` sets `data` from
`data_folder` (absent: `data`). Wizard Data page shows both (path → line edit
+ Browse, as now). Defaults keep every first install as today:
`tests/setup/test_profiles.cpp` renders unchanged.

### 3.3 What is checked before writing (apps)

Same checks from UI and `elctl`; `libs/setup` takes an opener as doctor does
(`DoctorOptions::open_database`): apps inject one when built with the store.

- Local file that does not exist: created (folder too) and migrated, as
  `elctl init` does for a new local database; instrument install then
  seeded (`seed_instrument_database`, ensure-only).
- Local file that exists: opened with `migrate = false`. Schema behind:
  refused with "run `elctl db migrate --db <url>` first" (never migrated
  from here: that file may be somebody's copy that older builds still read,
  `libs/persistence/AGENTS.md`). Not a store (no schema): refused.
- Server: opened with `migrate = false` and the password given (or kept).
  Fails or schema behind: refused, reason in plain words (doctor's
  wording). Never migrated (2026-10-03 spec, 3.6). Opens: instrument
  install seeded, as reconfigure does for a server (seed skipped and said
  when it cannot run).
- `--no-check` (elctl) / "Save without checking" (UI, after a failed check):
  writes without opening; for server not reachable from here now. Doctor
  says the rest later.
- Data folder: created when missing; refused when not writable.
- Build without store: database checks skipped and said
  (`skip  check: built without the DVC store`), as `init` says today.

### 3.4 `elctl data-source`

```
elctl [--install NAME] data-source                       show: database url (no password), data folder,
                                                         whether a password is kept; exit 0
elctl [--install NAME] data-source set
      (--local [FILE] | --server HOST[:PORT]/DB --user USER [--password-stdin])
      [--data DIR] [--no-check] [--yes]
```

- `--install` absent: default install, as other commands
  (`resolve_install`).
- Only what is given changes: `--data` alone keeps database; `--local`
  alone keeps data folder. `--local` without FILE: `data/pychron.db`.
- Password never on command line (process list, shell history):
  `--password-stdin` reads one line; absent and terminal attached: prompt
  without echo when server is new or differs from current; absent otherwise:
  kept.
- Prints plan (before → after for database and data folder, notes), asks
  `Write them?` unless `--yes`, then checks (3.3), writes (3.1), prints
  doctor's database check. Exit 0 written; 1 refused or check failed; 2
  usage.
- `elctl db status|migrate` keep `--db`; with `--install` and no `--db`
  they use install's URL (`setup::database_url`), so a switched install is
  migrated with `elctl --install NAME db migrate`.

### 3.5 Preferences page "Data source" (`apps/pychron-ui`)

Shown when app runs on an install (not `--sim` examples without install).
Fields:

- Database: (o) On this computer: file (line edit + Browse, "New file..."),
  ( ) Lab's server: host, port, database, user, password (blank: keep;
  placeholder says "kept" when one is stored). Same widgets as wizard's
  Data page; shared, not copied.
- Data folder: line edit + Browse.
- [Test connection]: runs 3.3 checks without writing, says result.
- Current values read with `current_data_source`.

Apply/OK with this page changed:

1. Checks (3.3). Failure: reason under page (dialog's `problem_` label),
   nothing written; button "Save without checking" offered.
2. Writes (3.1).
3. Store is fixed for process life (section 2), so: "Pychron must reopen to
   use the new data source. Reopen now / Later". Reopen now: same path as
   File > Installations (`window->close()`, `start_install(name)`); window's
   own close guards (unsaved queue edits) still apply.

Refused while a queue runs (instrument window): page read-only with "Stop
the queue to change the data source". Started with `--db URL`: page shows
install's source, notes that `--db` overrides it for this session; editing
still allowed (it changes install, not this session).

Page does not use QSettings or local override file: source is install's,
read by `elctl` too (same reason as Logging and Metrics,
`apps/pychron-ui/AGENTS.md`), and lives in site and install record, where
startup reads it.

### 3.6 Docs

- `docs/installation_runbook.md` 2.5: row "Change where data is read and
  written": Preferences > Data source, `elctl data-source set`.
- `docs/user/08-elctl-reference.md`: `data-source`.
- `docs/user/01-getting-started.md` section 9 (Preferences dialog): page.
- `libs/setup/AGENTS.md`: one bullet: data-source change writes credentials,
  install record and site entry, nothing else; reconfigure must keep it
  (test name).

## 4. Tests

- `tests/setup/test_data_source.cpp`:
  - `current_data_source` from record; from site entry when no record; v1
    record (no `db_file`, `data_folder`) gives `data/pychron.db`, `data`.
  - `plan_data_source` refusals (each of 3.1); nothing written on refusal.
  - apply local → server with password: credentials file 0600 with password,
    record answers and credentials sha updated, site entry `database` and
    `data` updated, other entry and other fields byte-equal; password not in
    `install.toml` nor `site.toml`.
  - server password `nullopt`: credentials file untouched.
  - apply, then `plan_install` reconfigure of same install with recorded
    answers: database and data unchanged, no conflicts (`.new`) for
    credentials.
  - no files under root other than three named changed (hash tree before
    and after).
- `tests/setup/test_profiles.cpp`: every profile renders with defaults as
  before; `db_file` relative and absolute give expected URL.
- `apps/elctl/tests/test_setup_commands.cpp`: `data-source` show;
  `set --local new.db --yes` creates and migrates file, instrument seeded;
  existing outdated file refused; `--server` unreachable refused, with
  `--no-check` written; `--password-stdin`; `--data` alone keeps database;
  `db status --install`. PostgreSQL case when `PYCHRON_TEST_PG_URL` set.
- `tests/ui`: page loads current source; Test connection on a temp SQLite;
  Apply writes site and record (temp site path), offers reopen; read-only
  while queue running. Offscreen, as wizard tests.

## 5. Out of scope

- Other installation fields (lab, line, canvas, spectrometer, devices,
  profiles, `seed.toml`, simulation flag): not needed for owner's ask;
  `elctl init --reconfigure` and hand edits stay the way for them.
- Copying or moving data between sources (old database → new, records →
  store). Pointing elsewhere only.
- Switching store inside a running process without reopening.
- Migrating a server, or an existing local file, from this change.
- Keychain for the password (2026-10-03 spec: follow-up).
- Several data sources at once in one install.
- Renaming or moving an install root.

## Open questions

None.
