# elctl reference

`elctl` is Pychron's command-line tool. Use it to check configuration files,
set up and check an installation, drive the extraction line from a terminal,
run an experiment queue without a screen, work with lasers and trays, and do
bulk database work (legacy import, sample entry, publication export, the
EarthBank workbooks).

This page is a reference. For a first look at Pychron, start with
[01 Getting started](01-getting-started.md). Related pages:
[export](../export.md), [entry](../entry.md),
[legacy import](../legacy_import.md), [notifications](../notifications.md),
[installation runbook](../installation_runbook.md),
[04 Experiments](04-experiments.md), [05 Laser](05-laser.md),
[07 Configuration reference](07-configuration-reference.md),
[10 Troubleshooting](10-troubleshooting.md).

`elctl help` (or `elctl --help`, `elctl -h`) prints a command summary. Each of
`import`, `export`, `earthbank` and `entry` also has its own `help`.

## Contents

- [Synopsis and global flags](#synopsis-and-global-flags)
- [How elctl finds its files](#how-elctl-finds-its-files)
- [Exit codes](#exit-codes)
- [Which commands need persistence](#which-commands-need-persistence)
- [Offline checks](#offline-checks): `validate`, `canvas-check`, `list-drivers`, `list`, `conditionals-check`
- [Setup](#setup): `init`, `doctor`, `import-line`
- [Hardware and simulation](#hardware-and-simulation): `probe`, `state`, `open`, `close`, `read`, `heater`, `scan`, `trace`, `sim`
- [Experiments](#experiments): `exp validate`, `exp run`, `exp notify`
- [Lasers](#lasers): `laser ...`
- [Legacy data](#legacy-data): `import ...`
- [The store's schema](#the-stores-schema): `db status`, `db migrate`, `db derive`
- [Publication data](#publication-data): `export`, `publish`, `embargo`
- [EarthBank workbooks and account](#earthbank-workbooks): `earthbank export-xlsx`, `login`, `account`, `packages`, `preflight`, `upload`, `import-profiles`
- [Sample and package entry](#sample-and-package-entry): `entry ...`
- [Not yet implemented / unverified](#not-yet-implemented--unverified)

## Synopsis and global flags

```bash
elctl [-c <extraction_line.toml> | --install <name>] [--sim] <command> [args...]
elctl --version
elctl --help
```

Global flags go **before** the command. The first word that is not a global flag
is the command; everything after it belongs to the command.

| Flag | Meaning |
|---|---|
| `-c FILE`, `--config FILE` | The extraction-line config to use. Default: `extraction_line.toml` in the current folder (or the install's, see below). |
| `--install NAME` | Use the files of the install of that name (see `doctor`) as defaults: its line config, spectrometer config, canvas, lab folder and data folder. An unknown name is an error. |
| `--sim` | Run every transport as `kind = "sim"`, whatever the config says. An install that was set up for simulation implies `--sim`. |
| `--version` | Print the version, and where the shipped `profiles` and `examples` were found. |
| `-h`, `--help` | Print the command summary and exit 0. |

`init`, `doctor` and `import-line` need no extraction line. Everything else
(`validate`, `list`, `probe`, `exp`, `laser` and so on) uses the config chosen
by `-c` / `--install`.

## How elctl finds its files

- With `-c FILE`, that file is the line config. `--install` alone, or no `-c`
  at all, looks in the site config (see
  [01 Getting started](01-getting-started.md#where-things-are-kept)) and uses
  the **default install**, or the only install if there is just one. If no
  install can be found and you did not name one, elctl falls back to
  `extraction_line.toml` in the current folder.
- An install fills in these defaults for `exp` and `laser`: the **lab** folder
  is the install's folder, the spectrometer config is the install's, the canvas
  is the install's, and the data folder is the install's data folder (default
  `data`). A queue file named relatively is also looked for in the install's
  `queues/` folder, then in the install's folder itself.
- Without an install, the **lab** folder for `exp` is the queue file's
  directory, or its parent when that is `queues/` (for `laser`, the folder
  holding the line config), the data
  folder is `<lab>/data`, and `<lab>/spectrometer.toml` is used as the
  spectrometer config if it exists. Command-line flags always win.
- The environment variable `PYCHRON_SITE_CONFIG` overrides the path of the site
  config.
- Durations (`scan --for`, `--interval`) are a number with an optional unit:
  `500ms`, `10s` or `10`, `2m`, `1h`. A fraction such as `1.5s` is accepted.

## Exit codes

| Code | Meaning (general) |
|---|---|
| 0 | Success. |
| 1 | The command ran but failed: a config error, a hardware error, a check that did not pass, a queue that did not complete. |
| 2 | Usage error (a bad or missing argument, an unknown command), **and also** the fatal errors of `import`, `export`, `earthbank` and `entry` (a database that cannot be opened, a bad file), and `elctl was built without persistence`. `earthbank login`, `account`, `packages` and `preflight` also exit 2 in a build without network support. |

Command-specific details are given under each command. Errors are written to
standard error as `error: ...`; usage errors as `elctl: ...` followed by the
usage line.

## Which commands need persistence

Persistence means the build includes the database store (Qt 6 Core and Sql
found, and not built with `-DPYCHRON_PERSISTENCE=OFF`). Release packages are
built with it. Check with `elctl import status --db sqlite:/tmp/x.db`: a build
without it answers `elctl was built without persistence` and exits 2.

| Needs persistence | Works without |
|---|---|
| `import` (all subcommands), `db`, `export`, `earthbank` (all subcommands), `entry` (all subcommands) | `validate`, `canvas-check`, `list-drivers`, `list`, `conditionals-check`, `init`, `doctor`, `import-line`, `probe`, `state`, `open`, `close`, `read`, `heater`, `scan`, `trace`, `sim`, `exp`, `laser` |

Two softer dependencies:

- `init` of a **data-reduction** install with a local `sqlite:` database creates
  the database only in a build with persistence; without it the database is
  created later.
- `doctor` checks the database only in a build with persistence; otherwise it
  reports "not checked: built without the database library".
- Importing needs `git` 2.32 or newer on the path, plus time-zone data
  (`tzdata`); see [legacy import](../legacy_import.md).

## Offline checks

### validate

Check a system config and every driver's settings. Prints every error.

```bash
elctl validate [file]
elctl -c extraction_line.toml validate
```

Without a file it checks the global config. Prints `ok: <file>` and exits 0, or
the errors, `error: N error(s) in <file>` / `error: N driver error(s)`, and
exits 1. More than one file is a usage error (2).

### canvas-check

Check a canvas file, then cross-check it against the line config (every valve,
gauge and stage it draws must exist there).

```bash
elctl -c extraction_line.toml canvas-check [canvas.toml]
```

Without a file it uses `canvas.toml` in the same folder as the line config.
Warnings are printed as `warning: ...` and do not fail; it ends with
`ok: <file> (N warning(s))` and exits 0. Errors exit 1.

### list-drivers

```bash
elctl list-drivers
```

Prints every driver kind that Pychron knows and the settings each one reads.
Use it when writing a `[drivers.*]` table.

### list

```bash
elctl -c extraction_line.toml list
```

Prints the configured valves (with actuator and address, interlocks and
required-open valves), manual valves, switches, heaters (with units) and gauges
(driver, channel, units and alarm limits), one per line, each with its
description. Exit 0, or 1 if the config does not load.

### conditionals-check

Parse a conditionals file, print each conditional in canonical form, and check
the names in it (gauges, detectors, isotopes) against the line config and an
optional spectrometer config.

```bash
elctl conditionals-check <file> [--spectrometer <spectrometer.toml>]
```

Prints `warning:` lines (exit stays 0) and `error:` lines; exit 1 if there is
an error, else `ok: <file> (N conditional(s))`. A missing file argument or an
unknown flag is a usage error (2).

## Setup

These need no extraction line.

### init

Install a profile (set up an install) from a terminal. It is the command-line
version of the setup wizard in [01 Getting started](01-getting-started.md#10-the-setup-wizard-and-the-installations-dialog).

```bash
elctl init --list
elctl init <profile> [--root DIR] [--name NAME] [--answers FILE] [--set id=value]... [--yes]
elctl init --reconfigure [--install NAME] [--set id=value]... [--yes]
```

| Flag | Meaning |
|---|---|
| `--list` | List the installable profiles (name, kind, title and summary). Profiles that are only building blocks are not listed. |
| `<profile>` | The profile to install, for example `argus`, `helix`, `ngx`, `data-reduction`. |
| `--root DIR` | The install folder. Default: a per-profile default under your user area, derived from the name. |
| `--name NAME` | The install's name in the site config. Default: derived from the profile. |
| `--answers FILE` | A TOML file of answers (the way to set table questions such as detectors). Questions it answers are not asked. |
| `--set id=value` | Answer one question; repeat for several. Overrides `--answers`. Values are typed to the question. |
| `--yes`, `-y` | Do not ask anything and do not ask "Write them?"; use defaults for anything not answered. |
| `--reconfigure` | Re-render an existing install with new answers (`--set`/`--answers`) on top of the answers it was made with. Files you have edited are kept; a new version goes beside them as `.new`. Takes no profile. |
| `--install NAME` | With `--reconfigure`: which install. (This may also be given as the global flag before `init`.) |
| `--profiles DIR` | Where the profiles are. Default: the ones shipped with elctl. |

Without `--yes`, `--answers` or `--reconfigure`, `init` asks the profile's
questions, group by group; press Enter to accept the value in `[brackets]`.
Table questions are shown with their default and can only be changed in an
answers file. `init` then prints the plan (the files to write or update) and
asks `Write them? [Y/n]`.

It then writes the files, registers the install in the site config, and (for a
data-reduction install with a local `sqlite:` database, in a build with
persistence) creates the database. It finishes by running `doctor` on the new
install and prints any placeholder files you must replace before measuring.

Exit: 0 when written and the doctor found no failure; 1 on any error, if you
answer no to "Write them?" (prints `nothing written`), if input ends before a
required question is answered, or if the doctor reports a failure; 2 for a
usage error.

```bash
elctl init --list
elctl init data-reduction --yes
elctl init argus --root ~/pychron/argus --name argus --set simulation=true --yes
elctl init --reconfigure --install argus --set simulation=false
```

### doctor

Check an install. It reports one line per check, `[OK]`, `[WARN]` or `[FAIL]`,
with a hint where there is one.

```bash
elctl doctor [--install NAME] [--strict] [--probe] [--profiles DIR]
```

| Flag | Meaning |
|---|---|
| `--install NAME` | The install to check. Default: the default install (or the only one). |
| `--strict` | Treat warnings as failures (exit 1). |
| `--probe` | Also try to connect to the instrument's transports and the spectrometer (otherwise it does not touch hardware). Failures here are warnings. |
| `--profiles DIR` | Where the profiles are (default: shipped). |

Checks include: the install folder and the files the install recorded; the
extraction line config (warns if every transport is simulated); the canvas; the
spectrometer config; the lab files; placeholders still marked
`SIMULATION PLACEHOLDER` or `CONFIRM` (a warning unless the install is
simulated); notifications (the `curl` program, and whether the secrets named in
`notifications.toml` are set in the environment; see
[notifications](../notifications.md)); and the database.

Exit 0, or 1 if any check fails (or warns, with `--strict`), or if no install
can be found (`nothing is installed yet`, `several installs and no default ...;
pass --install NAME`).

### import-line

Convert a legacy pychron `setupfiles` extraction line and canvas into
`extraction_line.toml` and `canvas.toml`.

```bash
elctl import-line <setupfiles folder> [--out DIR] [--force]
```

It prints what it read and what was not carried over or was changed (devices
with no driver yet are simulated; the notes say which). Without `--out` it
prints both files to the terminal and writes nothing. With `--out DIR` it writes
`extraction_line.toml` and `canvas.toml` there, and refuses to replace existing
files unless you give `--force`. Exit 0; 1 if the folder cannot be read or a
target exists; 2 for a usage error. Then run `validate` and `canvas-check` on the
result.

## Hardware and simulation

These build the extraction line from the config, open its transports and talk to
the hardware, or to simulators if the transports are `sim` or you gave `--sim`.
Valve locks and interlocks are enforced. If a transport cannot be opened
elctl prints `warning: transport NAME: ...` and carries on, because a dead gauge
link must not stop valve work.

Simulated, these commands (`open`, `close`, `read`, `scan` and the rest, and
the `sim` session) use `elctl`'s own stand-ins for the devices and have no gas
model, so a gauge reads the same number whatever the valves do; `exp run` and
`pychron-ui` do have one ([The simulated lab](../simulator.md)).

### probe

Open every transport and ping every driver; prints the health of each.

```bash
elctl -c extraction_line.toml probe
```

Output has a `TRANSPORT` section (state, or `FAIL` and why) and a `DEVICE`
section (driver kind, `ok N read(s), health ...`, or `FAIL`). Exit 0 if
everything answered, 1 if anything failed.

### state

Read back every switch and gauge.

```bash
elctl state
```

One line per valve, manual valve and switch (`name  kind  open|closed|unknown`,
with `locked` and `owner=` when set), then one line per gauge with its pressure
in scientific notation and units (`torr`, `mbar` or `pa`). Exit 0, or 1 if a
refresh or gauge read failed.

### open, close

Actuate a valve, enforcing locks and interlocks.

```bash
elctl open <valve>
elctl close <valve>
```

Prints `<valve> open` or `<valve> closed`. A locked valve, a violated interlock
or a driver error prints `error: ...` and exits 1. Exactly one name is required
(usage error, 2). The actor recorded for the change is `elctl`.

### read

```bash
elctl read <gauge>
```

One pressure reading, for example `IG1  1.200e-08 torr`. An unknown gauge or a
read error exits 1.

### heater

```bash
elctl heater list
elctl heater status <name>
elctl heater on <name>
elctl heater off <name>
elctl heater pid <name> on|off
elctl heater setpoint <name> <value>
```

`list` shows the configured heaters and their units (it needs only the config).
Every other subcommand acts, then reads the heater back and prints one line:
`<name>  on|off  pid on|off  setpoint <value> <units>  readback <value> <units>`.
`setpoint` takes a number in the heater's own units (shown by `list`). Wrong
argument counts, `pid` without `on`/`off`, and a setpoint that is not a number
are usage errors (2); a device error exits 1.

### scan

Stream gauge pressures and alarms for a while.

```bash
elctl scan --for <dur> [--interval <dur>]
elctl --sim scan --for 10s --interval 500ms
```

`--for` is required. `--interval` defaults to the line's `scan_interval_ms`.
Each line is `<seconds>s  <gauge>  <pressure units>`; alarms print as
`<seconds>s  ALARM <source>  <message>`. Exit 0. An invalid or non-positive
duration is a usage error (2).

### trace

Record transport traffic to files for replay and debugging.

```bash
elctl trace                       # show whether tracing is on
elctl trace on [transport...]     # all transports if none are named
elctl trace off [transport...]    # all off if none are named
```

The setting is saved beside the config and takes effect the next time the line
is run; the traces are written to a `traces` folder next to the config. An
unknown transport name exits 1; anything other than `on`/`off` is a usage error.

### sim

An interactive session against simulated hardware. Every transport is simulated.

```bash
elctl -c extraction_line.toml sim
```

You get a `sim>` prompt. Type any of the hardware commands above (`state`,
`open A`, `close A`, `read IG1`, `heater ...`, `scan ...`, `list`, `help`), with
no `elctl` in front. Hardware state is shared between commands, so a valve you
open stays open. Lines starting with `#` are ignored; `quit` or `exit` (or end of
input) leaves the session, exit 0.

## Experiments

### exp validate

Check a queue against the lab's plans, scripts and conditionals, without running
anything.

```bash
elctl exp validate <experiment.toml> [--lab DIR] [--spectrometer FILE] [--canvas FILE]
```

Prints the queue name, run count and estimated duration, one line per run
(`row  identifier  type  plan  estimate`, or `skip`), then every problem
(`warning:` and `error:`). Exit 0 and `ok: <file>` if it is runnable, else 1.

### exp run

Run a queue.

```bash
elctl exp run <experiment.toml> [--lab DIR] [--data DIR] [--spectrometer FILE]
              [--canvas FILE] [--from ROW | --resume] [--dry-run] [--sim-speed X] [--db URL]
              [--sim-save]
```

| Flag | Meaning |
|---|---|
| `--lab DIR` | The lab: where plans, scripts, conditionals, patterns, trays and `notifications.toml` are found. Default: see [How elctl finds its files](#how-elctl-finds-its-files). |
| `--data DIR` | The data folder: the spool (`DIR/spool`), the partial record kept after each extraction and the artifacts (`DIR/records`), and the executor's saved state. Without a database the finished records are written here too (`DIR/records`). Default: the install's data folder, else `<lab>/data`. |
| `--spectrometer FILE` | The spectrometer config. Default: the install's, or `<lab>/spectrometer.toml` if present. Without one the queue runs without a spectrometer. |
| `--canvas FILE` | The canvas file. Default: `canvas.toml` next to the line config if it exists. |
| `--from ROW` | Start at this row number (0 is the first row, as printed by validate). |
| `--resume` | Continue where a stopped or crashed queue left off, using the saved state in the data folder. Exclusive with `--from`. |
| `--dry-run` | Validate and print the queue (as `exp validate`) and stop. With a database (below), also says what it would refuse of the rows that would run (from `--from`, or after `--resume`). |
| `--sim-speed X` | Run simulated time `X` times faster than real time. Needs `--sim` or an install set up for simulation, and `X` must be positive; `max` does not wait at all. What a simulated queue measures is in [The simulated lab](../simulator.md). |
| `--db URL` | Write the runs to this database (`sqlite:<file>` or `postgresql://...`), which must already have its schema (`elctl db migrate`). Default: the install's own database, when `--install` names an instrument install that has one. A simulated session needs `--sim-save` as well. |
| `--sim-save` | Let a simulated session write its runs to the database. Without it simulated runs go to a database only when it is the install's own and a file on this computer; otherwise they are record files (below). Refused when there is no database to save to. |

It first prints the same report as `exp validate`; if the queue has errors, it
prints `error: the queue has errors; nothing was run` and exits 1. While running
it prints progress: `run N <identifier>: started`, state changes, notes,
`conditional <name> tripped: ...`, waits (`waiting 0:00:05: <reason>`),
notifications, and `run N <id>-<aliquot><step>: <state>`. The aliquot is
printed as it is (`run 1 a-1: success`); the database and the data window
show the same run with two digits (`a-01`). The `ETA` of the first line is an
estimate: for an install's shipped queue it reads `0:08:50`, and the queue
takes about 19 minutes of simulated time.

Where the runs go. With `--db`, or under `--install` when the install has a
database, each finished run is written to the database and no record file is
written for it. With neither, each finished run is a file under
`<DIR>/records`. [04 Experiments](04-experiments.md#85-what-is-written-run-records)
says what the database gets.

Simulated runs are kept out of databases that may hold a lab's measurements.
A simulated session (`--sim`, an install set up for simulation, or a
spectrometer file that names only simulators) writes to a database only when
it is the install's own and a `sqlite:` file on this computer, which is what
`elctl init` makes. For any database given with `--db`, and for an install's
database on a server, it writes record files and prints one line on standard
error:

```
note: simulated runs are written to /home/you/Pychron/argus/data/records, not to sqlite:/home/you/other.db; pass --sim-save to save them there
```

That is not an error: the queue runs, and the exit status is the queue's.
`--sim-save` sends the runs to the database after all. Either way each
simulated run's record says that it is simulated, and so does the stored
analysis.

With a database, two things stop `exp run` before anything runs, with exit 1:

- The database cannot be opened. A schema that is behind says how to bring it
  up to date:

  ```
  error: database schema is out of date (migration 1 init not applied); bring it up to date with: elctl db migrate --db <url>
  Run: elctl db migrate --db sqlite:/home/you/Pychron/argus/data/pychron.db
  ```

  An install's database whose password cannot be read is the same kind of
  failure (`error: the installation's database: ...`); it does not fall back
  to files. `--db` overrides the install's database.
- The database cannot take the queue: it lacks the queue's mass spectrometer,
  or an identifier or extract device the queue names, a row's step is not
  letters, or it already holds a run that a row fixes by aliquot and step.
  Only rows that will run are checked (from `--from`, or after `--resume`).
  Each problem is a line, and `--dry-run` prints them too:

  ```
  error: the queue cannot be saved:
  error: identifier '66001' is not in the database
  ```

  Add what is missing with [`elctl entry`](#sample-and-package-entry) and run the queue again.

A run the database cannot take while the queue runs (the database is locked or
away) does not stop the queue: its record waits in `<DIR>/spool` and is sent
again when the next queue starts. The run says why as it ends, and its last
line is marked:

```
  ba: not saved to the data store, kept in the spool: <the database's reason>
run 0 ba-1: success (not saved)
```

A build without persistence refuses `--db` (`built without the DVC store`); without `--db` it
writes files. Under `--install`, when the install names a database, it says so first:

```
warning: built without the DVC store: runs are written to /home/you/Pychron/argus/data/records, not to the installation's database
```

**Ctrl-C** is staged: the first stops the queue after the current run, a second
cancels the run in progress, a third aborts. At the end it prints
`queue <end>` and where the runs went, with a warning when any are still in
the spool:

```
queue completed
5/5 run(s) succeeded; in sqlite:/home/you/Pychron/argus/data/pychron.db
```

A database is shown without its password. Without one the line ends
`records in <DIR>/records`. When some runs were measured and not saved, the
line counts those that were, and a warning gives how many are waiting and why
the last was refused, on one line:

```
5/5 run(s) succeeded; 0 of them in sqlite:/home/you/Pychron/argus/data/pychron.db
warning: 5 record(s) waiting in /home/you/Pychron/argus/data/spool: <record id>: <the database's reason>
```

Without a database the warning begins `warning: N record(s) still in the spool`.

Exit 0 when the queue completed or was stopped; 1 if it was cancelled, aborted or
failed, the line could not start, the queue file would not load, or the
database could not be opened or refused the queue; 2 for a
usage error (including `--sim-speed` without `--sim`, and `--from` together with
`--resume`).

```bash
cd configs/examples
elctl -c extraction_line.toml --sim exp validate queues/experiment.toml
elctl -c extraction_line.toml --sim exp run queues/experiment.toml \
    --spectrometer spectrometer.sim-integrated.toml --sim-speed 50
elctl -c extraction_line.toml --sim exp run queues/experiment.toml --resume \
    --spectrometer spectrometer.sim-integrated.toml
```

On an instrument install, a queue named without a folder is looked for in the
install's `queues/` folder, then in the install's folder:

```bash
elctl --install argus --sim exp run experiment.toml --sim-speed max
```

### exp notify

Send a test message on each channel configured in `<lab>/notifications.toml`.

```bash
elctl exp notify [--lab DIR]
```

Prints `sent: <channel>` or `failed: <channel>: <why>` for each. Exit 0 if every
channel sent; 1 if any failed, the file has problems, or none are configured.
`--lab` defaults to the install's folder, or the current folder. Setting up
channels is in [notifications](../notifications.md).

## Lasers

The laser commands drive a laser through the line config's extraction device
(for example `co2`), using the lab's `tray_maps`, `stage_calibrations`,
`cameras.toml` and `patterns`. They run against the simulated laser with
`--sim`. They are described in more depth in [05 Laser](05-laser.md).

```bash
elctl [-c <extraction_line.toml>] [--sim] laser <command> [--lab DIR]
```

`--lab DIR` defaults to the install's folder, or the folder holding the line
config. `--timeout SECONDS` (above 0) applies to `goto`, `autocenter`,
`camera-scale` and `pattern`; the defaults are 120 s, or 60 s where the command
moves little, and for `pattern` twice the expected time plus 60 s.

| Command | What it does |
|---|---|
| `trays` | List the tray maps (with hole counts) and, per extraction device, whether each is calibrated (and with how many points and what error), or why not. Exit 1 if a tray map or calibration has a problem. |
| `calibrate <device> <tray> point <hole> [--x X --y Y]` | You have moved the stage onto `<hole>`: record where the stage is. The position is read from the device unless `--x` and `--y` are given (they go together). |
| `calibrate <device> <tray> center [--x X --y Y]` | The same, at the tray map's center calibration hole. |
| `calibrate <device> <tray> right [--x X --y Y]` | The same, at the map's east calibration hole. |
| `calibrate <device> <tray> show` | Print the current calibration. |
| `calibrate <device> <tray> clear` | Forget the calibration. |
| `goto <device> <tray> <hole> [--timeout S]` | Move the stage to a hole and report the miss. |
| `autocenter <device> <tray> <hole> [--timeout S]` | Move to a hole and center it with the camera; the result is saved as the hole's correction. |
| `corrections <device> <tray> [clear [<hole>]]` | Show where autocenter found holes, or forget them (all, or one hole). |
| `look <device> [--tray <tray>]` | Show what the camera's finder sees now. Moves nothing. |
| `cameras` | List the cameras this build can reach and what to call them. |
| `camera-scale <device> <tray> <hole> [--step MM] [--timeout S]` | Go to the hole, then measure the camera's pixels-per-mm and flips by jogging the stage (`--step` 0.01 to 2 mm). Saved for the device. |
| `camera-scale <device> show\|clear` | Print the measured scale, or forget it. |
| `snapshot <device> [<name>]` | Save what the camera sees to `snapshots/<device>/<name>.png`. |
| `patterns` | List the lab's patterns: kind, points, length and time. |
| `pattern <device> <name> [--dry-run] [--timeout S]` | Run a pattern about where the stage is (the laser is **not** fired), or with `--dry-run` print its points. |

Flag rules: `--x`/`--y` only with `calibrate`; `--dry-run` only with `pattern`;
`--tray` only with `look`; `--step` only with `camera-scale`; `--timeout` is
refused by commands that do not move or wait. A violation is a usage error (2)
with the laser usage text. Exit 0 on success, 1 when the device, tray, camera or
move fails.

## Legacy data

`elctl import` brings legacy pychron data (a database dump and the git
repositories) into the store. **Needs persistence** and `git` 2.32 or newer.
The full guide, with the order to do things in and how to check the result, is
[legacy import](../legacy_import.md); `elctl import help` prints every option.

```bash
elctl import <add|run|status|conflicts|verify> --db <url> [--cache <dir>] [options]
```

`--db` is required. `--db` is `sqlite:/path/to/file.db` or
`postgresql://user:password@host/db`. `--cache DIR` is where each source's
settings (`<id>.toml`) and mirrors of remote repositories are kept (default: the
user's cache directory, `pychron/import`). Only `add` and a real `run` create or
migrate a database; the other subcommands need one that exists (a missing or
empty SQLite file is an error).

### import add

Register a source. Prints its id.

```bash
elctl import add --db <url> --kind legacy_db|meta_repo|project_repo
                 --source <path|url> --tz <IANA zone>
                 [--branch <b>] [--author-map <file.toml>]
                 [--catalog-from-repos] [--reference-runs]
```

| Flag | Meaning |
|---|---|
| `--kind` | `legacy_db` (the directory written by `tools/legacy_dump_to_jsonl.py`), `meta_repo` (the lab's metadata repository), or `project_repo` (a project's analysis repository). |
| `--source` | For a dump, its directory. For a repository, a local path (read in place, never modified) or a URL (mirrored into the cache with your git configuration). A URL with a password or token in it is refused; use a git credential helper (`ssh://git@host/...` is fine). Register the database dump before the project repositories. |
| `--tz` | The lab's IANA time zone, for example `America/Denver`: legacy times are local times without a zone. |
| `--branch` | Default: the branch the repository's HEAD names. Not for `legacy_db`. |
| `--author-map FILE` | TOML, one line per author: `"git email" = "user name"`. |
| `--catalog-from-repos` | `project_repo` with no database dump: make identifiers, positions and spectrometers from the records. |
| `--reference-runs` | `project_repo`: this repository holds blanks, airs and cocktails. `run` imports these repositories first. |

Registering a source again keeps what was imported and replaces its
`--reference-runs` and `--author-map` settings; changing its `--tz` or
`--catalog-from-repos` is refused because they decide what was imported.

### import run

Import, resuming where the last run stopped.

```bash
elctl import run --db <url> [--source <id|name> | --all] [--batch N] [--limit N]
                 [--replay] [--dry-run]
```

Without `--source`, every source is imported: the dump first, then meta
repositories, repositories of reference runs, then the other project
repositories. `--batch` sets commits (or rows of a dump) per batch (default 500;
2000 for a dump). `--limit` stops after that many batches. `--replay` walks the
source again from its start: what is imported is left alone, and analyses that
were refused earlier (`unknown_analysis`) are imported now that the catalog has
their identifier. `--dry-run` writes and fetches nothing and prints what a run
would do. `--source` and `--all` are mutually exclusive. **Ctrl-C** finishes the
batch being written and stops (`paused: <name> at N/M`); a second Ctrl-C ends
the program at once (the batch in flight is one transaction, so it is either
stored or not). Progress goes to stderr, a summary per source to stdout.

### import status

```bash
elctl import status --db <url>
```

One line per source: `id kind name status done/total head`.

### import conflicts

```bash
elctl import conflicts --db <url> [--source <id|name>] [--kind <kind>] [--all] [--json]
```

What could not be imported, or was imported with a note, one line each: `kind
path entity detail`. Only pending ones, unless `--all`. Kinds: `unparseable`,
`unknown_analysis`, `identity_clash`, `value_mismatch`, `hand_edit`,
`provisional_renumber`. With `--json` the list is JSON, including a `blocking`
flag.

### import verify

```bash
elctl import verify --db <url> [--source <id|name>] [--tolerance <relative>]
                    [--constants legacy_preferences|legacy|default] [--json]
```

Checks each import: the source is imported to its end, every file of it is
accounted for, a second run would write nothing, the ages stored with each
interpreted age are reproduced from the imported data (to `--tolerance`, default
`1e-6`), and no conflict that means missing or disagreeing data is pending.
`--constants` chooses the decay constants and atmospheric ratios the ages are
computed with (default `legacy_preferences`, what legacy pychron used unless
your lab changed them).

### import exit codes

| Code | Meaning |
|---|---|
| 0 | OK, including a paused run and a run that left only warnings. |
| 1 | `verify` is not OK, or a run finished with blocking conflicts pending. |
| 2 | Usage error or fatal error, or a source could not be opened (the others are still run or verified). |

## The store's schema

A store's tables and indexes are its schema, and a newer pychron may need a
newer one. The application, `export`, `flux` and `entry` open a store as it
is and refuse one that is behind: they never change a schema in passing.
After an update, this is how an existing store is brought up to date.

```bash
elctl db status --db sqlite:/path/to/store.db
elctl db migrate --db sqlite:/path/to/store.db
```

### db status

Says whether the store's schema is the one this build needs, and lists the
migrations it has. Exit code 0 when up to date, 1 when behind (the message
names the first migration it lacks). It changes nothing.

### db migrate

Applies the migrations the store lacks, in order, and lists what it then
has. Each migration is applied in one transaction: whole, or not at all.
Run again it does nothing and says the store is up to date. Close pychron
first if it has the store open.

Neither command makes a database: a SQLite path that is not there, or an
empty file, is an error (exit 2). To make a new store use `import add` or
set up a data-reduction install.

A copy of the file is a complete backup of a SQLite store; make one first if
the store is the only copy of its data.

### db derive

```bash
elctl db derive --db <url> [--all] [--limit N] [--quiet]
```

Fills the store's cache of ages: the age the data browser shows in its
**Age (Ma)** column and filters by (see
[06 Data analysis](06-data-analysis.md#the-age-column-and-the-cached-age)).
It walks every analysis of the store, newest first and invalid ones
included, in pages of 200. An analysis whose cached age was computed from
its inputs as they are now is passed over; any other (no cached age yet, or
its flux, blanks, IC factors, fits or production changed since) is loaded,
reduced with the default settings, and its age, age with the error of J,
and K/Ca are stored, in Ma. A blank or an air is stored as having no age, so
the next run passes over it too.

Run it after an update that brings a store made before the cache, after an
import made with `--no-derive`, and after saving a flux fit, blanks or IC
factors (the flux window says how many cached ages a save left out of
date). `elctl import run` runs the same pass when an import finishes. The
application keeps the cache up to date for every analysis it reduces, so
this is only for the ones nobody has looked at since.

| Flag | Meaning |
|---|---|
| `--db URL` | The store (`sqlite:<file>` or `postgresql://...`). Required. Its schema must be up to date (`db migrate`); a store that is behind is refused, never migrated. |
| `--all` | Reduce and store every analysis walked, current or not. |
| `--limit N` | Walk no more than `N` analyses (the newest `N`). |
| `--quiet` | Print only the summary line, not one line a page. |

Output: one line a page, with the counts so far, then a summary, for
example:

```text
  200 seen, 37 written (4 without an age), 163 current, 0 failed
  ...
derived: 8716 seen, 1210 written (95 without an age), 7506 current, 0 failed
```

An analysis that could not be loaded is named on stderr
(`elctl db derive: <run id>: <why>`) and the walk goes on. It is safe to stop
with Ctrl-C: what earlier pages stored stays, and the next run starts where
there is work left. Two runs at once on one database do no harm; they write
the same rows.

Exit code: 0 when every analysis walked is current or was stored, 1 when any
failed to load, 2 on a usage error or when the store could not be opened,
read or written.

## Publication data

`elctl export` writes a 40Ar/39Ar data report after Schaen et al. (2021) from the
store. **Needs persistence.** The report's columns and meaning are in
[export](../export.md); `elctl export help` prints every option.

```bash
elctl export --db <url> --out <file.csv|file.json> [selection] [options]
```

`--out` ending in `.json` writes JSON, anything else CSV.

**Selection** (repeat a flag for several values; every flag narrows the set):

| Flag | Meaning |
|---|---|
| `--sample NAME`, `--identifier LABNUMBER`, `--project NAME`, `--irradiation NAME` | Match these. |
| `--type TYPE` | Analysis type. Default: `unknown`. |
| `--uuid UUID` | Explicit analyses; the other filters are then ignored. |
| `--from YYYY-MM-DD`, `--to YYYY-MM-DD` | UTC, inclusive (`--to` includes the whole day). `--to` before `--from` is an error. |
| `--include-invalid` | Keep analyses tagged invalid. |

**Options:**

| Flag | Meaning |
|---|---|
| `--group-by auto\|aliquot\|identifier\|sample\|none` | One summary row per group. Default `auto`: aliquot when any analysis is a heating step, else identifier. |
| `--sigma 1\|2` | Level of every plus-or-minus column. Default 2. |
| `--constants default\|legacy\|legacy_preferences` | Decay constants and ratios used. |
| `--decay-error` | Propagate the decay-constant uncertainty into the ages. |
| `--plateau fleck\|mahon` | Plateau criterion (default `fleck`). |
| `--plateau-steps N` | Minimum plateau steps, 2 or more (default 3). |
| `--plateau-gas PERCENT` | Minimum 39ArK in the plateau, 0 to 100 (default 50). |
| `--lab NAME` | The laboratory, written in the metadata. |
| `--note TEXT` | A metadata row; repeatable. |
| `--limit N` | At most N analyses (default 5000). |

Exit codes: 0 wrote the file; 1 no analysis matched; 2 usage or fatal error.

```bash
elctl export --db sqlite:$HOME/pychron/data.db --out report.csv --sample FC-2 --sigma 1
elctl export --db postgresql://user:pw@host/pychron --out report.json \
    --project examples --irradiation NM-300 --group-by sample --plateau mahon
```

The file is marked as a user file after it is written so macOS Gatekeeper lets
you open it.

### publish

`elctl publish` makes a *release*: an immutable, citable directory of a
selection's reduced data (GeoParquet tables, an OGC API - Records catalogue,
the Schaen report, a manifest of every file's hash and of the revisions
pinned). **Needs persistence.** The directory's contents are documented in
[release format](../release_format.md); `elctl publish help` prints every
option. Nothing is public by default: a release is a private draft until it
is published, and private until it is made public.

```bash
elctl publish create  --db <url> [selection] [--tag T]... [--ia NAME|--no-ia] --name <slug> --title <text>
                      --author "<name>[|orcid|affiliation]"... [--licence <spdx>] [--description <text>]
                      [--publisher <name>] [--embargo-until YYYY-MM-DD] [--supersedes <name|name@vN|uuid>]
                      [reduction and report options as export]
elctl publish build   --db <url> --release <r> --out <dir> [--private] [--with-figures] [--with-viewer]
elctl publish verify  <dir> [--db <url>]
elctl publish publish --db <url> --release <r> --manifest <dir>/manifest.json [--public]
elctl publish withdraw --db <url> --release <r> --reason <text>
elctl publish list    --db <url> [--state draft|published|withdrawn]
elctl publish show    --db <url> --release <r>
elctl publish delete  --db <url> --release <r>
```

`<r>` is a release name (its newest version), `name@vN`, or a uuid.

| Command | Does |
|---|---|
| `create` | Pins the selection's heads and references (and every interpreted age whose members are all selected, or the ones named by `--ia`) in a bookmark and writes a new draft release. The selection and reduction flags are `export`'s; `--tag` keeps only analyses with those head tags (default: every tag but `invalid`). A second `create` of a name makes version 2, superseding version 1. The licence preselects `CC-BY-4.0`; the texts of CC-BY-4.0, CC-BY-SA-4.0 and CC0-1.0 ship with pychron, another id is named in `LICENSE.txt` only. Prints the release's uuid and counts; exit 1 when nothing matched. |
| `build` | Writes `<dir>/<name>-v<N>`. Embargoed samples and analyses are left out and counted in the manifest; `--private` keeps them and marks the build private. Without Apache Arrow (`PYCHRON_PUBLISH`) the Parquet tables are not written and the command says so. `--with-figures` adds `figures/<sample>/` with an ideogram, a spectrum per step-heated aliquot and an inverse isochron (three or more analyses), each as SVG and PDF (`docs/release_format.md`, Figures); `verify --db` then rebuilds with them. `--with-viewer` is accepted and ignored in this version. |
| `verify` | Every file against `manifest.json`; with `--db`, also against a rebuild of the same release from that store (row hashes per table, bytes per non-Parquet file). Exit 1 names each difference. |
| `publish` | Marks the draft published, recording the manifest of the build given; refuses a manifest whose pins are not this release's. Private unless `--public`. |
| `withdraw` | A published release, with a reason; it goes private and never changes again. |
| `list`, `show` | What releases the store has; one release's details, counts and bookmark. |
| `delete` | A draft only. |

`doi`, `igsn` and `upload` are not built yet.

### embargo

An embargo hides a project, a sample or an analysis from every public release
and hosted view until a date, or until it is lifted. **Needs persistence.**

```bash
elctl embargo set  --db <url> (--project P | --sample S | --analysis A)... [--until YYYY-MM-DD] [--reason <text>]
elctl embargo lift --db <url> (--id <uuid> | --project P | --sample S | --analysis A)...
elctl embargo list --db <url> [--live]
```

A project is its name or uuid; a sample `name`, `name@project` or a uuid; an
analysis a run id (`70001-01`, `70001-01A`) or a uuid. An ambiguous name
(a sample in several projects) is refused naming the candidates. `--until`
is an inclusive date: the embargo ends at midnight UTC after it; without it
the embargo lasts until lifted. `lift` by scope lifts every live embargo on
it. `list` shows each embargo's id, scope, end and reason; `--live` only the
ones in force. Exit 2 for a usage error or an ambiguous name.

## EarthBank workbooks

`elctl earthbank export-xlsx` writes the 40Ar/39Ar results of the selected
analyses into EarthBank's own import workbooks (EarthBank is AuScope's
AusGeochem data platform). **Needs persistence.** `elctl earthbank help` prints
every option. The guide, for the window and the command line alike, with what
is sent for a sample and what each message means, is
[Publishing to EarthBank](../earthbank.md).

```bash
elctl earthbank export-xlsx --db <url> --out <dir> [selection] [options]
```

The command contacts nothing: it fills copies of the service's two template
workbooks, and **you import the workbooks by hand on the service's website**.
Each sample becomes one data point with the one age reported for it.

Files written into `--out` (made if it is not there; nothing is ever
overwritten). The time in a file name is UTC:

| File | Holds |
|---|---|
| `ArArDataPoint_<yyyymmdd-hhmmss>.xlsx` | Every sample: its data point, one measurement row per analysis, its aliquots, the age reported and how it was calculated. With `--per-sample`, one `ArArDataPoint_<sample>.xlsx` per sample instead. |
| `Samples_<yyyymmdd-hhmmss>.xlsx` | One row per sample. Left out with `--no-samples`. |

**Selection** is that of [`export`](#publication-data): `--sample`,
`--identifier`, `--project`, `--irradiation`, `--type`, `--uuid`, `--from`,
`--to`, `--include-invalid`, `--limit`.

**Ages** are made as `export` makes them, by the same flags: `--constants`,
`--decay-error`, `--plateau`, `--plateau-steps`, `--plateau-gas`. A workbook is
of samples and always at one sigma, so a sample's age and uncertainty in it are
those of `elctl export --group-by sample --sigma 1` with the same selection and
flags. `export`'s own defaults differ: it groups step-heated analyses by
aliquot and reports at two sigma.

**Options:**

| Flag | Meaning |
|---|---|
| `--out DIR` | Where the workbooks go. Required, except with `--check`. |
| `--per-sample` | One Ar-Ar workbook per sample, not one for all. |
| `--no-samples` | No Sample workbook. |
| `--check` | Print what the export would print and write nothing. |
| `--templates DIR` | The directory holding the service's two templates. Default: the directory the environment variable `PYCHRON_EARTHBANK_TEMPLATES` names; when it is not set, the ones installed with Pychron. |
| `--overrides FILE.toml` | What the analyses cannot say; see below. |

**The age reported for a sample.** A sample with heating steps reports its
plateau when it has one and its total gas (integrated) age when it has not; a
sample of fusions reports the weighted mean. `preferred_age` in the overrides
file chooses another. A sample with more than one step-heated aliquot is not
left to the rule: the command stops and asks for a `preferred_age`.

**The overrides file** is TOML. Every key may be left out. A key that is not in
this table, or a sample that is not among the selected analyses, is an error
naming it. So is a path that is not a file.

```toml
setup = "Example Lab ARGUS VI"
decay_constant = "Min et al. 2000"
air_ratio = "Lee et al. 2006"
age_includes_j_error = true

[samples."S-STEP"]
interpretation = "Eruption age"
preferred_age = "plateau"
flux_monitor = "Fish Canyon Tuff sanidine Kuiper et al. 2008"
datapoint_name = "S-STEP sanidine"
irradiation = "NM-300"
```

| Key | Meaning | Left out |
|---|---|---|
| `setup` | Your laboratory's analytical set-up, by the name it has on EarthBank, where it must already exist. | The column is left empty and a note says so. |
| `decay_constant` | The service's name for the decay constant, or a citation. | Named from the value the reduction used. |
| `air_ratio` | The same, for the atmospheric 40Ar/36Ar. | Named from the value the reduction used. |
| `age_includes_j_error` | `true`: the reported age's uncertainty includes the J uncertainty. `false`: analytical only. The age's description says which. | `true`. |
| `[samples."<name>"]` | A table per sample, by the sample's name in the store. | |
| `interpretation` | The service's name for what the age dates. | `Unknown`. |
| `preferred_age` | `plateau`, `weighted_mean`, `isochron` or `integrated`. One that cannot be computed is an error. | The rule above. |
| `flux_monitor` | Replaces the monitor the analyses name. | The analyses' monitor. |
| `datapoint_name` | The data point's name, which ties its rows together. | `<sample> <material>`. |
| `irradiation` | The irradiation batch, by its name on EarthBank. | The irradiation the analyses name. |

**What is printed.** One line per sample (its analyses and the age reported)
with the notes about it under it (the sentence written as the age's
description is shown as `description: ...`), then `wrote <file>` for each workbook, then
`note:` lines about the workbooks: a value that is not in a list of the
template, a column the template marks Required that a row leaves empty. Read
the notes before importing; several name the key of the overrides file that
resolves them. Errors go to standard error.

A sample that cannot be published stops the whole export, and nothing is
written for any sample: analyses of one sample that disagree about what the
sample is (material, location, constants, flux monitor), a sample with no
analysis that can be published, a preferred age that cannot be computed. The
message names the sample and, where there is one, the setting that resolves
it. An analysis without an age, or without all five argon isotopes measured, is
left out of its sample and counted in the sample's notes.

Exit codes: 0 wrote the files (with `--check`: there was something to write);
1 no analysis matched; 2 usage or fatal error.

```bash
elctl earthbank export-xlsx --db sqlite:$HOME/pychron/data.db --check --project examples
elctl earthbank export-xlsx --db sqlite:$HOME/pychron/data.db --out earthbank/ \
    --project examples --overrides earthbank.toml --per-sample
```

The workbooks are marked as user files after they are written so macOS
Gatekeeper lets you open them.

### The EarthBank account

These are the only `elctl` commands that contact EarthBank, and they contact
the server of your profile and nothing else. They need Qt6 Network in the
build (the released programs have it); without it they print `this build has
no network support (Qt6 Network was not found)` and exit 2. Like the rest of `elctl earthbank`, they are built with
persistence.

A **profile** names an account: the server, your login, and the institution
and data package uploads go to. Profiles live in one file (the one the
Pychron window uses, so a login made here is a login there):
`~/Library/Application Support/Pychron/earthbank/profiles.toml` on macOS,
`%APPDATA%\Pychron\earthbank\profiles.toml` on Windows,
`~/.config/pychron/earthbank/profiles.toml` on Linux. Every subcommand takes
`--profiles FILE` for another. The file never holds a password: it is kept by
the operating system (the login keychain on macOS, the Credential Manager on
Windows, the secret service on Linux; where there is none, until the program
ends, and `login` says so).

| Command | What it does |
|---|---|
| `earthbank login [--profile NAME] [--username U] [--base-url URL] [--institution ID]` | Signs in, prints the account's name, keeps the password, saves the profile and makes it the active one. `--profile` defaults to the active profile, else `default`. The user name is needed the first time. The password is asked for without echo, or taken from `$EARTHBANK_PASSWORD`; with neither (no terminal, as in a script) it exits 2 and says so, and never waits for input. |
| `earthbank account [--profile NAME]` | Signs in with the kept password (or `$EARTHBANK_PASSWORD`) and prints the profile and who the account is. |
| `earthbank packages [--institution ID] [--profile NAME]` | Lists the data packages, of one institution when given or when the profile has one. `*` marks the one the profile uploads to. |
| `earthbank packages --create NAME [--institution ID]` | Makes a private data package in progress and sets the profile's data package to it. |
| `earthbank preflight --db URL [selection] [ages] [--overrides FILE] [--base-url URL]` | Builds what an upload would send for the selected analyses and asks EarthBank whether it knows every controlled term in it. Writes nothing to the service. Without a login it asks what needs none and lists the rest as not asked. |
| `earthbank upload --db URL [selection] [ages] [--overrides FILE] --yes [--package ID] [--allow-unresolved] [--new-datapoint] [--in-doubt written:ID\|not-written] [--journal DIR]` | Sends the selected analyses to a data package: for each sample the sample itself (if the package has none of that name), a data point, its aliquots and measurements, the age calculation and the age summary. See [Uploading](#uploading) below. |
| `earthbank import-profiles FILE` | Takes the profiles of a legacy Pychron preferences file into the profiles file, without passwords (sign in once per profile with `login`). A profile whose name is already there is left alone. |

`preflight` prints, for each sample, the terms EarthBank does not know (or
could not be asked about) and the notes the mapping made, then the terms it
found as something other than what is written (an id written to EarthBank
cannot be corrected, so read these) and the fields it does not check. Exit
codes: 0 nothing unknown and nothing unasked; 1 some terms unknown or unasked;
2 could not run. `login`, `account` and `packages` exit 1 when EarthBank
refuses or does not answer, and 2 when they could not try (no profile, no
password, a bad flag). No command prints a password or a token.

```bash
elctl earthbank login --profile mylab --username a.user --institution 1001
EARTHBANK_PASSWORD=... elctl earthbank account            # in a script
elctl earthbank packages --create "Ar-Ar results 2031"
elctl earthbank preflight --db sqlite:$HOME/pychron/data.db --project examples
```

### Uploading

`elctl` cannot change or remove a record on EarthBank, so `upload` shows what
it is about to do before it does anything. It signs in, then prints the
account, the data package (its id and its name, as EarthBank has them), for
each sample the age it reports and the records it would write, and the
pre-flight report. Without `--yes` it stops there and exits 2: read it, then
run the same command with `--yes`.

```bash
elctl earthbank upload --db sqlite:$HOME/pychron/data.db --sample MB-12 --overrides earthbank.toml
elctl earthbank upload --db sqlite:$HOME/pychron/data.db --sample MB-12 --overrides earthbank.toml --yes
```

- **The data package** is `--package ID`, else the profile's (set by
  `packages --create`). With neither, nothing is sent.
- **The password** is the one `login` kept, else `$EARTHBANK_PASSWORD`, else
  it is asked for at a terminal. With none of these, or when EarthBank refuses
  it, the command exits 2 having sent nothing and says to run `login`.
- **Terms EarthBank does not know** stop the upload (exit 1, nothing sent).
  Correct them in the overrides file, or add `--allow-unresolved` to send the
  records without those fields; each one left out is listed under its sample.
  A term EarthBank could not be asked about always stops it: try again.
- **Progress** is one line for each record as EarthBank takes it, with the id
  it gave, then one line for each sample: `complete`, `partial`, `stopped`,
  `in doubt` or `refused`, with the data point's id.

What has been written is kept in a **journal** on this computer, one file for
each sample and data package (`--journal DIR`; by default the `journal`
directory beside the profiles file). It is the only record of an upload that
did not finish, so do not delete it, and finish an upload on the computer it
was begun on.

- **`partial` or `stopped`**: some records were not written (EarthBank
  refused one, or the connection failed before it was sent). The command
  prints the line that finishes it: the same command again. Only what is
  missing is sent.
- **`in doubt`**: a record was sent and no answer came back, so it may or may
  not be on EarthBank, and a record sent twice is there twice. Nothing more of
  that sample is sent. Look for the record on EarthBank, then run one of the
  two lines the command prints: `--in-doubt written:ID` with the id EarthBank
  shows for it, or `--in-doubt not-written`. The answer is applied to one
  sample: when several are in doubt, upload them one at a time (`--sample`).
  A sample in doubt needs no answer; run the command again and it is looked
  for by name.
- **`refused`**, the data changed: the analyses, or what the overrides say of
  them, are not what the journal's upload sent. `--new-datapoint` sends the
  sample again as new records; the old ones stay on EarthBank until you remove
  them there.

Exit codes: 0 every sample is complete; 1 no analysis matched, or terms
unknown or unasked; 2 anything else, a sample that is not complete included.

## Sample and package entry

`elctl entry` adds and edits samples, packages (irradiations), their levels and
positions, and identifiers in the store. **Needs persistence.** Changes go
through the same checked path as the Entry windows (see [entry](../entry.md) for
the file formats and rules); `elctl entry help` prints every option.

```bash
elctl entry <what> <action> [args] --db <url> [--user <name>]
```

`--db` is required for everything except `help` and `samples template`.
`--user NAME` is recorded as the author of changes; default: the `USER`
environment variable, else `pychron`. `--dry-run` (where listed) prints the row
each change would make and writes nothing.

| Command | What it does |
|---|---|
| `samples import <file.csv> [--update-existing] [--errors <out.csv>] [--dry-run]` | Add the samples in a CSV (or tab-separated) file, with their PIs, projects and materials. Rows already stored are left alone; with `--update-existing`, rows whose fields differ are updated. If any row is in error, nothing is written, and `--errors` writes the rows with their problems to a CSV. |
| `samples template <out.csv>` | Write a CSV with every column the import reads. Needs no database. |
| `samples list [--pi NAME] [--project NAME] [--material NAME] [--text T]` | List samples, filtered. |
| `package add <name> [--kind irradiation\|package] [--levels A-C \| A,B,D] [--holder NAME] [--z Z] [--reactor NAME] [--chronology FILE --tz ZONE]` | Add a package and its levels in one change. Kind `irradiation` (the default) needs a reactor (production values come from `reactors.json`) and a chronology file of `power,start,end` lines in local time (`--chronology` needs `--tz`). |
| `package show <name> [--level L] [--csv]` | Show a package and its levels (or one level), as text or CSV. |
| `package set-kind <name> irradiation\|package` | Change a package's kind. |
| `positions import <package> <file.csv> [--dry-run]` | Put samples in a package's positions. Columns: `level`, `position`, `sample`, and optionally `project`, `principal_investigator`, `material`, `grainsize`, `weight`, `packet`, `note`. |
| `identifiers generate <package> [--overwrite] [--dry-run]` | Number every position that has a sample and no identifier, continuing the store's sequence. `--overwrite` renumbers identifiers nothing has used. |
| `holders import <file.txt> [--name NAME]` | Import a legacy irradiation holder file. |
| `settings show` | Print the entry settings as JSON. |
| `settings set <key> <value> [--dry-run]` | Change one entry setting (a value that is valid JSON is used as JSON, otherwise as text). |

Exit codes: 0 OK; 1 nothing was written (a stale, refused or invalid row, or the
settings changed meanwhile); 2 usage or fatal error (including a database that
cannot be opened). `elctl entry` with no arguments prints the full help and exits 2;
`elctl entry help` prints it and exits 0.

```bash
elctl entry samples template samples.csv
elctl entry samples import samples.csv --db sqlite:$HOME/pychron/data.db --dry-run
elctl entry package add NM-300 --kind irradiation --levels A-C --reactor "Triga" \
    --chronology chron.csv --tz America/Denver --db sqlite:$HOME/pychron/data.db
elctl entry positions import NM-300 positions.csv --db sqlite:$HOME/pychron/data.db
elctl entry identifiers generate NM-300 --db sqlite:$HOME/pychron/data.db --dry-run
```

## Not yet implemented / unverified

- This page was written from the source, not from running every command. Output
  wording was copied from the code where it was short and described where it was
  long. If a message differs on your screen, the screen wins; please report it.
- `elctl help` (the built-in summary) does not list `laser cameras`,
  `laser camera-scale`, `laser snapshot`, or the `entry` subcommands `samples
  list`, `samples template`, `holders import` and `settings`; they exist and are
  documented above from the code. `elctl --help` also does not mention
  `--canvas` for `exp`.
- The default install folder `init` chooses when `--root` is omitted, and the
  default install name, were not checked; pass `--root` and `--name` when you
  care.
- Whether `elctl` as shipped in a release package is built with persistence
  (and OpenCV for `laser look` and `autocenter`) depends on the build; the
  installed program's behaviour is the test. Without OpenCV the camera finder
  parts are stubs.
- No driver has been run against a real instrument. The hardware commands are
  tested against simulators and recorded traces only; see the
  [README](../../README.md) "Hardware status" before pointing `open`, `close` or
  `heater` at a real line.
- The exact exit code when `import` pauses for a limit or Ctrl-C is 0 per the
  built-in help; this was not exercised.
