# Legacy Pychron config survey (from Google Drive setupfiles)

Surveyed read-only: Argus VI "melbourne" (Jan 2026), NGX (2023, 2025), Quad
(2023), SFT (2023), Reston (NGX + furnace), LDEO (Helix + Agilent, 2022).
Decoded copies were saved to the session scratchpad
(`scratchpad/legacy/{argus_2026,ngx_2023,ngx_2025,quad_2023,sft_2023,reston,ldeo}`). They
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
- Reston's NGX host is in the Columbia University address block (LDEO's
  network). The config was likely cloned from LDEO and the host may be stale.
- Reston enables a `RestonFurnace` plugin, which is another extraction device
  for the extraction-device spec (see Lasers and stages).

## Suggested next steps
1. Build the importer test fixtures from these 7 labs.
2. Extend `TransportKind` (telnet, visa, udp), add framing keys, allow
   shared transports and serial port globs.
3. Add valve support for `inverted_logic`, multi-address valves,
   `state_source`, and manual valves.
4. Write a temperature-controller and extraction-device spec.
5. Choose the canvas coordinate model: world units plus a view box is
   recommended, since it matches legacy files and eases conversion.
