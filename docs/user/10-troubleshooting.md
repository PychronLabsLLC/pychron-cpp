# Troubleshooting

Messages you may see, what they mean, and what to do. Grouped by where they
appear. Most messages show in the Log dock of `pychron-ui` or on the terminal
for `elctl`. Install-level problems are also in
[the installation runbook](../installation_runbook.md#troubleshooting).

First checks for almost any problem:

```bash
elctl doctor            # checks the config; --strict makes warnings fail
elctl probe             # tries every transport and device (prints FAIL per one)
```

Everything here was collected from reading the source; wording on screen may
differ slightly.

## Starting the programs

| You see | Cause | Do |
|---|---|---|
| `--sim-speed needs --sim`, `--device needs --laser` | Flag used without the flag it depends on. Exit code 2. | Add the missing flag. See [getting started](01-getting-started.md). |
| `sim.toml:<line>:<key>: unknown volume 'X'; known: ...`, `... out of range, must be ...` | A name or number in the simulated lab's `sim.toml` is wrong. The line does not load. | Fix the key the message names. See [The simulated lab](../simulator.md#errors-you-will-meet). |
| `--laser opens only the laser window: it takes no --queue/--spectrometer` | Incompatible flags | Drop `--queue` / `--spectrometer`. |
| `config files, --install, --setup and --examples exclude each other` | More than one way to say which config | Use one. |
| `--queue needs a file` | The next word began with `--` and was read as a flag | Give the file name right after `--queue`. |
| `no installation named 'X' in <site.toml>` | Typo, or the install was never made | `pychron-ui --setup`, or File > Installations. |
| `several installs and no default; pass --install NAME` | More than one install, none marked default | Pass `--install NAME`. |
| `nothing is installed yet` | No install exists | Run the setup wizard (`pychron-ui --setup` or `elctl init`). |
| `installation 'X': ... elctl doctor --install X says more` | Install config fails to load | Run the command it names. |
| `pychron-ui --setup can set it up again` | A data-reduction install cannot open its database, and has nothing else to show, so the program does not start. An instrument install starts anyway (next row). | Re-run setup, check the database server is up and credentials are right. |
| The window opens, the data window has a banner `The database could not be opened: <reason>` and `Showing the records folder <path>.`, the Log dock has `ERROR [ui]` with the reason, and no queue will start | An instrument install's database could not be opened (the server is away, its password could not be read, or the schema is behind). The line can still be worked; the data window shows only the record files that folder holds, not the install's analyses, and the Entry and Fit menus are missing. A queue is refused with `the queue cannot be saved:` and `the database cannot be reached: <reason>`. | Put right what the reason names: start the server, check `<root>/.pychron/credentials.toml`; for `database schema is out of date` run `elctl db migrate --db <url>` (the reason names it), with pychron closed. Then restart `pychron-ui`. `elctl doctor --install <name>` says more. |
| `--db` exits 2, "database schema is out of date" | The store was made by an older pychron (the UI never migrates). | `elctl db migrate --db <url>` (see [elctl reference](08-elctl-reference.md#the-stores-schema)), with pychron closed. |
| `--db` exits 2, "built without persistence" | The build has no database support. | Use a build with persistence. |
| `built without the DVC store` / `elctl was built without persistence` | The build skipped persistence. `import`, `export`, `entry` are unavailable. Exit 2. | Install a build with persistence (Windows CI builds have none). |
| `elctl` with no `-c` uses a surprising config | With no install found it quietly falls back to `extraction_line.toml` in the current folder | Pass `-c`, or `cd` to the right folder. |

## The window opens but something is greyed out

- **View > Spectrometer, Experiment, Laser all greyed:** the extraction line
  did not start. The Log dock shows `ERROR [ui] start failed` and
  `experiment unavailable: extraction line did not start`. Read the line above
  it; usually a bad `extraction_line.toml` or a transport that cannot open.
- **Only View > Spectrometer greyed:** the spectrometer config failed to load
  (`ERROR [ui] spectrometer not loaded: ...`).
- **`simulation requested but <file> is not a simulated spectrometer`:**
  `--sim` refuses any spectrometer config that is not fully simulated, even a
  `thermo_qtegra` driver on a `sim` transport. This stops a dry run being
  mistaken for real hardware. Use the `sim-*` spectrometer configs.
- **Entry menu missing:** it appears only when there is a database. On an instrument install whose database could not be opened, see "The database could not be opened" above.
- **The Experiment menu's Queue, Rows, Executor, Scripts say "Open View > Experiment to use this menu":** open the Experiment window first.

## Extraction line

See [extraction line](02-extraction-line.md).

| You see | Meaning | Do |
|---|---|---|
| `'X' is software locked` | The valve is locked. Locks persist in `<config>.state.toml` beside `extraction_line.toml`, so a valve that refuses everything after a restart is usually locked. | Right-click > Unlock (asks to confirm). Manual valves cannot be locked. |
| `cannot open 'A': interlocked with 'C' which is open` | Interlock. | Close `C` first. |
| `... requires 'B' open, it is closed` | Required valve closed. | Open `B` first. |
| `'X' read back closed after open` | Command sent, hardware reports closed. | Check wiring, relay, air. |
| A valve with unknown state blocks its interlock partners | No read-back yet. | Wait for the first scan or check the transport. |
| `ERROR [ui] <valve> rejected: ...` | Any refused command; the valve also flashes. | Read the reason after the colon. |
| `WARN [canvas] valve 'X' is not drawn ...` | In the line config, missing on the canvas; it cannot be clicked. | Add it to `canvas.toml`. |
| Canvas names something not in the line config | Hard error. | Fix the name. |
| Alarms never go away | The Alarms dock clears only on Acknowledge, one row per source. | Acknowledge the row. |
| Red chip in the health bar | Transport not connected. Amber: connected with errors. Hover for the last error. Chips show age such as `12s`, `3m`. | Check cable, address, power. |
| Valves look fine but nothing moves (legacy NMGRL) | The importer converts Arduino and furnace controllers to `sim_valves`. | Configure a real driver. |

## Spectrometer

See [spectrometer](03-spectrometer.md).

| You see | Meaning | Do |
|---|---|---|
| Banner `no frame from acquirer N within X s` (also an alarm, source `acquisition`) | Scan stalled. | Press Restart; check the transport. |
| `unknown key 'x'`, `unknown detector 'X'`, `unresolved alias '@x'`, `reference detector 'H1' not in hop and no 'position'`, `every detector excluded` | Plan file mistakes; the plan loader is strict. | Fix the plan; run `elctl exp validate`. |
| The v2 `[[segment]]` / `[[gas]]` plan format is rejected | Draft spec only. | Use the shipped plan format. |
| Peak centring fails on a real instrument | The shipped `window` (0.06) suits the simulator; the built-in default is 0.015. A window that misses the real peak fails. | Set `peak_center.toml` for your instrument; flagged by `elctl exp validate` if the named config is missing. |
| Magnet move asks for confirmation | Threshold defaults to 5 amu in File > Preferences > Spectrometer. | Set 0 to turn the question off. |
| Opening the window starts the scan; closing stops it | By design. | |

**Real instruments.** The Qtegra and NGX drivers have never been run against an
instrument, and example calibration values are simulation placeholders. Wrong
field tables can leave a detector unprotected during a move. `elctl doctor`
warns on any file still marked `SIMULATION PLACEHOLDER` or `CONFIRM`. Do the
bring-up checklist before use. Qtegra source ramping is not implemented: HV and
trap current change in one step.

## Experiments and scripts

See [experiments](04-experiments.md) and [scripting](09-scripting.md).

| You see | Meaning | Do |
|---|---|---|
| `the queue has errors; nothing was run` | Validation failed. | `elctl exp validate`, fix rows. |
| `--from and --resume are exclusive` | | Use one. |
| `no extraction line config at ...` | | Pass `-c`. |
| `script check failed: <script>:<line>: <code>: ...` | Static check of a script. | Fix the line. |
| `<script>:<line>: NameError: ...` | Unknown name in a script. | Check [the API](09-scripting.md). |
| `waitfor timed out after N s` | A wait ran out. | Check what it waited for. |
| `gosub 'x' not found`, `gosub depth limit reached` | | Fix name; remove recursion. |
| `read-only` error | You assigned to a run variable such as `duration`. | Use another name. |
| `cancelled: ...`, `aborted: ...` | Operator stopped the run. Cancel acts only at blocking commands; Abort refuses all hardware calls, even in `finally:`. A tight loop with no blocking call ends only with Abort. | |
| A run ends `success (not saved)`, its row is flagged, the Executor pane says "N run(s) waiting to be saved to the database: ...", or `elctl exp run` ends `warning: N record(s) waiting in <data>/spool: ...` | The run was measured and its record is safe in `<data>/spool`, but the database (or the records folder) did not take it. The reason follows the colon and is also the run's own line, `not saved to the data store, kept in the spool: <reason>`. A notification goes out as for a failed run. | Put right what the reason names (the database is away or locked, the disk is full, an identifier was removed from the catalog) and start a queue: the spool is sent again at every queue start, and a run is never stored twice. |
| Row shows "saved to the spool only" | The same, for a record that could not be completed (the run's error says what was missing). | Read the run's error. |
| `the queue cannot be saved:` and one line each, and the queue does not start | The database could not take a run of the queue: `identifier 'X' is not in the database`, `mass spectrometer 'X' is not in the database`, `extract device 'X' is not in the database`, `the queue names no mass spectrometer`, `X: step 'A1' is not letters: the database cannot hold it`, `66001-01 is already in the database`, or `the database cannot be reached: ...`. The queue is checked against the database before it starts, so nothing is measured first. Only rows that will run are checked; an edit to a running queue is refused with the same lines. | Add what is missing with **Entry** or `elctl entry` (a sample and its identifier, in the Entry window or `elctl entry`; see [entry.md](../entry.md)); on an install from before its database knew the instrument or its extract devices, `elctl entry seed <install>/seed.toml --db <url>`; fix the step; give the row another aliquot, or start from a later row. For `the database cannot be reached`, check that the server is up and the credentials are right; if the reason says the schema is out of date, `elctl db migrate --db <url>`. |
| `note: simulated runs are written to <data>/records, not to <database>; pass --sim-save to save them there` (`elctl exp run`; `pychron-ui` logs the same text without `note:`, as an `INFO [ui]` line) | A simulated session does not write to a database unless it is the install's own SQLite file on this computer (not one given with `--db`). Not an error; the data window goes on showing the database, which does not list these runs. | Nothing, or add `--sim-save` when the simulated runs are wanted there ([Experiments](04-experiments.md#9-simulation-and-simulated-speed)). |
| A run finished and is not in the data window | The data window follows a queue by itself, but only lists what is in its source. Usual causes: the session is simulated and wrote record files (the `note:` above); the window's source is a records folder or another database than the one the run went to; a filter or **Hide invalid** hides it (Date is counted from the newest analysis, not from today); or the run was not saved (the row is flagged, above). | Check the Log dock for the `simulated runs are written to` line, the window title (`Data - <source>`) and the filters; press **Rescan** for a run saved by someone else. |
| `--sim-save: there is no database to save to` | `--sim-save` with neither `--db` nor an install that has a database. | Drop it, or name a database. |
| `warning: built without the DVC store: runs are written to <data>/records, not to the installation's database` | This build cannot write to a database. | Use a build with persistence. |
| A cancelled run saves nothing; a failed run ends the queue | By design. | |
| `not supported: get_intensity`, `not supported: shared resources` | These script commands have no provider in a queue run, and are caught only at run time. | Avoid them. |
| `opt.<name>` is empty | The run's `options` text is not passed to scripts in automated runs. | |
| Script editor shows no error, run fails | The editor assumes every capability, e.g. `dump_sample()` on a laser. | Check the device supports it. |
| Saving a queue or the conditionals editor drops comments | Known limit. | Keep notes elsewhere. |
| Pause row with a duration is rejected | Validator forbids extraction fields on `pause` rows. | |

On the command line: the first Ctrl-C stops after the current run, the second
cancels, the third aborts. A cancelled or aborted queue exits 1. Pass `--data DIR`
for the walkthrough, otherwise records go to `<examples>/data` inside the
repository.

## Laser

See [laser](05-laser.md).

| You see | Meaning | Do |
|---|---|---|
| `not supported: extract in watts (a Chromium takes percent)` | Queue `units` defaults to watts. | Set `units = "percent"`. |
| `unknown extract units 'amps'` | The queue accepts `amps` and `volts`; a script's `extract()` knows only percent, watts and temp. Passes the queue check, fails at run. | Use a supported unit. |
| `the laser is not enabled` | `extract()` before `enable()`. | Call `enable()` first. |
| `cannot enable: laser interlock tripped (...)` | | Clear the interlock. |
| `camera of <dev>: ...` and the queue will not start | A `cameras.toml` table with no `source` is a simulated camera; over a real laser it is refused. | Set `source`. |
| `no tray map 'x' in .../tray_maps`, `no hole N on tray T` | | Fix the name or hole. |
| `<device> / <tray> is not calibrated`, `... calibration is stale (the tray map has changed ...)` | | Recalibrate the stage. |
| `unknown extraction device 'x'`, `unknown pattern x` | | Fix the name. |
| `pattern x follows the glow and <dev> has no camera` | | Use a fixed pattern or add a camera. |
| `the run names a hole and the queue has no tray` | | Set a tray on the queue. |
| Furnace, pipette, motor commands fail with `not supported` | No driver ships. | |

No real Chromium laser has been driven; only the simulator. The pylon camera
driver reports "built without pylon" unless built with it.

## Importing and exporting data

See [legacy import](../legacy_import.md), [export](../export.md) and the
[elctl reference](08-elctl-reference.md).

| You see | Do |
|---|---|
| `no database at <path>` / `<path> is empty: not a pychron store` | Run `import add` first or point at the right store. |
| `--source: the url carries a password or a token` | Pass credentials the supported way ([legacy import §4](../legacy_import.md#4-credentials)). |
| `--tz: no time zone 'X'` | Install `tzdata` or use a valid IANA name. |
| `already registered with other settings` | Source exists with different options; use the same ones. |
| Import fails to start | Needs `git` 2.32 or newer on PATH and `tzdata`. |
| `import verify` exits 1 | Store differs from the source. Read the report. |
| `export` exits 1 | Nothing matched. Default selects `--type unknown` only, hides `invalid`-tagged analyses unless `--include-invalid`, and `--limit` defaults to 5000. |
| `no answer for <id> (input ended)` from `elctl init` | stdin ended mid-prompt. |
| `nothing written` from `elctl init` | You answered no. |
| A hand-edited file gets a `.new` copy | `init` never overwrites your edits; compare and merge. |

## Data analysis and configuration files

See [data analysis](06-data-analysis.md) and the
[configuration reference](07-configuration-reference.md).

| You see | Meaning | Do |
|---|---|---|
| `Not saved: <who>` / `Not restored: ...` | Saving fits, blanks, IC factors or restoring a revision is compare-and-swap. Someone else moved the head; nothing is written (all or nothing for batches). | Recall again, redo. |
| Save and History disabled | A records-folder source keeps no revisions. It is the source when there is no database, or when an instrument install's database could not be opened (banner above). | Use `pychron-ui --db`, or an install that has a database, and put its database right. |
| No ages from a records folder | Needs `<data>/records/references.toml`. `Rescan failed: <file>: ...` means it is broken; `unknown production ratio key '...'` a bad key. | Fix the file. |
| An analysis has no age | Recall's Summary gives the reason: no J, no production ratios or chronology, unknown intercept/baseline/blank/IC, zero 39Ar, or `1 + J F` not positive. | Fix that input. |
| Date filter surprises | It is relative to the newest analysis in the source, not today. | |
| Export, Publish or a figure button is greyed out | Selected is empty. | Add analyses to Selected (**Add →**, **Add all**). |
| Figure exclusions vanish | Not stored; lost when the window closes. `invalid` analyses in Selected are still used; **Hide invalid** only keeps them out of Filtered. | |
| Export differs from `elctl export` | Figure windows and browser Export use constants preset `default`, no decay error. | Use `elctl export --constants` / `--decay-error`. |
| Preset warning on the status line | A bad value is dropped; a wrong `schema` key is an error. Saving under a factory name shadows it; Delete restores it. | |
| Config file fails to load | Loads are all or nothing and unknown keys are errors. A `*.local.toml` may override only transport host, port, baud and timeout (line file also data_bits, stop_bits, parity, and the `[logging]` and `[metrics]` tables; spectrometer file also driver credentials). | Remove other keys. |
| `table 'x' not found (expected tables/x/current)` and other spectrometer validation errors | Field table, axis, detector column, acquirer channel or HV settings do not match. Details in the [configuration reference](07-configuration-reference.md). | Fix the config. |
| Run override of a plan parameter refused | Parameter not in `parameters.expose`. | Mark the run `advanced` or expose it. |
| Calibration invalid after editing a tray map | Stage calibration is tied to the tray map by SHA-256. | Recalibrate. |
| Camera `use = "view"` or `source = "recorded"` cannot drive follow-glow | Also, simulated and live camera/laser mixes are refused. | |
| Database password problems | The password lives in `<root>/.pychron/credentials.toml`; an invalid file stops a data-reduction install's startup. On an instrument install the program starts and shows the banner above. The `postgresql://` URL needs a user part. | |

## Interface quirks

- A panel has disappeared (Log, Alarms, Heaters, Evolutions, ...): it was
  closed, or floated off to a screen that is no longer there. Bring the
  window to the front and choose **Window > Panels** to tick it back on, or
  **Window > Reset Layout** to put every panel of that window back as
  installed.
- The theme is always light. Preferences are per user; out-of-range saved
  values silently fall back to defaults.
- An unsaved-queue-edits prompt can block Quit, or switching install in
  File > Installations.
- Canvas `[[image]]`, `[legend]`, `[colors] valve` and stage `fill`/`volume`
  are accepted but not drawn.
- `elctl open`, `close`, `state`, `scan` print `warning: transport X: ...` and
  carry on when a transport will not open.

## Logs

Both programs write a log file as they run, with no setup.

- **Where it is.** An install keeps its logs in `<install>/logs`:
  `pychron.log` for the window, `elctl.log` for `elctl`. Without an install
  (the examples, or config files given on the command line) the file is in
  `~/Library/Logs/pychron` on macOS, `$XDG_STATE_HOME/pychron/logs` (or
  `~/.local/state/pychron/logs`) on Linux and `%LOCALAPPDATA%\pychron\logs` on
  Windows. File > Preferences > Logging can move it or turn it off. To go back
  to the default folder after a `dir` was set, remove `dir` from
  `logging.toml` (or the `[logging]` table of `extraction_line.toml`): a file
  cannot say "unset".
- **Old files.** A file is closed at 50 MB and the last ten are kept
  (`pychron.1.log`, `pychron.2.log`, and so on).
- **Two copies running.** Only one program writes a given file. A second copy
  writes `pychron-<pid>.log` (the number is its process id) beside it.
- **What to send with a problem report.** `pychron.log`, or `elctl.log` for a
  command line problem, from the time of the problem, plus the one before it
  if the program was restarted. For a device that does not answer, set
  `"*.wire" = "trace"` first (see the extraction line chapter) and repeat.
- **Reading a line.** Each line is
  `time [level] [thread] run=<id> logger: message`. The time is UTC (in a
  simulated run, the simulated time). The thread field names the thread that
  wrote the line: `queue` for the experiment queue, `run` for the thread
  that carries out a run, `scheduler` for timed jobs such as gauge and
  temperature polling, `transport.<name>` for a device link, `ui` for the
  window, `elctl` for the command line; a thread pychron did not name shows
  as a number (`t3`). `run=<id>` is there only on lines written while a run
  is in progress.
- **What a run looks like.** With the default `debug` level the log holds
  the whole sequence of an experiment. Lines named `trace.<event>` are the
  events the programs pass between their parts, `script.<script>` the
  scripts and every call they make, `experiment.run` the steps of a run,
  `device.<name>` each call to a device, `spool` and `store` the saving of
  the record. Shortened, from a simulated queue (polling lines and most
  script calls left out):

  ```
  2026-10-23T10:21:15.317Z [info] [queue] trace.QueueStarted: rows=3, from_row=0
  2026-10-23T10:21:20.317Z [debug] [queue] trace.RunStarted: row=0, run=bu-01, run_id=f60b5d8f-9a15-432a-9f4d-fe730a3f857d, identifier=bu
  2026-10-23T10:21:20.317Z [debug] [run] run=bu-01 trace.RunStateChanged: run_id=f60b5d8f-9a15-432a-9f4d-fe730a3f857d, from=preparing, to=extracting, reason=
  2026-10-23T10:21:20.317Z [debug] [run] run=bu-01 script.extraction/sim_extract.py: start extraction/sim_extract.py path=extraction/sim_extract.py sha=f062bc07dd7b0bb9627525100a62d1edeefbf18bc62a7f4b16340109559ce87a kind=extraction
  2026-10-23T10:21:20.317Z [debug] [run] run=bu-01 device.actuator1: close ok in 0 ms
  2026-10-23T10:21:20.317Z [info] [run] run=bu-01 switches: valve C closed
  2026-10-23T10:21:20.317Z [debug] [run] run=bu-01 script.extraction/sim_extract.py: close(name=C) -> ok in 0 ms
  2026-10-23T10:21:22.317Z [debug] [run] run=bu-01 script.extraction/sim_extract.py: sleep(seconds=2) -> ok in 2000 ms
  ...
  2026-10-23T10:25:08.917Z [debug] [run] run=bu-01 spool: put bu-1 (f60b5d8f-9a15-432a-9f4d-fe730a3f857d)
  2026-10-23T10:25:08.917Z [debug] [run] run=bu-01 store: save bu-1: wrote .../records/bu/bu-1.json in 0 ms
  2026-10-23T10:25:08.917Z [debug] [run] run=bu-01 spool: hand-off f60b5d8f-9a15-432a-9f4d-fe730a3f857d: sent
  2026-10-23T10:25:08.917Z [debug] [queue] trace.RunFinished: row=0, identifier=bu, run_id=f60b5d8f-9a15-432a-9f4d-fe730a3f857d, aliquot=1, step=, state=success, truncated=false, save_error=false, status=complete
  2026-10-23T10:32:55.717Z [info] [queue] trace.QueueEnded: end=completed, reason=, runs=3
  ```
- **Finding one run.** The `run=` field is the run's name as the queue
  shows it: identifier, two-digit aliquot and step (`bu-01`, `66001-02A`).
  `grep 'run=66001-02A '` gives every line written for that run, on whichever
  of its threads wrote it. The `trace.RunStarted` line gives both the name
  (`run=`) and the run's internal id (`run_id=`, a long code), which the
  `trace.RunStateChanged`, `trace.RunFinished` and `spool` lines use. The
  Log panel shows the name in its Run column, and its filter box matches it.
- **What `elctl` printed.** Everything `elctl` writes to the terminal's error
  output is also in `elctl.log`, under the logger `elctl`: a line starting
  `error` at `error`, any other at `warn`.
- **No log file.** If `logging.toml` or `logging.local.toml` cannot be read,
  logging goes on with the built-in defaults and the first error in the log
  (and in the Log panel) says what is wrong with the file. Without an install
  and with `HOME` (Windows: `LOCALAPPDATA`) unset there is no folder for the
  file; the program says `no log folder: HOME is not set` once and runs
  without one.

## Still stuck

Collect `elctl doctor` output and the Log dock text, and see
[Escalation in the runbook](../installation_runbook.md#escalation).
