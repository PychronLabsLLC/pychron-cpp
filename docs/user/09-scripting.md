# Scripting: extraction and post-measurement scripts

A **script** is a short program that tells the extraction line what to do
during one step of a run: close a valve, move the laser stage to a hole, heat
for a time, pump the prep section. A queue run names its scripts; the program
runs them at the right moment. This chapter explains the script language, every
command a script may use, the script editor, how errors appear, and the
safeguards around scripts.

Related: [Experiments](04-experiments.md) (queues and runs),
[Laser extraction](05-laser.md), [Extraction line](02-extraction-line.md),
[Configuration reference](07-configuration-reference.md),
[Troubleshooting](10-troubleshooting.md), [Glossary](11-glossary.md).

You do not need to be a programmer to adapt the example scripts: a script is a
list of commands, one per line, with a few Python rules (indentation matters).

## 1. Language and host

Scripts are written in **Python 3**, run by an interpreter (CPython) built into
the program. Nothing needs to be installed: an installed pychron carries its
own Python; a build from source uses the Python it was built with. (A build
made without scripting, `-DPYCHRON_SCRIPTING=OFF`, can still open and edit
queues, but every script step fails with "not supported: scripting".)

Scripts are **not** ordinary Python programs. They run in a restricted space:

* The commands in this chapter are available as plain functions (`open('A')`,
  `sleep(5)`).
* Only a safe subset of Python's built-in functions is available (section 7).
* Only a short list of modules may be imported: `math`, `random`,
  `statistics`, `json`, `re`, `datetime`, `collections`, `itertools`,
  `functools`, `string`, `operator`.
* The values of the run (its identifier, hole, heating value, duration...) are
  already there as read-only variables (section 3).

### 1.1 Script kinds and where they live

A script is a file `<lab>/scripts/<kind>/<name>.py`. A queue names it without
the `.py`; a `:` stands for a folder (`co2:degas` is `co2/degas.py`).

| Kind (folder) | Named by | Runs | Must define |
|---|---|---|---|
| `extraction` | the run's `extraction.script` | to extract the gas (Extracting) | `main()` |
| `post_equilibration` | the run's `post_equilibration` | when the spectrometer inlet closes, while the measurement continues | `main()` |
| `post_measurement` | the run's `post_measurement` | after the measurement, usually to pump out | `main()` |
| `measurement_hooks` | the plan's or run's `hook` | at points of the measurement (section 10) | any of `before_main`, `after_main`, `on_whiff_result` |
| `lib` | `gosub('name')` only | called from another script (section 4.7) | `main()` |

When a script is run, the file is first loaded, then its `main()` is called.
Put your commands inside `main()`; do not give commands at the top level.

### 1.2 A first script

This is `configs/examples/scripts/extraction/laser_extract.py`, the example
laser extraction:

```python
#! pychron: eqtime=20
# Isolate the prep section, drive the stage to the run's hole, heat for the
# run's duration (or for as long as the run's pattern takes), then let the gas settle.

def main():
    info('extracting {} at hole {}: {} {}'.format(run_identifier, position, extract_value, extract_units))
    close('C')
    move_to_position()
    enable()
    extract()
    fire_laser()
    if pattern:
        execute_pattern()   # a path, or a dragonfly following the glow for `duration`
    else:
        sleep(duration)
    end_extract()
    disable()
    sleep(2)
```

and the simulated-lab versions:

```python
# scripts/extraction/sim_extract.py
def main():
    info('extracting {} at {} {}'.format(run_identifier, extract_value, extract_units))
    close('C')
    if not analysis_type.startswith('blank'):
        open('P2')
        sleep(2)
        close('P2')
        open('P1')
        sleep(2)
        close('P1')
    sleep(duration)
    sleep(2)
```

```python
# scripts/post_measurement/sim_pump.py
def main():
    signal_pump_time_start()
    open('C')
    open('B')
    info('pumping the spectrometer and prep after {}'.format(run_identifier))
    sleep(50)
    close('B')
```

(Nothing in the simulated lab gives gas off when it is heated, so
`sim_extract` lets one pipette of the tank's air into the line in its place
for any run but a blank; `sim_air.py` and `sim_blank.py` are the air shot and
its blank on their own.)

The `#! pychron:` line is a header (section 8). Lines starting with `#` are
comments. Indentation (four spaces) shows what belongs to `main()` and to
`if`.

## 2. Valve names

Valve names are those in `extraction_line.toml`. The script editor and every
run check a valve name written as text in the script against the line, so
`close('Z')` with no valve `Z` is an error before anything moves
(`unknown-valve`). Names that are built in the script (for example from a
variable) are not checked until they run.

## 3. The run's values

Every script of a run sees these variables, set from the run. They are
**read-only**: assigning to one (`duration = 5`) is an error. Use another name
for your own variables.

| Variable | Value | Example |
|---|---|---|
| `analysis_type` | Run type, text | `'unknown'`, `'blank_air'` |
| `run_identifier` | Identifier, aliquot and step, text | `'66001-1'`, `'66001-1A'` |
| `extract_device` | The extraction device's name | `'co2'` |
| `extract_value` | The heating value | `20.0` |
| `extract_units` | Its units | `'percent'`, `'watts'` |
| `duration` | Heating time in seconds | `10.0` |
| `cleanup`, `pre_cleanup`, `post_cleanup` | Cleanup times in seconds | `120.0` |
| `tray` | The queue's tray name (empty text if none) | `'example-9'` |
| `position` | The run's hole: a **number** (the first hole) when the run has one, otherwise empty text `''` | `3` |
| `pattern` | The run's pattern name, empty text if none (so `if pattern:` works) | `'hexagon'` |
| `beam_diameter` | The run's beam diameter, or `None` | `None` |
| `ramp_rate`, `ramp_duration` | Ramp rate and ramp time in seconds | `0.0` |
| `cryo_temperature` | The run's cryo temperature, 0.0 if none | `0.0` |
| `opt` | Script options, read as `opt.name` (not filled in by queue runs yet, see section 12) | |
| `light_value`, `load_identifier`, `disable_between_positions` | Present for old scripts, with fixed values (`0.0`, `'default_load'`, `False`) | |

`opt.name` is an error if the option does not exist; `'name' in opt` and
`opt.get('name', default)` are safe.

## 4. Commands

In the signatures, `x=None` means the argument is optional. A command that
needs hardware the extraction device or line does not have fails (section 9).
**Blocking** commands wait for something and notice a Cancel while they wait;
the others take effect and return.

### 4.1 Valves

| Signature | What it does |
|---|---|
| `open(name=None, description=None)` | Open a valve. `description` is accepted in place of `name`. |
| `close(name=None, description=None)` | Close a valve. |
| `lock(name=None, description=None)` | Software-lock a valve: every actor, scripts included, is refused when trying to change it until it is unlocked. |
| `unlock(name=None, description=None)` | Remove the software lock. |
| `is_open(name=None, description=None)` | `True` if the valve is open. |
| `is_closed(name=None, description=None)` | `True` if it is closed. |

```python
close('A')
open(name='B')
lock('C')
if is_open('B'):
    info('B is open')
unlock('C')
```

An interlock (see [Extraction line](02-extraction-line.md)) can refuse an
`open` (for example while its partner valve is open or its state is unknown),
and a locked valve refuses both; the script then stops with an interlock
error.

### 4.2 Extraction device

| Signature | What it does |
|---|---|
| `enable()` | Enable the device (for a laser: enable the laser). Checks interlocks. |
| `disable()` | Disable it; ends any extraction. |
| `prepare()` | Prepare the device (for a Chromium: check that the program on the port is a Chromium). |
| `extract(value=None, units=None, block=None)` | Set the heating output. With no arguments, uses the run's `extract_value` and `extract_units`. `block` is accepted and ignored. A Chromium takes percent only, and must be **enabled first**. |
| `end_extract()` | End the extraction (beam off). |
| `ramp(start=0, end=0, rate=0, duration=0, period=1)` | Raise or lower the output smoothly in steps, in the run's units. Give `duration` (seconds), or `rate` (units per second) and the duration is worked out. A step every `period` seconds. **Blocking.** |
| `fire_laser()` | Open the beam at the current output. Needs a laser, enabled, with no interlock tripped. |
| `warmup(block=False)` | Start the laser's warm-up if it has one (a Chromium does nothing). |
| `get_device(name=None)` | The run's extraction device, as an object with `.name`, `.output()` (current output) and `.is_enabled()`. A name that is not the run's device is an error. |

```python
enable()
extract(20, 'percent')
fire_laser()
sleep(duration)
end_extract()
disable()

ramp(start=0, end=20, duration=10)       # 0 to 20 percent in 10 steps over 10 s
dev = get_device()
info('output is {} on {}'.format(dev.output(), dev.name))
```

Whatever a script does, **after the extraction script ends (even by an
error or a Cancel) the run stops any pattern, ends the extraction and disables
the device**. A script should still do this itself so the rest of the script
is safe.

### 4.3 Stage and patterns

These need a device with a stage (a laser with a stage). Positions in a queue
are holes on the queue's tray (see [Laser extraction](05-laser.md)).

| Signature | What it does |
|---|---|
| `move_to_position(position=None, autocenter=True, block=True)` | Move to a hole (default: the run's `position`) and, with a camera that centers, center it under the beam. **Blocking** when `block=True`. With a camera that centers, `block=False` needs `autocenter=False`. A move that is cancelled stops the stage where it is. |
| `set_x(value, velocity=None, block=True)` | Move the stage's x axis to `value` mm. `velocity` is accepted and ignored. |
| `set_y(value, velocity=None, block=True)` | The same for y. |
| `set_z(value, velocity=None, block=True)` | The same for z. |
| `set_xy(x, y, velocity=None, block=True)` | Move x and y together. |
| `set_tray(tray=None)` | Set the tray (default: the queue's tray). The run does this before the script, so you rarely need it. |
| `execute_pattern(pattern=None, block=True, duration=None)` | Run a laser pattern (default: the run's `pattern`). A path pattern moves the stage along the path; a dragonfly follows the glow for `duration` (default: the run's duration). **Blocking**; `block=False` is refused for devices whose patterns need the script to wait. |

```python
move_to_position()                       # the run's hole, centered
move_to_position(7, autocenter=False)    # hole 7, no centering
set_xy(25.0, 25.0)
set_z(10.0)
execute_pattern('hexagon')
execute_pattern(duration=30)             # the run's pattern, for 30 s
```

A hole move does not change z. What a move did (centered, moved how far, or
why not) is written to the run's log.

### 4.4 Furnace, pipettes, motors, cryo, imaging, pressure

These commands exist in the language, but the only extraction device driver in
this build is the Chromium laser (see [Laser extraction](05-laser.md)). **On a laser they stop the run
with `not supported: <feature>`.** They are listed so scripts converted from
older systems can be read.

| Signature | What it does |
|---|---|
| `dump_sample()` | Furnace: empty the crucible. |
| `drop_sample(position=None)` | Furnace: drop the sample at `position` into the crucible. |
| `set_pid_parameters(value)` | Furnace: choose the PID set tuned for this output. |
| `begin_heating_interval(duration, min_rise_rate=None, check_time=60, check_delay=60, check_period=1, temperature=None, timeout=300, name='interval')` | Furnace: begin an interval of `duration` seconds (see `complete_interval`). Other arguments are accepted and ignored. |
| `load_pipette(identifier, timeout=None)` | Fill a pipette. |
| `extract_pipette(identifier, timeout=None)` | Release a pipette's gas into the line. |
| `set_motor(name, value, block=False)` | Move a named motor. **Blocking** with `block=True`. |
| `get_value(name)` | Read a named motor's position. |
| `set_cryo(value, block=False)` | Set the cryostat's setpoint (kelvin), or give a text name of a named setpoint. With `block=True`, waits until it is reached (**blocking**). Needs a `[cryo]` controller on the line. |
| `get_cryo_temp(channel=1)` | Read a cryostat input in kelvin. |
| `snapshot(name='', note='', pic_format='.jpg')` | Save a camera picture and return where it was saved. `note` and `pic_format` are accepted and ignored. |
| `video_start(name='')`, `video_stop()` | Start and stop recording video. |
| `video_recording(name='')` | Use as `with video_recording('run1'):` to record for the length of the block. |
| `get_pressure(controller, gauge)` | Read a gauge: the controller's and gauge's names. |
| `get_manometer_pressure(name='manometer')` | Read a manometer. |

```python
set_cryo(12.0, block=True)
t = get_cryo_temp()
p = get_pressure('ig_controller', 'IG1')
with video_recording('heating'):
    sleep(duration)
```

### 4.5 Timing

| Signature | What it does |
|---|---|
| `sleep(duration=0, message=None)` | Wait `duration` seconds. **Blocking.** |
| `delay(duration=0.5, message=None)` | The same, with a default of half a second. **Blocking.** |
| `pause(duration=0, message=None)` | Wait `duration` seconds; with `duration=0` wait **until `wake()`** is called (from another script). **Blocking.** |
| `wake()` | End a `pause()` that is waiting without a time. |
| `waitfor(func, timeout=0, check_period=1, start_message='', end_message='')` | Call `func()` every `check_period` seconds until it gives a true value. With `timeout` seconds and no success, fail with a timeout error. `timeout=0` waits for ever. **Blocking.** |
| `begin_interval(duration=0, name='interval')` | Start a named timer of `duration` seconds. |
| `complete_interval(name='interval')` | Wait for the rest of the timer (nothing if it has already run out). An unknown name is an error. **Blocking.** |

`message`, `start_message` and `end_message` are accepted and not shown.

```python
sleep(5)
begin_interval(30)
open('B')                                  # do work while the interval runs
waitfor(lambda: is_open('B'), timeout=12, check_period=0.5)
complete_interval()                        # wait for the remainder of the 30 s
```

### 4.6 Logging

| Signature | What it does |
|---|---|
| `info(message, *args, **kw)` | Write `message` to the run's log. Extra arguments are ignored; build the text with `.format()`. |
| `print(...)` | Works as `info` with the arguments joined by spaces. |

```python
info('heating {} at {} {}'.format(run_identifier, extract_value, extract_units))
```

What the script says appears in the Executor pane's **Events** list under the
run's identifier, is printed by `elctl exp run`, and is saved with the record
as notes.

### 4.7 Calling other scripts: `gosub`

| Signature | What it does |
|---|---|
| `gosub(name=None, root=None, klass=None, argv=None, **kw)` | Run another script's `main()` now. Only `name` and the keywords are used. |

The target is looked for in the calling script's kind folder, then in
`scripts/lib/`. A `:` stands for a folder: `gosub('common:prepare_line')` runs
`common/prepare_line.py`. Keywords become variables in the called script
(overriding a run value of the same name):

```python
gosub('common:prepare_line', settle=5)
```

with `scripts/lib/common/prepare_line.py`:

```python
def main():
    close('A')
    open(name='B')
    sleep(settle)
```

Calls may nest to a depth of 8. The editor and the run check a called script
too (an unknown `gosub` target is an error). A `gosub` whose name is not a
text constant is only warned about, and checked when it runs.

### 4.8 Shared resources and intensity (not available in queues yet)

These commands are part of the language, but **a queue run does not provide
what they need in this version**, so they stop the run with `not supported:
shared resources` or `not supported: get_intensity`. The static check does not
catch this; it appears when the command runs.

| Signature | Meant to |
|---|---|
| `acquire(name, clear=False)` | Take a named shared flag, waiting if another script holds it. |
| `wait(name, criterion=0)` | Wait until a shared value equals `criterion`. |
| `release(name)` | Release a flag. |
| `set_resource(name, value)` | Set a shared value. |
| `get_resource_value(name)` | Read a shared value. |
| `get_intensity(key)` | Latest intensity of an isotope or detector (`'Ar40'`, `'H1'`), **post-measurement scripts only**. |

### 4.9 Pump time: `signal_pump_time_start()`

`signal_pump_time_start()` (**post-measurement scripts only**) tells the
executor that pumping has begun, so that an overlapped next run may count its
pump time. If the script never calls it, pump time is taken to start when the
script ends. See "Overlapping runs" in [Experiments](04-experiments.md).

```python
def main():
    signal_pump_time_start()
    open('C')
    sleep(5)
```

## 5. Python you can use

* `if`/`elif`/`else`, `for`, `while`, `try`/`except`/`finally`, functions
  (`def`), `lambda`, classes, `with`, list/dict comprehensions, string
  formatting (`'{}'.format(x)`, f-strings).
* The built-ins in section 7, and the modules in section 1.
* Define helper functions at the top of the file and call them from `main()`.

```python
import math

def heat_for(seconds):
    extract()
    fire_laser()
    sleep(seconds)
    end_extract()

def main():
    enable()
    for hole in (1, 2, 3):
        move_to_position(hole)
        heat_for(math.ceil(duration))
    disable()
```

## 6. The script editor

Open it with **Scripts > Script Editor...** (Ctrl+Shift+K) in the Experiment
window, or select a row and choose **Rows > Edit Extraction Script** or
**Edit Post-Measurement Script** (also in the right-click menu), which opens
the script that row names.

* **Left:** the lab's scripts by kind. Double-click to open. Every open script
  is a tab with highlighting and completion of the commands that kind may call.
* **New...** (Ctrl+N) asks for the **Kind** and the **Name** (`co2_degas` or
  `co2:degas`) and makes `<lab>/scripts/<kind>/<name>.py` with a starting
  `main()`. **Save** is Ctrl+S, **Close Tab** Ctrl+W. Closing a tab or the
  window with unsaved edits asks first.
* **Bottom:** the checker's messages (a line number, `error` or `warning`, and
  the message; click one to jump to the line), and an **Estimate**, such as
  "Estimate 0:00:25", the time the script will take. Both refresh about 0.6 s
  after you stop typing; **Code > Check Now** (F7) does it at once.
* **Gosub:** Ctrl+click on `gosub('name')`, or F2 with the cursor on it, opens
  the script it runs.

The estimate adds up the sleeps, pauses and waits with a timeout. If the
script has a `while` loop, a `pause()` with no time, or a `waitfor` with no
timeout, the estimate is flagged "at least" with the first reason, because the
true time is not known.

The editor checks a script as a run would, but **without hardware**: it
assumes the device has every capability, and uses the line's valve names. So
a command that your device cannot do (`dump_sample()` on a laser) is **not**
flagged in the editor, but is flagged when a run using that device begins.
Saving a script re-checks the queue's rows that name it.

## 7. What the checker and the run enforce

Every script is checked **statically** (read, not run) in the script editor
and again at the start of each run that uses it; a script with an error fails
the run before any hardware moves. The messages look like

```
script check failed: extraction/laser_extract.py:9: unknown-command: unknown command 'exctract' (+1 more)
```

| Code | Meaning |
|---|---|
| `syntax` | The text is not valid Python. |
| `header` | A `#! pychron:` line is malformed. |
| `unknown-command` | A function that is not a command, a function you defined, or an allowed built-in (usually a typo). |
| `unknown-name` | A variable that is not defined. |
| `arity` | A command called with the wrong number or names of arguments (for example `sleep(1, 2, 3)`). |
| `unknown-valve` | A valve name not on the line. |
| `not-supported` | The command needs hardware this run lacks (`not supported: furnace (dump_sample)`). |
| `not-allowed` | A command not available in this kind of script (`get_intensity` outside post-measurement). |
| `import` | A module not on the allowed list. |
| `restricted` | Access to a name like `__class__` (double underscores on both sides). |
| `read-only` | Assigning to a run variable such as `duration` or to `opt`. |
| `no-main` | The script defines no `main()`. |
| `unknown-gosub`, `gosub-depth` | A `gosub` target that does not exist, or nesting deeper than 8. |
| `unbounded-loop` (warning) | A `while` loop: the duration cannot be estimated. |
| `dynamic-gosub` (warning) | A `gosub` whose target is not a constant. |

**Allowed built-ins:** `abs all any bool callable chr dict divmod enumerate
filter float format frozenset hasattr hash int isinstance issubclass iter len
list map max min next ord pow print range repr reversed round set slice
sorted str sum tuple zip object staticmethod classmethod property super`
and the exceptions `Exception BaseException ArithmeticError AssertionError
AttributeError ImportError IndexError KeyError LookupError NameError
NotImplementedError RuntimeError StopIteration TypeError ValueError
ZeroDivisionError`. There is no `open` for files (`open` is the valve
command), and no `eval`, `exec`, `getattr`, `type` or `__import__`.

## 8. The script header

A line anywhere in a script of the form

```python
#! pychron: eqtime=20, label="two words"
```

is a header: `key=value` pairs separated by commas or spaces, a value may be
quoted, and a later key replaces an earlier one. A malformed header is an
error. Headers carry information about the script (an equilibration time, a
label). **In this version no run setting reads them**, so `eqtime` does not
change the equilibration time (that comes from the measurement plan).

## 9. Errors, Cancel and what you see

When a script fails the run ends **Failed**, with the message in the run's
Status tooltip and in the Events list ("run 3 66001-1: failed: ..."), and
(if configured) in a [notification](../notifications.md). The queue stops
unless the failure was only in saving.

The error is one of:

| What you see | Meaning |
|---|---|
| `script check failed: <script>:<line>: <code>: ...` | The static check (section 7). |
| `<script>:<line>: NameError: ...` (or another Python error type) | A bug in the script found while it ran: the file, the line and the Python error. |
| `not supported: <feature>` | The extraction device or line cannot do it: `furnace`, `pipette`, `motor`, `cryo`, `imaging`, `pattern`, `stage`, `laser`, `valves`, `pressure`, `extraction device`, `shared resources`, `get_intensity`, `extract in watts (a Chromium takes percent)`. |
| `the laser is not enabled` | `extract()` before `enable()`. |
| `cannot enable: laser interlock tripped (...)` (or `cannot fire: ...`) | A laser interlock is tripped; the names say which. |
| `waitfor timed out after N s` | A `waitfor` ran out of time. |
| `unknown extract units 'x'` | The units given to `extract()` (or the run's units) were not `percent`, `watts` or `temp`. A queue file accepts `amps` and `volts` too, but a script's `extract()` does not. |
| `gosub 'x' not found`, `gosub depth limit reached at 'x'` | Called script missing or too deeply nested. |
| `cancelled: ...` / `aborted: ...` | The operator stopped the run (below). |

A hardware problem arrives as an error with a **kind** (`timeout`, `io`,
`protocol`, `config`, `not_connected`, `interlock`, `cancelled`) and the device
name. A script may catch it:

```python
def main():
    try:
        move_to_position()
    except HardwareError as e:
        info('move failed ({}): {}'.format(e.kind, e))
        raise                      # let the run fail; leave this out to carry on
```

`NotSupportedError` is a kind of `HardwareError` (kind `config`).

### 9.1 Cancel and Abort

* **Cancel** raises `ScriptCancelled` at the script's next **blocking**
  command (a sleep, a move, a pattern...). Non-blocking commands in between
  still happen. `finally:` blocks run, and `ScriptCancelled` is not caught by
  `except Exception:`. Post-equilibration and post-measurement scripts still
  run after a Cancel, and the cancelled run's data are not saved.
* **Abort** stops at the next line of the script and then refuses every
  hardware command, including those in `finally:` blocks. No later script
  runs.
* A tight loop with no blocking command (`while True: pass`) is not
  interrupted by Cancel; only Abort ends it.

## 10. Measurement hooks (advanced)

A hook is a script in `scripts/measurement_hooks/` named by a plan's or run's
`hook`. It does not define `main()`. Instead it may define any of these
functions, which the measurement calls with an **`api`** object:

```python
def before_main(api):            # before the main measurement
    api.position('Ar40', 'H1')
    api.add_conditional('Ar40 > 1000 -> truncate')
    api.log('hook ready')

def after_main(api):             # after it
    api.log('done')

def on_whiff_result(api, result):    # after a whiff check; result is the text 'run_remainder', 'pump' or 'abort'
    if result == 'run_remainder':
        api.acquire(5, integration_time=1.048576)
```

| Method | What it does |
|---|---|
| `api.position(isotope, detector)` | Put an isotope on a detector. |
| `api.acquire(counts, integration_time=1.0)` | Collect that many readings. |
| `api.open(valve)`, `api.close(valve)` | Operate a valve. |
| `api.add_conditional(spec)` | Add a conditional for this measurement: `"<check> -> <action>"` (the action is one of `truncate`, `truncate:quick`, `terminate`, `cancel`, `set_param NAME=V`, `run_hook NAME`, `notify`; without `->` it truncates). Queue actions (such as `skip_next`) are refused. See [conditionals](04-experiments.md#7-conditionals). |
| `api.truncate(quick=False)` | Truncate the measurement. |
| `api.log(message)` | Write to the run's log. |

A hook may also use the commands of section 4 (everything except those for
post-measurement scripts).

## 11. Safety notes

* **A script is your instrument's control program.** Test a new script with
  `--sim` first, then step through it on the real line with the beam path
  safe. A queue run, after a script, **always** ends the extraction and
  disables the device, but the script itself should leave valves in a safe
  state.
* **Heat at a known state.** Enable, then `extract`, then `fire_laser`; end
  with `end_extract` and `disable`, ideally in a `finally:`:

  ```python
  def main():
      enable()
      try:
          extract()
          fire_laser()
          sleep(duration)
      finally:
          end_extract()
          disable()
  ```

  (After an Abort the commands in `finally:` are refused, but the run's own
  clean-up still switches the laser off.)
* **Never leave a valve combination open that exposes the spectrometer.**
  Valve interlocks in `extraction_line.toml` are the safety net, not a
  replacement for a careful script.
* **Time limits.** A script has no wall-time limit by default. A `while` loop
  waits for ever unless you give it a way out; prefer `waitfor(..., timeout=)`.
* **What a script cannot do.** It cannot read or write files, start programs,
  use the network or import modules outside the allowed list. This keeps
  mistakes from harming the computer; it is a guard rail for a laboratory's
  own scripts, not a defence against a deliberately hostile script, so keep
  the `scripts` folder as writable only by trusted people.
* **Every run records a fingerprint of each script** (a SHA-256 hash of its
  text), the script's name, and the script's log, in the analysis record, so
  you can tell later which version of a script produced an analysis.

## 12. Unverified / not yet implemented

* **`opt` and script options.** `opt` exists, but a queue run does not
  currently pass the run's `options` text into it, so `opt.name` is an error
  in automated runs.
* **Header values** are parsed but not used by any run setting (section 8).
* **`get_intensity()` and the shared-resource commands** (`acquire`, `wait`,
  `release`, `set_resource`, `get_resource_value`) have no provider in a
  queue run; they fail with `not supported` (section 4.8).
* **Furnace, pipette, motor and imaging commands** have no driver in this
  build (section 4.4). Cryo works only with a `[cryo]` controller on the line.
* **Run limits.** The script host supports an executed-line limit and a
  wall-time limit, but a queue run does not set them.
* **Arguments accepted and ignored:** `block` in `extract`, `velocity` in
  `set_x/y/z/xy`, `note` and `pic_format` in `snapshot`, `message` in the
  timing commands, `min_rise_rate`... in `begin_heating_interval`,
  `root/klass/argv` in `gosub`, `timeout` in the pipette commands.
