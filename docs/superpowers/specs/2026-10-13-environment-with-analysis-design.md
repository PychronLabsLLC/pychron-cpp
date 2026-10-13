# Environmental readings kept with each analysis

Status: draft 2026-10-13, not agreed. Written by an unattended run; questions
for owner in section 9. Nothing built. Task T032 (`pychron usage notes.md`):
"collect environmental data along with analysis".

## 1. Problem

Lab temperature, humidity and compressed-air pressure explain drifts in
blanks, baselines and detector gains. An analysis measured by pychron-cpp
carries none of them. The place for them exists and is empty:

- store: `analysis_meta.environmental` (jsonb, migration 0001, line 126),
  written from `AnalysisMeta::environmental_json`
  (`libs/persistence/src/ingest.cpp:58`);
- reader: `StoreSource` flattens that JSON's numbers into
  `Analysis::environmentals`
  (`libs/processing/adapters/store/src/store_source.cpp:371`);
- record: event kind `"environment"` is named in
  `libs/experiment` `record/types.hpp:197`; nothing writes one;
- only writer today: the legacy importer (`libs/dvc/src/legacy_record.cpp:347`).

## 2. Legacy (examined 2026-10-13, `~/Programming/pychron`)

- At save, the run asked LabSpy (a web service and its database) for the
  latest `lab_temperatures`, `lab_humiditys`, `lab_pneumatics`
  (`experiment/automated_run/automated_run.py:1909-1925`): each a list of
  `{device, name, value}`.
- Stored whole in the analysis JSON under `environmental`
  (`dvc/dvc_persister.py:820-824`).
- Named picks from those lists: lab temperature and humidity
  (`EnvironmentalMonitor`), air pressure (`AirPressure`), two more
  temperatures (`RPiWeather`), outside temperature (`NOAA`)
  (`experiment/utilities/environmentals.py:11-16`).
- One sample per analysis, taken at the end, from whatever the service last
  heard: its age was not recorded.

## 3. Goal

Each analysis saved by pychron-cpp carries the lab's environmental readings
taken during that run, each with its time, name and units, from devices the
lab already has configured; visible in recall and usable as a quantity in a
time series.

## 4. Design

### 4.1 What is an environmental reading

A named reading of a device already on the line (a temperature controller
channel, a gauge, an analog input), declared in the line's config:

```toml
[[environment]]
name = "lab_temperature"        # key in the record; [a-z0-9_]
device = "lab_monitor"          # a driver of this line
channel = "temperature"         # as that driver names its readings
units = "C"
```

- No new device kind and no web service: a lab without such a device has no
  `[[environment]]` and nothing changes for it.
- `elctl doctor` and line validation refuse an unknown device or channel
  and a duplicate name.

### 4.2 When

Sampled by the run, through the line's existing polling (no extra request
when the device is already polled; the last polled value and its time are
taken):

- at run start, at time zero, at the end of the measurement.
- A reading older than `max_age_s` (per entry, default 300) at the moment
  it is taken is recorded as missing, not as a stale number.

### 4.3 Where

- Record: one event per sample, kind `environment`, detail
  `<name> <value> <units>`, at the sample's time on the line's wall clock
  (`clock.wall_now()`). No change of record version (events exist).
- Store: the store adapter writes `analysis_meta.environmental` as

  ```json
  {"lab_temperature": {"units": "C", "samples": [{"t": "...Z", "at": "start", "value": 21.4}, ...]}}
  ```

  and the reader gives, per name, the value at time zero (else the nearest
  sample) as `Analysis::environmentals[name]`.
- Legacy-imported analyses keep their JSON as imported; the reader already
  flattens it. The two shapes are told apart by the `samples` key.
- No migration: the column exists.

### 4.4 Seen

- Recall: a section "Environment" listing each name, value at time zero,
  units, and the range over the run.
- Quantity `env.<name>` for time-series panels (`quantity.hpp`), so blanks
  can be plotted over lab temperature.

## 5. Rules

- A missing or failing environmental device never fails, delays or flags a
  run: the sample is missing and the run's notes say so once.
- No wait is added to a run for a reading (4.2 takes what polling has).
- Times from the injected clock; a simulated run stamps simulated time.
- Names are the lab's; nothing in the code knows "temperature".

## 6. Out of scope

- A lab-wide environmental log outside analyses (LabSpy's role), dashboards,
  alarms on environment.
- Outside weather from a web service.
- Correcting any measured value by an environmental reading.
- New device drivers for environmental sensors.

## 7. Files (expected; plan confirms)

- Line config parse and validation (`libs/systems` or where
  `extraction_line.toml` is read), `libs/setup` shipped example (one
  commented `[[environment]]`).
- `libs/experiment`: run samples at three points, record events, store
  adapter writes the JSON.
- `libs/processing`: store source reads the new shape; `env.<name>`
  quantity; recall section.
- `docs/user/07-configuration-reference.md`, `docs/user/06-data-analysis.md`.

Read first: `libs/experiment/AGENTS.md`, `libs/setup/AGENTS.md`,
`libs/persistence/AGENTS.md`.

## 8. Tests

- Config: unknown device, unknown channel, duplicate name each refused by
  name.
- Run, simulated lab with a simulated sensor: three samples per entry with
  the right `at`; a stale reading recorded as missing; a failing device
  leaves the run successful with one note.
- Store round trip: saved JSON has the shape of 4.3; `Analysis::environmentals`
  gives the time-zero value; a legacy-shaped JSON still reads as before.
- Time series: `env.lab_temperature` plots one point per analysis.

## 9. Questions for owner

1. Source: devices of the line (drafted), or a service like legacy's LabSpy
   that several instruments share? A shared sensor read by two lines needs
   either two connections or a small server.
2. Three samples a run (drafted), one, or a series at the polling rate for
   the whole run?
3. Which readings matter first at NMGRL (lab temperature, humidity, air
   pressure as legacy), and through which device do they arrive today?
4. Should a run be flagged (not failed) when an environmental reading is out
   of a configured range, or is that a later conditional?
