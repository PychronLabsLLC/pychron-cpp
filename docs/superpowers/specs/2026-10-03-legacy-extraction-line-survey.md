# Legacy extraction lines: survey and importer

Surveyed read-only on 2026-10-03 from `PychronConsulting/setupfiles` on Drive
(folder ids in `2026-09-30-legacy-config-survey.md`, appendix A). As with the
earlier surveys, no lab file was copied into the repo: the importer's tests
use synthetic files in the same formats.

Files read: melbourne `extractionline/valves.yaml`, `canvas2D/canvas.yaml`,
`canvas2D/canvas_config.xml`, `devices/switch_controller.cfg`,
`devices/QtegraGPActuator.cfg` (Argus VI, Jan 2026); an NMGRL
`extractionline/valves.xml` (34 valves, five actuators) and the matching
older `canvas2D/canvas.xml` and `valves2D.cfg`; LDEO's numeric-name
`valves.yaml`; actuator cfgs from ASU, Purdue, UAF, Reston, WiSCAr and NMGRL.

## 1. What a legacy line is made of

| Piece | Where | Formats seen |
|---|---|---|
| Valves, manual valves, pipettes, interlocks | `extractionline/` | `valves.yaml` (list of maps), `valves.xml` (`<valve>`, `<manual_valve>`, `<pipette>`, `<group>` nesting) |
| Drawing | `canvas2D/` | `canvas.yaml` (one list per element kind), `canvas.xml` (one element per kind tag), `valves2D.cfg` (INI, pixel `pos` per `[Valve-X]`) |
| View box, colours | `canvas2D/canvas_config.xml` | `<origin>`, `<xview>`, `<yview>`, `<color tag=...>`, `<valve_dimension>` |
| Actuators | `devices/<actuator>.cfg` | INI; `[General] type=<Class>` pointing at `devices/<Class>.cfg`, or `[Communications]` in the file itself |

A valve names its actuator by device name (`<actuator>furnace_switch_controller`);
without one it uses `switch_controller`.

### Valves

- YAML keys: `name`, `address`, `description`, `interlock` (one name or a
  list), `kind` (`valve` default, `manual_valve`, `pipette` with
  `inner`/`outer`), `actuator`.
- XML: the element text is the name (`<valve query_state="false">FD<address>…`),
  children `address`, `description`, `interlock`, `actuator`,
  `check_actuation_enabled`, `check_actuation_delay`; attribute
  `query_state`. `<manual_valve>NAME</manual_valve>` and
  `<pipette>Air<inner>Z</inner><outer>Y</outer></pipette>` are siblings;
  `<group>name …</group>` only groups.
- Addresses are whatever the actuator understands: relay numbers (`312`),
  Qtegra output names (`Valve 1_9 Set`), single letters (`FD`). Names can be
  numbers (`'209'`).
- Interlocks are written on both valves of a pair (melbourne I↔J).
- Commented-out valves are common, in both formats, including a malformed
  XML comment (`<!--<<address>312</address>-->`).

### Canvas

- World units, y up. A valve's `translation` is its centre; a box's
  (stage, spectrometer, turbo, ...) is its lower-left corner, with
  `dimension: 8,3` its size: melbourne's 55-wide stage `S1` at `-26,0` is
  what every valve from x -25 to 25 connects to. The view box comes from `canvas_config.xml`
  (`xview -50,50`) or the canvas file itself (`<xvidew>`: misspelt in a real
  file, so the reader accepts both).
- Element kinds: `valve`, `manual_valve`, `rough_valve`, `stage`,
  `spectrometer`, `turbo`, `ionpump`, `getter`, `laser`, `tank`, `pipette`,
  `label`, and connections `connection` (XML, `orientation=` attribute),
  `hconnection`, `vconnection`, `tee_connection` (`left`/`mid`/`right`).
- Connections reference elements that are commented out or never drawn;
  connection ends can carry an `offset`.
- `pipette` carries a `vlabel` with a Python format string (`Shots={:04d}`).
- `valves2D.cfg` is the oldest form: pixel positions only, no connections.

### Actuators

| Legacy class | Seen at | Communications | pychron-cpp |
|---|---|---|---|
| NGXGPActuator | ASU, Purdue, UAF, Reston, WiSCAr | TCP, port 1099, `invert=True` | `ngx_valves` |
| QtegraGPActuator | melbourne | TCP `localhost:1069` | none yet |
| AgilentGPActuator | ASU, NMGRL | serial 19200, or VISA USB | none yet |
| ArduinoGPActuator | NMGRL | USB serial | none yet |
| NMGRLFurnaceActuator | NMGRL | (subsystem) | none yet |

## 2. The importer

`setup::import_legacy_line(folder)` (libs/setup) reads a setupfiles folder
(or its `extractionline/` and `canvas2D/` parents) and returns
`extraction_line.toml`, `canvas.toml` and a report of what it read and what
it could not carry over. Its output is loaded with the real loaders, and the
canvas checked against the line, before it is returned.

Precedence, reported: `valves.yaml` over `valves.xml`; `canvas.yaml` over
`canvas.xml` over `valves2D.cfg`.

Mapping:

- Valves, manual valves and pipettes map one to one; interlocks are kept
  (both directions, as written).
- Each actuator becomes a driver and a transport named after the legacy
  device. NGXGPActuator becomes `ngx_valves` on the legacy host and port.
  Every other class becomes a `sim_valves` driver (any address, state kept in
  memory) on a simulated transport, with the legacy class and endpoint in a
  comment: the line loads and runs in simulation, and the comment says what
  driver is still needed. An actuator whose cfg is missing gets the same
  stand-in and a warning; it never falls back to another controller.
- Not carried over, and listed in the report: `invert`, `query_state`,
  `check_actuation_*`, connection-end offsets, pipette `vlabel` formats,
  colours other than the background, images, and gauges (legacy gauge
  controllers are not imported yet: gauge elements are left off the canvas).
- The canvas is rescaled from world units to pixels (y flipped) over the
  view box; elements a connection names but nobody draws, and canvas valves
  the valve file does not have, are dropped and reported.

Wired in as `elctl import-line <folder> [--out DIR]`, and as a third choice on
the setup wizard's Extraction line page ("Convert a legacy Pychron setupfiles
folder"), which plans the converted files like any other and shows the report
on the Ready page.
