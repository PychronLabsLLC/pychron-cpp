# Observability: Prometheus metrics and Grafana dashboards

Date: 2026-10-08
Status: Draft
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

  // Runs on the scraping thread at the start of every render().
  using Collector = std::function<void(Registry&)>;
  [[nodiscard]] CollectorHandle add_collector(Collector c);  // RAII, like SignalBus::Subscription

  std::string render() const;
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
  (`TransportHealth::error_count`, `JobStats`). It never lowers the value: a
  smaller total is ignored.
- A family holds at most 1000 series. Beyond that a new label set gets the
  unrendered series, one `warn` record per family, and a count in
  `pychron_metrics_dropped_series_total`. This bounds a labelling bug.
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
- **Two clocks.** The line's `Clock` may be a `VirtualClock`, so:
  - a timestamp that a PromQL expression compares with `time()` is real
    time, from `std::chrono::system_clock`, taken when the event is handled;
  - a duration between two events is the difference of their `ts`, on the
    line's clock. In a simulation it is simulated seconds, which is what a
    dashboard of a simulated queue should show.
  Exporters take the real-time source as a `std::function<double()>`
  (seconds since the epoch) so tests can fix it.
- The exposition carries no timestamps: Prometheus stamps each sample at the
  scrape.
- A reading whose device has stopped answering keeps its last value, which
  would mislead on a dashboard. Every sampled source therefore also has a
  `pychron_last_sample_timestamp_seconds` series; panels show its age.

### 2.3 Lifetime

`Registry` is declared before the exporters and the server and outlives
them. On shutdown: subscriptions and collector handles are reset, the server
is stopped and its thread joined, then the registry is destroyed. The server
thread is not a participant in the line's clock and never waits through it.

Wiring is in `apps/pychron-ui/src/main.cpp`, after the line is created, and
only when `[metrics]` is enabled. Disabled, nothing is constructed.

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
| `pychron_actuation_failures_total` | counter | `valve` | `ActuationFailed` |
| `pychron_last_sample_timestamp_seconds` | gauge | `kind` = `pressure`, `temperature`, `heater`; `source` | each sample event; real time |
| `pychron_alarms_total` | counter | `source`, `severity` | `Alarm` |

A pressure keeps the unit its gauge is configured with, named in the `unit`
label. Converting in the exporter would disagree with what the operator sees
on the canvas. For a temperature, `source` in the timestamp series is
`<source>/<input>`.

### 3.2 Run and queue operations (`ExperimentMetrics`)

| Metric | Type | Labels | Source |
|---|---|---|---|
| `pychron_executor_state` | gauge, 0 or 1 | `state`, the seven `ExecutorState` names in lower snake case | `ExecutorStateChanged` |
| `pychron_queue_active` | gauge, 0 or 1 | none | 1 from the first state that is not `Idle`; 0 on `QueueEnded` or a return to `Idle` |
| `pychron_queue_runs` | gauge | `status` = `total`, `done` | `QueueEdited` (queue size); `RunFinished` (done); done resets when a queue starts |
| `pychron_queues_ended_total` | counter | `end` = `completed`, `stopped`, `cancelled`, `aborted`, `failed` | `QueueEnded` |
| `pychron_runs_started_total` | counter | none | `RunStarted` |
| `pychron_runs_finished_total` | counter | `state` = `success`, `failed`, `cancelled`, `aborted`; `truncated` = `true`, `false` | `RunFinished` |
| `pychron_run_save_errors_total` | counter | none | `RunSummary::save_error` |
| `pychron_run_state_duration_seconds` | histogram | `state`, the non-terminal `RunState` names | time between a run's consecutive `RunStateChanged::ts` |
| `pychron_measurement_blocks_total` | counter | `block`, `ok` | `BlockFinished` |
| `pychron_conditional_trips_total` | counter | `kind`, `level` | `ConditionalTripped` |
| `pychron_executor_waits_total` | counter | `reason` = `delay`, `scheduled_start`, `extraction_device`, `pump_time`, `other` | `ExecutorWaiting` |
| `pychron_last_run_finished_timestamp_seconds` | gauge | none | `RunFinished`; real time |
| `pychron_notifications_total` | counter | `channel`, `event`, `ok` | `NotificationSent` |

`pychron_run_state_duration_seconds{state="saving"}` is how long the record
and the store took: no instrumentation of the persister is needed. Buckets,
in seconds: 1, 2.5, 5, 10, 30, 60, 150, 300, 600, 1200, 2400, 3600, 7200.

`ExperimentMetrics` keeps, per live run id, its last state and `ts`; the
entry is erased when the run reaches a terminal state, so the map is bounded
by the runs in flight (two, with overlap). The run id is a key in that map
and never a label.

When the queue size is not known from a `QueueEdited` (a queue that nothing
edits), `total` is taken from the executor at start: `ExperimentMetrics`
has a `queue_started(std::size_t rows)` call for the session to make.

### 3.3 Application health

| Metric | Type | Labels | Source |
|---|---|---|---|
| `pychron_build_info` | gauge, always 1 | `version`, `os`, `compiler` | constants |
| `pychron_process_start_time_seconds` | gauge | none | real time at start |
| `pychron_log_records_total` | counter | `level`, `component` | `Log`; `component` is the logger name up to its first dot |
| `pychron_transport_connected` | gauge, 0 or 1 | `transport` | `TransportHealth` |
| `pychron_transport_errors_total` | counter | `transport` | `TransportHealth::error_count` (`set_total`) |
| `pychron_scheduler_job_runs_total` | counter | `job` | `Scheduler::job_stats()` |
| `pychron_scheduler_job_failures_total` | counter | `job` | the same |
| `pychron_scheduler_job_skipped_overlaps_total` | counter | `job` | the same |
| `pychron_scheduler_heartbeat_timestamp_seconds` | gauge | none | a scheduler job every 5 s; real time |
| `pychron_metrics_scrape_duration_seconds` | gauge | none | `MetricsServer`, the last `render()` |
| `pychron_metrics_bad_requests_total` | counter | none | `MetricsServer` |
| `pychron_metrics_dropped_series_total` | counter | none | `Registry` |

A record is counted only if it reaches the bus, so the `trace` and `debug`
counts follow the configured levels. `warn` and `error` are the ones the
dashboard shows.

### 3.4 Deferred

- **Blank and air intensities as gauges.** `RunFinished` carries neither the
  analysis type nor the fitted intercepts, so this needs a new event or a
  wider `RunSummary`. A follow-up of its own.
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
| A client is slow, oversized or sends garbage | The connection is closed and counted; logged at `debug` only, so a port scanner cannot fill the log |
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
     and time() - pychron_scheduler_heartbeat_timestamp_seconds > 60
   ```

Known limit, stated in the guide: a queue stuck inside one run while the
scheduler keeps ticking is not caught. Catching it needs a per-lab bound on
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
| `tests/metrics/test_registry.cpp` | Counter, gauge and histogram semantics; a label set is a series; a name reused with another type; `set_total` never lowers; cumulative buckets with `+Inf` equal to `_count`; the series cap; updates from several threads sum exactly; a collector runs at each render and stops when its handle is reset |
| `tests/metrics/test_render.cpp` | Golden text: `# HELP` and `# TYPE` once per family; escaping; `NaN` and the infinities; ordering; a removed series is absent |
| `tests/metrics/test_server.cpp` | A real socket on `127.0.0.1`, port 0: `/metrics` with its content type, `/healthz`, 404, 405, a query string, oversized headers, a slow client, the ninth connection, shutdown with a connection open, a bind failure returned as an error |
| `tests/metrics/test_core_exporter.cpp` | Each core event published on a real `SignalBus`, then the rendered series; `Snapshot` seeds valves; nullopt heater fields remove their series; the logger name is cut at its first dot; timestamps come from the injected real-time source |
| `tests/metrics/test_packaging.cpp` | Every `pychron_` name in a dashboard or alert query is a name the exporters register |
| `tests/experiment/test_experiment_metrics.cpp` | One scripted queue: runs that succeed, fail, truncate and fail to save, then `QueueEnded`. Counters, `queue_active` going 1 then 0, state durations from a `ManualClock`, the wait-reason mapping, the per-run map empty at the end |
| `tests/core/test_config.cpp`, extended | `[metrics]`: defaults, each field, each diagnostic |
| `tests/core/test_scheduler.cpp`, extended | `job_stats()` |
| `tests/integration/`, extended | One simulated queue with both exporters attached: the run is counted, and the rendered text contains neither its run id nor its identifier |

`test_packaging.cpp` is the guard against drift: rename a metric without
its dashboard and the test fails. It builds a registry, constructs both
exporters against it (the experiment one only when `libs/experiment` is
built), collects the names and compares them with the names found by a
regular expression in the files under `packaging/observability/`. Histogram
suffixes (`_bucket`, `_sum`, `_count`) are stripped before the comparison.

Rules that apply:

- Fakes (`ManualClock`, the bus, the registry) are declared before the
  exporter under test.
- The server test never uses a fixed port: `ctest` runs tests in parallel.
- `libs/metrics` does not depend on persistence, so it builds and is tested
  on all four CI jobs, Windows included.
