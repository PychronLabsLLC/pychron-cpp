# Observability: Prometheus metrics and Grafana dashboards

Date: 2026-10-08
Status: Implemented
Owner: Jake Ross
Builds on: `2026-09-29-instrument-control-design.md` (Qt-free core,
`SignalBus`, `Scheduler`), `2026-10-01-logging-design.md` (whose non-goals,
remote shipping and alarms on logs, this spec partly takes up),
`2026-10-06-virtual-clock-design.md` (simulated time).

## 1. Intent

Each lab has one box, provided by PychronLabs, on the lab network. It already
runs Prometheus and Grafana. This spec makes `pychron-ui` a scrape target for
that box and ships the dashboards and the one alert that go with it.

The box should answer three questions:

- **Instrument health**, for lab staff: pressures, temperatures, heaters,
  valve states, device communication errors.
- **Run and queue operations**, for the lab manager: what the queue is doing,
  how runs end, how long each phase takes, which conditionals trip.
- **Application health**, for the developer supporting the lab: version,
  transports, error-log rate, scheduler failures, how long saving takes.

Decisions taken with the owner:

| Decision | Choice | Reason |
|---|---|---|
| Transport | Prometheus pull over the lab network | The box and the instrument computer share a network. No push gateway, no cloud, no credentials. |
| Library | Our own registry and an asio endpoint | No new dependency on four CI toolchains. The text format is small and stable. `asio` is already linked. |
| Run data | Prometheus only | One data path. No database role on the box, and SQLite labs get the same dashboards. |
| Alerting | Dashboards and one dead-man alert | The application's own alarms and notifications stay the alert path. The dead-man covers the one failure the application cannot report: itself. |
| Which process | `pychron-ui` only | It holds the devices and runs the queues. |
| Default | Off | Opening a port is the lab's decision. |

Not in scope: log shipping; a Postgres datasource in Grafana; alert rules for
pressures, failed runs or communication errors; fleet or usage analytics
across labs; TLS or authentication on the endpoint; an endpoint in `elctl`;
a preferences pane.

## 2. Architecture

A new library, `libs/metrics`: Qt-free, depending on `libs/core` and `asio`
only. Built by default; `-DPYCHRON_METRICS=OFF` drops it and its wiring.

| Unit | What it does | Depends on |
|---|---|---|
| `Registry`, `Counter`, `Gauge`, `Histogram` | Holds metric families keyed by name and label set. `render()` returns the Prometheus text format, version 0.0.4. Accepts collect callbacks that run at scrape time. | nothing |
| `MetricsServer` | An asio acceptor on its own thread. Serves `GET /metrics` from `Registry::render()`. | `Registry`, asio |
| `CoreExporter` | Subscribes to the core bus events and updates metrics. | `Registry`, `SignalBus` |
| `SchedulerMetrics` | A collector over `Scheduler::job_stats()` and the heartbeat job. | `Registry`, `Scheduler` |

Metrics for the experiment system live in `libs/experiment`: `ExperimentMetrics`,
and `MetricsService`, which builds and owns everything `[metrics]` turns on
(the registry, the three exporters, the server). The service needs no Qt, so
it is tested with GoogleTest on every platform and the application only
calls it.

Metrics for the experiment system live in `libs/experiment`
(`ExperimentMetrics`), not in `libs/metrics`: the dependency runs
`experiment -> metrics -> core`, and `libs/metrics` never learns an
experiment type.

```
drivers, systems, executor --publish--> SignalBus --> CoreExporter, ExperimentMetrics --> Registry
Scheduler::job_stats() <--read at scrape-- collect callback ---------------------------> Registry
the box's Prometheus --GET /metrics--> MetricsServer --render()--> Registry
```

### 2.1 Registry

```cpp
namespace pychron::metrics {

using Labels = std::vector<std::pair<std::string, std::string>>;  // name -> value

class Counter   { public: void inc(double by = 1.0); void set_total(double total); };
class Gauge     { public: void set(double v); void inc(double by = 1.0); void dec(double by = 1.0); };
class Histogram { public: void observe(double v); };

class Registry {
 public:
  // The same name and labels return the same series. A name registered as
  // another type, or an invalid name, returns a series that is not rendered
  // and is counted in pychron_metrics_dropped_series_total.
  Counter&   counter(std::string_view name, std::string_view help, const Labels& labels = {});
  Gauge&     gauge(std::string_view name, std::string_view help, const Labels& labels = {});
  Histogram& histogram(std::string_view name, std::string_view help,
                       std::vector<double> buckets, const Labels& labels = {});
  void remove(std::string_view name, const Labels& labels);  // a series that no longer applies
  // Names a family before its first series exists, so names() lists it.
  void declare(MetricType type, std::string_view name, std::string_view help, std::vector<double> buckets = {});

  // Runs on the scraping thread at the start of every render().
  using Collector = std::function<void(Registry&)>;
  [[nodiscard]] CollectorHandle add_collector(Collector c);  // RAII, like SignalBus::Subscription

  // Called once per family, outside the registry's lock, when it first refuses a series for being full.
  void on_family_full(std::function<void(std::string_view family)> handler);

  std::string render();  // runs the collectors, so it is not const
  std::vector<std::string> names() const;  // family names, for the packaging test
};

}  // namespace pychron::metrics
```

- A series, once returned, is updated with atomics only. The lookup that
  finds or creates it takes the family's mutex. Exporters keep the reference
  for series they know in advance and look up the rest per event.
- References stay valid for the registry's life (series are never moved).
  `remove` hides a series from `render()`; it does not free it.
- `Counter::set_total` exists for sources that already count
  (`TransportHealth::error_count`, `JobStats`). The counter rises by the
  increase since the last total. A total smaller than the last one means the
  source started again from zero: the counter rises by the new total and
  never falls.
- A family holds at most 1000 series. Beyond that a new label set gets the
  unrendered series and a count in `pychron_metrics_dropped_series_total`,
  and `on_family_full` is called once for the family; `MetricsService` logs
  one `warn` from it. The registry itself logs nothing: it depends on
  nothing, and a record logged under its lock would come back in through the
  `Log` event. This bounds a labelling bug.
- A label value known in advance (an enumeration) has its series created at
  zero, so a rate over it is zero rather than no data.
- `render()` sorts families by name and series by labels, so output is
  deterministic. `# HELP` and `# TYPE` appear once per family. Label values
  escape `\`, `"` and newline. Non-finite values are written `NaN`, `+Inf`,
  `-Inf`.

### 2.2 Rules for exporters

- Bus handlers run synchronously on the publishing thread: a scheduler
  worker, a transport, the log writer. A handler never blocks and allocates
  only when it first sees a label set.
- Label values come only from configured names (gauge, valve, heater,
  transport, job) and from enumerations. A run id, a sample identifier, an
  error message or any other free text is never a label value.
- Free text that must be classified (`ExecutorWaiting::reason`) maps to a
  fixed set, with `other` for anything unrecognised.
- **Three clocks, none compared with another.**
  - A duration between two events is the difference of their `ts`, on the
    line's `Clock`. In a simulation it is simulated seconds, which is what a
    dashboard of a simulated queue should show.
  - An age (of a reading, of the scheduler's heartbeat, of the last run,
    of the process) is real time, measured on this computer's steady clock
    and worked out at the scrape by a collector. Exporters take that clock
    as a `RealClock` (`std::function<double()>`, seconds) so tests can move
    it.
  - The box's clock is never involved. The first design exported
    timestamps for PromQL to subtract from `time()`; that makes the alert
    depend on the box and the instrument computer agreeing what time it is,
    and a lab network cut off from the internet has no time server. An
    instrument computer 90 s slow would have fired the dead-man alert two
    minutes into every queue.
- The exposition carries no timestamps: Prometheus stamps each sample at the
  scrape.
- A reading whose device has stopped answering keeps its last value, which
  would mislead on a dashboard. Every sampled source therefore also has a
  `pychron_last_sample_age_seconds` series.
- A counter is created at zero as soon as what it counts is known: at
  construction for enumerations, and on first sight of a configured name (a
  valve's failures when the valve is first reported, a source's alarms when
  it is first read, a component's five levels at its first record, a
  channel's outcomes at its first notification). A counter that first
  appears at 1 has no earlier sample, so `increase()` over it is zero and
  the first event, often the only one, would not be drawn. What cannot be
  named in advance (an alarm from a source that is never read) keeps that
  gap, and the guide says so.

### 2.3 Lifetime

`Registry` is declared before the exporters and the server and outlives
them. On shutdown: subscriptions and collector handles are reset, the server
is stopped and its thread joined, then the registry is destroyed. The bus
does not wait for a handler that is mid-call when its subscription goes, so
the service is destroyed only once nothing publishes: after the line has
stopped. The heartbeat job, which `Scheduler::cancel` does not wait for
either, shares ownership of the one value it writes. The server
thread is not a participant in the line's clock and never waits through it.

`apps/pychron-ui/src/main.cpp` calls `MetricsService::start` before the line
starts (so the start-up `Snapshot` and each transport's first
`TransportHealth` reach it) and resets it after the line stops. Disabled, it
returns null and nothing is constructed.

### 2.4 One addition to `Scheduler`

`Scheduler` reports statistics by `JobId` only. The collector needs every
job, so one method is added; nothing else in the scheduler changes:

```cpp
struct NamedJobStats { std::string name; JobStats stats; };
std::vector<NamedJobStats> job_stats() const;  // thread-safe; a snapshot
```

One-shot jobs (`after`) are left out: their names are not a bounded set.

## 3. Metric catalog

Prefix `pychron_`. Counters end in `_total`. Prometheus adds `job` and
`instance`; the scrape configuration adds `instrument`.

### 3.1 Instrument health (`CoreExporter`)

| Metric | Type | Labels | Source |
|---|---|---|---|
| `pychron_pressure` | gauge | `gauge`, `unit` | `PressureSample` |
| `pychron_temperature_kelvin` | gauge | `source`, `input` | `TemperatureSample` |
| `pychron_heater_readback` | gauge | `heater` | `HeaterSample`; removed when nullopt |
| `pychron_heater_setpoint` | gauge | `heater` | `HeaterSample`; removed when nullopt |
| `pychron_heater_enabled` | gauge, 0 or 1 | `heater` | `HeaterSample`; removed when nullopt |
| `pychron_valve_state` | gauge, 0 or 1 | `valve`, `state` = `open`, `closed`, `unknown` | `ValveChanged`; seeded from `Snapshot` |
| `pychron_valve_transitions_total` | counter | `valve` | `ValveChanged` whose state differs from the last seen |
| `pychron_actuation_failures_total` | counter | `valve`; `unknown` for a name the line never reported | `ActuationFailed` |
| `pychron_last_sample_age_seconds` | gauge | `kind` = `pressure`, `temperature`, `heater`; `source` | each sample event; real seconds since, at the scrape |
| `pychron_alarms_total` | counter | `source`, `severity` | `Alarm` |

A pressure keeps the unit its gauge is configured with, named in the `unit`
label. Converting in the exporter would disagree with what the operator sees
on the canvas. For a temperature, `source` in the age series is
`<source>/<input>`.

`ActuationFailed` is also published for a switch that does not exist, under
whatever name a script gave: only a name seen in a `Snapshot` or a
`ValveChanged` becomes a label.

### 3.2 Run and queue operations (`ExperimentMetrics`)

| Metric | Type | Labels | Source |
|---|---|---|---|
| `pychron_executor_state` | gauge, 0 or 1 | `state`: `idle`, `preparing`, `running`, `stopping`, `cancelling`, `aborting`, `finalizing` | `ExecutorStateChanged` |
| `pychron_queue_active` | gauge, 0 or 1 | none | 1 from the first state that is not `Idle`; 0 on `QueueEnded` or a return to `Idle` |
| `pychron_queue_runs` | gauge | `status` = `total`, `done` | `QueueStarted` and `QueueEdited` (size); `RunFinished` (done) |
| `pychron_queues_ended_total` | counter | `end` = `completed`, `stopped`, `cancelled`, `aborted`, `failed` | `QueueEnded` |
| `pychron_runs_started_total` | counter | none | `RunStarted` |
| `pychron_runs_finished_total` | counter | `state` = `success`, `failed`, `cancelled`, `aborted`; `truncated` = `true`, `false` | `RunFinished` |
| `pychron_run_save_errors_total` | counter | none | `RunSummary::save_error` |
| `pychron_run_state_duration_seconds` | histogram | `state`, the non-terminal `RunState` names but `pending` (the wait in the queue) | time between a run's consecutive `RunStateChanged::ts` |
| `pychron_measurement_blocks_total` | counter | `block`, `ok` | `BlockFinished` |
| `pychron_conditional_trips_total` | counter | `kind`, `level` | `ConditionalTripped` |
| `pychron_executor_waits_total` | counter | `reason` = `delay`, `scheduled_start`, `extraction_device`, `pump_time`, `other` | `ExecutorWaiting` |
| `pychron_last_run_finished_age_seconds` | gauge | none | `RunFinished`; real seconds since, at the scrape; absent until a run finishes |
| `pychron_notifications_total` | counter | `channel`, `event`, `ok` | `NotificationSent` |

A label value that names an enumeration is the application's own name for
it (`to_string`), with anything but `a-z`, `0-9` and `_` turned to `_`: the
block `baseline.after` is `block="baseline_after"`.

`pychron_run_state_duration_seconds{state="saving"}` is how long the record
and the store took: no instrumentation of the persister is needed. Buckets,
in seconds: 1, 2.5, 5, 10, 30, 60, 150, 300, 600, 1200, 2400, 3600, 7200.

`ExperimentMetrics` keeps, per live run id, its last state and `ts`; the
entry is erased when the run reaches a terminal state, so the map is bounded
by the runs in flight (two, with overlap). The run id is a key in that map
and never a label.

A queue that nothing edits publishes no `QueueEdited`, so the size at the
start comes from a new event, published by `LabSession` on the line's bus
just before the executor runs and declared beside `QueueEnded` in
`lab/session.hpp`:

```cpp
struct QueueStarted {
  std::size_t rows = 0;      // QueueSpec::runs.size()
  std::size_t from_row = 0;  // the row the queue starts at
};
```

`total` is `rows - from_row` and `done` returns to 0. `QueueEnded` also
empties the per-run map, so a run cut off by an abort leaves nothing behind.

### 3.3 Application health

| Metric | Type | Labels | Source |
|---|---|---|---|
| `pychron_build_info` | gauge, always 1 | `version`, `os`, `compiler` | constants |
| `pychron_process_uptime_seconds` | gauge | none | real seconds since start, at the scrape |
| `pychron_log_records_total` | counter | `level`, `component` | `Log`; `component` is the logger name up to its first dot |
| `pychron_transport_connected` | gauge, 0 or 1 | `transport` | `TransportHealth` |
| `pychron_transport_outages_total` | counter | `transport` | `TransportHealth` going from up to down |
| `pychron_scheduler_job_runs_total` | counter | `job` | `Scheduler::job_stats()` |
| `pychron_scheduler_job_failures_total` | counter | `job` | the same |
| `pychron_scheduler_job_skipped_overlaps_total` | counter | `job` | the same |
| `pychron_scheduler_heartbeat_age_seconds` | gauge | none | real seconds since a scheduler job (every 5 s) last ran, at the scrape |
| `pychron_metrics_scrape_duration_seconds` | gauge | none | `MetricsServer`, the last `render()` |
| `pychron_metrics_bad_requests_total` | counter | none | `MetricsServer` |
| `pychron_metrics_dropped_series_total` | counter | none | `Registry` |

`TransportHealth` is published when a transport's state changes, and its
`error_count` is the failures in a row at that moment, not a running total.
So the events say when a transport is down and how often it went down; a
count of errors needs the transport to keep and publish one, which is
deferred (section 3.4).

A record is counted only if it reaches the bus, so the `trace` and `debug`
counts follow the configured levels. `warn` and `error` are the ones the
dashboard shows.

### 3.4 Deferred

- **Blank and air intensities as gauges.** `RunFinished` carries neither the
  analysis type nor the fitted intercepts, so this needs a new event or a
  wider `RunSummary`. A follow-up of its own.
- **A count of transport errors.** The transport would have to keep a running
  total and publish it on every failure.
- True scheduler lag (due time against start time). `JobStats` does not have
  it; `skipped_overlaps` stands in.
- Process CPU, memory and file descriptors. Three platform-specific
  implementations for numbers that `node_exporter` or `windows_exporter` on
  the instrument computer report better.
- Spectrometer signals, laser power and vision. They publish no bus events
  yet; each gets metrics when it does.

## 4. Endpoint and configuration

### 4.1 Configuration

A `[metrics]` table in `extraction_line.toml`, parsed beside `[logging]` in
`libs/core/src/config/loader.cpp` with the same diagnostics:

```toml
[metrics]
enabled = false      # the default; with no table, metrics are off
bind = "0.0.0.0"     # an interface address; "127.0.0.1" for this computer only
port = 9464
```

```cpp
struct MetricsConfig : Located {
  bool enabled = false;
  std::string bind = "0.0.0.0";
  std::int64_t port = 9464;
};
```

An unknown key, a `bind` that is not an IP address, or a port outside
1..65535 is a diagnostic with file and line. `MetricsConfig` lives in
`libs/core` (`config/metrics_config.hpp`) so the loader needs nothing from
`libs/metrics`.

### 4.2 HTTP

| Request | Response |
|---|---|
| `GET /metrics` | 200, `Content-Type: text/plain; version=0.0.4; charset=utf-8`, the rendered registry |
| `GET /healthz` | 200, `ok` |
| `GET` of another path | 404 |
| another method | 405 |
| headers over 8 KiB, or no complete request within 5 s | the connection is closed |

- Every response carries `Connection: close`: one request per connection.
- At most 8 connections at once; another is closed on accept.
- No compression, no TLS, no authentication.
- A query string is ignored (`/metrics?x=1` is `/metrics`).
- The server has its own thread and `io_context`. It touches neither the
  scheduler's pool nor the UI thread.

```cpp
class MetricsServer {
 public:
  struct Options { std::string bind = "0.0.0.0"; std::uint16_t port = 9464; };
  // Binds and listens before returning; a failure is an Error, never an abort.
  static Result<std::unique_ptr<MetricsServer>> start(Registry&, Options);
  std::uint16_t port() const;  // the bound port (Options::port 0 asks the OS for one)
  ~MetricsServer();            // closes connections, joins the thread
};
```

### 4.3 Failure

A failure of metrics leaves the application without metrics and affects
nothing else.

| Failure | Behaviour |
|---|---|
| Bind or listen fails | One `error` log record and one `Alarm` (Warning, source `metrics`); the application runs without the endpoint |
| Invalid `[metrics]` | A configuration diagnostic; metrics off |
| A handler throws | `SignalBus` already swallows it |
| A client is slow, oversized or sends garbage | The connection is closed and counted; nothing is logged, so a port scanner cannot fill the log |
| An exception on the endpoint's thread | That request is lost; the thread goes on serving |
| Too many series in a family | Section 2.1 |
| Shutdown during a scrape | The connection is aborted; the thread is joined |

### 4.4 Security

The endpoint is read-only. It exposes operational numbers and the configured
names of devices: pressures, valve states, run counts, transport names. It
exposes no sample identifier, project, user name or credential. Anyone on
the lab network who can reach the port can read it. A lab that finds that
too open sets `bind` to the address of the interface facing the box, or
firewalls the port to the box's address. It is off until the lab turns it
on.

### 4.5 User interface

None, beyond one `info` log record at start (`metrics: listening on
0.0.0.0:9464`). Configuration is TOML only, as for logging.

## 5. What ships for the box

```
packaging/observability/
  prometheus/pychron.scrape.yml
  grafana/provisioning/dashboards/pychron.yml
  grafana/provisioning/alerting/pychron-deadman.yml
  grafana/dashboards/instrument-health.json
  grafana/dashboards/run-operations.json
  grafana/dashboards/app-health.json
docs/observability.md
```

### 5.1 Scrape job

```yaml
- job_name: pychron
  scrape_interval: 15s
  static_configs:
    - targets: ["INSTRUMENT_PC:9464"]
      labels: { instrument: "INSTRUMENT_NAME" }
```

One entry in `static_configs` per instrument computer.

### 5.2 Dashboards

Each has an `instrument` variable and takes its datasource from a variable,
so the same file loads on any box.

| Dashboard | Panels |
|---|---|
| Instrument health | Pressures (log axis); cryostat temperatures; heater readback against setpoint; valve state timeline; age of the last sample per source; alarm rate by severity; actuation failures |
| Run operations | Executor state timeline; queue progress; runs by outcome; run phase durations (median and 95th percentile per state); conditional trips; queue endings; time since the last run finished; notifications sent and failed |
| Application health | Version and uptime; transports connected and their error rate; log records by level and component; scheduler job failures and skipped overlaps; duration of the saving phase; save errors; scrape duration |

### 5.3 Dead-man alert

The box has no Alertmanager, so the alert is a Grafana-managed rule,
provisioned from YAML. It fires on either condition, after two minutes:

1. The application is gone: the scrape fails and a queue was active at the
   last scrape that succeeded.

   ```
   up{job="pychron"} == 0
     and on(instance) last_over_time(pychron_queue_active[24h]) == 1
   ```

   `last_over_time`, because with `max_over_time` over a short window the
   alert would resolve by itself once the old samples aged out, with the
   application still dead.

2. The application answers but its scheduler has stopped. The metrics thread
   is independent, so a stuck scheduler still answers scrapes.

   ```
   pychron_queue_active == 1
     and pychron_scheduler_heartbeat_age_seconds > 60
   ```

In the provisioning file Grafana fills `${PYCHRON_PROM_UID}` in from its
environment and leaves a template's `{{ $labels.instrument }}` as it is. A
doubled dollar is not an escape there: tried on Grafana 12.1, `$$labels` was
stored with both dollars and the message did not render.

Known limits, stated in the guide: quitting within one scrape of a queue's
end leaves the last reading at "running", and the first rule fires. And a
queue stuck inside one run while the scheduler keeps ticking is not caught. Catching it needs a per-lab bound on
the longest sane run, which belongs to later alert rules.

The rule goes to Grafana's default contact point. Who receives it is the
lab's choice and is set in Grafana, following the guide.

### 5.4 Guide

`docs/observability.md`, written for the lab manager in the manner of
`docs/notifications.md`: turn `[metrics]` on; check with
`curl http://INSTRUMENT_PC:9464/metrics`; add the scrape job and reload
Prometheus; copy the provisioning files and dashboards and restart Grafana;
set the contact point; test the alert by quitting the application during a
simulated queue. It ends with the metric reference and section 4.4.

## 6. Testing

GoogleTest, one file per component.

| File | Covers |
|---|---|
| `tests/metrics/test_registry.cpp` | Counter, gauge and histogram semantics; a label set is a series; a name reused with another type; `set_total` across a reset of its source; cumulative buckets with `+Inf` equal to `_count`; the series cap; updates from several threads sum exactly; a collector runs at each render and stops when its handle is reset |
| `tests/metrics/test_render.cpp` | Golden text: `# HELP` and `# TYPE` once per family; escaping; `NaN` and the infinities; ordering; a removed series is absent |
| `tests/metrics/test_server.cpp` | A real socket on `127.0.0.1`, port 0: `/metrics` with its content type, `/healthz`, 404, 405, a query string, oversized headers, a slow client, the ninth connection, shutdown with a connection open, a bind failure returned as an error |
| `tests/metrics/test_core_exporter.cpp` | Each core event published on a real `SignalBus`, then the rendered series; `Snapshot` seeds valves; nullopt heater fields remove their series; the logger name is cut at its first dot; timestamps come from the injected real-time source |
| `tests/metrics/test_scheduler_metrics.cpp` | Job counters, two jobs with one name, a job replaced, the heartbeat |
| `tests/integration/test_metrics_packaging.cpp` | Every `pychron_` name in a dashboard or alert query is a name the exporters register, and every exported metric is on a dashboard; where a JSON library is built, the dashboards' structure |
| `tests/experiment/test_metrics_service.cpp` | Disabled builds nothing; enabled serves what the bus says; a port in use is one alarm and the application runs on; a full family is logged once |
| `tests/experiment/test_experiment_metrics.cpp` | One scripted queue: runs that succeed, fail, truncate and fail to save, then `QueueEnded`. Counters, `queue_active` going 1 then 0, state durations from a `ManualClock`, the wait-reason mapping, the per-run map empty at the end |
| `tests/core/test_config.cpp`, extended | `[metrics]`: defaults, each field, each diagnostic |
| `tests/core/test_scheduler.cpp`, extended | `job_stats()` |
| `tests/integration/test_lab_session.cpp`, extended | One simulated queue with the service attached, scraped over the socket: the runs are counted, and no label value is a run id or an identifier; scraping throughout a queue does not disturb it; `QueueStarted` comes before the first run |

`test_metrics_packaging.cpp` is the guard against drift: rename a metric
without its dashboard and the test fails. It starts a `MetricsService`,
collects the names it registers and compares them with the names found by a
regular expression in the files under `packaging/observability/`. Histogram
suffixes (`_bucket`, `_sum`, `_count`) are stripped before the comparison.

### 6.1 The virtual box

`packaging/observability/box/docker-compose.yml` runs Prometheus and Grafana
in two containers with the real box's files mounted as they are; only the
data source, the scrape configuration's wrapper and the list of targets are
its own. It listens on the loopback interface only, since its Grafana has no
sign-in. `MetricsPackaging.TheVirtualBoxAgreesWithTheRealOnesFiles` keeps its
own files in step with the ones it mounts.

Checked with it, on Grafana 12.1.1 and Prometheus 3.5.0, against
`pychron-ui` on the simulated example line:

- the data source, the three dashboards and both alert rules provision
  without error;
- all 39 dashboard queries are valid PromQL. One returned nothing: queue
  progress divided two series that differ in `status` without
  `ignoring(status)`. Fixed, and pinned by a test;
- both rules fire and clear. A stand-in exporter reporting a running queue
  and a heartbeat 120 s old fired `PychronStuck` for its instrument only;
  stopping it fired `PychronGone` and cleared `PychronStuck`; each message
  named the instrument.

Not checked: a queue started in the application itself with the box
watching (it cannot be started without the window), how the panels look,
and delivery to a contact point.

Rules that apply:

- Fakes (`ManualClock`, the bus, the registry) are declared before the
  exporter under test.
- The server test never uses a fixed port: `ctest` runs tests in parallel.
- `libs/metrics` does not depend on persistence, so it builds and is tested
  on all four CI jobs, Windows included.
