# Legacy Pychron config survey (from Google Drive setupfiles)

Surveyed read-only: Argus VI "melbourne" (Jan 2026), NGX (2023, 2025), Quad
(2023), SFT (2023), Reston (NGX + furnace), LDEO (Helix + Agilent, 2022),
and the remaining setupfiles folders: ASU, UF, UAF, Purdue, USGS Denver /
NMGRL valve box, NMGRL CO2 and diode lasers, Jan (Argus), Felix (Argus),
Hal (QMS He line), WiSCAr. (GSC holds irradiation data only; UA and UMan are
empty; USGS Reston duplicates Reston.)
Decoded copies were saved to the session scratchpad under
`scratchpad/legacy/<lab>/`. They
are not committed because they contain lab hosts and IPs. Copy any fixtures you
want into `tests/` by hand.

## Gaps vs current `extraction_line.toml` / `canvas.toml` schema

### Transports
- Real labs need: TCP (Qtegra :1069, NGX :1099, Lascon :9125), serial
  (115200, plus 7/odd/GPIB for the Model335 cryostat), **telnet** (SPC ion
  pumps, `read_terminator='>'`), Modbus RTU (`slave_address`), and **VISA/USB**
  (manufacturer/model/serial). Add `telnet` and `visa` kinds. Add a `unit_id`
  field for Modbus.
- The framing keys appear everywhere: `write_terminator` (CRLF, `chr(10)`),
  `read_terminator`, and `strip`. These belong on the transport (or the codec)
  as first-class keys.
- Hosts can be DHCP hostnames. `*.local.toml` overrides are still needed.

### Actuators and valves
- **Address formats vary by lab**:
  - free-text Qtegra switch names (`"Pipet Ref. Out Set"`)
  - ints (NGX)
  - symbolic names (Quad `SV14`)
  - the name used as the channel (SFT `'309'`)

  `address` must stay an opaque string, interpreted by the driver.
- A **double-actuation valve** is one valve driven by two addresses
  (`address: 101,102`). This needs an `addresses = [..]` field or
  `kind = "double_actuation"`.
- **Per-valve actuator override** is common (NGX `MSIP` → `ngx_valve_controller`,
  `address: PIV`). The current per-valve `actuator` already covers it.
- `inverted_logic = true` is needed (NGX).
- `interlock` is a scalar in some labs and a list in others. The importer must
  normalize it and flag typos (Quad has `inerlocks:`).
- Because of the Quad typo, the SV14→SV16 interlock was never enforced in
  legacy Pychron; only SV16→SV14 was. The importer should warn about
  one-sided interlock pairs (A lists B but B does not list A) and should
  reject unknown keys. It must not guess what a misspelled key meant.
- Pipette names are not unique in the legacy files (Quad has 3× `AirStandard`).
  The importer must disambiguate them or report an error.
- Legacy actuator indirection means `switch_controller.cfg` only names a type
  such as `QtegraGPActuator`. In the new schema, map this to a
  `drivers.<name>.kind`.
- New driver kinds are needed: `qtegra_gp_actuator`, `ngx_gp_actuator`,
  `agilent_gp_actuator`.

### Gauges, pumps, and temperature controllers
- Gauge drivers are needed: `igc100` (2 controllers, per-channel
  `names/display_names/lows/highs` as parallel lists, which map to
  `[[gauges]]` with `alarm_low/high`), and `spc_ion_pump` (telnet, scan only).
- Temperature controllers are missing entirely from the current schema:
  - Model335 cryostat: range bands, setpoints, and a `cryotemps.yaml`
    named-setpoint table (`He_freeze: 14,0`).
  - SFT controller: setpoint min/max and calibration coefficients.

  Add a `[[temperature_controllers]]` or a generic `[[sensors]]` section.
- The legacy `[Scan] enabled/period` block is per device. It could become a
  per-driver `scan_interval_ms` override.

### Lasers and stages
- Lasers and stages are not in the current schema: Lascon (TCP), ChromiumCO2
  and ChromiumUV (plugin-level TCP client), and Kinesis motion.
  - `stage.cfg`: `[Axes Limits] x=0,50` and `[Signs]`.
  - `motion_profiler`: velocity and tolerances.
- These should get an extraction-device spec before any schema work.

### Canvas
- Element kinds that exist in the legacy files but are not in the canvas spec:
  `spectrometer`, `ionpump`, `turbo`, `laser`, `tank`, `getter`, `gauge`,
  `coldfinger`. Map them to `stage` plus `symbol = "..."`, or add them as
  kinds.
- Legacy coordinates are in abstract units: centre-origin `translation` plus
  `xview/yview` from `canvas_config.xml` (e.g. `-28,28`). The converter must
  map them to the pixel `pos`/`size`, or the new canvas should adopt world
  units plus a view box.
- Connection endpoints appear either as `{name, offset: "5,5"}` or as bare
  strings. `offset` has no equivalent in the new schema yet.
- `cross_connection` is SFT only; `cross` is already in the spec.
- Colors are given as `r,g,b` ints or names (`lightblue`). Accept both.
- Pipette `vlabel` is a format string (`"Shots={:04d}"`) bound to the shot
  counter. The template needs a runtime value.

### Spectrometer (validates spectrometer spec §7.5 importer)
- `detectors.yaml` fields map cleanly: kind Faraday/IonCounter/CDD,
  `deflection_correction_sign`, `protection_threshold`, `software_gain`,
  `serial_id`. `index` is sometimes an int and sometimes a float. `color` is
  sometimes hex and sometimes int RGB.
- `mftable.csv`: line 1 is the fit type (`parabolic`), then the columns `iso`
  plus one per detector.
- `readout.yaml` has two anonymous lists (source params with min/max/compare,
  and detectors). Map it to profile verify.
- `config.cfg` `[Deflections]` keys are lowercase detector names, so the
  importer should case-fold them.

### Global and experiment
- `initialization.xml` becomes nothing. Its plugin toggles are dropped per
  spec §5.5. The `<globals>` `communication_simulation` flag maps to the
  `sim` transport kind.
- `startup_tests.yaml` (plugin → test methods) maps to `elctl selftest`,
  which would be a candidate feature.
- `scripts/defaults.yaml` (analysis type → script per phase) maps to the
  experiment spec template library.
- `flux_constants.yaml` and `ratio_change_detection.yaml` belong to
  reduction/experiment, not instrument config.
- Measurement pyscripts embed a YAML docstring
  (baseline/equilibration/multicollect/peakcenter/peakhop). This matches the
  MeasurementPlan importer's input exactly (experiment spec §5.3).

## Reston and LDEO additions

These two labs confirm most gaps above and add the following.

### Transports
- **UDP** is a real transport: LDEO talks to Qtegra (Helix) on :1069 over
  `kind=UDP`, not TCP. Add a `udp` kind.
- **Shared endpoints**: several legacy devices point at the same host:port
  (LDEO: Qtegra actuator, Qtegra gauge readback and spectrometer
  microcontroller; Reston: NGX actuator and spectrometer microcontroller on
  :1099). The new schema must let several drivers share one transport
  instance, or the importer must merge them. Opening one socket per driver
  would break TCP servers that accept a single client.
- **Serial port names are not stable paths**: Keyspan names such as
  `USA19H14641P1.1` (LDEO) and a bare prefix `usbmodem` (Reston). Support
  prefix/glob matching on the port, resolved at connect time.
- `terminator` (cryostat) is an alias for the read/write terminator pair.
  The importer should map it to both.
- `scheduler=gauges` (Varian) serializes several devices on one serial bus.
  This maps to a shared transport with a request queue.
- Varian `address=00` is a multi-drop bus address. It belongs on the driver,
  not the transport.

### Actuators and valves
- **Separate state readback device** (LDEO): each autovalve is driven by one
  controller (`actuator: switch_controller_Agilent2`, `address: '101'`) and
  its state is read from another (`state_device: {name:
  switch_controller_Agilent1, address: '102'}`). This differs from
  double-actuation. Add an optional `state_source = { driver, address }`.
- **Two indirection styles**: Reston's `switch_controller.cfg` uses
  `type=AgilentGPActuator`, while LDEO's uses `name=actuator_X` plus
  `klass=AgilentMultifunction` and puts the comms in `actuator_X.cfg`. The
  importer must follow both.
- New driver kind needed: `agilent_multifunction` (state readback).
- `invert` is set per actuator in `[General]` (Reston NGX, LDEO Agilent2), in
  addition to per-valve `inverted_logic`. The effective value is actuator
  XOR valve. The importer should resolve it onto each valve and say so.
- **Dangling actuator references**: Reston valve `G` names
  `ngx_switch_controller`, but only `ngx_valve_controller.cfg` exists. The
  importer must error on unresolved actuator names and must not fall back to
  the default controller.
- LDEO uses one `valves.yaml` list with `kind: valve | manual_valve |
  pipette`. Pipettes reference their valves by `inner`/`outer` name, and
  pipette names contain spaces (`Tank A`).
- **Manual valves** have no actuator. They exist in LDEO `valves.yaml` and as
  a `manual_valve` canvas kind in Reston (M1–M11, present only in the
  canvas). They need a first-class operator-set state that interlocks and
  path tracing can read.
- LDEO interlocks are symmetric (PV1↔PV2 and so on), so the one-sided-pair
  check stays quiet here, as expected.

### Gauges and temperature controllers
- Varian multi-gauge: `channels=CNV1,IMG1,HFIG1` are wire IDs distinct from
  `names`. `[[gauges]]` needs a `channel` separate from `name`.
- Qtegra gauge readback is a named value (`Ion Gauge MS Readback`) over the
  same UDP link. Gauge names can contain spaces.
- Cryostat `[Range]` uses expression strings (`1 = v<10`, `2 = 10<v<30`,
  `3 = v>30`). Convert them to numeric `[[ranges]] {band, min, max}` at import.
  Do not carry a string expression language forward.

### Spectrometer
- `mftable.csv` fit type `cubic` also appears (Reston), besides `parabolic`.
  A second table (`avftable.csv`) is in DAC units, not mass. The fit type and
  units must be per table.
- `detectors.yaml` isotope is not unique (Reston: Ar40 on both H5 and H1).
  Key detectors by name only.
- Reston `config.cfg` `[Deflections]` names detectors that no longer exist
  (`h2`, `h1`, `l1`, `cdd`), and `[Protection] detectors=CDD` names none.
  The importer should warn on references to unknown detectors.
- LDEO detector names contain parentheses (`H2(CDD)`, `AX(CDD)`). TOML keys
  must be quoted, or detectors should be an array of tables with a `name`
  field.
- LDEO still ships `detectors.cfg` (INI) alongside `detectors.yaml`. The
  importer must pick a precedence and report which file it used.
- Colors add named colors (`maroon`, `magenta`, `orange`, `lime green` with a
  space) to the hex and int-RGB forms.
- Reston `default_conditionals.yaml` is mostly empty template rows (`teststr:
  ''`). Its one real rule is a cancelation `Ar40.bs_corrected>60000.0` for
  unknown and air. The experiment importer should drop empty rows and parse
  the expression.

### Canvas
- LDEO keeps the canvas in **XML** (`canvas.xml`), not YAML, so the converter
  needs both readers.
- `canvas_config.xml` `<origin>` is non-zero (Reston `5,0`, LDEO `0,-7`) and
  view boxes are asymmetric (LDEO x `-21,24`). This supports the world-units
  plus view-box model.
- More element kinds: `manual_valve`, `circle_stage` (Reston furnace),
  `use_symbol: True` on ionpump and laser.
- Per-kind default colors live in `canvas_config.xml` (`<color
  tag="gauge">`). Map them to a canvas `[defaults.<kind>]` table.

### Global
- Reston's `initialization.xml` has `communication_simulation` and
  `dashboard_simulation` both on. The file on Drive may be a simulation copy
  rather than production.
- Reston's configs were assembled from other labs: the NGX device host is in
  the Columbia University address block (LDEO's network), and the
  spectrometer folder is byte-identical to ASU's. Hosts and spectrometer
  values may be stale.
- Reston enables a `RestonFurnace` plugin, which is another extraction device
  for the extraction-device spec (see Lasers and stages).

## Remaining labs (ASU, UF, UAF, Purdue, NMGRL, WiSCAr, Hal)

Findings below are new relative to the sections above. Cloning between labs
is common (Purdue = UAF devices, Reston spectrometer = ASU, `valve` =
USGS Denver minus a 2022 draft), so the importer should flag endpoints and
files that are identical across labs as possibly stale.

### File formats and parsing hazards
- **Three valve formats**: `valves.yaml`, `valves.xml` (NMGRL: `<group>`
  nesting, `<switch>` with `<parent>`, bare `<manual_valve>NAME</manual_valve>`,
  pipettes via `<inner>/<outer>`, often no `<address>` so the name is the
  address), and an older INI form. **Three canvas formats**: `canvas.yaml`,
  `canvas.xml`, and `valves2D.cfg` (pixel `pos`, `window_width`). Several labs
  ship two of these that disagree (UF, Hal, USGS Denver). The importer needs
  an explicit precedence rule and must report which file it read.
- INI hazards seen in real files: duplicate keys (`host=` twice; strict
  configparser rejects it), values with leading spaces (`name= NGX`) or
  trailing spaces (`port=... `, `units: F `), list items with spaces
  (`names=IG, CG1, CG2`), quoted values (`base_url='http://...'`), a value
  beginning with `;` (`prefix = ;LB.`), `type=ethernet`/`Ethernet` alongside
  `kind=TCP`, and lowercase/uppercase enum mixes (`parity = even`/`EVEN`).
  Trim, unquote and case-fold on import.
- Numeric forms: leading zeros (`06.0`), trailing-dot floats (`40.`,
  `-22.,19`), and unquoted YAML ints as valve names/addresses (UF, Hal,
  USGS Denver draft). Stringify names and addresses.
- Line endings: CRLF (mftables, pid.csv) and bare CR (Jan deflection
  tables). Tables may lack a final newline.
- Commented-out whole files (`readout.yaml`) parse to null; `tests:` may be
  null instead of a list. Treat null as empty.
- Malformed XML comments (`<!--<<address>312</address>-->`) and stray text
  tails after closing tags appear in NMGRL XML. Use a lenient reader and
  report what it skipped.

### Transports and framing
- **Length/checksum framing**: `message_frame=L4,-,C4` (Jan client, Fusions
  chiller) and Eurotherm `terminator=ETX` with `terminator_position=-2` (a
  checksum byte after the terminator). Framing needs to be a pluggable codec,
  not just terminator strings.
- `use_end=True/False` is per driver and differs between two drivers sharing
  one endpoint (UAF). Shared transports must keep per-driver framing or the
  importer must report the conflict.
- **HTTP** transport: Purdue's Kinesis stage is a local REST service
  (`base_url`). The setup doc and the cfg disagree on port and file name.
- **Pychron-to-Pychron link**: Jan runs `ClientExtractionLine` against
  Felix's `PychronGPActuator` over TCP. Needs a `pychron_gp_actuator` driver.
- One serial port is claimed by two devices at different baud rates (Jan
  diode module and unidex; Diode temperature monitor and Omega ADC). Detect
  port collisions at load.
- Scan periods carry different units: `units=s`, `units=m`, or no units with
  `period=30000` (ms) or `period=1000`. Normalize to `scan_interval_ms` and
  require explicit units in the new schema.

### Valves and actuators
- New per-valve keys: `query_state="false"` (never read state),
  `check_actuation_enabled`, `check_actuation_delay` (seconds),
  `ignore_lock_warning`, `inverted` (NMGRL spelling of `inverted_logic`),
  `description`.
- `rough_valve` is a canvas kind but a plain valve in `valves.xml`
  (USGS Denver, Jan). Decide whether "rough" is a valve attribute.
- New actuator kinds: `u3_actuator` (LabJack U3, USB, with a separate serial
  `switch_readback` on the same port, Hal), `pychron_gp_actuator`,
  `proxr_adc` pneumatics (12-bit ADC with linear `coefficients`).
- Hazards: all valves on `address: 1` (USGS Denver draft), an empty
  `<address>` with `actuator: NGXSwitchController` (a class name, not a
  device; WiSCAr), unused actuator cfgs (Purdue `AgilentGPActuator.cfg`),
  devices enabled in `initialization.xml` with no cfg (UF
  `switch_controller`, Diode `control`, USGS Denver `becker_box`), and
  pipettes named differently in valves vs canvas (`Air` vs `AirPipette`).

### Interlocks, monitors and conditions
- **Pressure-trip interlocks** (`monitors/system_monitor.cfg`):
  `pressure_trip=5e-9`, `pressure_reset=1e-9` (hysteresis), `gauge=Bone.IG`
  (dotted controller.gauge), `disable_valves=` listing valves **by
  description**, some of which do not exist. Add a
  `[[interlocks.pressure]]` table keyed by valve name; the importer must
  resolve descriptions and error on misses.
- **Non-valve preconditions** (Jan furnace `logic.yaml`): actions gated on
  predicates like `funnel_up`, `no_motion`, `no_dump`. One mapping
  contradicts the valve description (`FF: TS` top shutter vs "Lower
  Shutter").
- **Furnace dump sequence** (`dump_sequence.txt`): a tiny script language
  (`open`, `sleep`, `lower_funnel`) that references nonexistent valves. Port
  to the experiment script layer, not config.
- **Expression strings** recur: gauge conversions `IG1=3*v**2-3*v-5`,
  notification triggers `cmp: x>=78`, run monitors `rule= x>1e-5` with
  `parameter=Pressure, Bone, IG`, conditionals `Hub.IG1.pressure>1e-7`.
  Define one small, safe comparison/polynomial grammar and convert at import
  where possible (polynomials to coefficient arrays).
- `system_monitor.cfg [General] sample_delay`, `dashboard.xml` telemetry
  (`func`, `bind`, `period` or `on_change`, `change_threshold`, units), and
  `notification_triggers.yaml` have no home yet. They belong in a
  monitoring/telemetry spec, not instrument config.

### Gauges, ADCs and sensors
- Composite devices: a pyrometer or power meter read through an ADC
  (`adc=OmegaADC`, `[ADC] klass=OmegaADC`, `voltage_scalar`), current loop
  scaled to temperature (`resistance`, 0–0.02 A mapped to 300–2000 °C).
  Model as a sensor with a `source = { driver, channel }` and a conversion.
- Agilent multiplexer channels with `coefficients=1,0,0` polynomials and an
  `[RPC] port`.
- MicroIon, ADC gauge controller (`gaugesetup.txt`), temp/humidity
  microserver (`dm_kind=csv`), DPi32 temperature monitor, ThermoRack: more
  driver kinds to list, not necessarily implement.

### Temperature controllers and PID
- **Gain-scheduled PID tables** in four shapes: SFT/Diode `pid.csv`
  (`Set, Prop, cool, It, Dev, max power %`, header in a `#` comment),
  Diode `pid_large_packet.csv` (`temp, ph, pc, i, d, max_output%`), Eurotherm
  tab-separated `HO,XP,TI,TD` (Jan), and Eurotherm `|`-separated with
  `LB/HB` (Felix, 8 dated variants). Values are not monotonic. Add
  `[[pid_bins]] { setpoint, p, i, d, max_output }` and per-format importers.
- Watlow EZ-Zone over Modbus: `[Setpoint] min/max`, `[MemoryBlock]`,
  `[Output] use_pid_bin`, `scale_high`, `clear_output`.
- Eurotherm serial 7/even (also in CO2 zobs).

### Lasers, stages and motors
- Newport ESP stages: per-axis `xaxis/yaxis/zaxis.cfg` with 40–50 keys
  (limits, velocities, `_home_search_mode`, encoder resolution), plus raw ESP
  command dumps (`1VA3.81`) that disagree with `group_parameters.cfg`. A
  persisted runtime value (`velocity = 2.3685438564654024`) and a typo
  (`master_slace_jog_velocity_scaling_coefficients`) appear; y and z both
  set `slave_axis = 2`. Treat ESP params as a passthrough block, validate
  only limits and units.
- Motors (Snap, Thor, steppers): `steps`/`nsteps`, `sign`, `min`/`max`,
  `nominal_position`, `hysteresis`, `rpm`, `[Homing] home_limit`.
  `home_position` is a 0–1 fraction in one file and mm in another; the
  schema must carry units.
- Laser power calibration: `[PowerOutput] coefficients` (3 terms CO2,
  4 terms diode), key with a space (`power min`).
- Laser monitors: `max_temp`, `max_coolant_temp`, `max_duration`.
- Cameras: pixel-to-mm polynomial, `swap_rb`; `camera.cfg` and
  `camera.yaml` both exist and disagree (zoom coefficient 22.5 vs 31.75;
  typo `swap_rb: falsez`).
- Purdue passes stage positions as a per-run `"x,y"` string and waits with a
  fixed `sleep(10)` instead of a motion-complete check. The experiment spec
  needs a typed position field and an explicit wait.
- Other: Arduino fiber light (`control_module=`), chiller,
  `pid_degasser.yaml`, `[DIO] water_flow_channel`.

### Spectrometer
- **Multiple configurations per instrument**: `configurations/` holds
  `argon.cfg`, `ne.cfg`, `ne_av.cfg`, `argon_2CDD.cfg`, each with its own
  `[Magnet] mftable` and `hv`. Import every `*.cfg` as its own profile
  bound to its own table (spec §7.5 currently maps only `config.cfg`).
- Key spellings drift between labs (`ionrepeller`/`ion_repeller`,
  `operatingvoltage`/`ioncountervoltage`, CamelCase in `argon_2CDD.cfg`).
  Use an alias table.
- mftables: a third fit type `discrete` (`ic_mftable.csv`); tables without a
  fit-type line; columns for detectors that do not exist; isotope keys not
  in `molecular_weights.csv` (`Ar40d`, `CO2`); a non-monotonic value
  (`39.04138` in `argon.fil1.csv`). Validate columns against detectors and
  warn on non-monotonic columns.
- `molecular_weights.csv` is tab-separated and includes pseudo-masses
  (`PM41`, `PM42`, `Ar35`).
- Multiple competing detector files (`detectors.cfg`,
  `detectors_FAC.cfg`, `detectors_atona.cfg`) with different `ic_factor`
  values, and ATONA `_B` detector variants.
- `scan.yaml` (`valves: [A, C, U]`) binds valve events to scan-graph markers;
  often references valves that do not exist.

### Canvas
- More kinds: `rectangle` (`fill`, `border_width`), `rconnection`,
  `elbow` (`corner="ur"`), `gate` and `funnel` (with an alternate
  `<state>open` geometry), `legend` (`llabel`, `lline`, `rect`), `label`
  with `<font>`, `<image>` with scale, and `widget` (`devname`, `funcname`;
  a live device readout).
- `<volume>` on stages and spectrometers (Felix has it twice: 650 and
  3200). Volumes matter for gas-expansion math; give them a home in the
  extraction-line model, not just the canvas.
- Colors: RGBA 4-tuples, `0x0080FF`, and **the name of another element**
  (`<color>AirPipette</color>`).
- Pipette `vlabel` can be a mapping `{text, translation, font}` and uses the
  `n` format type (`{:04n}`).
- Elements nested inside `stage`, `laser` and `tank`; the furnace is drawn as
  a `laser`.
- Hazards: duplicate elements (UAF `LaserInlet` twice, a duplicated
  vconnection, repeated label `NP10`), names with spaces or trailing
  spaces (`F GP-50`, `S1 `), `display_name` colliding with another element's
  `name` (UAF `IPB`), canvas-only valves not in any valve file (Felix `J`,
  UF `A`), and dangling connection ends (`FATurbo`, `12`, `G1`).

### Global
- `initialization.xml` adds `<system master_host=...>` (multi-system locking,
  also `system_locks.cfg`), `<flag>`/`<timed_flag>`, `<processor>`. Locking
  between systems sharing a line (Jan/Felix) is a real requirement for the
  daemon.
- Some Drive copies are simulation/dev setups (WiSCAr: all hardware plugins
  off, junk canvas). Do not build fixtures from them without saying so.

## Suggested next steps
1. Build importer test fixtures from the production labs (skip simulation
   copies, dedupe clones). Include the hazard cases above as negative tests.
2. Extend `TransportKind` (telnet, visa, udp, http), make framing a
   pluggable codec (terminators, length/checksum frames), allow shared
   transports with per-driver framing, serial port globs, and port-collision
   detection.
3. Add valve support for `inverted_logic`, multi-address valves,
   `state_source`, manual valves, `query_state`, and actuation checks.
4. Write a temperature-controller spec (PID bins, sensors via ADC) and an
   extraction-device spec (lasers, stages, motors, furnaces, cameras).
5. Add pressure-trip interlocks and decide on one small expression grammar
   for conditions and conversions.
6. Choose the canvas coordinate model: world units plus a view box is
   recommended. Define precedence among canvas.yaml / canvas.xml /
   valves2D.cfg and among valves.yaml / valves.xml.
7. Treat monitoring, dashboards and notifications as a separate spec.
