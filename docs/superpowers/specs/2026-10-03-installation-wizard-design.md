# Installation and setup wizard

Date: 2026-10-03
Status: Draft
Owner: Jake Ross

## 1. Goal

Two kinds of install, with very different weight:

- **Data reduction.** A geochronologist installs pychron on a laptop to
  browse, reduce and plot data. This must feel like any other desktop
  application: download an installer, run it the usual way for the platform,
  launch the app, answer two or three questions, done. No terminal, no Python
  environment, no editing files.
- **Instrument.** A lab sets pychron up to run an Argus VI, a Helix or an NGX
  with its extraction line. This takes real configuration (addresses,
  credentials, detectors, calibration, valves). The wizard gets the lab to a
  working, validated starting point that runs in simulation and connects to
  the instrument, and says plainly what is still placeholder and must be
  calibrated before the magnet is moved.

Owner decisions (2026-10-03): the NGX driver is written before the wizard
(its own spec, `2026-10-03-ngx-driver-design.md`), so the NGX profile can
drive an instrument; data reduction connects to a database (local file or the
lab's server) as part of this work; installers for macOS, Windows and Linux
are in scope.

## 2. What exists and what it teaches

- **pcm** (`PychronLabsLLC/pcm`, Python, click): clones pychron source, builds
  a conda/EDM environment, renders Jinja templates into `~/Pychron`, writes a
  launcher script. Templates per spectrometer kind (`argus_mftable.csv`,
  `ngx_detectors.yaml`, `ngx_mftable.csv`, NGX device `.cfg` files); the CLI
  offers `helix` but ships no Helix templates. Its `wizard` command is a flag
  list followed by one yes/no.
- **Legacy pychron (2026 layer)**: `cli_profiles.py` / `starter_bundles.py`.
  Profiles name directories and files, compose with `includes`, and are
  grouped into versioned bundles (`data-reduction`, `experiment-control`,
  `ngx-collection`, ...). Files are written only when missing, so re-running
  is a repair; the managed files are recorded in
  `.appdata/bootstrap_profiles.json`; `pychron doctor` validates the result;
  `export-config` / `import-config` zip a lab's setup. No Argus/Helix
  profiles; database and credentials are configured elsewhere (plain text).
- **Legacy pychron (2016-2021 `install.py`)**: interactive prompts with
  defaults, one "use all defaults" escape, credentials in the launcher.
- **pychron-cpp today**: no `install()` rules, no packaging; the UI falls back
  to a compiled-in path into the source tree; nothing records where a lab is;
  the data browser reads a folder of JSON records; the DVC store
  (`libs/persistence`) is not linked into any app; Argus and Helix share the
  `thermo_qtegra` driver with one untested example config; NGX has a codec
  only.

Kept from these: profiles that compose; write-only-missing (a re-run repairs,
never clobbers); a manifest of managed files; a doctor; export/import.
Dropped: Python environments, source checkouts, launcher scripts, secrets in
plain files, a flag list presented as a wizard.

## 3. Pieces

```
installers (CPack)          macOS .dmg, Windows setup.exe, Linux .deb / AppImage
  └─ pychron-ui             first run -> Setup wizard (Qt, QWizard)
  └─ elctl init / doctor    same engine, scriptable
        └─ libs/setup       Qt-free: profiles, questions, rendering, site config, doctor
              └─ share/pychron/profiles/<name>/   shipped profile data
```

### 3.1 Profiles (`profiles/` in the repo, `share/pychron/profiles` installed)

A profile is a directory with a `profile.toml` manifest and template files.

```toml
name = "argus"
title = "Thermo Argus VI"
kind = "instrument"            # instrument | data_reduction | fragment
summary = "Argus VI over Qtegra RemoteControlServer, 5 Faradays + CDD"
includes = ["lab-common", "extraction-line-starter"]
version = 1                    # bumped when the files change; doctor reports drift

[[questions]]
id = "qtegra_host"
prompt = "Qtegra computer address"
type = "host"                  # string | host | port | int | float | bool | choice | path | secret | list
default = "192.168.0.10"
help = "The PC running Qtegra and RemoteControlServer."
group = "Instrument connection" # wizard page

[[questions]]
id = "detectors"
type = "list"
default = ["H2", "H1", "AX", "L1", "L2", "CDD"]
group = "Detectors"

[[files]]
template = "spectrometer.toml"       # under the profile directory
to = "spectrometer.toml"             # under the install root
[[files]]
template = "spectrometer.local.toml"
to = "spectrometer.local.toml"
secret = true                        # written 0600; never echoed in summaries
[[files]]
copy = "tables/"                     # copied verbatim, directory
to = "tables/"
[[files]]
template = "notes/CALIBRATE.md"
to = "CALIBRATE.md"
when = "!simulation"                 # simple conditions over answers
```

- `includes` resolve depth-first, de-duplicated; a file written by two
  profiles is a profile error (caught by a test over every shipped profile).
- Templates use a small renderer in `libs/setup`: `{{ id }}` substitution,
  `{{ id | toml }}` (TOML-quoted string or array), `{% if cond %} ...
  {% else %} ... {% endif %}`, `{% for x in list %} ... {% endfor %}`. No
  other logic; unknown names are render errors. Every rendered TOML file is
  parsed after rendering, and the install fails before writing anything if
  one does not parse.
- Shipped profiles:
  - `data-reduction` (kind data_reduction): data folder layout, database
    choice, `references.toml` starter, figure presets.
  - `argus`, `helix` (instrument, `thermo_qtegra`): detector sets differ
    (Argus VI: H2 H1 AX L1 L2 CDD; Helix MC Plus: five collector positions,
    each Faraday or CDD, asked per position, defaults from the lab survey
    and marked "confirm"), Qtegra host/port, field and HV tables marked
    SIMULATION PLACEHOLDER until calibrated.
  - `ngx` (instrument, `isotopx_ngx`): host/port, login user and password
    (secret), detectors H4..L5 with kind per detector (Faraday, CDD, ATONA)
    from pcm's template, mass table placeholder, NGX valves through the NGX
    controller (optional).
  - Fragments: `lab-common` (plans, scripts, conditionals, `defaults.toml`,
    `identifiers.toml`, `notifications.toml.example`), and
    `extraction-line-starter` (a two-valve line with a gauge on a sim
    transport; real lines are built with the operator afterwards or
    imported, see section 7).
- Every instrument profile has an implicit `simulation` answer (bool): when
  true the transports are written as `kind = "sim"` so the whole install runs
  without hardware, and switching to hardware later is flipping it back with
  `elctl init --reconfigure` (section 3.4).

### 3.2 Install root and site config

- **Install root**: the directory a profile writes into (the lab directory of
  `lab::LabPaths`). Defaults:
  - data reduction: `~/Documents/Pychron` (Windows: `Documents\Pychron`).
  - instrument: `~/Pychron/<instrument name>`.
- **Site config**: the per-user file that says which installs exist and which
  one the app opens. Platform-standard location (what Qt's
  `QStandardPaths::AppConfigLocation` returns, reproduced in `libs/setup`
  without Qt): macOS `~/Library/Application Support/Pychron/site.toml`,
  Windows `%APPDATA%\Pychron\site.toml`, Linux
  `$XDG_CONFIG_HOME/pychron/site.toml`.

  ```toml
  default = "argus-lab"
  [[installs]]
  name = "argus-lab"
  kind = "instrument"
  root = "/Users/lab/Pychron/argus-lab"
  profile = "argus"
  line = "extraction_line.toml"       # relative to root
  canvas = "canvas.toml"
  spectrometer = "spectrometer.toml"
  data = "data"
  [[installs]]
  name = "reduction"
  kind = "data_reduction"
  root = "/Users/lab/Documents/Pychron"
  database = "sqlite:data/pychron.db"  # or a postgresql:// URL without password
  ```

- Each install root holds `.pychron/install.toml`: profile names and
  versions, the answers given (secrets omitted), and the managed files with
  their hashes at install time. This is what `doctor` and
  `--reconfigure` read.
- **Secrets** (NGX password, database password) never go in `site.toml`,
  `install.toml` or a rendered template other than a file marked `secret`
  (0600 on POSIX, user-only ACL is out of scope on Windows; the file lives
  in the user's profile). The OS keychain (macOS Keychain, Windows Credential
  Manager, libsecret) is the follow-up once a keychain dependency is chosen;
  the secret file is behind one interface so that swap is local.

### 3.3 App startup

`pychron-ui` resolves what to open in this order: explicit command-line files
(today's behaviour, unchanged for developers); `--install <name>`; the site
config's `default`; otherwise the **Setup wizard** opens (first run). The
compiled-in example path becomes a developer fallback only when the binary
runs from the build tree. A File > Installations... dialog lists installs,
opens another, starts the wizard again, or removes an entry (never the
files).

`elctl` gains `--install <name>` (resolves `-c`, `--lab`, `--spectrometer`,
`--data` from the site config); explicit flags still win.

### 3.4 `elctl init` and `elctl doctor`

```
elctl init --list                                  profiles and their questions
elctl init <profile> [--root DIR] [--name NAME]    interactive: one prompt per question,
                                                   default in brackets, Enter accepts
elctl init <profile> --answers answers.toml        non-interactive (unattended installs, CI)
          [--set id=value ...] [--yes]
elctl init --reconfigure [--install NAME]          re-ask with the previous answers as defaults;
                                                   rewrites only files still identical to what
                                                   the installer wrote (hash in install.toml);
                                                   changed files get a .new beside them
elctl doctor [--install NAME] [--strict]           checks; exit 1 on FAIL (on WARN with --strict)
```

Writing is two-phase: render everything, parse every TOML result, check the
root is writable and no target outside the root, then write. Existing files
are never overwritten by `init` (the legacy write-only-missing rule); the
summary lists written, kept and skipped files.

`doctor` checks, each OK / WARN / FAIL with a hint:
- the site config entry and root exist; managed files exist; edited managed
  files reported (WARN, informational);
- the line config, canvas, spectrometer config and lab files load and
  validate (the existing loaders and `check_lab_queue` machinery);
- SIMULATION PLACEHOLDER markers still present in a hardware install (WARN);
- instrument reachability: TCP connect to each `tcp` transport (WARN on
  failure; `--probe` runs the driver connect step);
- data reduction: the database opens, the schema is at the latest migration
  (offers to migrate), the data folder is writable;
- `curl` on PATH when notifications use email or webhooks.

### 3.5 Setup wizard (Qt, `apps/pychron-ui`)

A `QWizard` (native style per platform: Aero on Windows, Mac style on
macOS) driven by the same profile manifests: question groups become pages,
types map to widgets (host -> line edit with validation, port -> spin box,
secret -> password field, choice -> combo, list -> editable table, path ->
line edit + Browse).

Data reduction path (four pages, everything defaulted):
1. Welcome: "Set up pychron for: (o) Data reduction  ( ) An instrument
   ( ) Open an existing setup".
2. Location: folder (default `Documents/Pychron`).
3. Data: "(o) On this computer (a database file in that folder)
   ( ) My lab's database server": host, port (5432), database, user,
   password, [Test connection] (opens the store, reports the schema version
   or the error in plain words).
4. Ready: what will be created; Finish -> files written, site config
   updated, doctor run, the data browser opens.

Instrument path: Welcome -> Instrument (Argus VI, Helix, NGX cards) ->
Location and name -> Connection (host, port, credentials; [Test connection]
runs the driver's connect step) -> Detectors -> Extraction line (starter or
import) -> Simulation (run in simulation first: recommended) -> Ready
(file list, placeholders listed) -> Finish (write, doctor, open the main
window). Placeholder calibration items are shown as a checklist on the last
page and kept in `CALIBRATE.md` in the root.

The wizard never blocks on hardware: a failed Test connection is a warning;
the install can finish in simulation.

### 3.6 Data reduction database

Already on main (2026-10-03): `processing::StoreSource` over the DVC store
and `pychron-ui --db <url>` to browse it. Setup adds:

- Local install: `sqlite:<root>/data/pychron.db`, created and migrated by
  `elctl init` (and the wizard) when the store library is built in.
- Server install: `postgresql://user@host:port/db` in the site config; the
  password in `<root>/.pychron/credentials.toml` (owner-only), joined into
  the URL only when the store is opened (`setup::database_url`). A server is
  never migrated by setup or the app; doctor reports a schema it cannot open
  with "ask your administrator".
- `pychron-ui --install <name>` (or the default install) opens a data
  reduction install's database the way `--db` does.

### 3.7 Installers

- CMake `install()` rules for `pychron-ui`, `elctl`, `share/pychron`
  (profiles, example configs, figure presets, migrations), licenses. A
  runtime resource lookup (`pychron::resource_dir()`) finds `share/pychron`
  relative to the executable (macOS: `Contents/Resources`), falling back to
  the source tree only in build-tree runs.
- CPack generators:
  - macOS: `.app` bundle (`MACOSX_BUNDLE`, icon, Info.plist), `macdeployqt`,
    DragNDrop `.dmg` ("drag Pychron to Applications").
  - Windows: NSIS installer (Next / install location / Finish, Start menu
    shortcut, uninstaller in Apps & features), `windeployqt`.
  - Linux: `.deb` (Ubuntu 24.04 dependencies) and an AppImage (linuxdeploy +
    Qt plugin).
- Embedded Python: extraction scripts need CPython. Installers bundle a
  relocatable CPython (python-build-standalone) beside the app and set
  `PYTHONHOME` at startup from `resource_dir()`. Data-reduction use never
  starts the interpreter.
- CI: a `release.yml` workflow on `v*` tags builds the three platforms and
  attaches the artifacts to the GitHub release; PR CI builds the packages
  once (no upload) so packaging breaks are caught early.
- Code signing and notarization (macOS Developer ID, Windows Authenticode)
  are listed as required before public distribution; certificates are the
  owner's to provide; unsigned builds work but warn on first launch.

## 4. Order of work

1. NGX driver (separate spec), with an NGX example config.
2. `libs/setup`: manifests, renderer, site config, install record, doctor
   core; profiles for data-reduction, argus, helix, ngx and the fragments;
   `elctl init`, `elctl doctor`, `--install`.
3. `StoreSource` and database settings; the data browser's source selector.
4. Setup wizard and first-run; File > Installations.
5. Install rules, resource lookup, CPack, release workflow.

Each step lands on `main` with tests (profiles: every shipped profile renders
with its defaults, the result loads with the real loaders, and `doctor`
passes in simulation).

## 5. Testing

- Renderer: substitution, filters, conditions, loops, error cases.
- Every shipped profile x its default answers x simulation on/off renders,
  parses, loads (line, canvas, spectrometer, lab) and passes doctor; the
  instrument ones run the example queue in simulation (the existing elctl
  exp run test harness).
- `init` idempotency: a second run writes nothing; `--reconfigure` rewrites
  untouched files and leaves `.new` beside edited ones.
- Answers files: unknown ids, wrong types, missing required answers.
- Secrets never appear in `install.toml`, `site.toml`, logs or summaries.
- Wizard: QtTest drives both paths headless (offscreen) to Finish and checks
  the files and the site config.
- Packaging: CI builds the packages and runs `elctl --version` and a
  headless `pychron-ui --self-test` from the installed layout.

## 6. Risks

- Instrument defaults (Helix collector layout, NGX detector kinds) come from
  the legacy survey and pcm templates, not from instruments; every such
  default is marked "confirm" in the wizard and in `CALIBRATE.md`.
- Bundling CPython and Qt makes the packages large (about 150-250 MB).
- Unsigned installers will alarm users; signing needs the owner's
  certificates.

## 7. Out of scope here

- Importing a legacy pychron setup (setupfiles, preferences, `valves.yaml`,
  canvas XML, `mftable.csv`, `detectors.yaml`). It is the natural next step
  for instrument installs ("Import an existing pychron setup" on the
  extraction line page is shown disabled until it exists).
- Automatic updates; a keychain dependency; multi-user lab servers.
