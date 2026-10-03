# Legacy Pychron setupfiles survey

A survey of the real lab configuration trees (`setupfiles`) from the Python
Pychron, as stored on Google Drive. Surveyed read-only on 2026-10-03. Use it to
pick fixtures and to know which format variations a loader has to accept.

Nothing here was copied into the repo; the files stay on Drive. Folder ids are
given so they can be found again.

## Where the files are

Root: `PychronConsulting/setupfiles`, folder id
`1Al7k47FSWOkfvMUyevhpl-tZkiZayXPy`.

| Lab | Folder id | Instrument / role | Date |
|---|---|---|---|
| melbourne | `1HiYE_1HLOnXXq6m4jKvFYkVeFffWh2bC` | Argus VI via Qtegra, extraction line; ChromiumCO2 present but disabled | Jan 2026 |
| asu | `1--mRBD_l8oumYB4rBK3HrXlV1vZehdwN` | NGX, ChromiumUV. Subfolders `setupfiles/` (`1Gr8qzf9poLGXc4vvlbXY95TOzYUTb0y7`), `setupfiles_sft/`, `orig/`, `scripts/` | 2023 |
| usgsreston | `1M62KbrfoyyqqvwGsUNtUzBzkOWq_ACwa` | NGX, RestonFurnace. `setupfiles/` = `1NNvbMPf3W-Of4DjPxU4uf5qZAVWAiZCo` | 2023 |
| usgsdenver | `1zC3A1g600FtmP6c3aNI9afOKUxBC1aA0` | Extraction-line / valve box only; `gaugesetup.txt` | 2022 |
| valve | `1pBY5_lZwcSJSRTTAuxB6Nhvz8SOhV9yE` | NMGRL valve box; usgsdenver derives from it | 2021 |
| ldeo | `1EofYUk0ONHAekpVZQqXsNiWMSqvwXLB4` | NGX / Helix / HelixSFT, ThermoFurnace, diode laser client, Agilent switches, XGS600 gauge, Model335 cryostat | 2022 upload |
| wiscar | `1nQSLba3PRQ97SW57xy03bcSmZJ9uz39g` | Argus, NMGRLFurnace, Fusions UV/Diode/CO2. The "everything" template; `asu/orig` is a copy | 2022 upload |
| hal | `1FlnueM0ssX9v3ayPNjK7AoTqIBzxAR6b` | Argus/NGX/Helix listed, U3 actuator, cryostat, Fusions lasers | 2022 upload |
| jan | `1bo3LhIBpLq7SlX07eCXOg-vfB5fSjS_a` | NMGRL Argus experiment box; lasers as TCP clients; NMGRLFurnace | 2022 upload |
| felix | `1qmIKSS5Z4_iW8rg7pIwB46-AQtUeImus` | NMGRL Helix (initialization not read) | 2022 upload |
| co2 | `1AqYp9cmighrvHup0cjT5S88wDnjhJyvq` | NMGRL FusionsCO2 laser box | 2022 upload |
| diode | `1L-oa36ivyMgqMLTXODE80J0nK-wLdZEH` | NMGRL diode laser box (initialization not read) | 2022 upload |
| uf | `1cGyyIQHW7EW4ApH90c4v3mJwCvoWUFE6` | Extraction line only; plus `original/setupfiles/` | 2021 |
| uaf | `15N4yuHa_AqvHCXYc08ziT1fRFK8NAN7o` | Flat NGX files: valves.yaml, canvas.yaml, actuator / switch / microcontroller cfg | 2022-23 |
| purdue | `11Deqm3lC5ZHxU_BCpcFfckU5aezHFUbr` | Flat NGX files, Agilent actuator, Kinesis controller + setup notes | 2022-23 |
| gsc | `1ilu3fipzw59iDPPTAoYE0xO5YgORaSmH` | Irradiation data only, no configs | Jun 2026 |
| uman, ua | `1Gg2b1PGDkwitc613OqTI70bLueRxD7oH`, `13b9z0FxGTkrbGUMltj1-TSQ4uAsGol9n` | Listing returned nothing | - |

Other copies outside the root:

- `PychronConsulting/PychronFolder/Purdue/setupfiles`
  (`1wspdM6GQbv_ZqgM7g_dwA0vwc9pQ4CAa`), a newer Purdue tree, files to Oct 2025.
- `PychronConsulting/PychronFolder/Pychron_copy/setupfiles`
  (`1Nnojzf5Ing-12ZAG1lwHJ1su3YZKXZDt`), initialization from Apr 2025.
- Two older trees whose location was not traced:
  `1gGIaf4GZ1OM90mhTo30I_Up6PjnEwnhC` (2019; NGX, ChromiumCO2, LDEOFurnace, MKS
  gauge) and `1--mA9LM8SGQe5Ypvrn96x63sPZa4tLwx` (2020).
- `Pfieffer_QuadPVMassSpec` folders and 2015-2017 source dumps. Old, low value.

## Tree layout

A full lab tree has:

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

## melbourne (Argus VI, Jan 2026)

The newest tree and the only one that is consistent end to end.

`devices/`:

| File | Content |
|---|---|
| `spectrometer_microcontroller.cfg` | `name = Argus`; ethernet, TCP, `localhost:1069`, timeout 3 |
| `switch_controller.cfg` | `type=QtegraGPActuator` |
| `QtegraGPActuator.cfg` | same endpoint, `test_cmd=GetData`, timeout 3 |
| `NGXGPActuator.cfg` | stray; empty host, port 1099 |

`spectrometer/detectors.yaml` is a flow-style list:

| Name | Active | Isotope | Kind | Index | Relative position | Notes |
|---|---|---|---|---|---|---|
| H2 | - | Ar40 | Faraday | 0 | 0.963553562 | entry commented out |
| H1 | yes | Ar39 | Faraday | 1 | 0.981570944 | |
| AX | yes | Ar38 | Faraday | 2 | 1.0 | |
| L1 | no | Ar37 | Faraday | 3 | 1.019457506 | |
| L2 | no | Ar36 | Faraday | 4 | 1.039696524 | |
| CDD | no | Ar35 | IonCounter | 5 | 1.058303063 | deflection sign -1, protection_threshold 0.5 |

Other keys per detector: `color` (hex string), `deflection_name`,
`deflection_correction_sign`, `serial_id`, `software_gain`, `use_deflection`,
`ypadding` (a quoted string, `'0.1'`).

`spectrometer/readout.yaml` is two YAML lists. The first is source parameters
with `name`, `min`, `max`, `compare`:

| Name | Min | Max | Compare |
|---|---|---|---|
| HighVoltage | 0 | 5 | False |
| ElectronEnergy | 53 | 153 | False |
| YSymmetry | -100 | 100 | True |
| ZSymmetry | -100 | 100 | True |
| ZFocus | 0 | 100 | True |
| IonRepeller | -22.5 | 53.4 | True |
| ExtractionLens | 0 | 100 | True |

The second lists detectors H2, H1, AX, L1, L2, CDD, each `compare: True`. The
file has CRLF line endings.

`spectrometer/molecular_weights.csv` is plain text, name and mass separated by a
space: Ar33 33.5, Ar35 35, Ar36 35.9675, Ar37 36.9668, Ar38 37.9627,
Ar39 38.964, Ar40 39.9624, Ar41 40.962, PM41 41, PM42 42.

`spectrometer/mftables/mftable.csv`:

```
parabolic
iso,H2,H1,AX,L1,L2,CDD
Ar40,5.78595,5.89471,6.00644,6.12488,6.25235,6.35914
Ar39,5.67677,5.78567,5.89760,6.01601,6.13507,6.24482
Ar36,5.35165,5.45468,5.56320,5.67181,5.79005,5.89793
```

Line 1 is the fit kind, line 2 the header.

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

`deflection_backup/` and `mftables/backup/` listed empty.

## asu (NGX, 2023)

`devices/`:

| File | Content |
|---|---|
| `spectrometer_microcontroller.cfg` | `name= NGX`; ethernet, TCP, `NGX-019.istb4.dhcp.asu.edu:1099`, timeout 12, `write_terminator=CRLF`, `read_terminator=CRLF`, `use_end=False`, `strip=False`, `verbose=True` |
| `NGXGPActuator.cfg` | same communications block; `invert=True` under `[General]` |
| `ngx_valve_controller.cfg` | `type=NGXGPActuator` |
| `switch_controller.cfg` | `type=AgilentGPActuator` |
| `AgilentGPActuator.cfg` | `type=visa`, USB (`board`, `manufacture_id`, `model_code`, `serial_number`, `usb_interface_number`); a serial transport is commented out |

`spectrometer/`:

- `detectors.yaml`: block-style list of ten entries named H5, H4, H3, H2, H1,
  AX, L2, L3, L4, L5. Every `serial_id` is `'00000'`, colors are integers,
  isotopes and relative positions repeat, indexes are fractional (0.0, 0.3,
  1.0, ...), and L4 has `kind: CDD` with `software_gain: 1.6e-08`. It reads as
  an untuned template, not a real NGX calibration.
- `readout.yaml`: entirely commented out.
- `scan.yaml`: `valves: [A, C, U]`, the valves that add a marker to the scan
  graph.
- `default_conditionals.yaml`: sections `actions`, `cancelations`,
  `equilibrations`, `modifications`, `post_run_terminations`,
  `pre_run_terminations`, `terminations`, `truncations`. One real rule, a
  cancelation for `unknown` and `air`: `teststr: Ar40.bs_corrected>60000.0`,
  `attr: Ar40`, `frequency: 3`, `ntrips: 3`. The rest are empty placeholders.
- `molecular_weights.csv`: an Excel HTML export, not CSV. Carries Ar35-Ar40,
  PM41, PM42, Ne20, Ne21, Ne22.

## usgsreston (NGX)

- Device cfgs date from 2018 and point at `129.236.40.241:1099` with
  `write_terminator=chr(10)`. That subnet matches the LDEO config, so the tree
  was inherited from LDEO and is probably stale.
- `AgilentGPActuator.cfg`: `type=serial`, `port=usbmodem`, `timeout=1`.
- `spectrometer/` yaml files are copies of asu's: same sizes and timestamps.
  Only `detectors.yaml` was downloaded and compared; it is identical.

## What a loader has to accept

- `mftable.csv` has a one-line preamble (fit kind) before the header.
- `molecular_weights.csv` may be space-separated text or an Excel HTML export.
- Detector `kind` is `Faraday`, `IonCounter` or `CDD`.
- Detector `color` is a hex string or an integer.
- Detector `index` may be fractional.
- `detectors.yaml` is flow-style or block-style, and may have entries commented
  out.
- A detector missing from `detectors.yaml` can still appear in `mftable.csv`,
  `readout.yaml` and `[Deflections]` (melbourne H2).
- `[Deflections]` keys are lowercase; detector names are uppercase. CDD has no
  deflection key.
- `[Magnet] mftable` names the table without its `.csv` extension.
- Terminators are spelled `CRLF` or `chr(10)`.
- Files may have CRLF line endings.
- `readout.yaml` can be fully commented out, i.e. parse to nothing.
- Booleans appear as `True`/`False`/`true`/`false`.

## Fixture value

- melbourne: use for Argus. Complete and current.
- asu: use for NGX communications cfg. Do not trust its detectors.
- usgsreston: adds only the `chr(10)` terminator variant.

## Limits

- Lab-level table: only `initialization.xml` was read, through search snippets.
  Except for melbourne the snippets drop `enabled=` attributes, so the table
  shows plugins a lab lists, not plugins it enables.
- felix, diode, uman, ua contents unverified.
- Not opened: asu and usgsreston `mftables/`, `configurations/`, `ZOBS/`; every
  `devices/backup/`, `chromium*/`, `furnace/`; all `extractionline/`,
  `canvas2D/` and script folders.

## Re-running the survey

With the Drive connector: `search_files` with `parentId = '<id>'` lists a
folder (the first page can return as few as five items; follow
`nextPageToken`). Adding `snippetVerbosity: MEDIUM` returns the text of `.cfg`,
`.csv` and `.xml` files inline. `.yaml` files come back without a snippet and
need `download_file_content`, which returns base64.
