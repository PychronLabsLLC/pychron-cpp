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
| `pychron-ui --setup can set it up again` | A data-reduction install cannot open its database | Re-run setup, check the database server is up and credentials are right. |
| `--db` exits 2 | Schema not current (the UI never migrates), or build without persistence | Migrate with `elctl` (see [elctl reference](08-elctl-reference.md)); use a build with persistence. |
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
- **Entry menu missing:** it appears only when there is a database.
- **Queue, Rows, Executor, Scripts menus say "Open View > Experiment to use this menu":** open the Experiment window first.

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
| Run shows "saved to the spool only" | Record could not be saved to the store. The spool is resent at the next queue start. A warning also prints if records are still spooled at exit. | Check the database; start a queue. |
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
| Save and History disabled | A records-folder source keeps no revisions. | Use `pychron-ui --db`. |
| No ages from a records folder | Needs `<data>/records/references.toml`. `Rescan failed: <file>: ...` means it is broken; `unknown production ratio key '...'` a bad key. | Fix the file. |
| An analysis has no age | Recall's Summary gives the reason: no J, no production ratios or chronology, unknown intercept/baseline/blank/IC, zero 39Ar, or `1 + J F` not positive. | Fix that input. |
| Date filter surprises | It is relative to the newest analysis in the source, not today. | |
| Plot, Export or fits act on too many rows | With nothing selected they use all loaded rows (page of 200). | Select rows, or Load more first. |
| Figure exclusions vanish | Not stored; lost when the window closes. Selected `invalid` analyses are still used unless "Hide invalid". | |
| Export differs from `elctl export` | Figure windows and browser Export use constants preset `default`, no decay error. | Use `elctl export --constants` / `--decay-error`. |
| Preset warning on the status line | A bad value is dropped; a wrong `schema` key is an error. Saving under a factory name shadows it; Delete restores it. | |
| Config file fails to load | Loads are all or nothing and unknown keys are errors. A `*.local.toml` may override only transport host, port, baud and timeout (line file also data_bits, stop_bits, parity; spectrometer file also driver credentials). | Remove other keys. |
| `table 'x' not found (expected tables/x/current)` and other spectrometer validation errors | Field table, axis, detector column, acquirer channel or HV settings do not match. Details in the [configuration reference](07-configuration-reference.md). | Fix the config. |
| Run override of a plan parameter refused | Parameter not in `parameters.expose`. | Mark the run `advanced` or expose it. |
| Calibration invalid after editing a tray map | Stage calibration is tied to the tray map by SHA-256. | Recalibrate. |
| Camera `use = "view"` or `source = "recorded"` cannot drive follow-glow | Also, simulated and live camera/laser mixes are refused. | |
| Database password problems | The password lives in `<root>/.pychron/credentials.toml`; an invalid file stops startup. The `postgresql://` URL needs a user part. | |

## Interface quirks

- The theme is always light. Preferences are per user; out-of-range saved
  values silently fall back to defaults.
- An unsaved-queue-edits prompt can block Quit, or switching install in
  File > Installations.
- Canvas `[[image]]`, `[legend]`, `[colors] valve` and stage `fill`/`volume`
  are accepted but not drawn.
- `elctl open`, `close`, `state`, `scan` print `warning: transport X: ...` and
  carry on when a transport will not open.

## Still stuck

Collect `elctl doctor` output and the Log dock text, and see
[Escalation in the runbook](../installation_runbook.md#escalation).
