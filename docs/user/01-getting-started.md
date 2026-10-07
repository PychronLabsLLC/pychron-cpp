# Getting started

This page is for lab operators and lab managers. It explains what Pychron is,
what its main ideas are, how to start it, what every menu does, and walks you
through a 15-minute tour on the built-in simulator, so you can learn the
program without touching an instrument.

> **Status.** Pychron (the C++ rewrite) is in development. Everything runs
> against the built-in simulator. No instrument driver has been run against a
> real instrument yet; see "Hardware status" in the
> [README](../../README.md). Until your lab has done the bring-up in
> [the installation runbook](../installation_runbook.md), treat an instrument
> install as a simulation.

Where to go next:

| You want to | Read |
|---|---|
| Install Pychron on a lab computer | [Installation runbook](../installation_runbook.md) |
| Operate the valves, gauges and heaters | [02 Extraction line](02-extraction-line.md) |
| Operate the mass spectrometer | [03 Spectrometer](03-spectrometer.md) |
| Build and run queues of analyses | [04 Experiments](04-experiments.md) |
| Run a laser | [05 Laser](05-laser.md) |
| Browse, reduce and plot data | [06 Data analysis](06-data-analysis.md) |
| Edit configuration files | [07 Configuration reference](07-configuration-reference.md) |
| Use the command-line tool | [08 elctl reference](08-elctl-reference.md) |
| Write extraction scripts | [09 Scripting](09-scripting.md) |
| Fix something that is not working | [10 Troubleshooting](10-troubleshooting.md) |
| Look up a word | [11 Glossary](11-glossary.md) |

## 1. What Pychron is

Pychron is data acquisition and control software for noble-gas (Ar/Ar)
geochronology labs. It drives the extraction line (valves, gauges, pumps,
heaters), a mass spectrometer, and optionally a laser; it runs automated queues
of analyses; and it stores and reduces the results.

Two programs are installed:

- **pychron-ui** (called **Pychron** on macOS) is the application you work in.
  It has windows for the extraction line, the spectrometer, experiments, the
  laser and the data.
- **elctl** is a command-line tool for checking configuration, driving the
  line from a terminal, running queues without a screen, and bulk work such as
  importing legacy data. See [08 elctl reference](08-elctl-reference.md).

## 2. Key concepts

**Extraction line.** The vacuum plumbing between the sample and the
spectrometer: valves, gauges, pumps, heaters, a pipette and so on. Pychron
shows it as a drawing (the *canvas*) where each valve is a clickable symbol.
It is described by two files, `extraction_line.toml` (what the hardware is and
how it is wired) and `canvas.toml` (where things are drawn).

**Spectrometer.** The mass spectrometer: magnet, detectors and source. Its
configuration is a separate file (for example `spectrometer.toml`). Pychron
opens it in its own window (View > Spectrometer) with a live strip chart and
detector intensities.

**Run.** One analysis: extract gas from a sample, clean it up, measure it on
the spectrometer, pump out. A run has an identifier (a lab number such as
`66001`), an extraction (a script, with a power or temperature and a duration),
a measurement (a *plan* that says which isotopes are measured and how), and a
post-measurement script.

**Queue.** An ordered list of runs, with delays between them. A queue is a
file (for example `experiment.toml`). You open it in the Experiment window,
check it, and start it. The *executor* runs the rows in order and can be
stopped, cancelled or aborted.

**Analysis.** The saved result of a finished run: the measured signals,
their fits, and the data needed to compute an age. Analyses are what you
browse, recall and plot in the data browser.

**Store, DVC and records.** Results are kept in one of two places. The simple
one is the **records folder** (`<data>/records`), plain files written as a queue
runs. The fuller one is the **store** (called the *DVC store*), a database
(PostgreSQL on a server, or SQLite on one computer) holding analyses, samples,
irradiation packages and their history. The store is used by data-reduction
installs, by Entry, by `elctl export` and `elctl import`, and by the UI when
you pass `--db`. The store only exists if your Pychron was built with
persistence; the release packages are.

**Simulation mode.** Every hardware connection ("transport") can be replaced by
a simulated one, so the same program runs with no instrument: a simulated line
with gauges that pump down, a simulated spectrometer with a beam, a simulated
laser. Simulation is switched on by the `--sim` flag, or because an install was
set up "in simulation". A window running against simulators says
"(Simulation)" in its title. `--sim` will not load a spectrometer
config that is not fully simulated, so the flag cannot be used to dry-run
against real hardware by mistake.

**Installs and profiles.** An **install** is a named folder on this computer
that holds one set of settings and data: a line config, a canvas, a
spectrometer config, scripts, plans, a data folder, and optionally a database
URL. A **profile** is a template that creates an install by asking you a few
questions. The profiles that ship with Pychron are:

| Profile | Kind | What it sets up |
|---|---|---|
| `data-reduction` | data reduction | Browse, reduce and plot data from a database (a new local one, or your lab's PostgreSQL server). No instrument. |
| `argus` | instrument | Thermo Argus VI over Qtegra RemoteControlServer |
| `helix` | instrument | Thermo Helix MC over Qtegra RemoteControlServer (collector kinds are marked CONFIRM) |
| `ngx` | instrument | Isotopx NGX over its controller's ASCII protocol (not implemented as a driver yet; see the README) |

An instrument install starts in simulation by default ("Run in simulation
first?" defaults to yes). The list of installs on this computer is kept in a
per-user file, `site.toml` (see "Where things are kept" below). You can
switch between installs from File > Installations.

## 3. First launch

Start **Pychron** from Applications (macOS), the Start menu (Windows), or run
`pychron-ui` from a terminal.

What opens depends on what is installed on this computer:

1. **Nothing installed yet.** The **setup wizard** opens (section 10). When it
   finishes, tick "Open it now" to start working in the new install.
2. **One install, or a default install.** It opens directly.
3. **Several installs and no default.** The **Installations** dialog opens so
   you can choose one (section 10).
4. **Development shortcut.** With `--sim` and nothing installed, or with
   `--examples`, the example lab shipped with Pychron opens instead of the
   wizard.

While it loads, a splash screen shows each step ("Loading the extraction line:
...", "Bringing up the spectrometer: ...", "Loading the lab: ..."). Failures
appear as a message box and on stderr.

### What kind of window you get

- An **instrument install** opens the **main window** (the extraction line)
  with the install's line, canvas and spectrometer configs. If the install was
  set up for simulation it behaves as if `--sim` were given.
- A **data-reduction install** opens only the **data browser** on its
  database, with a smaller menu bar (File, Entry if there is a database,
  Window, Help). There are no extraction-line, spectrometer, experiment or
  laser windows.
- `--laser` opens only a **laser window** (for a laser PC).

### Where things are kept

| What | Where |
|---|---|
| Site config (the list of installs, the default) | macOS `~/Library/Application Support/Pychron/site.toml`; Windows `%APPDATA%\Pychron\site.toml`; Linux `$XDG_CONFIG_HOME/pychron/site.toml` or `~/.config/pychron/site.toml`. The environment variable `PYCHRON_SITE_CONFIG` overrides the path. |
| An install's files | the install's folder (shown in File > Installations) |
| Preferences, window sizes and positions | the per-user settings store (Qt `QSettings`, organization "PychronLabs", application "pychron-ui") |
| Figure presets | the user's application config folder under `presets`, plus lab presets in `<lab>/figures` |
| Records of finished runs | `<data folder>/records` |
| Log file | the `dir` set in `[logging]` in `extraction_line.toml` (`pychron.log`); no file is written if `dir` is not set |

## 4. Command-line flags of pychron-ui

```bash
pychron-ui [--install NAME | --setup | --examples | extraction_line.toml [canvas.toml]]
           [--sim] [--sim-speed X] [--spectrometer FILE] [--lab DIR] [--data DIR]
           [--queue FILE] [--db URL] [--laser [--device NAME]]
pychron-ui --version
pychron-ui --self-test
pychron-ui --write-icons DIR
```

On macOS the executable is inside the app bundle:
`/Applications/Pychron.app/Contents/MacOS/pychron-ui` (the exact path may vary
with the package).

| Flag | Meaning |
|---|---|
| `extraction_line.toml [canvas.toml]` | Positional: open this line config, and optionally this canvas, instead of an install. |
| `--install NAME` | Open the install of that name from the site config. An unknown name is a fatal error. |
| `--setup` | Run the setup wizard even if installs exist. |
| `--examples` | Open the example lab shipped with Pychron (for development and learning). |
| `--sim` | Force every extraction-line transport to be simulated. Also loads the example `spectrometer.sim-integrated.toml` if no spectrometer is given. A spectrometer config that is not fully simulated is refused. |
| `--sim-speed X` | With `--sim` only: run the whole application on simulated time `X` times faster than real time (`X` must be a positive number). |
| `--spectrometer FILE` | The spectrometer config to load for View > Spectrometer. |
| `--lab DIR` | The lab directory the Experiment window runs queues against (plans, scripts, conditionals, patterns, trays). Default: the install's folder, or the folder holding the line config. |
| `--data DIR` | Where records are written and browsed (`DIR/records`). Default: the install's data folder, or `<lab>/data`. |
| `--queue FILE` | A queue to open in the Experiment window when it is opened. |
| `--db URL` | Browse this store in the data browser instead of the records folder, and enable the Entry menu. `postgresql://user:password@host/db` or `sqlite:/path/to/file.db`. The schema must already be current; it is never changed from here. Needs a build with persistence. |
| `--laser` | Open only the laser window of one extraction device, for a laser PC. Takes no `--queue` or `--spectrometer`. |
| `--device NAME` | With `--laser`: which extraction device (a laser driver in the line config). Needed only when the line has more than one; with exactly one it is used automatically. |
| `--version` | Print `pychron-ui <version>` and exit. Needs no display. |
| `--self-test` | Check that the Qt platform, the shipped profiles, the setup wizard and the database plug-in work; prints `OK` or `FAIL` lines, exits 0 if all pass, 1 otherwise. |
| `--write-icons DIR` | Write the application icon files into `DIR` and exit (used for packaging). |

Rules enforced when the flags are read (a violation prints
`pychron-ui: <message>` and exits with status 2):

- `--sim-speed` needs `--sim`, and a positive number.
- `--device` needs `--laser`.
- `--laser` cannot be combined with `--queue` or `--spectrometer`.
- Positional config files, `--install`, `--setup` and `--examples` exclude each
  other.
- `--lab`, `--data`, `--spectrometer` and `--queue` each need a value, and the
  value must not start with `--`. `--db` and `--install` need a non-empty value.
- An unknown `--option` is an error.

Exit status: 0 on a normal exit, 1 on a fatal load error (a message box is also
shown), 2 on a command-line error or a `--db` that could not be opened.

## 5. A tour of the main window

The main window is the extraction line. Its title is
`pychron — <system name>` (the `name` in `[system]` of the line config).

```
+--------------------------------------------------------------+
| menu bar                                                     |
+--------------------------------------------+-----------------+
|                                            | Alarms          |
|   canvas: the drawing of the line          | Heaters (if any)|
|   (click a valve to open or close it)      | Cryo (if any)   |
|                                            |                 |
+--------------------------------------------+-----------------+
| Log dock                                                     |
+--------------------------------------------------------------+
| status bar: one health chip per transport                    |
+--------------------------------------------------------------+
```

### The canvas

The canvas draws the line from `canvas.toml`: valves, stages, pipettes, gauge
readouts and labels.

- **Click a valve** to open it if it is closed, or close it if it is open.
  Commands go through the same lock and interlock checks as `elctl open` and
  `elctl close`; a refused command appears in the Log dock as
  `ERROR [ui] <valve> rejected: ...` and the valve flashes the reason.
- **Right-click a valve** for *Lock valve* / *Unlock valve*. A locked valve
  refuses open and close commands until it is unlocked; unlocking asks for
  confirmation. Manual valves cannot be locked.
- **Hover a valve** to see its state and how long it has been in it, whether it
  is locked or owned, when it was last actuated, and how many times it has
  opened, closed or failed.
- Gauge labels show the latest pressure; a gauge drawn on the canvas but not
  defined in the config is shown as not wired.

See [02 Extraction line](02-extraction-line.md) for operating details.

### The docks

Docks can be dragged, floated or closed. The main window has these:

| Dock | Where | What it shows |
|---|---|---|
| **Log** | bottom | The program's log lines. Controls: a minimum level, a logger filter (prefix or glob), a "message contains" filter, **Pause**, **Autoscroll**, **Clear**, **Save...** (writes the visible lines to a text file) and **Set logger level...** (change a logger's level while running, for example `transport.*`, when the line has a log hub). A "+N new" badge counts lines that arrived while paused. On start it loads the tail of `pychron.log` if a log directory is configured. |
| **Alarms** | right | One row per alarm source with severity (warning or critical), message and time. **Acknowledge** removes the selected rows, **Acknowledge all** clears the list. A new alarm from the same source replaces its row. |
| **Heaters** | right | Only if the line config defines heaters. Per heater: an on/off button (it asks "Turn X on?"), a **Use PID** checkbox, a setpoint field (press Enter to send), the readback, and a strip chart. |
| **Cryo** | right | Only if the line has a cryo controller. Temperature readbacks per input, setpoint fields per loop with a **Set** button, and a chart in kelvin. |

At the bottom of the window the **status bar** shows one *health chip* per
transport (connection to a device or controller). A chip shows whether the
transport is connected and how long since it last answered (`12s`, `3m`,
`1h`); hover for its error count and last error.

## 6. Menus

The menu bar is the same in every window (on macOS it is the single bar at the
top of the screen). Some menus fill in only when a window that needs them is in
front. A menu with nothing to offer yet shows a greyed hint,
"Open View > Experiment to use this menu".

### File

| Item | What it does |
|---|---|
| **Installations…** | Opens the Installations dialog (section 10). Shown only when the window was started from an install or can switch installs. Choosing another install closes this window (asking about unsaved queue edits) and starts Pychron again on it. |
| **Preferences…** | Opens the Preferences dialog (section 9). On macOS it appears in the application menu. |
| **Quit** | Closes the window. In the main window this asks about unsaved queue edits. |

### Queue, Rows, Executor, Scripts

These four menus belong to the **Experiment window** and work only while it is
in front. They are covered in [04 Experiments](04-experiments.md); the items
are listed here so you know what to expect.

| Menu | Items |
|---|---|
| **Queue** | Open..., Save, Save As..., Revalidate |
| **Rows** | Move Up, Move Down, Duplicate, Delete, Toggle Skip, End After, Edit Extraction Script, Edit Post-Measurement Script, Set Conditionals... |
| **Executor** | Start, Stop, Cancel..., Abort..., Truncate, Send Test Notification (see [notifications](../notifications.md)) |
| **Scripts** | Script Editor..., Conditionals Editor... (from the Experiment window). In the script editor it holds New..., Save, Close Tab, Check Now, Go to Gosub; in the conditionals editor, Save. |

Stop ends the queue after the current run; Cancel and Abort ask for
confirmation and end the current run (Abort immediately).

### View

The View menu opens the program's windows. Each item has a glyph, and an item is
greyed out when that part is not available (for example no spectrometer config
was loaded, or the line failed to start).

| Item | Opens |
|---|---|
| **Extraction Line** | Brings the main window back to the front. |
| **Spectrometer** | The spectrometer window: a live strip chart, detector intensities, integration time, graph settings (scan width, linear or log scale, autoscale, min and max), and magnet controls. Titled "Spectrometer (Simulation)" when simulated. See [03 Spectrometer](03-spectrometer.md). |
| **Experiment** | The experiment window, with the `--queue` file loaded if one was given. |
| **Data** | The data browser: filter, recall, plot (time series, ideogram, age spectrum, inverse isochron) and export. See [06 Data analysis](06-data-analysis.md) and [export](../export.md). |
| **Laser** | The laser window of the first extraction device. With several lasers the item has a submenu, one entry per device. Greyed out when the line has no laser. See [05 Laser](05-laser.md). |

### Entry

Present only when Pychron has a database to enter into (started with `--db`, or
a data-reduction install with a database; needs a build with persistence).

| Item | Opens |
|---|---|
| **Samples…** | The samples window |
| **Packages…** | The packages (irradiations) window |
| **Import Samples…** | The sample import dialog |
| **Holders…** | The irradiation holders dialog |
| **Entry Settings…** | The entry settings dialog |

These are described in [Entry](../entry.md).

### Window

Standard window management: **Minimize** (Ctrl+M), **Zoom** (maximize or
restore), **Bring All to Front**, then a list of the open windows with a tick
beside the one in front. Pick a window from the list to bring it forward.

### Help

| Item | What it does |
|---|---|
| **Command Palette…** | Opens the command palette (next section). |
| **Keyboard Shortcuts** | Opens a searchable list of every shortcut. |
| **About pychron** | The about box (version and credits). |

## 7. The command palette

Press **Ctrl+Shift+P** (Cmd+Shift+P on macOS) in any window, or choose Help >
Command Palette…. Type part of a command's name and press Enter to run it.

- It lists every menu command that is currently visible and enabled for the
  window in front, labelled like `View › Spectrometer`, with its shortcut on
  the right. A command that is greyed out in the menu is not offered.
- Matching is fuzzy: the letters you type must appear in order, and letters at
  the start of words score higher ("exs" finds "Rows › Edit Extraction
  Script").
- Up and Down move the selection, Page Up and Page Down move ten rows, Enter
  runs the selection, Escape closes the palette. A click on a row also runs it.

## 8. Keyboard shortcuts

On macOS read Ctrl as Cmd. The in-program list (Help > Keyboard Shortcuts, F1
on Windows and Linux) shows exactly what applies on your platform and has a
filter box. "Window shortcuts work while that window is in front; the others
work in every window."

### Everywhere

| Command | Shortcut |
|---|---|
| Preferences… | the platform's standard (Cmd+, on macOS) |
| Quit | the platform's standard (Cmd+Q on macOS) |
| Extraction Line window | Ctrl+Shift+L |
| Spectrometer window | Ctrl+Shift+S |
| Experiment window | Ctrl+Shift+E |
| Data browser | Ctrl+Shift+D |
| Laser window | Ctrl+Shift+B |
| Keyboard Shortcuts (this list) | the platform's standard Help key (F1 on Windows and Linux) |
| Command palette | Ctrl+Shift+P |
| Minimize the window in front | Ctrl+M |

### Experiment window

| Command | Shortcut |
|---|---|
| Open queue… | Ctrl+O |
| Save queue | Ctrl+S |
| Move rows up | Ctrl+Up |
| Move rows down | Ctrl+Down |
| Duplicate rows | Ctrl+D |
| Delete rows | Delete |
| Toggle skip | Ctrl+K |
| End after this run | Ctrl+E |
| Add runs (run factory) | Ctrl+Return |
| Start the queue | F5 |
| Script editor | Ctrl+Shift+K |

### Script editor

| Command | Shortcut |
|---|---|
| New script… | Ctrl+N |
| Save script | Ctrl+S |
| Close tab | Ctrl+W |
| Check now | F7 |
| Go to the gosub under the cursor | F2 |

### Conditionals editor

| Command | Shortcut |
|---|---|
| Save conditionals | Ctrl+S |

### Data browser

| Command | Shortcut |
|---|---|
| Recall the next analysis | Ctrl+N |
| Recall the previous analysis | Ctrl+B |

## 9. Preferences

File > Preferences… opens a dialog with a page list on the left. The buttons
are **OK** (apply and close), **Cancel**, **Apply** (apply and stay open) and
**Restore Defaults** (reset the fields in the dialog; press OK or Apply to keep
them). Values are saved per user.

| Page | Setting | Range / default | Effect |
|---|---|---|---|
| Appearance | **Interface font size** | 6 to 32 pt; "Default" uses the system's size | Text size of every window, open now and opened later. Window titles change in windows opened afterwards. |
| Appearance | **Script editor font size** | 6 to 32 pt; "Default" follows the interface size | Font of the script editors. |
| Data | **Browser page size** | 20 to 5000 analyses; default 200 (steps of 50) | How many analyses the data browser loads at a time; **Load more** fetches the next page. |
| Spectrometer | **Ask before moving the magnet more than** | 0 to 300 amu, steps of 0.5; 0 shows "Never ask" | A magnet move bigger than this change of mass on the reference detector asks for confirmation. A move from an unknown position always asks. Kept separately for each spectrometer. The page appears only when a spectrometer is loaded. |

A saved value that is missing, unreadable or out of range silently reads as its
default. The interface is always shown in the light theme; there is no dark
mode setting.

## 10. The setup wizard and the Installations dialog

### Setup wizard

It opens on first launch, with `pychron-ui --setup`, and from **New…** in the
Installations dialog. (`elctl init` does the same from a terminal; see
[08 elctl reference](08-elctl-reference.md).)

1. **Welcome.** "Choose what to set up on this computer." Profiles are listed
   under *Reduce and plot data* and *Run an instrument*. Pick one.
2. **Questions.** A page per profile asks its questions, grouped (for example
   Detectors, Instrument connection, Extraction line, Data). Some questions
   appear only if an earlier answer needs them. Table questions (such as the
   list of detectors) have **Add row** and **Remove row**; path questions have
   **Browse…**; a database question has a **Test connection** button. For an
   instrument, "Run in simulation first (no instrument needed)?" defaults to
   yes. For the extraction line you can start with the example line, copy line
   files you already have (they are checked first), or convert a legacy pychron
   `setupfiles` folder.
3. **Install location.** A **Name** for the install and the **Folder** where it
   keeps its settings and data (use **Browse…**). A message tells you if the
   folder already holds an install ("its settings are filled in") or is not
   empty ("files already there are kept as they are").
4. **Ready to install.** A summary of your answers and the files that will be
   written, updated, kept, or (where you have edited a file) put beside yours
   as `.new`. For a legacy conversion it also lists what could not be carried
   over. Press **Install**.
5. **Done.** A table of checks (the same as `elctl doctor`) with the install
   marked OK, with a warning, or failed, and a list of placeholder files to
   replace with your instrument's real values before measuring. Tick **Open it
   now** to start working in it.

To change an install's answers later, run the wizard on the same folder, or use
`elctl init --reconfigure`. Files you have edited are kept.

### Installations dialog

File > Installations… (or the choice at start when there are several installs
and no default). It lists each install with its profile and folder, tagged
"(opens by default)" and "(open now)".

| Button | What it does |
|---|---|
| **Open** | Switch to the selected install (not available for the one already open). |
| **Open by default** | Make it the install that opens when Pychron starts without `--install`. |
| **Remove from list** | Forget the install. Its folder and files are kept. Not available for the one that is open. |
| **New…** | Run the setup wizard. |
| **Close** | Dismiss the dialog. |

Double-clicking an install opens it. Run `elctl doctor` (or `elctl doctor
--install NAME`) when an install will not open; the error message points to it.

## 11. A 15-minute tour on the simulator

This uses the example lab in `configs/examples`. Nothing here needs hardware.
The example line is five valves (A, B, C, P1, P2), a manual valve (M1), a
pump power switch, two gauges (IG1, PG1) and a pipette on simulated
transports. The example queue has three runs: a blank (`bu`) and two
unknowns (`66001`) of the sample FC-2.

You need a built `pychron-ui` (see [dev_setup.md](../dev_setup.md); the release
installer is not needed). From the repository root:

```bash
mkdir -p ~/pychron-sim-data
build/dev-ui/apps/pychron-ui/pychron-ui --examples --sim --sim-speed 50 \
    --data ~/pychron-sim-data \
    --queue configs/examples/experiment.toml
```

`--data` keeps the records out of the repository. `--sim-speed 50` makes
simulated time run 50 times faster than the clock on the wall.

**Minutes 0 to 3: the line.**

1. The main window opens, titled `pychron — example`, with "(Simulation)" shown
   on the splash screen. Look at the status bar: one health chip per
   simulated transport (`valve_bus`, `gauge_net`, `laser_pc`).
2. Click a valve, for example **P1**. It changes state (or is refused if an
   interlock applies; the reason appears in the Log dock). Click it again.
3. Right-click a valve and choose **Lock valve**, then try clicking it; the
   command is refused. Right-click again and **Unlock valve** (confirm the
   question).
4. Hover a valve to read its history. In the Log dock, type `transport` in
   the logger filter, then clear it.

**Minutes 3 to 6: the spectrometer.**

5. Press **Ctrl+Shift+S** (or View > Spectrometer). The strip chart scrolls
   with simulated detector intensities. Change **Scale** to log, tick
   **Autoscale Y**, change **Scan Width (mins)**. In **Magnet**, pick a
   detector and an isotope and press **Apply**; a large move asks for
   confirmation (File > Preferences… > Spectrometer sets the threshold).

**Minutes 6 to 11: run the example queue.**

6. Press **Ctrl+Shift+E** (or View > Experiment). The three rows of
   `experiment.toml` are loaded. Use **Queue > Revalidate** to re-check them
   against the lab's plans and scripts.
7. Try **Rows > Toggle Skip** (Ctrl+K) on the first row, then toggle it back.
8. Start the queue: **Executor > Start** (F5). Watch the rows change state, the
   timeline, and the isotope evolutions as each run measures. At 50 times speed
   the queue finishes in a few minutes of wall time.
9. Back in the main window the valves open and close as the scripts run, and the
   Log dock records it.

**Minutes 11 to 15: look at the data.**

10. When the queue ends, press **Ctrl+Shift+D** (or View > Data). Press
    **Rescan** if the list is empty. Double-click an analysis to recall it, or
    select the two `66001` runs and choose **Plot** to try a time series.
11. Try the palette: **Ctrl+Shift+P**, type `shortcuts`, Enter.
12. Open **File > Preferences…**, raise the interface font size, press Apply,
    then Restore Defaults and Apply.
13. Quit with the platform's Quit shortcut. If a queue has unsaved edits you
    are asked first.

To do the same without the window, from the example folder:

```bash
cd configs/examples
../../build/dev/apps/elctl/elctl -c extraction_line.toml --sim exp run experiment.toml \
    --spectrometer spectrometer.sim-integrated.toml --sim-speed 50
```

See [08 elctl reference](08-elctl-reference.md#exp-run).

If you want to browse these records with the command-line tool or the store, see
[export](../export.md) and [legacy import](../legacy_import.md).

## 12. Not yet implemented / unverified

- Everything above was checked against the source, but the program was not run
  while this page was written. If a label differs on screen, the label wins; please
  report it.
- No driver has been run against a real instrument ([README](../../README.md)).
  The `ngx` profile can be installed, but the NGX driver is not implemented.
- The README says the experiment window is "Window > Experiment"; the menu that
  actually holds it is **View > Experiment**.
- The exact standard shortcuts for Preferences, Quit and Keyboard Shortcuts on
  Windows and Linux come from Qt's platform defaults and were not individually
  verified; use Help > Keyboard Shortcuts to see them.
- Which window title and menus a data-reduction install shows beyond File,
  Entry, Window and Help was derived from reading the code, not from running it.
- Installing from a release package, signing, and the "damaged" macOS message
  are covered in the [installation runbook](../installation_runbook.md).
