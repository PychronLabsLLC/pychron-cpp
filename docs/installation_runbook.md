# Installation runbook

How to put pychron-cpp on a lab computer, configure it, and bring a legacy
pychron lab's data across. It is written for the person doing the install.
Each part links to the page that holds the detail; this page holds the order,
the checks and the way back.

> **Status.** Everything runs against the built-in simulator. No driver has
> been run against a real instrument yet ([README](../README.md), "Hardware
> status"). An instrument install therefore starts, and for now stays, in
> simulation until the bring-up in part 2.4 has been done by hand.

## When to use this

| You are setting up | Do |
|---|---|
| A computer that only browses, reduces and plots data | Parts 1, 2.2, then 3 if the lab has legacy data |
| A computer that will run an instrument (Argus, Helix, NGX) | Parts 1, 2.3, 2.4 |
| The lab's shared database, filled from legacy pychron | Part 3, then 2.2 on each workstation |

## Prerequisites

- An account on the computer that can install software.
- For an instrument: the address and port of the instrument's control PC
  (Qtegra RemoteControlServer, default port 1069; NGX controller, default
  1099), and its calibration (field table, detectors, source settings).
- For a server database: a PostgreSQL server with PostGIS installed (sample
  locations are a geometry column), an empty database, and a user that may
  create tables in it and create the `postgis` extension (PostGIS 3 is
  trusted: the database owner can; otherwise an administrator runs
  `CREATE EXTENSION postgis` once).
- For a legacy migration: `git` 2.32 or newer, `python3`, the system
  time-zone database (`tzdata`), the lab's IANA time zone
  (for example `America/Denver`), and read access to the lab's legacy sources
  (part 3.1).

## Part 1. Install the programs

Two programs are installed: `pychron-ui` (the application, called Pychron on
macOS) and `elctl` (the command line). Both come with the setup profiles and
the example configs.

### 1.1 From a release package

Packages are built by the `release` workflow on a `v*` tag and attached to the
GitHub release (<https://github.com/PychronLabsLLC/pychron-cpp/releases>).
Each carries its own Python for extraction scripts. The macOS `.dmg` is signed
with Pychron Labs' Developer ID and notarized by Apple once the repository has
the signing secrets (section 1.4); the Windows packages are not code-signed,
so Windows warns on first launch.

A macOS package from before that (v0.3.0 and earlier), or one built without
the secrets, is stopped by Gatekeeper on first launch. With "Apple could not
verify ..." open it once, dismiss the warning, then click Open Anyway under
System Settings > Privacy & Security (right-click > Open no longer does it
on macOS 15 and later). With "Pychron is damaged and can't be opened" (v0.3.0,
whose signatures do not hold together) there is no Open Anyway: copy the
application to Applications and clear its quarantine mark once, then open it:

```
xattr -dr com.apple.quarantine /Applications/Pychron.app
```

Such a build also leaves the files it writes quarantined; Pychron
clears that mark from the files it writes for you (data reports, figures, CSV
templates, level sheets), so they open without "Apple could not verify ... is
free of malware". A file written by an older release that still shows it can
be cleared by hand: `xattr -d com.apple.quarantine <file>`.

| Platform | Package | Install | Where `elctl` is |
|---|---|---|---|
| macOS (Apple silicon) | `Pychron-<version>-macOS-arm64.dmg` | Open it, drag Pychron to Applications | `/Applications/Pychron.app/Contents/MacOS/elctl` |
| Ubuntu 24.04 | `pychron_<version>_amd64.deb` | `sudo apt install ./pychron_*.deb` | on `PATH` |
| Linux, other | `.tar.gz` | Unpack anywhere; it is relocatable | `<dir>/bin/elctl` |
| Windows x64 | `Pychron-<version>-windows-x64.exe` (or `.zip`) | Run the installer | on `PATH` (the installer adds it) |

The `.deb` uses Ubuntu's Qt and pulls in `libqt6sql6-sqlite`. For a
PostgreSQL database also install the recommended `libqt6sql6-psql`.

On macOS, make `elctl` reachable for the rest of this runbook:

```bash
alias elctl=/Applications/Pychron.app/Contents/MacOS/elctl
```

### 1.2 From source

Use this when there is no package for the platform or you need an unreleased
change. [dev_setup.md](dev_setup.md) sections 1 and 7 have the toolchain and
the details.

```bash
cmake -S . -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_UI=ON -DBUILD_TESTS=OFF
cmake --build build/pkg
(cd build/pkg && cpack)
```

Then install the package `cpack` wrote as in 1.1.

On macOS `macdeployqt` prints `ERROR: Cannot resolve rpath ...` lines for
optional Qt modules (virtual keyboard, PDF, WebP) and a codesign verification
error. The `.dmg` is still written and the application runs; judge the build
by 1.3, not by those lines.

A package built this way uses the Python it was built against (on macOS,
Homebrew's), so it runs extraction scripts only on a computer that has the
same Python in the same place. To make a package for another computer, bundle
Python as the release workflow does: unpack a python-build-standalone
`install_only` archive and add
`-DPython_ROOT_DIR=<dir> -DPYCHRON_BUNDLE_PYTHON=<dir>` to the configure line
([dev_setup.md](dev_setup.md) section 7). Without Qt 6 (Core and Sql)
at configure time the database library is left out: `elctl import` is missing
and `doctor` reports the database as "not checked".

### 1.3 Check the install

```bash
elctl --version
```

Prints the version and the `profiles:` and `examples:` directories it found.
If those two lines are missing, the package is incomplete.

```bash
pychron-ui --self-test
```

Checks the profiles, the setup wizard and the database plugin, and prints one
`OK` line each. On macOS the program is
`/Applications/Pychron.app/Contents/MacOS/Pychron`. On a Linux machine with no
display, prefix it with `QT_QPA_PLATFORM=offscreen`; the macOS and Windows
packages carry only their native platform plugin, so leave it unset there.

### 1.4 Signing and notarizing the macOS package

The `release` workflow signs `Pychron.app` from the inside out with the
hardened runtime (`packaging/macos/sign_app.sh`, called by CPack before the
disk image is made), then signs the `.dmg`, submits it to Apple's notary
service and staples the ticket to it. The smoke test checks the signatures,
runs the installed programs (Python included) under the hardened runtime and,
for a notarized image, asks Gatekeeper (`spctl`) what it would decide.

It needs five repository secrets (Settings > Secrets and variables >
Actions). Without the first two the app is signed ad hoc: everything is still
signed and run under the hardened runtime, but the package is not fit to hand
out, and a release says so in a warning.

| Secret | What it is |
|---|---|
| `MACOS_CERTIFICATE_P12_BASE64` | The Developer ID Application certificate and its private key, exported as a `.p12` and base64-encoded |
| `MACOS_CERTIFICATE_PASSWORD` | The password the `.p12` was exported with |
| `APPLE_API_KEY_P8_BASE64` | An App Store Connect API key (`AuthKey_<id>.p8`), base64-encoded |
| `APPLE_API_KEY_ID` | That key's ID |
| `APPLE_API_ISSUER_ID` | The issuer ID shown above the keys list |

To make them (an Apple Developer Program membership, as an Account Holder or
Admin):

1. In Xcode > Settings > Accounts > Manage Certificates, add a "Developer ID
   Application" certificate (or create one under Certificates at
   developer.apple.com from a certificate signing request made in Keychain
   Access).
2. In Keychain Access, find "Developer ID Application: <team> (<team ID>)"
   under My Certificates, select it together with its private key, and export
   them as a `.p12` with a password. Then:
   `base64 -i Certificates.p12 | pbcopy` and paste it into
   `MACOS_CERTIFICATE_P12_BASE64`.
3. In App Store Connect > Users and Access > Integrations > App Store Connect
   API, generate a team key with the Developer role. Download the `.p8` (it
   can be downloaded once), then `base64 -i AuthKey_<id>.p8 | pbcopy` into
   `APPLE_API_KEY_P8_BASE64`; the key ID and the issuer ID go into the other
   two.

To check them before a release, run the `release` workflow by hand (Actions >
release > Run workflow, no tag): the macOS job signs, notarizes and assesses
the image and keeps it as a workflow artifact. A rejected submission fails the
job with Apple's log, which names each file and why.

The programs' entitlements are in `packaging/macos/entitlements.plist`: the
camera (a laser's live camera, which the hardened runtime otherwise refuses),
loading libraries not signed by Pychron's team (Python's extension modules, a
vendor's camera SDK) and Python's `ctypes`. A new need, such as another device
class, goes there.

## Part 2. Configure

### 2.1 How configuration is laid out

- An **install** is one folder of plain TOML files written from a **setup
  profile**. List the profiles with `elctl init --list`: `data-reduction`,
  `argus`, `helix`, `ngx`.
- The **site config** records which installs exist on this computer and which
  one opens by default. The first install made becomes the default.

  | Platform | Site config |
  |---|---|
  | macOS | `~/Library/Application Support/Pychron/site.toml` |
  | Linux | `$XDG_CONFIG_HOME/pychron/site.toml`, else `~/.config/pychron/site.toml` |
  | Windows | `%APPDATA%\Pychron\site.toml` |

  `PYCHRON_SITE_CONFIG` overrides the location. The file is safe to edit.
- Each install folder holds `.pychron/install.toml`: the profile, the answers
  given (no secrets) and a hash of every file setup wrote. `doctor` and
  `--reconfigure` read it.
- Secrets (a database password, an NGX login) are written only to
  owner-readable files inside the install folder
  (`.pychron/credentials.toml`, `spectrometer.local.toml`), never to the site
  config.

There are two ways to make an install, and they do the same thing:

- **The wizard.** Start Pychron with nothing installed and it opens. Later:
  `pychron-ui --setup`.
- **The command line.** `elctl init <profile>` asks the same questions.
  `--yes` takes every default, `--set id=value` and `--answers file.toml`
  answer without prompting.

### 2.2 A data-reduction install

With a database on this computer (SQLite, created empty and migrated by
setup):

```bash
elctl init data-reduction --root ~/Documents/Pychron --yes
```

With the lab's PostgreSQL server:

```bash
elctl init data-reduction --root ~/Documents/Pychron \
  --set data_source=server --set db_host=labdb.example.org --set db_port=5432 \
  --set db_name=pychron --set db_user=pychron
```

Setup asks for the password and keeps it in
`<root>/.pychron/credentials.toml`.

Setup and the application never create or migrate a **server** database. Its
schema is made by the importer (part 3.3: `elctl import add` and
`elctl import run` are the only commands that migrate). A server database that
has had no import run against it has no schema, and `doctor` fails it with
"ask your administrator about the schema".

Check:

```bash
elctl doctor
```

Expect `[OK] database: ... (schema version N)`. Then open Pychron; a
data-reduction install opens the data browser.

### 2.3 An instrument install

```bash
elctl init argus --root ~/Pychron/argus
```

Press Enter to accept each default. The install starts in simulation. It
writes the extraction line and canvas, `spectrometer.toml`, field tables,
three measurement plans, scripts, conditionals, an example queue and
`CALIBRATE.md`.

| Plan | What it measures |
|---|---|
| `multicollect` | every detector collects its isotope, one hop |
| `detector_ic` | the reference isotope (Ar40) hopped onto each Faraday in turn, for the detector intercalibration factors |
| `multicollect_hop_ar39` | every isotope but Ar39 together, then Ar39 hopped onto the ion counter (the `hop_detector` answer) |

Choose the extraction line with the `line_source` question:

| `line_source` | What you get |
|---|---|
| `starter` (default) | The five-valve example line on simulated controllers |
| `import` | Your own `extraction_line.toml` and `canvas.toml` (`line_file`, `canvas_file`), checked before anything is written |
| `legacy` | A legacy Pychron `setupfiles` folder (`legacy_folder`) converted; see part 3.6 |

Check, then run the example queue in simulation:

```bash
elctl doctor --install argus
```

```bash
elctl --install argus exp run experiment.toml --sim-speed 50
```

Expect every `doctor` line `[OK]` and the queue to finish. Records go to
`<root>/data`.

Notifications are optional: copy `notifications.toml.example` to
`notifications.toml`, fill in a channel, and send a test with
`elctl --install argus exp notify`. `notifications.md` walks a lab manager
through email by a mail service, from making the account to the test.

### 2.4 From simulation to the instrument

Do not skip steps. Detector protection on a magnet move is planned from the
field table, so a table that does not describe the instrument can leave the
CDD unprotected.

1. Work through `CALIBRATE.md` in the install folder: field table, detectors,
   source and protection thresholds, the real extraction line, your scripts.
   Every file still marked `SIMULATION PLACEHOLDER` or `CONFIRM` must be
   dealt with.
2. Turn simulation off and give the instrument's address:

   ```bash
   elctl init --reconfigure --install argus --set simulation=no --set qtegra_host=10.0.0.5
   ```

   (`ngx_host`, `ngx_port`, `ngx_user` for an NGX.) Files nobody edited are
   rewritten; an edited file is kept and the new version is left beside it as
   `<file>.new` to merge by hand.
3. Check without and then with a connection:

   ```bash
   elctl doctor --install argus --strict
   ```

   ```bash
   elctl doctor --install argus --probe
   ```

   `--strict` fails on warnings, so it fails while a placeholder or a
   fully simulated line remains. `--probe` opens each TCP transport and runs
   the drivers' connect step.
4. For a Thermo instrument, follow "Running against a Thermo instrument" in
   [dev_setup.md](dev_setup.md) and the bring-up checklist in section 8 of
   [the Qtegra driver spec](superpowers/specs/2026-10-01-qtegra-driver-design.md).
   Turn on the wire trace for first contact.

### 2.5 Day-to-day configuration

| Task | How |
|---|---|
| Change an answer | `elctl init --reconfigure --install NAME --set id=value`, or `pychron-ui --setup` |
| Edit a config by hand | Edit the TOML in the install folder, then `elctl doctor` |
| Switch, set default or forget an install | Pychron: File > Installations. Forgetting removes the entry, never the files |
| Use one install from the command line | `elctl --install NAME <command>`, `pychron-ui --install NAME` |
| Check a line or canvas file | `elctl -c extraction_line.toml validate`, `elctl canvas-check` |

## Part 3. Migrate legacy pychron data

`elctl import` reads a legacy lab's database dump and git repositories into a
store with their full history. It can be stopped, resumed and run again
without duplicating anything, and it checks its own work.
[legacy_import.md](legacy_import.md) is the reference for every option,
conflict kind and known limit; read its section 5 before a real migration.

Rules that hold throughout:

- Legacy sources are never modified. A local repository is read in place; a
  url is mirrored into the cache.
- **One importer at a time per database.** Nothing stops a second one; do not
  start it.
- Use one `--cache` directory for the whole migration and keep it with the
  database. `run`, `verify`, `status` and `conflicts` need it.
- The time zone given at `add` cannot be changed afterwards. Get it right.

### 3.1 Gather the sources

| Source | `--kind` | What to get |
|---|---|---|
| The legacy MySQL database | `legacy_db` | A `mysqldump` of it |
| The MetaData repository | `meta_repo` | Its path or url |
| Per-spectrometer repositories of blanks, airs and cocktails | `project_repo` with `--reference-runs` | Path or url of each |
| Project repositories | `project_repo` | Path or url of each |

For private repositories set up a git credential helper or an ssh agent
first. A url with a password or token in it is refused
([legacy_import.md](legacy_import.md) section 4).

### 3.2 Convert the database dump

```bash
python3 tools/legacy_dump_to_jsonl.py pychrondvc.sql catalog/
```

The conversion is complete only when `catalog/MANIFEST.json` exists. Keep the
directory. With no dump, skip this and add `--catalog-from-repos` to each
project repository in 3.3; principal investigators are then absent, and the
result can depend on where runs were stopped.

### 3.3 Register the sources

Set the store and the cache once. For the local database of a data-reduction
install the store is `<root>/data/pychron.db`:

```bash
DB=sqlite:$HOME/Documents/Pychron/data/pychron.db
CACHE=$HOME/Documents/Pychron/import-cache
```

For a server use `DB=postgresql://pychron@labdb.example.org/pychron`, with the
password supplied by a PostgreSQL password file rather than in the url. The
first `add` creates the schema.

```bash
elctl import add --db $DB --cache $CACHE --kind legacy_db --source catalog/ --tz America/Denver
```

```bash
elctl import add --db $DB --cache $CACHE --kind meta_repo --source https://github.com/NMGRLData/MetaData --tz America/Denver
```

```bash
elctl import add --db $DB --cache $CACHE --kind project_repo --source ~/data/Felix_blank180 --tz America/Denver --reference-runs
```

```bash
elctl import add --db $DB --cache $CACHE --kind project_repo --source ~/data/IR1010 --tz America/Denver
```

Mark every repository of blanks, airs and cocktails with `--reference-runs`.
A blank or IC-factor reference to an analysis that is not in the store yet is
stored unlinked and stays unlinked, so those repositories must go in first;
the flag is what makes `run --all` do that.

### 3.4 Run

Optionally try one source first (writes nothing):

```bash
elctl import run --db $DB --cache $CACHE --source IR1010 --dry-run
```

Then import everything, in the right order:

```bash
elctl import run --db $DB --cache $CACHE --all
```

Ctrl-C finishes the current batch and prints `paused: <name> at
<done>/<total>`; the same command resumes. Exit code 0 is finished or paused,
1 is finished with blocking conflicts, 2 is an error or a source that could
not be opened.

```bash
elctl import status --db $DB --cache $CACHE
```

Expect every source `finished`.

### 3.5 Check and accept

```bash
elctl import conflicts --db $DB --cache $CACHE
```

```bash
elctl import verify --db $DB --cache $CACHE
```

`verify` exits 0 when every source is ok: the import is finished, every file
at every commit is accounted for, a second run would write nothing, the
legacy ages are reproduced from the imported data, and no blocking conflict is
pending.

Before accepting:

- Read the `not comparable` lines. A source whose ages are all "not
  comparable" has not had its ages checked.
- A lab that changed legacy pychron's decay constants passes
  `--constants legacy` or `default` to `verify`.
- Blocking conflicts cannot be waved through. Mend the source and import
  again. The common one is `unknown_analysis` because the catalog lacked an
  identifier: import the dump (or fix the catalog), then

  ```bash
  elctl import run --db $DB --cache $CACHE --source IR1010 --replay
  ```

- Warnings (`imported`, `synthesized`, `late_revision_not_applied`) annotate
  data that was imported; `verify` does not fail on them.

Then point the workstations at the store (part 2.2) and browse it in Pychron
(View > Data). The legacy repositories keep changing while the old system is
in use: run `elctl import run --all` again to pick up new commits. Once an
item has been edited in pychron-cpp, the legacy source no longer updates it.

### 3.6 Migrate legacy configuration

Only two parts of a legacy `setupfiles` tree are converted by a tool.
Everything else (spectrometer config, measurement and extraction scripts,
experiment queues) is written new, starting from what the profile installs;
[the legacy config survey](superpowers/specs/2026-09-30-legacy-config-survey.md)
describes what real labs' legacy files hold and where the new schema differs.

**Extraction line and canvas** (`extractionline/valves.yaml` or `valves.xml`,
`canvas2D/`, `devices/`). Preview the conversion, then install with it:

```bash
elctl import-line ~/Pychron/setupfiles
```

```bash
elctl init helix --root ~/Pychron/helix --set line_source=legacy --set legacy_folder=~/Pychron/setupfiles
```

The report lists what was not carried over. Valve controllers that have no
pychron-cpp driver yet become simulated (`sim_valves`): the line loads and
draws, but those valves do not move hardware.

**Conditionals** (needs PyYAML):

```bash
python3 tools/pychron_conditionals_import.py system_conditionals.yaml -o conditionals/system.toml
```

Every rewrite, default and skipped entry is listed in its report. Check the
result with `elctl conditionals-check conditionals/system.toml`.

## Rollback

| To undo | Do |
|---|---|
| An install | File > Installations > forget it (or delete its `[[installs]]` entry in the site config), then delete the install folder. Setup never overwrites a file, so a re-run of `init` is safe |
| A reconfigure | Files nobody edited were rewritten: `init --reconfigure` again with the old answers. Edited files were not touched; delete the `<file>.new` beside them |
| A local import | Stop the importer, delete `<root>/data/pychron.db` (and its `-wal` and `-shm` files) and the cache directory, then `elctl init --reconfigure` to recreate an empty database |
| A server import | Drop and recreate the database, and delete the cache directory. There is no command that removes one source from a store |
| The programs | Drag Pychron to the Trash; `sudo apt remove pychron`; Windows Apps & features. Install folders and the site config are left in place |

The legacy system is untouched by all of this: nothing here writes to a legacy
repository or database, so falling back is a matter of continuing to use it.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `nothing is installed yet (elctl init --list)` | No install in the site config. Make one (part 2), or check `PYCHRON_SITE_CONFIG` |
| `several installs and no default` | Pass `--install NAME`, or set the default in File > Installations |
| `doctor`: `database ... not checked: built without the database library` | Built without Qt Sql. Use a release package, or rebuild with Qt 6 found |
| `doctor` fails a PostgreSQL database | Server, user or password wrong, the QPSQL plugin is missing (`libqt6sql6-psql`), or the schema was never created (part 2.2) |
| `doctor` warns `every transport is simulated` | Simulation is off but the line is still the starter. Describe the real controllers in `extraction_line.toml` |
| `import add` refuses a time zone or flag | The source is already registered with another `--tz` or `--catalog-from-repos`; these cannot change |
| `import run` or `verify`: `history was rewritten` | The legacy repository's history changed (for example a force-push) after it was imported. `run` and `verify` refuse it; see [legacy_import.md](legacy_import.md) section 3 |
| A fetch fails without prompting | The importer never prompts. Set up a git credential helper |
| Time-zone errors during import | `tzdata` is not installed (common in minimal containers) |

## Escalation

Single-developer project. Open an issue at
<https://github.com/PychronLabsLLC/pychron-cpp/issues> with the output of
`elctl --version`, `elctl doctor`, and for an import `elctl import status` and
`elctl import conflicts --json`.

## Known gaps

- No driver has been run against a real instrument.
- A server database gets its schema only from the importer; there is no
  stand-alone "create schema" command for a lab starting with no legacy data.
- The importer is not built or tested on Windows in CI.
- Windows packages are unsigned. The macOS package is signed and notarized
  only once the repository has the signing secrets (section 1.4).
- A macOS package built with Homebrew's Qt carries the SQLite database driver
  only: it cannot open a PostgreSQL store unless Qt's QPSQL plugin was
  installed when it was built.
