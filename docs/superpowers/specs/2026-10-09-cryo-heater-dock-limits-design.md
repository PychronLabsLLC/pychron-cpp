# Cryo and heater docks: limits, named setpoints, paired rows

Status: design only, not implemented.

The cryo dock and the heater dock exist (plan 2026-10-05, tasks C6 and E3).
Set against the legacy panels they port (`BaseLakeShoreController`'s control
group, `HeaterMixin.heater_view`) the heater dock is level and the cryo dock
is short of four things: a setpoint field bounded by the output's limits, the
named setpoints of `cryotemps.yaml`, an input shown beside the loop it feeds,
and the segmented display legacy read an input on (`LCDEditor`), which the
heater dock already uses for its readback. This closes those, and gives the heater dock limits and units on its
chart, which legacy did not have.

Showing temperatures in Celsius is not part of this: the interface stays in
kelvin and a controller configured for Celsius converts in its driver.

## 1. Where a limit is enforced

In the extraction line, not in the dock. Legacy checked a programmatic
`set_setpoint` too (`_validate_setpoint`), and a script is where a wrong
setpoint does harm. The dock reads the same limits to bound its field, so the
operator is stopped before the click and a script is refused with the same
words.

Two alternatives were set aside: a clamp in the dock alone leaves scripts
unbounded, and limits derived only from a Lake Shore's bands give a heater, or
a cryostat with no bands, no limit at all.

## 2. Configuration

```toml
[cryo]
driver = "lakeshore"
setpoint_min = [4.0, 0.0]       # value i is output i's, as in [cryo.setpoints]
setpoint_max = [325.0, 325.0]

[cryo.setpoints]
He_freeze = [14.0, 0.0]

[[heaters]]
name = "bakeout"
driver = "plc_heater"
units = "C"
setpoint_min = 0.0              # in the heater's own units
setpoint_max = 200.0
```

Every key is optional, and either end may be given without the other. An
output past the end of an array has no configured limit.

The loader refuses:

- a minimum above its maximum;
- a named setpoint with a value outside the limits of the output it goes to,
  so the mistake is found when the file is loaded and not when the name is
  used.

## 3. Devices

`ITemperatureController` gains

```cpp
// The range of setpoints this output accepts, or nullopt when the driver
// has none of its own.
virtual std::optional<std::pair<double, double>> setpoint_limits(int output) const = 0;
```

`Lakeshore` answers the envelope of that output's `[[drivers.<name>.ranges]]`
bands (the lowest `min_k`, the highest `max_k`), and `nullopt` for an output
with no bands. A setpoint inside the envelope but in a gap between bands is
still the driver's Config error, as now.

`IHeater` is unchanged: a heater's limits are only what `[[heaters]]` says.

## 4. Extraction line

```cpp
// Configured limits narrowed by the driver's; either end may be absent.
struct SetpointLimits { std::optional<double> min, max; };
SetpointLimits cryo_limits(int output) const;
SetpointLimits heater_limits(std::string_view name) const;

Result<void> set_cryo_setpoint(int output, double kelvin);
// Every output the name gives, checked before the first is written.
Result<void> set_cryo_setpoints(const std::map<int, double>& setpoints);
Result<void> set_cryo_named(std::string_view name);
```

- A value outside its limits is a Config error naming the value and the
  range, and nothing is sent. For a set of several outputs, all are checked
  before any is written.
- `set_heater_setpoint` checks `heater_limits` the same way.
- `LineCryoService::apply` and `CoreBridge::cryo_command` go through these
  instead of calling the controller. The service keeps what it tracks for
  `cryo_settling()`; the lookup of a name moves to the line so the dock and
  the script use one.

A change in behaviour: a script whose `set_cryo` is outside configured limits
now fails where it used to send. Only a line that configures limits sees it.

## 5. Cryo dock

- One row per control loop: the input's temperature, the setpoint field and
  its Set button, the setpoint the controller reports. Output i sits with
  input i, as legacy paired them and `LineCryoService` waits on them. Inputs
  with no loop follow as plain rows; a loop with no input has an empty
  temperature.
- An input's temperature is read on the display the heater dock's readback
  uses: a `QLCDNumber` of 7 digits, flat segments, two decimals, `-` before
  the first reading. The unit is not on the display (it has no K): a `K`
  label follows it, and the tooltip names the input. This holds for every
  input, paired or not. The setpoint the controller reports stays a text
  label, so a row's one large number is the temperature.
- The two docks build the display from one helper, so they cannot drift
  apart: `lcd_readout(parent)` in the UI sources, which the heater dock's
  readback moves to with no change in what it shows.
- `CryoDock::temperature_text(input)` gives way to
  `CryoDock::temperature(input)`, the input's `QLCDNumber*`, as
  `HeaterDock::readback(heater)` is.
- The field's range is `cryo_limits(output)`, and its tooltip says the range.
  An absent end falls back to today's 0 and 1000 K.
- Under the rows, one button per `[cryo.setpoints]` name, in name order. The
  tooltip lists the value for each output. A click calls
  `CoreBridge::set_cryo_named`; the bridge posts `cryoSetpoint` for every
  output the name touched, so each readback is refreshed. No confirmation:
  legacy had none.
- While a set is unanswered the Set buttons and the named buttons are
  disabled.
- The strip chart is unchanged.

## 6. Heater dock

- The setpoint field's validator takes `heater_limits`. Enter on a value
  outside them sends nothing and says why in the status line. The tooltip
  says the range.
- The chart's axis reads `Readback (C)` when every heater has the same units,
  and `Readback` otherwise.

## 7. Tests

Written first, one layer at a time.

| Where | What |
|---|---|
| `tests/core` (loader) | the new keys are read; min above max refused; a named setpoint outside limits refused |
| `tests/devices/test_lakeshore.cpp` | `setpoint_limits` is the envelope of the bands; `nullopt` without bands |
| `tests/systems/test_line_cryo_service.cpp` | out of range refused and nothing reaches the sim, for one output, for a named set (none written when one of several is out), and with config and driver limits narrowing each other |
| `tests/systems/test_line_heaters.cpp` | out of range refused, nothing written |
| `tests/ui/test_cryo_dock.cpp` | paired rows and leftover inputs; each input's display shows `-` and then the reading to two decimals; the field's range; a named button per name, its set, the readbacks after; buttons disabled until the answer |
| `tests/ui/test_heater_dock.cpp` | the readback display is unchanged by the shared helper; an out-of-range Enter sends nothing and sets the status; the axis label with shared and mixed units |

## 8. Commits

`feat(config)`, `feat(devices)`, `feat(systems)`, `feat(ui)` for the cryo
dock and `feat(ui)` for the heater dock, in that order: each builds and
passes on its own.
