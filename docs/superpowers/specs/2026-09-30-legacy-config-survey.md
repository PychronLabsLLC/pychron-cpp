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

Appendix A gives the Drive folder ids, the tree layout, the melbourne
reference values and how to re-run the survey (added 2026-10-03).
Appendix B is a file-level pass over every lab's `extractionline/` and
`canvas2D/` folders, with corrections to the sections below (added 2026-10-03).
Appendix C does the same for the `devices/` folders and compares the device
kinds with the drivers pychron-cpp has (added 2026-10-03).

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

## Appendix A: Drive locations and reference values (2026-10-03)

A second read-only pass over the same Drive tree. It adds where the files are
and one worked example. Hosts and IPs are left out, as above.

### A.1 Where the files are

Root: `PychronConsulting/setupfiles`, folder id
`1Al7k47FSWOkfvMUyevhpl-tZkiZayXPy`. It also holds loose `Tube2x.txt` and
`Tube4x.txt` (Jun 2026).

| Lab | Folder id | Notes |
|---|---|---|
| melbourne | `1HiYE_1HLOnXXq6m4jKvFYkVeFffWh2bC` | Jan 2026 |
| asu | `1--mRBD_l8oumYB4rBK3HrXlV1vZehdwN` | `setupfiles/` = `1Gr8qzf9poLGXc4vvlbXY95TOzYUTb0y7`; also `setupfiles_sft/`, `orig/` (copy of wiscar), `scripts/` |
| usgsreston | `1M62KbrfoyyqqvwGsUNtUzBzkOWq_ACwa` | `setupfiles/` = `1NNvbMPf3W-Of4DjPxU4uf5qZAVWAiZCo` |
| usgsdenver | `1zC3A1g600FtmP6c3aNI9afOKUxBC1aA0` | `setupfiles/` plus `gaugesetup.txt` |
| valve | `1pBY5_lZwcSJSRTTAuxB6Nhvz8SOhV9yE` | NMGRL valve box |
| ldeo | `1EofYUk0ONHAekpVZQqXsNiWMSqvwXLB4` | |
| wiscar | `1nQSLba3PRQ97SW57xy03bcSmZJ9uz39g` | |
| hal | `1FlnueM0ssX9v3ayPNjK7AoTqIBzxAR6b` | |
| jan | `1bo3LhIBpLq7SlX07eCXOg-vfB5fSjS_a` | |
| felix | `1qmIKSS5Z4_iW8rg7pIwB46-AQtUeImus` | |
| co2 | `1AqYp9cmighrvHup0cjT5S88wDnjhJyvq` | |
| diode | `1L-oa36ivyMgqMLTXODE80J0nK-wLdZEH` | |
| uf | `1cGyyIQHW7EW4ApH90c4v3mJwCvoWUFE6` | `setupfiles/` plus `original/setupfiles/` |
| uaf | `15N4yuHa_AqvHCXYc08ziT1fRFK8NAN7o` | flat |
| purdue | `11Deqm3lC5ZHxU_BCpcFfckU5aezHFUbr` | flat; `backup/` |
| gsc | `1ilu3fipzw59iDPPTAoYE0xO5YgORaSmH` | irradiation data only |
| uman, ua | `1Gg2b1PGDkwitc613OqTI70bLueRxD7oH`, `13b9z0FxGTkrbGUMltj1-TSQ4uAsGol9n` | empty |

Copies outside the root:

- `PychronConsulting/PychronFolder/Purdue/setupfiles`
  (`1wspdM6GQbv_ZqgM7g_dwA0vwc9pQ4CAa`), a newer Purdue tree, files to Oct 2025.
- `PychronConsulting/PychronFolder/Pychron_copy/setupfiles`
  (`1Nnojzf5Ing-12ZAG1lwHJ1su3YZKXZDt`), initialization from Apr 2025.
- Two older trees whose location was not traced:
  `1gGIaf4GZ1OM90mhTo30I_Up6PjnEwnhC` (2019; NGX, ChromiumCO2, LDEOFurnace, MKS
  gauge) and `1--mA9LM8SGQe5Ypvrn96x63sPZa4tLwx` (2020).

### A.2 Tree layout

```
initialization.xml
devices/            *.cfg, one per device, plus backup/ and per-laser folders
spectrometer/       detectors.yaml, readout.yaml, molecular_weights.csv,
                    scan.yaml, default_conditionals.yaml,
                    mftables/, configurations/, deflection_backup/
extractionline/
canvas2D/
monitors/  blocks/  tray_maps/  irradiation_tray_maps/
incremental_heat_templates/  patterns/  pipeline/
startup_tests.yaml  experiment_defaults.yaml  users.yaml
flux_constants.yaml  system_health.yaml
```

### A.3 melbourne reference values

The newest tree, and the best candidate for an Argus importer fixture.

`devices/`: `spectrometer_microcontroller.cfg` (`name = Argus`, ethernet, TCP,
port 1069, timeout 3); `switch_controller.cfg` (`type=QtegraGPActuator`);
`QtegraGPActuator.cfg` (same endpoint, `test_cmd=GetData`); a stray
`NGXGPActuator.cfg` with an empty host.

`spectrometer/detectors.yaml`, a flow-style list:

| Name | Active | Isotope | Kind | Index | Relative position | Notes |
|---|---|---|---|---|---|---|
| H2 | - | Ar40 | Faraday | 0 | 0.963553562 | entry commented out |
| H1 | yes | Ar39 | Faraday | 1 | 0.981570944 | |
| AX | yes | Ar38 | Faraday | 2 | 1.0 | |
| L1 | no | Ar37 | Faraday | 3 | 1.019457506 | |
| L2 | no | Ar36 | Faraday | 4 | 1.039696524 | |
| CDD | no | Ar35 | IonCounter | 5 | 1.058303063 | deflection sign -1, protection_threshold 0.5 |

`spectrometer/readout.yaml`, first list (CRLF line endings):

| Name | Min | Max | Compare |
|---|---|---|---|
| HighVoltage | 0 | 5 | False |
| ElectronEnergy | 53 | 153 | False |
| YSymmetry | -100 | 100 | True |
| ZSymmetry | -100 | 100 | True |
| ZFocus | 0 | 100 | True |
| IonRepeller | -22.5 | 53.4 | True |
| ExtractionLens | 0 | 100 | True |

The second list names H2, H1, AX, L1, L2, CDD, each `compare: True`.

`spectrometer/mftables/mftable.csv`:

```
parabolic
iso,H2,H1,AX,L1,L2,CDD
Ar40,5.78595,5.89471,6.00644,6.12488,6.25235,6.35914
Ar39,5.67677,5.78567,5.89760,6.01601,6.13507,6.24482
Ar36,5.35165,5.45468,5.56320,5.67181,5.79005,5.89793
```

`spectrometer/configurations/config.cfg`:

```ini
[Default]
eqtime = 15

[SourceParameters]
ion_repeller = -3.81
electron_energy = 75.08

[Trap]
current = 200
ramp_step = 2
ramp_period = 1
ramp_tolerance = 25

[SourceOptics]
y_symmetry = -4.04
z_symmetry = 7.16
z_focus = 48.34
extraction_lens = 25.52

[Deflections]
h2 = 0
h1 = 0
ax = 125
l1 = 250
l2 = 500

[CDDParameters]
ioncountervoltage = 2150

[Protection]
use_beam_blank = False
beam_blank_threshold = 0.1
use_detector_protection = False
detectors = CDD

[Magnet]
mftable = mftable
```

`molecular_weights.csv` carries Ar33, Ar35-Ar41, PM41, PM42.
`deflection_backup/` and `mftables/backup/` are empty.

### A.4 Findings not listed above

- A detector commented out of `detectors.yaml` (melbourne H2) still has an
  mftable column, a readout entry and a `[Deflections]` key. This is the live
  form of the Reston stale-detector case: the importer should warn, not fail.
- `[Deflections]` has no key for the CDD. A missing key means zero.
- `[Magnet] mftable` names the table without its `.csv` extension.
- `detectors.yaml` is flow-style in melbourne and block-style in asu. Fractional
  `index` values (0.3, 1.3) and `ypadding` as a quoted string both occur.
- asu's `detectors.yaml` has ten Helix-named entries (H5..L5) with every
  `serial_id` `'00000'` and repeated isotopes and positions. It reads as an
  untuned template. Do not build an NGX detector fixture from it.
- asu's `readout.yaml` is the fully commented-out case.
- Reston's device cfgs date from 2018; only the spectrometer folder was
  refreshed in 2023.

### A.5 Re-running the survey

With the Drive connector: `search_files` with `parentId = '<id>'` lists a
folder (the first page can return as few as five items; follow
`nextPageToken`). `snippetVerbosity: MEDIUM` returns the text of `.cfg`, `.csv`
and `.xml` files inline. The snippet is a rendering, not the bytes: it drops
XML attributes and wraps some CSV files in HTML, so do not infer file format
from it. `.yaml` files return no snippet and need `download_file_content`,
which returns base64.

## Appendix B: `extractionline/` and `canvas2D/` file survey (2026-10-03)

Every text file in these two folders was read, for every lab, from the local
Drive mirror. Nothing failed to read. Dates are file mtimes; a uniform
"Feb 2022" across a lab is probably the Drive copy date. None of these files
contains a host, IP or credential.

### B.1 Inventory

BK = backup or editor leftover. DUP = byte-identical by hash. Numbers are line
counts.

| Lab | `extractionline/` | `canvas2D/` |
|---|---|---|
| asu (2023) | valves.yaml 47 | canvas.yaml 262; canvas_config.xml 20 |
| melbourne (2026) | valves.yaml 39, CRLF | canvas.yaml 250; canvas_config.xml 41; alt_config.xml 18 (2013) |
| usgsreston (2023) | valves.yaml 21 | canvas.yaml 434; canvas.yaml.bak BK; canvas_config.xml 38 |
| uaf (2023, flat) | valves.yaml 25 | canvas.yaml 257; no canvas_config |
| ldeo | valves.yaml 214; backup/valves.xml 200 BK | canvas.xml 566; canvas.xml~ BK; canvas_config.xml 10 |
| hal | valves.yaml 35 | canvas.yaml 124; canvas.yaml.bak BK; "canvas copy.yaml" (stub); canvas.xml 173; ~canvas.xml BK, not well-formed; canvas_config.xml 38 |
| uf (2021) | valves.yaml 36 (hal's file) | canvas.yaml 376; canvas.yaml.bak BK; canvas.xml 25 (2-valve stub); canvas_config.xml 10; alt_config.xml DUP |
| uf/original | valves.yaml 51 (one live entry) | canvas.yaml 376 (3 lines differ from uf); rest DUP of uf |
| valve | valves.xml 184; ~valves.xml 64 BK (2016, pre-furnace); zobs/ with valves.txt and three copies, valve_groups.txt, section_definitions.cfg (2010-2013) | canvas.xml 870 (2021); canvas_92121.xml 870; canvas_prefurnace.xml 568; canvas_config.xml 33; canvas_config_prefurnace.xml 39; alt_config.xml 32; two BK; zobs/ with four older canvases, valves2D.cfg, valves2D.txt |
| usgsdenver | valves.xml DUP of valve; valves.yaml 35 (2022, Denver only); zobs/ DUP of valve | canvas.yaml 239 (2022); canvas_config.xml DUP of valve; nmgrl/ = nine files, each DUP of valve's; zobs/ DUP of valve |
| jan | valves.xml 162; furnace_valves.xml 45; ~valves.xml BK | canvas.xml 547; canvas_config.xml 26; alt_config.xml 25; dumper.xml 64; two BK; zobs/ |
| felix | no folder | canvas.xml 746; canvas_config.xml 32; alt_config.xml 24; dumper.xml (DUP of jan); two BK; zobs/ |
| diode | empty | canvas.xml 307; canvas-new.xml 305; canvas.xml~ BK; valves2D.cfg |
| wiscar | valves.xml 54 | canvas.xml 24; canvas_config.xml 36 |
| co2 | empty | camera.cfg only |

Binary, not read: `zobs/base*.jpg` and `zobs/canvas.elc` (a Python pickle of
designer valve objects) in valve and usgsdenver. `canvas3D/` exists in valve
and usgsdenver (one XML plus a `zobs/` of fourteen `*3D.cfg` files) and is
empty in co2 and diode.

### B.2 Valve files

**valves.yaml**: a flat list of mappings.

| Key | Examples | Labs |
|---|---|---|
| `name` | `A1`, `Tank A`, unquoted ints `0`..`11` | all |
| `address` | `105`; `101,102`; `PIV`; `Valve 1_9 Set`; `'101'` | all; absent on manual valves and pipettes |
| `kind` | `valve`, `manual_valve`, `pipette`, `double_actuation_valve` | asu, melbourne, ldeo, denver; absent means valve |
| `description` | free text | most |
| `interlock` | scalar (asu, melbourne); list (ldeo) | 3 labs |
| `inverted_logic` | `True` (one with a trailing space) | asu |
| `actuator` | a device name | asu, reston, ldeo |
| `state_device` | `{name, address}` | ldeo |
| `inner`, `outer` | valve names | pipettes |

**valves.xml** (valve/denver, jan, wiscar, ldeo backup): `<root>` with optional
`<group>NAME`, then `<valve>`, `<switch>`, `<manual_valve>`, `<pipette>`; the
name is the element's leading text. Attribute `query_state`. Children
`address`, `description`, `interlock`, `actuator`, `check_actuation_delay`
(0.75, 1, 2.75, 4), `check_actuation_enabled`, `ignore_lock_warning`,
`inverted`, `inner`, `outer`. LDEO's backup uses `<state_device>` as a string
with a separate `<state_address>`.

**Older text forms** (valve `zobs/`): `valves.txt` is CSV whose first data line
(`H,Q`) names the mass-spec and quad inlet valves, then rows
`NAME,ADDR,INTERLOCKS,DESC,query_state,section[,actuator]`;
`valve_groups.txt` is one group per line; `section_definitions.cfg` is INI with
`[Section-X] components=` and `testN=VALVE,n,state-label`, a system-state
definition.

No explanation, procedure or separate pipette files exist in any lab.

| File | Valves | Switches | Manual | Pipettes | Groups | Interlocked | Actuators named |
|---|---|---|---|---|---|---|---|
| asu | 6 (1 double-actuation) | 0 | 10 | 1 | - | 2 | 1 |
| melbourne | 9 | 0 | 3 | 1 | - | 2 | 0 |
| ldeo | 18 | 0 | 6 | 3 | - | 6 | 2 + 1 state device |
| usgsreston | 7 | 0 | 0 | 0 | - | 0 | 1 (dangling) |
| uaf | 12 | 0 | 0 | 0 | - | 0 | 0 |
| hal = uf | 12 | 0 | 0 | 0 | - | 0 | 0 |
| denver valves.yaml | 11 (all address 1) | 0 | 5 | 1 | - | 0 | 0 |
| valve valves.xml | 33 | 0 | 7 | 2 | 5 | 4 | 4 |
| valve ~valves.xml | 13 | 10 | 4 | 2 | 0 | 4 | 0 |
| jan valves.xml | 24 (none has an address) | 0 | 5 | 2 | 4 | 4 | 0 |
| jan furnace_valves.xml | 0 | 8 | 0 | 0 | 0 | 0 | 0 |
| wiscar | 13 | 0 | 0 | 0 | 0 | 0 | 1 (a class name) |

Every interlock pair is symmetric in every file, and every pipette
`inner`/`outer` resolves to a valve.

### B.3 Canvas files

**canvas.yaml**: a mapping of kind to a list of elements. Common keys `name`,
`translation` (an `"x,y"` string), `dimension`, `color`, `display_name`,
`border_width`, `use_symbol`. Files saved by the designer (hal, uf) also write
`fill`, `display_name: null`, RGBA colours, two-decimal floats, empty lists
(`laser: []`) and the empty kinds `switch` and `manualswitch`. Connections:
`connection`, `hconnection`, `vconnection` with `start`/`end` as
`{name[, offset]}`; `tee_connection` with `left`/`mid`/`right`; `elbow` with
`start`/`end`. `offset` is `"5,5"` or the empty string.

**canvas.xml**: the same kinds as tags, name as leading text; children
`translation`, `dimension`, `color`, `volume`, `font`; attributes
`display_name`, `border_width`, `use_symbol`, `fill`, `use_border`.
`<connection orientation="horizontal|vertical">`; with no orientation it is a
straight line. `offset` is an attribute on `<start>`/`<end>`/`<left>`/`<right>`.
Also `rconnection`, `elbow` (`corner="ur"`), `tee_connection`, `legend`.

**canvas_config.xml / alt_config.xml**: `origin`, `xview`, `yview`,
`<color tag="...">`, `valve_dimension`, `connection_dimension`, `font`,
`image` with `translation` and `scale`. Colour tags: `bgcolor`, `gauge`,
`getter`, `label`, `ionpump`, `spectrometer`, `tank`, `turbo`,
`roughing_inner_border_color`.

**dumper.xml** (felix = jan): `switch` with `<association>`, `gate` and
`funnel` with a `<state>open` geometry, `label`.

**valves2D.cfg**: INI, `[General] window_width/height`, then
`[Valve-X] pos=x,y` in pixels. **valves2D.txt**: CSV
`name,default_state,x,y`. `camera.cfg` in these folders is video calibration,
not canvas.

View boxes (world units, y up):

| Lab | origin | xview | yview |
|---|---|---|---|
| asu | 0,0 | -28,28 | -40,25 |
| melbourne | 0,0 | -50,50 | -50,50 |
| usgsreston | 5,0 | -40,40 | -40,40 |
| ldeo | 0,-7 | -21,24 | -20,20 |
| hal, felix, wiscar | 0,0 | -45,35.5 | -40,40 |
| denver, valve | 0,0 | -50,35.5 | -40,40 |
| jan, uf | 0,0 | -28,28 | -25,25 |
| uaf | none | none | none |

Colour forms: `r,g,b`; `r, g, b`; RGBA; names; `0xFF8000`; `'#FF9967'`;
floats 0-1 (`1,0.5,0`); another element's name.

Element counts in the main canvas file:

| Kind | asu | melb | uaf | reston | denver | valve | ldeo | jan | felix | uf | diode |
|---|---|---|---|---|---|---|---|---|---|---|---|
| valve | 6 | 9 | 12 | 7 | 11 | 30 | 18 | 23 | 27 | 8 | 24 |
| rough_valve | 0 | 0 | 0 | 0 | 0 | 3 | 0 | 1 | 2 | 0 | 1 |
| manual_valve | 10 | 3 | 6 | 11 | 5 | 8 | 6 | 1 | 3 | 11 | 0 |
| stage | 6 | 1 | 5 | 4 | 5 | 7 | 14 | 3 | 7 | 5 | 4 |
| spectrometer | 1 | 1 | 1 | 1 | 1 | 3 | 1 | 3 | 3 | 1 | 3 |
| ionpump | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 0 |
| turbo | 2 | 1 | 1 | 0 | 3 | 5 | 2 | 3 | 5 | 1 | 2 |
| laser | 2 | 2 | 1 | 1 | 2 | 3 | 2 | 2 | 4 | 2 | 0 |
| tank | 1 | 1 | 1 | 1 | 1 | 2 | 3 | 2 | 2 | 1 | 0 |
| pipette | 1 | 1 | 1 | 1 | 1 | 2 | 3 | 2 | 2 | 1 | 0 |
| getter | 2 | 2 | 3 | 6 | 2 | 7 | 3 | 4 | 4 | 2 | 0 |
| gauge | 2 | 0 | 0 | 0 | 0 | 5 | 4 | 3 | 5 | 0 | 0 |
| connections | 32 | 18 | 30 | 33 | 32 | 76 | 55 | 50 | 66 | 33 | 35 |

In jan's canvas, 20 valves, 27 connections, 2 turbos and a spectrometer are
nested inside `<stage>`; more are nested inside `<laser>` and `<tank>`.

### B.4 Valve file against canvas

| Lab | In valve file, not on canvas | On canvas, not in valve file | Dangling connection ends | Other |
|---|---|---|---|---|
| asu | 0 | 0 | 0 | clean |
| melbourne | 0 | 0 | 0 | clean |
| ldeo | 3 pipettes (`Tank A/B/C`) | 3 (`PipetteA/B/C`) | 0 | naming mismatch only |
| usgsreston | 0 | 12 (M1-M11, AirPipette) | 0 | `ngx_switch_controller` has no cfg |
| uaf | 0 | 6 | 0 | canvas `LaserInlet` twice |
| valve | 2 pipettes | 3 | 0 | one address used twice, on different actuators |
| jan | 6 | 2 | 0 | no actuator cfg |
| denver yaml | 0 | 0 | 0 | address `1` used 11 times (draft) |
| uf | 5 | 13 | 0 | valve file is hal's |
| hal xml | 0 | 0 | 2 (`12`, `G1`) | yaml and xml canvases disagree |
| wiscar | 13 (all) | 4 | none at all | placeholder canvas |

### B.5 Fixture value

- **asu** and **melbourne** are clean pairs: no mismatch, no dangling end. asu
  exercises more schema (double actuation, `inverted_logic`, actuator
  override, manual valves, `gauge`, `elbow`, `offset`); melbourne has Qtegra
  string addresses, an interlock, a pipette and a `vlabel`.
- **ldeo** is the best XML-canvas fixture and the only one with `state_device`.
- **valve** is the stress case: groups, four actuators, `rconnection`, a
  legend, names with spaces, 870 lines.
- Not fixtures: wiscar (placeholder names), the denver YAML pair (a draft),
  uf (borrowed valve file, stub XML), hal (two canvases that disagree),
  diode (a 2012 layout), usgsdenver `nmgrl/` and every `zobs/` (copies).

### B.6 New findings

- The kind string is exactly `double_actuation_valve`.
- jan's `valves.xml` has no `<address>` on any valve: the name is the address.
  Its furnace valves are in a separate `furnace_valves.xml`, as `<switch>`.
- `<switch>` is also used for non-valve Qtegra outputs: getter degas/operate
  and ion-gauge enable (valve `~valves.xml`).
- Pre-XML formats exist and may still be met in old trees: `valves.txt`,
  `valve_groups.txt`, `section_definitions.cfg`, `valves2D.txt`, and a pickled
  `canvas.elc`. The importer should name them in its "unsupported" message.
- The typo `<xvidew>` is in the live melbourne `alt_config.xml` and in diode's
  canvas. A reader that ignores unknown tags silently loses the x view box.
- Older `canvas.xml` files carry `origin`, `xview` and `<color tag>` inside
  the canvas file itself, not in `canvas_config.xml`.
- New config keys: `valve_dimension`, `connection_dimension`, `font`.
- New colour forms: `'#RRGGBB'` and 0-1 floats.
- New YAML shapes: `offset: ''`, empty kind lists, kinds `switch` and
  `manualswitch`, a connection-level `dimension`, a named connection.
- hal's `canvas.xml` uses `hconnection`/`vconnection` as XML tags.
- LDEO's `canvas.xml` puts each name on its own line; names need trimming.
- YAML `elbow` (asu) has no `corner`.
- melbourne's `canvas.yaml` carries commented-out UAF and Denver elements: it
  was cloned from UAF.
- uaf has no `canvas_config.xml`; its view box must be derived or defaulted.
- Element nesting is deep in the NMGRL canvases (valves and connections inside
  `stage`, `laser`, `tank`).

### B.7 Corrections to the sections above

- "Canvas-only valves not in any valve file (Felix `J`)": felix has no
  `extractionline/` folder; `J` is defined in valve's `valves.xml`. Felix's
  canvas should be checked against the valve box's file.
- The double `<volume>` on spectrometer `Felix` is in valve's `canvas.xml` as
  well as felix's.
- "`valve` = USGS Denver minus a 2022 draft" is confirmed by hash. Denver's
  NMGRL canvases sit under `canvas2D/nmgrl/`; its top-level canvas is the YAML
  draft.
- UF is listed among labs shipping two formats that disagree. Its `canvas.xml`
  is a two-valve stub and its `valves.yaml` is hal's file, so UF is not
  evidence for a precedence rule.

## Appendix C: `devices/` file survey (2026-10-03)

Every text file in every lab's `devices/` tree was read from the local Drive
mirror; `backup/` and `zobs/` files were hashed and read only where they differ
from every live file. Hosts, IPs and serial numbers are left out. Device
classes come from the file where it names one, otherwise from `<klass>` in the
lab's `initialization.xml`.

### C.1 Which folders are real

| Folder | Status |
|---|---|
| melbourne | Own files, 2025-2026, CRLF. Argus over TCP. One stray `NGXGPActuator.cfg` with an empty host |
| asu/setupfiles | Own files, 2023. NGX plus an Agilent actuator on VISA-USB |
| ldeo | Own files. Twelve cfgs, three-way actuator indirection, UDP |
| usgsdenver | Widest device mix (25 cfgs), 2015-2022 |
| valve | Byte-identical to usgsdenver, all 76 files |
| co2, diode | Laser boxes. Each has a live laser folder and a dated snapshot (`fusions_co2_033120`, `fusions_diode_port3`); 13 files in each snapshot match the live folder |
| felix | Top level, `co2/`, `diode/`, `furnace/`, `furnace/1/` (a snapshot). `diode/` is 16/17 identical to `co2/` |
| jan | Mostly felix's files: `co2/` 16/17, `diode/` 17/17, `furnace/1/` 14/14 |
| hal | NGX files from wiscar, cryostat files from ldeo (port name included); only the two LabJack switch files are its own |
| wiscar = asu/orig | Identical; three NGX files |
| usgsreston | NGX files identical to wiscar's (2018) |
| uaf | Three flat NGX cfgs |
| purdue | uaf's three files plus reston's Agilent file plus a Kinesis cfg |
| uf/original | Three NGX cfgs on a loopback endpoint (development) |
| uf/setupfiles | Empty |
| asu/setupfiles_sft | No `devices/`; a Watlow cfg and `pid.csv` at the root, both diode's |

One NGX actuator file, by hash, sits in asu/orig, hal, wiscar and usgsreston;
all four point at one endpoint. `newport_parameters/` is identical in co2,
diode, usgsdenver and valve.

### C.2 Device catalogue

| Legacy class or file | What it is | Labs | Transport |
|---|---|---|---|
| NGXGPActuator, ngx_valve_controller | Valves through the NGX | asu, wiscar, hal, usgsreston, uf/orig, uaf, purdue | TCP 1099 |
| spectrometer_microcontroller (`name= NGX`) | NGX link | same | TCP 1099 |
| spectrometer_microcontroller (Argus/Helix) | Qtegra remote control | melbourne (TCP); ldeo, felix (UDP) | 1069 |
| QtegraGPActuator and named variants | Valves through Qtegra | melbourne (TCP); ldeo (UDP); usgsdenver (no `kind`) | 1069 |
| QtegraGaugeController, QtegraDevice | Values read through Qtegra | ldeo; usgsdenver | 1069 |
| AgilentGPActuator | Agilent switch unit | asu (VISA-USB); usgsreston, purdue, usgsdenver, ldeo (serial) | VISA or serial |
| AgilentMultifunction | Valve state readback | ldeo | serial |
| ArduinoGPActuator | Arduino valves | usgsdenver | serial 115200 |
| NMGRLFurnaceActuator | Furnace firmware valves | usgsdenver, felix, jan | TCP 4567 |
| PychronGPActuator | Valves through another Pychron | felix | TCP 1061 |
| U3Actuator | LabJack actuator and readback | hal | USB and serial |
| MicroIonController | Granville-Phillips gauge, three on one bus | usgsdenver | serial 19200, bus address |
| PychronMicroIonController | Gauge read through the furnace host | usgsdenver | TCP 4567 |
| XGS600GaugeController | Multi-gauge controller | ldeo | serial 9600 |
| Model335TemperatureController | Cryostat | ldeo (hal is a copy) | serial 9600 7/odd |
| WatlowEZZone | Temperature controller | diode, asu-sft, felix, jan | Modbus RTU |
| NMGRLFurnaceEurotherm | Furnace temperature | felix, jan | TCP 4567; serial 7/even |
| DPi32TemperatureMonitor | Temperature monitor | usgsdenver; diode, felix, jan | serial |
| ProXRADC + Pneumatics | ADC and air-pressure sensor | usgsdenver | serial 115200 |
| OmegaADC | ADC behind the pyrometer and power meter | diode, felix, jan | serial 9600, address |
| TempHumMicroServer | Room temperature and humidity | usgsdenver | TCP 2000 |
| ThermoRack; PychronChiller | Chiller | usgsdenver; co2, diode | serial 9600; TCP 1061 |
| UPS | UPS monitor | usgsdenver | serial 1200 |
| Fusions logic board, zoom and beam motors | Laser controller | co2, diode (serial 19200); felix (TCP 1063, 8000) | |
| Newport ESP stage, axes, group, profiler, joystick | Motion | co2, diode (serial 19200); felix, jan (TCP 8000) | |
| Chromium `stage.cfg` | Stage limits and signs only | asu, melbourne | none |
| VueMetrix / diode control module | Diode module | diode, felix, jan | serial 115200 |
| Pyrometer | Pyrometer | diode, felix, jan | serial 9600 even |
| Fiber light + Arduino module | Illumination | co2, diode | serial 115200 |
| Furnace feeder, funnel, dumper, magnets | Furnace drives | felix, jan | TCP 4567 |
| Agilent multiplexer | ADC channels | felix, jan | serial, plus `[RPC]` |
| APIS controller | Pipette system client | felix, jan | TCP 1057 |
| Unidex | Motion | felix, jan | serial 9600 |
| Kinesis controller | Thorlabs stage service | purdue | HTTP |

Only in `zobs/` or `backup/`: an RPC chiller, a GPIB stage, six Modbus bakeout
controllers, pump controllers, the ATL UV laser, a UDP diode server.

### C.3 Transport keys as used

| Transport | Keys and values |
|---|---|
| Ethernet | `type` = ethernet / Ethernet; `kind` = TCP / UDP / **absent**; `host` an address, a DHCP hostname, `localhost`, or empty; `timeout` 1, 2, 3, 12; `write_terminator` CRLF or `chr(10)`; `read_terminator` CRLF; `use_end`; `strip`; `verbose`; `test_cmd`; `scheduler` |
| Serial | `port` as a Keyspan name, `usbserial-<id>`, `usbmodem<digits>`, bare `usbmodem`, bare `usb`; `baudrate` 1200 to 115200 or absent; `bytesize=7`; `parity` odd / even / EVEN; `stopbits`; `timeout`; `terminator` CRLF or ETX with `terminator_position=-2`; `read_delay=0.05`; `scheduler` gauges / agilent / Agilent |
| Modbus RTU | `type=modbus`, `slave_address=01`, `port`, `baudrate` 9600 or 38400 |
| VISA-USB | `type=visa`, `board`, `manufacture_id`, `model_code`, `serial_number`, `usb_interface_number` |
| USB | `type=USB`, `port=usbmodem` |
| HTTP | `[General] base_url`, quoted |

No file sets a retry count. No Modbus TCP and no telnet appear in any
`devices/` folder. GPIB appears only in a `zobs/` file.

### C.4 References between files

| Pattern | Example |
|---|---|
| `[General] type=<X>`: the comms are in `<X>.cfg` | `switch_controller` to `AgilentGPActuator`; furnace controller, feeder, funnel; pneumatics to the ADC |
| `[General] name=<file>` + `klass=<Class>` | ldeo `switch_controller_*` |
| `[ADC] klass=` and `[General] adc=` | power meter and pyrometer monitor to `OmegaADC` |
| `control_module=` | fiber light to the Arduino module |
| `[Motors] zoom=zoom_motor.cfg` | laser controller |
| `[Optional] joystick=, group=` (no extension) | stage controller |
| `scheduler=<name>` | a shared lock name, not a file |

Dangling or mismatched:

- uf/setupfiles and jan list `switch_controller` in `initialization.xml` with
  no cfg.
- felix and jan name the file `stagecontroller.cfg`; every other lab and the
  init files say `stage_controller`.
- diode's init lists device `control`; the file is
  `vue_metrix_controlmodule.cfg`.
- hal's init lists a temperature controller, a pyrometer, a stage controller
  and gauge controllers; none has a cfg.
- diode has two `OmegaADC.cfg` files with different ports.
- purdue's `AgilentGPActuator.cfg` and melbourne's `NGXGPActuator.cfg` are
  unreferenced. purdue's setup notes name `stage_controller.cfg`; the file is
  `kinesis_controller.cfg`.

### C.5 Shared endpoints

| Lab | Devices on one endpoint | Framing agrees |
|---|---|---|
| every NGX lab | NGX actuator and spectrometer link | yes, except uaf and purdue (`use_end` differs) |
| ldeo | Qtegra actuator, Qtegra gauge reader, spectrometer link | only the spectrometer sets a timeout |
| melbourne | Qtegra actuator and spectrometer link | yes |
| usgsdenver | three gauge controllers on one serial bus, by address | yes |
| usgsdenver | furnace actuator and a gauge controller on the furnace host | yes |
| felix, jan furnace | actuator, temperature controller, feeder, funnel, magnets, dumper | yes |
| felix, jan lasers | Unidex (9600) and the diode module (115200) on one serial port | **no** |
| felix, jan lasers | OmegaADC (9600) and the temperature monitor (19200) on one serial port | **no** |

### C.6 Coverage in pychron-cpp

Registered driver kinds: `proxr_relay`, `pfeiffer_maxigauge`, `gp_microion`,
`thermo_qtegra`, `isotopx_ngx`, `ngx_valves`, `dac_positioner`, `serial_hv`,
`adc_bank`, `pulse_counter`, and five `sim_*`. `TransportKind` has Serial, Tcp,
ModbusRtu, ModbusTcp, Sim and Link; `libs/transport` implements serial, tcp,
sim and link.

| Legacy kind | C++ driver | Labs (clones counted once) |
|---|---|---|
| NGX valves | `ngx_valves` | 8 |
| NGX spectrometer | `isotopx_ngx` | 8 |
| Qtegra spectrometer | `thermo_qtegra`; two of three labs use UDP | 3 |
| MicroIon gauge | `gp_microion` | 1 |
| Agilent switch-unit actuator | none | 5 |
| Watlow EZ-Zone | none | 4 |
| Fusions logic board and motors | none | 4 |
| Newport ESP stage | none | 4 |
| Qtegra valve actuator | none (the Qtegra driver has no valve code) | 3 |
| Furnace firmware actuator | none | 3 |
| Pyrometer, OmegaADC, diode module | none | 3 |
| Model335 cryostat | none | 1 real, 1 copy |
| Furnace drives, Eurotherm, multiplexer, APIS, Unidex | none | 2 |
| Chiller, fiber light, Chromium stage | none | 2 |
| Fourteen others | none | 1 each |

Missing transports: UDP (ldeo, felix), VISA-USB (asu), USB/LabJack (hal), HTTP
(purdue). Modbus RTU is in the enum without an implementation. Two C++ drivers
have no counterpart in these folders: `proxr_relay` (ProXR appears only as an
ADC) and `pfeiffer_maxigauge`.

By lab count, the next drivers worth writing are the Agilent actuator, then the
Qtegra valve actuator, which with UDP completes the three Qtegra labs.

### C.7 New parsing hazards

- Ethernet with no `kind=` (several usgsdenver, felix and purdue files). The
  importer must not assume TCP; require the user to state it.
- Files with no `[General]` section.
- Empty values: `host=`, `test_cmd =`.
- A duplicated section (`[Channel03]` twice) in one `zobs/` file.
- Bare-CR line endings with quoted fields in one Eurotherm table; `|` and tab
  separators for the same table kind in one folder.
- A trailing tab after a port value.
- Quoted values (`valve='C,U'`, `get_func='temp1'`).
- Blank lines between keys; no final newline.
- `.txt` setup files that are positional (`port,baud`), and
  `newport_parameters/*.txt` that are raw controller commands.
- A file whose extension lies (`.py` holding rendered HTML text).
- `clear_output` inside `[Communications]`; `[Communications]` with no `type`.
- Scheduler names differing only in case.

### C.8 Fixture value

- **Best**: melbourne, ldeo, asu/setupfiles.
- **Good but dated**: usgsdenver (widest mix; every `type=` reference
  resolves), diode `fusions_diode/` (a complete laser set).
- **With caveats**: co2 `fusions_co2/` (two camera files disagree), felix
  (furnace complete; port collisions; many dated PID variants).
- **Not fixtures**: valve, wiscar, asu/orig, hal, usgsreston, purdue, jan,
  asu-sft (clones or assembled), uf (development or empty), and the dated
  snapshot folders.

### C.9 Corrections to the sections above

1. "Model335 ... 7/odd/GPIB": the cryostat file is plain serial 9600 7/odd.
   GPIB occurs only in a `zobs/` stage file.
2. `message_frame=L4,-,C4` is commented out in every device file. It is not
   live configuration.
3. The serial-port collision between a temperature monitor and the Omega ADC
   is in the felix and jan laser folders, not in live diode, where the
   colliding line is commented out.
4. The `PychronGPActuator` cfg, and the `switch_controller` that names it, are
   in felix. jan has no `switch_controller` cfg. The files do not show which
   side is the client.
5. Lascon, the telnet ion-pump controllers and the IGC100 do not appear in any
   `devices/` folder surveyed here; those findings came from other folders or
   labs.
6. UDP for Qtegra is live in felix as well as ldeo.

### C.10 Not read

One zip archive in diode (a 2012-2019 copy of the diode laser folder) was
listed, not extracted. One `zobs/` script holds a plaintext credential; it is
not reproduced here and was reported to the owner.
