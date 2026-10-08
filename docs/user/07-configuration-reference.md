# Configuration reference

Every file you can edit to describe your instrument, your lab and your data:
where it lives, how the program finds it, and every key it reads. All of them
are TOML (a plain-text format: `key = value`, `[table]` headings, `[[list]]`
headings, `#` comments) except the tray maps. The keys below were taken from
the code that reads the files, so a key that is not listed here is not read.

Files you rarely write by hand are made for you by `elctl init` or the setup
wizard ([01 Getting started](01-getting-started.md)). The chapters that use
them are [02 Extraction line](02-extraction-line.md),
[03 Spectrometer](03-spectrometer.md), [04 Experiments](04-experiments.md),
[05 Laser](05-laser.md), [06 Data analysis](06-data-analysis.md) and
[09 Scripting](09-scripting.md). When a file does not load, read
[10 Troubleshooting](10-troubleshooting.md).

## Contents

1. [How the files are found](#how-the-files-are-found)
2. [Checking a file](#checking-a-file)
3. [Notation used in the tables](#notation-used-in-the-tables)
4. [extraction_line.toml](#extraction_linetoml)
5. [canvas.toml](#canvastoml)
6. [spectrometer.toml and its folder](#spectrometertoml-and-its-folder)
7. [Lab folder files](#lab-folder-files)
8. [Install files: site.toml, profiles, credentials](#install-files-sitetoml-profiles-credentials)
9. [Unverified or not yet implemented](#unverified-or-not-yet-implemented)

## How the files are found

There is no global search path. Each program is told where its files are, in
one of three ways, and every other file is found relative to those.

1. **An install** (what the setup wizard makes). `pychron-ui --install
   <name>` or `elctl --install <name>` reads the per-user **site file**
   (below) to find that install's root folder and the names of its line,
   canvas and spectrometer files. With no name, the site file's `default`
   install is used, or the only install if there is one.
2. **Named on the command line.** `pychron-ui <extraction_line.toml>
   [canvas.toml]`, `--spectrometer <file>`, `--lab <dir>`, `--data <dir>`;
   `elctl -c <extraction_line.toml>` and the same `--lab`, `--spectrometer`
   and `--data` on `exp`.
3. **The shipped examples**. `pychron-ui --examples` opens the example line
   and canvas that come with the program (`configs/examples`), all
   simulated. (`pychron-ui` with no arguments at all picks an install from
   the site file instead, and offers the setup wizard when it has none.)

From those:

| Thing | Where it is |
|---|---|
| Extraction line | the install's `line` file, else the first file argument |
| Canvas | the install's `canvas` file, else the second file argument |
| Spectrometer config | the install's `spectrometer` file, else `--spectrometer`. `--sim` with none uses the shipped `spectrometer.sim-integrated.toml` |
| **Spectrometer data folder** | the folder that holds the spectrometer config: it must contain `tables/<name>/` (field tables) and may contain `molecular_weights.toml` |
| **Lab folder** | `--lab <dir>`, else the install's root, else the folder of the extraction line file. Holds plans, scripts, conditionals, defaults, blocks and the rest of [Lab folder files](#lab-folder-files) |
| Data folder | `--data <dir>`, else the install's `data`, else `<lab>/data`. The acquisition records go in `<data>/records` |
| Figure presets | yours: `presets/` under the program's per-user configuration folder; the lab's: `<lab>/figures/` |

### Machine-local overrides

A config that is shared between computers (kept in a git repository, say)
can say the wrong thing for one machine: a COM port, an address, a password.
For both `extraction_line.toml` and the spectrometer config, a sibling file
named like it with `.local` before the extension is read automatically when
it exists and wins for the keys it is allowed to set:

| Main file | Local file |
|---|---|
| `extraction_line.toml` | `extraction_line.local.toml` |
| `spectrometer.toml` | `spectrometer.local.toml` |
| `spectrometer.qtegra.toml` | `spectrometer.qtegra.local.toml` |

| Which config | May override, on an existing `[transports.<name>]` | May also override |
|---|---|---|
| extraction line | `port`, `host`, `baud`, `data_bits`, `stop_bits`, `parity` | `[logging]` and `[metrics]`, any of their keys |
| spectrometer | `host`, `port`, `baud`, `timeout_ms` | on an existing `[drivers.<name>]`: `user`, `password`, `password_env` |

`[logging]` and `[metrics]` in the extraction line's local file are this
computer's logging and metrics endpoint. A key given there wins and the rest
come from the main file; `[logging.levels]` there is the whole list of
levels, not an addition to the main file's. File > Preferences writes these
two tables (its Logging and Metrics pages) and leaves the rest of the file,
comments included, as it is.

Anything else in a local file, a name that is not in the main file, or a key
that is not allowed, is an error naming the line. A local file must not hold
a secret you would not want in a backup: the setup wizard writes it
owner-readable only.

### The site file

The per-user list of installs on a computer. Location: `$PYCHRON_SITE_CONFIG`
if set, else `~/Library/Application Support/Pychron/site.toml` (macOS),
`%APPDATA%\Pychron\site.toml` (Windows), `$XDG_CONFIG_HOME/pychron/site.toml`
or `~/.config/pychron/site.toml` (elsewhere). A missing file is an empty
list. The wizard writes it; `elctl doctor` checks it.

| Key | Type | Meaning |
|---|---|---|
| `default` | text | name of the install the programs open when none is named |
| `[[installs]]` `name` | text | the install's name |
| `kind` | `instrument` or `data_reduction` | |
| `profile` | text | the setup profile it was made from |
| `root` | path | its folder |
| `line`, `canvas`, `spectrometer` | path relative to `root` | conventional: `extraction_line.toml`, `canvas.toml`, `spectrometer.toml` |
| `data` | path relative to `root` | the data folder (`data`) |
| `simulation` | true/false | run as `--sim` |
| `database` | text | data reduction: the database URL **without a password** |

The password for a `postgresql://` database is in
`<root>/.pychron/credentials.toml` (see
[Install files](#install-files-sitetoml-profiles-credentials)).

## Checking a file

Always check before you run.

```bash
elctl validate extraction_line.toml
elctl canvas-check canvas.toml
elctl list-drivers
elctl conditionals-check conditionals/system.toml --spectrometer spectrometer.toml
elctl exp validate experiment.toml --lab . --spectrometer spectrometer.toml
elctl doctor            # a whole install; add --probe to reach the instrument
elctl --install argus-lab doctor --strict
```

Every problem is reported at once with `file:line`, and a config loads **all
or nothing**: one problem and the program uses none of it. Unknown keys are
errors in nearly every file (they usually mean a typo); the exceptions are
named below.

`elctl list-drivers` prints every driver kind with the extra keys that
kind reads in `[drivers.<name>]`: that list is the reference for
driver-specific keys, so this page does not copy it.

## Notation used in the tables

*Required* means the file is refused without it. A *default* is what is used
when the key is absent. *Seconds*, *ms*, *mm* and so on are the unit of the
number. "name" values are the names other files refer to, so keep them
short and without spaces.

## extraction_line.toml

The valves, switches, gauges, heaters, pumps and the controllers that drive
them. Example: `configs/examples/extraction_line.toml`. Sections (anything
else at top level is an error): `[system]`, `[logging]`, `[transports.*]`,
`[drivers.*]`, `[[valves]]`, `[[manual_valves]]`, `[[switches]]`,
`[[gauges]]`, `[[heaters]]`, `[[pipettes]]`, `[cryo]`, `[aliases]`, `[sim]`,
`[metrics]`.

### [system] (required)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | text | required | the line's name |
| `scan_interval_ms` | whole number >= 1 | 1000 | how often gauges and heaters are read |

### [logging] (optional)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `dir` | path | none | folder for log files; with none there is no file log. `~` is your home folder |
| `max_size_mb` | whole number >= 1 | 10 | size at which a log file rolls over |
| `max_files` | whole number >= 1 | 5 | log files kept |
| `default_level` | `trace`, `debug`, `info`, `warn`, `error` | `info` | |
| `echo_stderr` | true/false | false | also print to the terminal |
| `[logging.levels]` | text = level | none | per-logger overrides by name pattern: `"*.wire" = "trace"` shows every transport's bytes; each transport logs to `<name>.wire`. `"scheduler"` and `"extraction_line"` are the line's own loggers |

### [metrics] (optional)

The numbers the lab's monitoring box collects. Off unless enabled; the guide
is [observability](../observability.md).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `enabled` | true/false | false | publish metrics at `http://<this computer>:<port>/metrics` |
| `bind` | IP address | `"0.0.0.0"` | which of the computer's addresses to listen on; `"127.0.0.1"` is this computer only |
| `port` | whole number 1 to 65535 | 9464 | |

### [transports.\<name\>]

How the program talks to a device. Every transport has:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `kind` | `serial`, `tcp`, `udp`, `modbus_rtu`, `modbus_tcp`, `sim`, `link` | required | |
| `timeout_ms` | whole number >= 1 | 500 | how long to wait for a reply |
| `retries` | whole number >= 0 | 0 | extra attempts after a failure |
| `trace` | true/false | false | record the bytes. Do not use it while a password is configured: the login is recorded too |

Plus, by kind:

| Kind | Keys |
|---|---|
| `serial`, `modbus_rtu` | `port` (required; `/dev/tty.usbserial-A1` or `COM4`), `baud` (9600), `data_bits` (5 to 8; 8), `stop_bits` (1 or 2; 1), `parity` (`none`, `even`, `odd`; `none`) |
| `tcp`, `udp` | `host` (required), `port` (required, 1 to 65535) |
| `modbus_tcp` | `host` (required), `port` (default 502) |
| `sim` | nothing: a simulated device (a drivers' own simulator answers) |
| `link` | `link` (required): the name of a connection another config owns, so one socket is shared (an NGX spectrometer and its valves) |

### [drivers.\<name\>]

A device. The name is what valves, gauges, heaters and (for lasers) a queue's
`extract_device` refer to.

| Key | Type | Meaning |
|---|---|---|
| `kind` | text, required | the driver. Built in: `sim_valves`, `pychron_valves`, `proxr_relay`, `plc2000_valves`, `plc2000_gauges`, `plc2000_heater`, `pfeiffer_maxigauge`, `varian_xgs600`, `gp_microion`, `agilent_switch`, `lakeshore`, `chromium` (a CO2 laser, an extraction device), `thermo_qtegra`, `qtegra_valves`, `qtegra_gauges`, `isotopx_ngx`, `ngx_valves`, `dac_positioner`, `adc_bank`, `pulse_counter`, `serial_hv` (the last four are the legacy split spectrometer; simulated twins are named `sim_dac_positioner`, `sim_adc_bank`, `sim_pulse_counter`, `sim_hv_supply`) |
| `transport` | text, required | a `[transports.*]` name |
| `channels` | list of whole numbers | the gauge or relay channels the device has. A gauge's `channel` must be among them when this is given |
| anything else | | read by the driver; see `elctl list-drivers`. An undeclared key, a wrong type or a missing required key is reported with the driver's name |

### [[valves]]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | text | required, unique | |
| `description` | text | empty | |
| `actuator` | text | required | a `[drivers.*]` name |
| `address` | text (a bare whole number is also accepted) | required | the channel on that actuator. Two valves cannot share an actuator and address |
| `interlocks` | list of valve names | none | this valve cannot open while any of these is open |
| `positive_interlocks` | list of valve names | none | every one of these must be open before this one opens. A valve cannot be both; a cycle is an error |
| `settle_ms` | whole number >= 0 | 0 | wait after changing state |
| `inverted` | true/false | false | the channel is wired backwards: opening drives it the other way and the read-back is reversed. The recorded state is always the valve's |
| `verify` | true/false | true | read the state back. `false`: no read-back; the commanded state is recorded |
| `state_source` | table `{ driver, address, inverted }` | none | read the state from another driver (a second controller). Not allowed with `verify = false` |

A valve cannot interlock with itself and may only name real valves.

### [[manual_valves]]

Hand valves the software cannot move but draws and tracks (clicked in the
window).

| Key | Type | Meaning |
|---|---|---|
| `name` | text, required | |
| `description` | text | |

### [[switches]]

An on/off thing that is not a gas valve (pump power, a relay, a shutter).
Same keys as a valve minus `interlocks` and `positive_interlocks`: `name`,
`description`, `actuator`, `address`, `settle_ms`, `inverted`,
`state_source`, `verify`. Shares the actuator address space with valves.

### [[gauges]]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | text | required, unique | |
| `driver` | text | required | a `[drivers.*]` name |
| `channel` | whole number >= 0 | 1 | the gauge's channel |
| `units` | `torr`, `mbar`, `pa` | `torr` | |
| `alarm_high` | number | none | alarm above this pressure |
| `alarm_low` | number | none | alarm below this; must be less than `alarm_high` |

### [[heaters]]

| Key | Type | Meaning |
|---|---|---|
| `name` | text, required, unique | |
| `driver` | text, required | a `[drivers.*]` that heats |
| `description` | text | |
| `units` | text | only shown (the controller decides: `C`) |

### [[pipettes]]

| Key | Type | Meaning |
|---|---|---|
| `name` | text, required, unique | |
| `inner`, `outer` | valve names, required, different | the two valves of the pipette |

### [cryo] (optional)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `driver` | text, required | | a temperature controller (a Lake Shore) |
| `tolerance_k` | number > 0 | 1.0 | band, in kelvin, that counts as at setpoint |
| `timeout_s` | number > 0 | 600 | a blocking set that has not arrived by then fails |
| `[cryo.setpoints]` | name = 1 to 4 kelvin values, none negative | none | named setpoints: value *n* goes to output *n*, which waits on input *n*. `He_freeze = [14.0, 0.0]` |

### [aliases] (optional)

Lab-specific names that measurement plans use as `@key`, so one shipped plan
works on every line. Nested tables become dotted keys; values are text,
numbers or true/false.

```toml
[aliases]
valves.inlet = "B"
valves.outlet = "C"
extraction.eqtime = 20
```

A key under `valves.` must be the name of a valve or manual valve.

### [sim] (optional)

For a line with simulated transports.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `file` | path, relative to this file | `sim.toml` beside this file, if there is one | the numbers of the simulated lab: sizes, pressures, conductances, pump speeds, the gas in a tank, the ion source, detector baselines. Its keys are in [The simulated lab](../simulator.md#simtoml). A file named here and not found is an error |

### What the check also enforces

Unknown transport or driver names; a duplicate valve, gauge, heater or
pipette name; an address used twice on one actuator; an interlock that names
a valve that does not exist; a gauge channel the driver does not declare;
`alarm_low` not below `alarm_high`; a pipette with the same inner and
outer valve or with a valve that does not exist. Cross-reference checks only
run once the file is otherwise clean, so you may see a second round of
messages after fixing the first.

## canvas.toml

The drawing of the line: where things are and how they connect. Presentation
only; states and readings come from the running line. Example:
`configs/examples/canvas.toml`. Check it against the line with
`elctl canvas-check canvas.toml`: a valve, manual valve, switch or pipette
drawn here must exist in `extraction_line.toml` (an error), a gauge may be
drawn that the line does not define (it shows no reading), and a line valve
that is not drawn is a warning.

Positions and sizes are `[x, y]` and `[width, height]` in pixels; sizes must
be positive. Top-level sections: `[canvas]`, `[colors]`, `[[valve]]`,
`[[manual_valve]]`, `[[rough_valve]]`, `[[switch]]`, `[[gauge]]`, `[[stage]]`,
`[[pipette]]`, `[[connection]]`, `[[elbow]]`, `[[tee]]`, `[[cross]]`,
`[[label]]`, `[[image]]`, `[legend]`.

| Section | Key | Type | Default | Meaning |
|---|---|---|---|---|
| `[canvas]` | `origin` | `[x, y]` | `[0, 0]` | |
| | `size` | `[w, h]` | `[1000, 700]` | |
| | `connection_width` | whole number >= 1 | 5 | pipe thickness |
| | `open_valve_color` | `green`, `inherit` | `green` | `inherit`: an open valve takes the colour of the region it joins |
| `[colors]` | any name = `"#rrggbb"` | text | | e.g. `valve`, `pipette` |
| `[[valve]]`, `[[manual_valve]]`, `[[rough_valve]]`, `[[switch]]` | `name` | text, required | | must match the line |
| | `pos` | `[x, y]`, required | | |
| | `display_name` | text | the name (blank on a manual valve) | the label on its face |
| `[[gauge]]` | `name`, `pos` | required | | |
| `[[stage]]` | `name`, `pos` | required | | a box: a volume, pump, tank, spectrometer |
| | `size` | `[w, h]` | `[50, 50]` | |
| | `volume` | number | none | cc: its size in the simulated line (50 cc with none) |
| | `fill` | true/false | false | |
| | `display_name` | text | the name | `""` for no label |
| | `use_symbol`, `symbol` | true/false; `spectrometer`, `quadrupole`, `laser`, `turbo`, `getter`, `ion_pump` | false | a glyph in the box |
| | `kind` | `volume`, `pump`, `pipette`, `laser`, `tank`, `spectrometer`, `getter` | from the symbol | what it is to the gas |
| | `precedence` | whole number >= 0 | the kind's | which source wins a shared region. 0 colours nothing |
| | `color` | `"#rrggbb"` | the theme's | |
| | `tank` | text | | for a pipette: the tank whose gas it holds |
| `[[pipette]]` | `name`, `pos` | required | | the name must be a pipette in the line |
| | `size`, `vlabel`, `display_name`, `precedence`, `color`, `tank` | | | as a stage; `vlabel` is the text beside it |
| `[[connection]]` | `start`, `end` | text, required | | names of drawn things |
| | `orientation` | `auto`, `h`, `v` | `auto` | |
| | `start_offset`, `end_offset` | `[x, y]` | `[0, 0]` | pixels from each element's centre |
| `[[elbow]]` | `start`, `end` | required | | |
| | `corner` | `ul`, `ur`, `ll`, `lr` | `ul` | |
| | `start_offset`, `end_offset` | | | |
| `[[tee]]` | `left`, `right`, `mid` | required | | all three joined with no valve between |
| `[[cross]]` | `left`, `right`, `top`, `bottom` | required | | all four joined |
| `[[label]]` | `text`, `pos` | required | | |
| | `font` | text | | `"Arial 12"` |
| `[[image]]` | `path`, `pos` | required | | a picture file |
| `[legend]` | `pos` | `[x, y]`, required | | where the colour key goes |

Gas colours: a region takes the colour of the connected source with the
highest precedence: a pump (120), then a tank (110), a pipette or laser
(100), a spectrometer (80), a getter (70). Each tank gets its own colour
and a pipette wears the colour of the tank across its valve unless `tank`
says otherwise.

## spectrometer.toml and its folder

The mass spectrometer: how to talk to it, where the magnet is positioned,
the detectors, and the field tables. Examples:
`configs/examples/spectrometer.qtegra.toml` (one Thermo box),
`spectrometer.ngx.toml` (Isotopx), `spectrometer.sim-integrated.toml` and
`spectrometer.sim-legacy.toml` (simulated). Check with
`elctl exp validate` or open it in `pychron-ui`.

```
spectrometer/                      (the folder holding the config)
  spectrometer.toml
  spectrometer.local.toml          optional, this computer only
  molecular_weights.toml           optional
  tables/<name>/<timestamp>.toml   field table versions
  tables/<name>/current            names the version in use
```

Sections (anything else is an error): `[system]`, `[transports.*]`,
`[drivers.*]`, `[magnet]`, `[source]`, `[acquisition]`, `[detector_control]`,
`[[detectors]]`. All but `[detector_control]` are required, and at least one
detector.

### [system]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | text | required | |
| `reference_detector` | detector name | required | the detector the magnet is positioned for |
| `integration_time_s` | number > 0 | 1.0 | |

### [transports.\<name\>]

Like the line's, with these differences: kinds are `tcp`, `udp`, `serial`,
`modbus_tcp`, `modbus_rtu`, `labjack_u3`, `sim`, `link`; `timeout_ms` defaults
to 1000; `baud` defaults to 9600; `retries` 0; `trace` false (records the wire
to `<trace dir>/<name>.trace`). `host` is required for `tcp`, `udp` and
`modbus_tcp`; `port` is a whole number for network kinds and a device path
for serial kinds (and required for serial); `link` (required, only for kind
`link`) names the shared connection.

### [drivers.\<name\>]

| Key | Type | Meaning |
|---|---|---|
| `kind` | text, required | `thermo_qtegra`, `isotopx_ngx`, `dac_positioner`, `adc_bank`, `pulse_counter`, `serial_hv`, their `sim_*` twins, ... see `elctl list-drivers` |
| `transport` | text, required | a `[transports.*]` name in this file |
| `roles` | list, required, not empty | which of `positioner`, `source`, `acquirer`, `detector_control`, `beam_blank` it plays. An integrated vendor box plays several; a legacy split uses one driver per role |
| `channels` | list of text | an acquirer must list its channel names (`["H2", "H1", "AX"]`) |
| anything else | | read by the driver (`limit_min`, `limit_max`, `sample_hz`, `link`, `user`, ...) |

### [magnet]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `positioner` | driver name | required | a driver with role `positioner` |
| `native_axis` | `dac`, `field`, `mass` | required | what the positioner accepts. The field table's `axis` must match |
| `limits` | `{ min, max }`, min < max | none | travel limits in native units, enforced on top of the driver's own: the stricter side wins |
| `settle_ms` | whole number >= 0 | 0 | wait after a move |
| `field_table` | text | required | a name under `tables/` |
| `hv_table` | text | none | a second table, for HV correction |
| `propagate` | true/false | false | when a peak center updates the table, update the other detectors' columns too |
| `corrections` | `{ deflection, hv }` true/false | both false | apply detector-deflection and HV corrections. `hv = true` needs `source.nominal_hv` and cannot be used with `native_axis = "mass"` |
| `protection` | `{ detectors = [...], beam_blank_threshold }` | | detectors to protect during a move (each must have its own `protection` table). `beam_blank_threshold` > 0 needs a driver with role `beam_blank` |
| `af_demag` | `{ enabled, period_s, duration_s, start_amplitude, threshold }` | disabled; 0.5, 10, 0.5, 0.5 | alternating-field demagnetising before a move. `period_s` and `duration_s` > 0 |

### [source]

| Key | Type | Meaning |
|---|---|---|
| `driver` | driver name, required | role `source` |
| `nominal_hv` | number > 0 | the HV the tables were made at; required with HV correction |
| `ramp` | table of source parameter = rate per second (> 0) | allowed names: `HV`, `TrapCurrent`, `TrapVoltage`, `Emission`, `ElectronEnergy`, `IonRepeller`, `ExtractionLens`, `ExtractionFocus`, `ExtractionSymmetry`, `YSymmetry`, `ZSymmetry`, `ZFocus`, `HorizontalSymmetry`, `Flatapole`, `RotationQuad`, `PoleN`, `PoleS`, `ESAPlus`, `ESAMinus` |

### [acquisition]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `acquirers` | list of driver names, required, not empty | | each with role `acquirer` |
| `stale_frame_guard` | true/false | true | discard a repeated frame |
| `timeout_factor` | number >= 1 | 2.0 | how many integration times to wait for a frame |
| `host_integration` | true/false | false | the computer merges and integrates the frames. **Required** when there is more than one acquirer |
| `bin_epoch` | `shared`, `per_acquirer` | `shared` | |
| `ignored_channels` | list of `"driver:channel"` | none | acquirer channels that are not detectors. Every acquirer channel must be bound to a detector or listed here |

### [detector_control] (optional)

`driver` (required): the driver with role `detector_control`. Needed for
any detector's `deflection.control` or `protection`.

### [[detectors]]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | text | required, unique | |
| `kind` | `faraday`, `counter`, `cdd`, `atona` | required | |
| `channel` | `"<acquirer driver>:<channel>"` | required | each channel is bound to at most one detector |
| `units` | text | `fA` (`cps` for counter and cdd) | |
| `software_gain` | number > 0 | 1.0 | |
| `isotope` | text | none | the isotope normally on it |
| `color` | `"#rrggbb"` | none | |
| `active` | true/false | true | an inactive detector needs no column in the field table |
| `deflection` | `{ control, correction = [c0, c1, ...], sign = 1 or -1, max, per_volt }` | | deflection with a polynomial correction (lowest order first); `max` > 0 |
| `protection` | `{ threshold > 0, on_move }` | | protect against a beam over this intensity; `on_move` also while the magnet moves |
| `saturation` | number > 0 | none | |
| `dead_time_ns` | number >= 0 | none | |
| `cdd_voltage` | number | none | |

### Field tables

`tables/<name>/<timestamp>.toml`, with `tables/<name>/current` a one-line
file holding the file name to use (`2026-09-30T000000.toml`). The program
writes a new version when a peak center updates the table, and moves
`current`; old versions stay.

| Key | Type | Meaning |
|---|---|---|
| `fit` | `discrete`, `linear`, `quadratic`, `cubic` | how positions between listed isotopes are found |
| `axis` | `dac`, `field`, `mass` | the unit of the values; must equal the positioner's `native_axis` |
| `[[points]]` `isotope` | text, required | |
| `mass` | number, required | in amu |
| any other key | number | the magnet value putting this isotope on that detector (`H1 = 5.001`). Every active detector needs a value in every point |

### molecular_weights.toml

Optional. Flat `name = mass` in amu, e.g. `Ar40 = 39.9623831237`; the file
**adds to or overrides** the built-in table (the helium, neon, argon,
krypton and xenon isotopes). A value must be a number above zero.

## Lab folder files

The lab folder is where everything that is about the lab, not the hardware,
lives. `elctl exp` and `pychron-ui` load it without needing the hardware;
a file that does not load is reported with the others by `elctl exp
validate`, and stops only what depends on it.

```
<lab>/
  plans/*.toml              measurement plans
  scripts/extraction/  post_equilibration/  post_measurement/  measurement_hooks/    (.py files)
  conditionals/*.toml       conditional sets (system.toml is always applied)
  identifiers.toml          what an identifier means (optional)
  peak_center.toml          named peak-center settings (optional)
  defaults.toml             what a new run starts with (optional)
  blocks/*.toml             reusable run sequences (optional)
  notifications.toml        email, webhook, command messages (optional)
  tray_maps/*.txt           sample trays (optional)
  patterns/*.toml           laser patterns (optional)
  cameras.toml              the camera of each laser (optional)
  stage_calibrations/       written by `elctl laser calibrate`
  stage_corrections/        written as runs centre holes
  camera_scales/            written by `elctl laser camera-scale`
  figures/<kind>/*.toml     shared figure presets (optional)
  data/                     records written by runs
```

### Queue files (experiment.toml)

A queue is a list of runs. Any file name works; `experiment.toml` is the
convention and what the setup wizard writes (`elctl exp run experiment.toml`).
Example: `configs/examples/experiment.toml`, `experiment.laser.toml`. Only
`queue` and `runs` exist at top level; an unknown key anywhere is an error.

`[queue]`:

| Key | Type | Meaning |
|---|---|---|
| `schema_version` | whole number | leave out or `1` |
| `name` | text | |
| `mass_spectrometer` | text | the instrument |
| `extract_device` | text | the default extraction device: a laser driver's name. With a laser in the line it must be one of its extraction devices |
| `tray` | text | a tray map name (see below) |
| `load` | text | |
| `username`, `email` | text | who ran it; the email can get a notification |
| `queue_conditionals` | text | a conditional set (a file in `conditionals/`) for the whole queue |
| `repository` | text | |
| `[queue.delays]` `before_analyses`, `between_analyses`, `after_blank`, `extract_delay` | seconds | pauses |

Each `[[runs]]`:

| Key | Type | Meaning |
|---|---|---|
| `identifier` | text | the sample's identifier, or a special prefix (below) |
| `aliquot` | whole number | |
| `step` | text | a heating step letter |
| `extraction` | table | `device`, `position` (a hole `"3"`, a list and ranges `"1,3-5;9"`, a number, or a list of numbers), `value`, `units` (`w`, `%`, `c`, `a`, `v`), `duration`, `cleanup`, `pre_cleanup`, `post_cleanup`, `pattern`, `beam_diameter`, `ramp_rate`, `ramp`, `cryo_temp`, `script`, `options` (script options). Durations are seconds |
| `measurement` | table | `plan` (a plan name), `hook`, `advanced`, `overrides` (a table of plan parameters, below) |
| `post_equilibration`, `post_measurement` | text | script names |
| `overlap`, `overlap_min`, `delay_after` | seconds | |
| `conditionals` | list of names or `{ name, kind }` | run-level conditionals |
| `truncate` | text or list | shorthand truncation conditionals |
| `comment`, `weight` | text; number | |
| `skip`, `end_after` | true/false | |
| `sample` | table | `sample`, `material`, `project`, `irradiation`, `level`, `irradiation_position` |

The extraction keys may also be written flat on the run (`duration = 10`
instead of `extraction = { duration = 10 }`, `e_value`, `extract_value`,
`extract_units`, `s_opt`, `t_o` for `truncate`); naming the same thing twice is
an error.

### identifiers.toml

Optional. Without it these prefixes are special (case does not matter):

| Type | Prefix |
|---|---|
| `unknown` | `u` |
| `blank_unknown` | `bu` (and `b`) |
| `blank_air` | `ba` |
| `blank_cocktail` | `bc` |
| `blank_extractionline` | `be` |
| `background` | `bg` |
| `air` | `a` |
| `cocktail` | `c` |
| `pause` | `pa` |
| `degas` | `dg` |
| `detector_ic` | `ic` |

Anything else is a sample identifier and an `unknown`. With the file:

| Section | Key | Meaning |
|---|---|---|
| `[prefixes]` | `<type> = "<prefix>"` | required (at least one). Only the listed types are special; the defaults are *not* added. A prefix cannot be given twice |
| `[patterns]` | `unknown`, `step` | regular expressions an identifier and a step must match. Defaults `^[A-Za-z0-9][A-Za-z0-9_-]*$` and `^[A-Za-z]{0,2}$` |

### defaults.toml

What the run factory fills into a new run, by analysis type and extraction
device. Example: `configs/examples/defaults.toml`.

```toml
[unknown."*"]                     # [<analysis type>.<device>]; "*" matches any device
template = "multicollect"         # measurement plan
script = "sim_extract"            # extraction script
post_equilibration = "..."        # optional
post_measurement = "sim_pump"     # optional

[unknown."*".extraction]          # all optional
units = "w"                       # w, %, c, a, v
value = 5
duration = 10                     # seconds; also cleanup, pre_cleanup, post_cleanup

[unknown."*".overrides]           # plan parameters
"main.cycles" = 2
```

An exact device match wins over `"*"`. The top-level names must be analysis
types (the table above); an unknown key anywhere is an error.

### peak_center.toml

One table per named setting; plans pick one with `[peak_center] config =
"name"`. A plan that centres with a name this file does not have is refused
by `elctl exp validate`; `default` is the name used when a plan names none.
Example: `configs/examples/peak_center.toml`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `detector` | text | the reference detector | |
| `isotope` | text | `Ar40` | |
| `additional_detectors` | list | none | fitted too, for display |
| `center` | number | from the field table | where to start, native units |
| `window` | number > 0 | 0.015 | half-width of the scan, native units |
| `step` | number > 0 and < `window` | 0.0005 | |
| `percent` | number, 0 to 100 exclusive | 80 | height at which the peak width is taken |
| `min_peak_height` | number | 1.0 | |
| `test_peak_flat` | true/false | true | |
| `n_tries` | whole number >= 1 | 2 | |
| `direction` | `increase`, `decrease` | `increase` | sweep direction |
| `integration_s` | seconds >= 0 | 0 (keep the current) | |
| `settle_s` | seconds >= 0 | 0 | per step |
| `baseline_timeout_s` | seconds >= 0 | 10 | |
| `dac_offset` | number | 0 | added to the fitted centre |
| `peak_shift_threshold` | number >= 0 | 0 (no limit) | refuse a centre this far from the start |
| `update_table` | true/false | true | write the centre into the field table |
| `propagate` | true/false | true | and into the other detectors' columns |

### blocks/*.toml

A reusable run sequence the run factory can insert (a blank, an air shot, a
blank). The `[[runs]]` are exactly as in a queue.

| Key | Meaning |
|---|---|
| `[block]` `name`, `description` | the block's name (the file name if missing) |
| `[[runs]]` | the runs, same keys as a queue's |

### Measurement plans (plans/*.toml)

A plan says how to measure: which valves, how long, which isotopes on which
detectors, how many cycles, how to fit. Example:
`configs/examples/plans/sim_multicollect.toml`; the setup profiles write
`multicollect`, `detector_ic` and `multicollect_hop_ar39` for your detectors.
The plan's name is `[plan] name` (not the file name) and a queue says
`measurement = { plan = "<name>" }`.

Top-level tables (others are errors): `plan`, `hook`, `detectors`,
`equilibration`, `sniff`, `peak_center`, `baseline`, `main`, `fits`,
`conditionals`, `whiff`, `parameters`.

| Table | Key | Type | Default | Meaning |
|---|---|---|---|---|
| `[plan]` | `name` | text | required | |
| | `instrument_family` | text | required | |
| | `description` | text | | |
| | `analysis_types` | list | | where it applies |
| top | `hook` | text | | a script in `scripts/measurement_hooks/` |
| `[detectors]` | `reference` | detector | | positions the magnet for every hop |
| | `exclude` | list | | dropped from every hop |
| `[equilibration]` | `inlet`, `outlet` | valve names | | usually `"@valves.inlet"` |
| | `time_s` | seconds >= 0 | 15 | |
| | `inlet_delay_s` | seconds >= 0 | 0 | |
| | `close_inlet` | true/false | true | |
| `[sniff]` | `enabled` | true/false | false | |
| | `counts` | whole number | 0 (>= 1 when enabled) | |
| | `integration_s` | seconds > 0 | 1 | |
| `[peak_center]` | `before`, `after` | true/false | false | |
| | `detector` | text | the reference | |
| | `isotope` | text | required when before or after | |
| | `config` | text | `default` | a name from `peak_center.toml` |
| `[baseline]` | `before`, `after` | true/false | false | |
| | `counts` | whole number | 0 (>= 1 when used) | |
| | `mass` | number > 0 | where the magnet is | |
| | `detector` | text | the reference | |
| | `settle_s` | seconds >= 0 | 0 | |
| | `integration_s` | seconds > 0 | 1 | |
| `[main]` | `cycles` | whole number >= 1 | 1 | |
| | `integration_s` | seconds > 0 | 1 | |
| | `time_zero` | `"on_inlet_close"`, `"on_first_count"` or `{ offset_s = N }` | `on_inlet_close` | |
| `[[main.hops]]` (at least one) | `positions` | table `isotope = "detector"` | required | one detector per isotope in a hop |
| | `counts` | whole number >= 1 | | |
| | `settle_s` | seconds >= 0 | 0 | |
| | `protect` | list of detectors | | |
| | `baseline` | true/false | false | a baseline hop |
| | `mass` | number > 0 | | baseline hops only |
| | `position` | `{ isotope, detector }` | | needed when the reference detector is not in the hop |
| `[fits]` | `signal`, `baseline` | a fit name or `{ default = ..., Ar36 = ... }` | signal `linear`, baseline `average` | `average`, `linear`, `parabolic`, `cubic`, `exponential` |
| | `error` | `sem`, `sd` | `sem` | |
| | `outliers` | `{ enabled, iterations >= 0, std_devs > 0 }` | disabled; 1; 2 | |
| `[conditionals]` | `include` | list | | conditional sets: `"@conditionals.default_unknown"` |
| | `truncations` | list of `{ check, start }` | | quick truncations |
| `[whiff]` | `enabled`, `counts`, `integration_s`, `checks` = list of `{ check, action }` | | disabled | `action`: `run_remainder`, `pump`, `abort` |
| `[parameters]` | `expose` | list of paths or `{ path, label }` | | which keys a queue may override |

A value of `"@some.key"` in any value (except `conditionals.include`) is
replaced by that key of the plan itself, then of the extraction line's
`[aliases]`, then of the spectrometer's aliases. An unresolved alias or a
cycle is an error. A queue run may override a parameter with
`overrides = { "main.cycles" = 1 }` only if the plan **exposes** it
(`parameters.expose`; an entry exposes everything below it); an `advanced`
run may override anything. A detector named in a plan must exist in the
spectrometer.

### conditionals/*.toml

Rules that stop, shorten or change a run when a measured value crosses a
limit. Each file is a named **set**; a plan includes a set by name
(`@conditionals.default_unknown`), a queue by `queue_conditionals`, a run
by `conditionals`. `system.toml` is applied to every run. Later levels add
to earlier ones: system, queue, plan, run. A conditional with the same name
replaces the earlier one; a top-level `disable = ["name", ...]` removes
earlier ones. Examples in `configs/examples/conditionals/`.

Tables, by when they are checked:

| Table | Effect | Default action |
|---|---|---|
| `[[pre_run]]` | before extraction and measurement | `cancel` |
| `[[truncations]]` | during measurement: end the run's data early (keep it) | `truncate` or `truncate:quick` |
| `[[terminations]]` | during measurement: end the run | `terminate` |
| `[[cancelations]]` | during measurement: stop the run and the queue | `cancel` |
| `[[actions]]` | during measurement: `truncate`, `terminate`, `cancel`, `set_param NAME=1.5`, `run_hook <name>`, `notify` | needs `action` |
| `[[modifications]]` | queue changes: `skip_next`, `skip_n 3`, `skip_aliquot`, `skip_to_last_in_aliquot`, `set_extract 1,2,3` (or `10%,20%`), `repeat`, `run_blank` | `skip_next` |
| `[[equilibrations]]` | evaluated on sniff readings | |
| `[[post_run]]` | after a run: `cancel` or any queue action | `cancel` |

Keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `check` | text, required | | an expression of measured values: `Ar40 < 1000`, `average(Ar40, window=10) < 100`, `gauge.IG1.pressure > 1e-6`, `CDD.inactive`, `Ar40.cur > 4e6` |
| `name` | text | `<kind>:<check>` | unique within a set |
| `start` | whole number >= 0 | 0 | readings ignored first (not for pre_run, post_run) |
| `frequency` | whole number >= 1 | 1 | evaluated every Nth reading after `start` |
| `ntrips` | whole number >= 1 | 1 | consecutive true evaluations before it fires |
| `action` | text | the kind's | see above; refused on kinds that take none |
| `resume` | true/false | false | actions only: keep measuring and re-arm |
| `window`, `mapper` | whole number >= 1; text | | legacy: apply a window or mapping to the check |
| `analysis_types` | list | all | `["unknown"]`, `"blank"` for every blank type |
| `abbreviated_count_ratio` | number in (0, 1] | 1 | truncations, modifications, equilibrations: scale the remaining counts |
| `truncate`, `terminate` | true/false | false | modifications only: also truncate or terminate; not both |

A conditional may only name gauges, detectors and isotopes that exist;
`elctl conditionals-check` and `exp validate` report the rest.

### notifications.toml

Email, a webhook or a program, when a run fails (`run_failed`) or a queue
ends (`queue_ended`). The step-by-step setup, mail services, and
troubleshooting are in [notifications.md](../notifications.md); a commented
starter is `configs/examples/notifications.toml.example`. Test with
`elctl exp notify --lab <dir>`. Unknown keys are errors.

| Where | Key | Default | Meaning |
|---|---|---|---|
| top | `curl` | `curl` | the program that sends |
| top | `timeout` | 30 | seconds per message, at least 1 |
| `[[email]]` | `name` | `email` | |
| | `provider` | SMTP | `brevo`, `resend` or `postmark`; leave out for SMTP |
| | `api_key_env` | | with `provider`: the environment variable holding the key |
| | `url`, `username`, `password_env`, `tls` | `tls` true | SMTP: `smtps://host:465` or `smtp://host:587` (STARTTLS required unless `tls = false`) |
| | `from` | required | the sender |
| | `to` | none | addresses that always get it |
| | `queue_user` | true | also the queue's `email` |
| | `on` | both | `run_failed`, `queue_ended` |
| `[[webhook]]` | `name`, `url`, `format` (`json` or `slack`), `on` | | |
| `[[command]]` | `name`, `argv` (list; first is the program), `on` | | the message goes on standard input; `PYCHRON_EVENT`, `PYCHRON_SUBJECT`, `PYCHRON_QUEUE` are set |

Keep keys and passwords in environment variables (`*_env`), never in the
file.

### cameras.toml

One table per laser (the driver's name): the camera used to centre holes and
follow the glow. A laser with no table has no camera. Example:
`configs/examples/cameras.toml`. See [05 Laser](05-laser.md).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `source` | `sim`, `recorded`, `opencv`, `pylon` | `sim` | `pylon` is read and checked, but its driver is not in this build |
| `use` | `center`, `view` | `center` | `view`: only a picture; nothing it sees moves the stage |
| `px_per_mm` | number > 0 | 23.0 | picture scale |
| `flip_x`, `flip_y` | true/false | false, true | which way the picture moves when the stage does |
| `aim_offset_px` | `[x, y]` | `[0, 0]` | where the beam is, from the picture's centre |
| `settle_ms` | 0 to 10000 | 200 | after a move, before a frame is trusted |
| `frames` | text | | `recorded`: the folder of recorded frames, relative to the lab |
| `[<laser>.opencv]` | `device` (index or file), `width`, `height`, `fps`, `channel` (`luma`, `r`, `g`, `b`), `rotate` (0, 90, 180, 270), `roi` (`[x, y, w, h]`), `timeout_ms` (100 to 60000; 1000) | | a real camera or a video file |
| `[<laser>.pylon]` | `serial`, `exposure_us`, `gain_db`, `pixel_format`, `packet_size` (576 to 16404), `timeout_ms` | | Basler |
| `[<laser>.sim]` | `tray_error_mm`, `noise` (0 to 1), `width`, `height`, `grain_offset_mm`, `glow_drift_mm_per_s`, `glow_sigma_mm` | | the simulated camera |
| `[<laser>.autocenter]` | `tolerance_mm` (0.03), `max_iterations` (1 to 50; 4), `max_step_mm` (0.5), `frames_per_step` (1 to 15; 3), `on_failure` (`continue` or `fail`; `continue`) | | `fail`: a hole that cannot be centred stops the run before the laser fires |

A simulated camera may only be used over a simulated laser, a live one
only over a real laser; the program refuses the other combinations when
the run starts. A scale measured with `elctl laser camera-scale` replaces
`px_per_mm` and the flips.

### Laser drivers (extraction_line.toml)

A laser is a driver in `extraction_line.toml` whose kind is an extraction
device (`chromium` today). Its name is the queue's `extract_device`. Example:
`configs/examples/laser.chromium.toml.example` (add its two tables to your
line). The Chromium driver's own keys:

| Key | Default | Meaning |
|---|---|---|
| `x_limits`, `y_limits`, `z_limits` | `[0, 50]` | stage travel in mm; a move outside is refused before it is sent |
| `signs` | `[1, 1, 1]` | 1 or -1 per axis |
| `move_speed` | | x, y, z speeds in microns per second |
| `in_position_um` | 10 | how near counts as arrived |
| `use_enable` | | `false` for a unit that refuses `Laser.Enable` |

`elctl list-drivers` shows the complete list for this and every other kind.

### tray_maps/*.txt

A sample tray, in legacy pychron's format. The map's name is the file name
without `.txt`. Example: `configs/examples/tray_maps/example-9.txt`. `#`
starts a comment anywhere.

| Line | Content |
|---|---|
| 1 | `shape,dimension`: `circle` or `square`, and the hole size in mm |
| 2 | valid hole ids, comma separated (may be empty; read but not enforced) |
| 3 | five calibration holes: north, east, south, west, centre (if not exactly five holes of this map, the map has no calibration holes) |
| then | one hole per line: `x,y`, `id,x,y`, `x,y,(assoc)`, `x,y,r<dim>` or `id,x,y,(assoc)`; mm in the tray's own frame. A line with no id is numbered by its place, from 1 |

### stage_calibrations/\<device\>.\<tray\>.toml

Where a tray sits on a laser's stage. Written by `elctl laser calibrate
<device> <tray> center --x X --y Y` (then `right`, or more `point`s); you do
not normally edit it. A calibration is tied to the exact tray map by a hash:
change the map and it must be redone (the check tells you). Only the
`[[points]]` are read back.

| Key | Meaning |
|---|---|
| `schema_version` | 1 |
| `device`, `tray` | which |
| `tray_sha256` | hash of the tray map file it was made against |
| `center`, `rotation_deg`, `scale`, `rms_mm` | the fit; for the reader |
| `[[points]]` `hole`, `x`, `y` | a hole and where it was found on the stage, mm |

### patterns/*.toml

A path the laser beam follows while heating; a run names it
(`pattern = "hexagon"`) and its extraction script runs it. The pattern's name
is the file name. Always `kind`; the other keys depend on it. Lengths mm,
angles degrees, speed mm/s.

| Kind | Keys (besides `velocity` and `iterations`) |
|---|---|
| `polygon` | `radius`, `nsides` (3 to 200), `rotation` |
| `linear` | `length`, `rotation`, `npasses` (1 to 100) |
| `circular_contour` | `radius`, `nsteps` (1 to 10), `percent_change` (0 to 100) |
| `line_spiral` | `radius`, `nsteps`, `percent_change`, `step_scalar` (1 to 20) |
| `square_spiral` | `radius`, `nsteps`, `percent_change` |
| `random` | `walk_x`, `walk_y`, `npoints` (1 to 50), `seed` |
| `rubberband` | `length`, `offset`, `rotation` |
| `raster` | `length`, `offset`, `rotation`, `dx`, `single_pass` |
| `trough` | `length`, `width`, `rotation`, `use_x` |
| `dragonfly` | `duration`, `perimeter_radius`, `saturation_threshold` (0 to 1), `aggressiveness` (0 to 10), `move_threshold`, `max_step`, `spiral` (`hexagon` or `square`), `spiral_base`, `target_radius`. Follows the glow: needs the laser's camera |

`velocity` > 0 (default 1.0); `iterations` 1 to 200 (default 1). A key a
kind does not have is an error. A pattern with a bad value stops only the runs
that name it.

### scripts

Python scripts, in `scripts/<kind>/`, where `<kind>` is `extraction`,
`post_equilibration`, `post_measurement` or `measurement_hooks`. A queue names a
script without the extension; a script in a subfolder is named
`folder:name` (`co2/degas.py` is `co2:degas`). See
[09 Scripting](09-scripting.md).

## Install files: site.toml, profiles, credentials

### Setup profiles (profiles/\<name\>/profile.toml)

What `elctl init` and the setup wizard install. Each directory under
`profiles/` is one profile: a `profile.toml` plus the template files it
renders or copies. The program ships `argus`, `helix`, `ngx`,
`data-reduction` (usable), and `instrument-common`, `lab-common`,
`extraction-line-starter`, `qtegra` (pieces other profiles include). You
only write one to add an instrument.

| Key | Type | Meaning |
|---|---|---|
| `name`, `title`, `summary` | text | |
| `kind` | `instrument`, `data_reduction`, `fragment` | a fragment is only included by others |
| `version` | whole number | |
| `includes` | list of profile names | their questions and files are added, each profile once |
| `groups` | list | the order the wizard asks groups in |
| `[values]` | table | fixed answers the templates use, never asked |
| `[[questions]]` | | see below |
| `[[files]]` | | see below |

`[[questions]]` (asked in order; includes' first):

| Key | Meaning |
|---|---|
| `id` | the answer's name |
| `prompt`, `help`, `group` | what the wizard shows |
| `type` | `string`, `host`, `port`, `int`, `float`, `bool`, `choice`, `path` (a file), `folder`, `secret`, `list`, `table` |
| `default` | the answer when none is given |
| `choices`, `labels` | for `choice`: values and optional display text |
| `columns` | for `table`: each row's keys |
| `when` | only ask when this holds over earlier answers (`not simulation`, `data_source == "server"`) |

A required question with no default must be answered. Two questions with the
same id must have the same type.

`[[files]]`:

| Key | Meaning |
|---|---|
| `template` | a file in the profile folder, rendered |
| `copy` | a file copied as is: relative to the profile, `@examples/...` (the shipped `configs/examples`; a trailing `/` is a folder), or `{{ answer }}` for a file the user named |
| `convert` | with `copy` naming a legacy folder: `legacy_line` makes `extraction_line.toml`, `legacy_canvas` makes `canvas.toml` |
| `to` | where in the install root. Two profiles cannot write one file |
| `when` | only when this holds |
| `secret` | write owner-only and never show in the summary |
| `check` | `line` or `canvas`: load it (and check the canvas against the line) before anything is written |

Templates use `{{ name }}`, `{{ name | toml }}` (quoted for TOML),
`{% if %}...{% else %}...{% endif %}` and `{% for x in list %}...{% endfor %}`.
Every shipped profile is tested to render, load and pass `doctor`.

An answers file for `elctl init --answers file.toml` is `id = value`
(a table question is an array of tables); `--set id=value` gives one answer.

### credentials.toml (data-reduction installs)

`<install root>/.pychron/credentials.toml`, written by the wizard for a
server database, readable only by you. The site file holds the database URL
without the password; the programs add it when they open the database.

| Key | Type | Meaning |
|---|---|---|
| `[database]` `password` | text | the database user's password. Missing file or empty password: the URL is used as it stands (a server needing no password) |

A broken file (not valid TOML) is reported by `doctor` and stops the
program starting. A `sqlite:` URL needs no credentials. A `postgresql://`
URL must contain a user (`postgresql://user@host/db`) for the password to
be added.

### references.toml (records folders)

`<data>/records/references.toml`. Gives flux (J), production ratios and
irradiation chronology to analyses read from a records folder, which carry
none of their own. A data-reduction install writes a commented starter. The
keys are listed in [06 Data analysis](06-data-analysis.md#a-records-folder-needs-a-referencestoml).

### Figure presets (figures/, presets/)

Figure and fit options saved by name, shared in `<lab>/figures/<kind>/` or
yours in the per-user `presets/<kind>/`. Format and every setting are in
[06 Data analysis](06-data-analysis.md#options-and-named-presets).

## Unverified or not yet implemented

- **Driver-specific keys.** Every key a driver reads is declared in the
  driver and printed by `elctl list-drivers`; this page lists the keys of
  the framework (`kind`, `transport`, `channels`, `roles`) and the example
  laser driver only. A key set for a driver that does not declare it is an
  error, so `list-drivers` is the authority.
- **Measurement plans, version 2.** A design for segments, positions and
  gases exists (`docs/superpowers/specs/2026-10-05-measurement-plan-v2-design.md`);
  the code in this release reads the hop-based plans documented above.
- **Source ramping.** The example Qtegra config says source ramping is not
  implemented for that driver: HV and trap current change in one step.
  `[source] ramp` is read, and limits the rate on drivers that support it
  (the simulated ones); check the driver before relying on it.
- **Untested hardware.** The Qtegra and NGX drivers and their example configs
  are marked in the examples as never run against an instrument; field
  tables, deflections, protection and saturation values in the examples are
  simulation placeholders and **must** be replaced with your instrument's
  calibration (see `profiles/instrument-common/CALIBRATE.md`).
- **`profiles/<name>.toml` in the spectrometer folder.** The folder layout
  reserves it for saved scan profiles; this reference found no loader for it,
  so nothing you put there is read.
- **Pylon camera.** `source = "pylon"` is read and checked but its driver is
  not part of this build.
- **Where the per-user preset folder is.** Qt's per-user configuration
  location for the application; the exact path was not checked on every
  platform.
- **Install layout of figures for data-reduction.** In the full program the
  lab's presets are `<lab>/figures`; in a data-reduction install they are
  `<install root>/figures`.
