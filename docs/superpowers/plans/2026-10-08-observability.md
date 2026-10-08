# Observability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `pychron-ui` a Prometheus scrape target and ship the Grafana dashboards and dead-man alert for the lab's box.

**Architecture:** A new Qt-free `libs/metrics` holds a registry, a text renderer and an asio HTTP endpoint. Exporters subscribe to the existing `SignalBus` events and update the registry; nothing in the instrument-control path is instrumented by hand. Dashboards, the scrape job and the alert ship as files under `packaging/observability/`, guarded by a test that ties their metric names to the code.

**Tech Stack:** C++20, standalone asio (already fetched), toml++ (config), nlohmann-json (packaging test only), GoogleTest, Prometheus text format 0.0.4, Grafana provisioning YAML/JSON.

**Spec:** `docs/superpowers/specs/2026-10-08-observability-design.md`. Read it before any task: metric names, labels, buckets and HTTP behaviour are fixed there and are not repeated here in full.

## Global Constraints

- No new third-party dependency. `libs/metrics` links `pychron::core` (public) and `asio::asio` (private); no asio type in a public header.
- No Qt in `libs/metrics` or in `libs/experiment`.
- Metric prefix `pychron_`; counters end `_total`; names, labels and label values exactly as spec section 3.
- A label value is a configured name or an enumeration value. Never a run id, an identifier, an error message or other free text.
- A timestamp gauge is real time (`std::chrono::system_clock`), injected as `std::function<double()>`. A duration is a difference of event `ts` on the line's `Clock`.
- Bus handlers never block and never throw.
- Metrics are off unless `[metrics] enabled = true`. A metrics failure never stops the application.
- Tests: fakes are declared before the object under test. No fixed port: bind `127.0.0.1` port 0. No test is skipped or disabled.
- Build and test with the `dev` preset: `cmake --preset dev && cmake --build --preset dev`, tests in `build/dev`. Warnings are errors.
- Commits follow Conventional Commits with the component as scope (`feat(metrics): ...`), each ending with the `Co-Authored-By` line the session gives.
- Follow the comment density and naming of the neighbouring files (short comments that say why).

## Review Focus

1. **A request that arrives in pieces.** Prometheus's request can span TCP segments, and carries headers we do not read. Expected: the same 200 as a request sent in one write. Test in Task 2.
2. **A configured name with a quote, a backslash, a newline or non-ASCII text** (a gauge called `IG "bone"`, a heater `Fürnace`). Expected: valid exposition text, the name readable in Grafana. Test in Task 1 (render) and Task 5 (through the exporter).
3. **A cumulative source that starts again** (a transport that reconnects and counts errors from zero). Expected: the counter keeps rising; it neither falls nor sticks. Test in Task 1 and Task 5.
4. **A second `pychron-ui` on the same computer**, or any port already in use. Expected: the second starts and runs, with one alarm saying metrics are off. Test in Task 2 and Task 7.
5. **A queue that is aborted mid-run, then another started.** Expected: `queue_active` returns to 0, nothing of the cut-off run remains, the next queue's progress starts from 0. Test in Task 6.

---

## File Structure

```
cmake/PychronAsio.cmake                          new: the asio target, moved out of libs/transport
libs/metrics/CMakeLists.txt                      new
libs/metrics/include/pychron/metrics/registry.hpp        Registry, Counter, Gauge, Histogram, Labels, CollectorHandle
libs/metrics/include/pychron/metrics/server.hpp          MetricsServer
libs/metrics/include/pychron/metrics/core_exporter.hpp   CoreExporter, BuildInfo
libs/metrics/include/pychron/metrics/scheduler_metrics.hpp  SchedulerMetrics
libs/metrics/src/{registry,render,server,core_exporter,scheduler_metrics}.cpp
libs/core/include/pychron/core/config/metrics_config.hpp new: MetricsConfig
libs/core/include/pychron/core/scheduler.hpp             + NamedJobStats, job_stats()
libs/experiment/include/pychron/experiment/lab/session.hpp   + QueueStarted
libs/experiment/include/pychron/experiment/metrics/experiment_metrics.hpp   new
libs/experiment/src/metrics/experiment_metrics.cpp                          new
apps/pychron-ui/src/metrics_wiring.{hpp,cpp}     new: builds and owns everything when enabled
packaging/observability/...                      new: spec section 5
docs/observability.md                            new
tests/metrics/{CMakeLists.txt,test_registry,test_render,test_server,test_core_exporter,test_scheduler_metrics,test_packaging}.cpp
tests/experiment/test_experiment_metrics.cpp
tests/integration/test_metrics_session.cpp
tests/ui/test_metrics_wiring.cpp
```

---

### Task 1: Registry and text rendering

**Files:**
- Create: `cmake/PychronAsio.cmake`, `libs/metrics/CMakeLists.txt`, `libs/metrics/include/pychron/metrics/registry.hpp`, `libs/metrics/src/registry.cpp`, `libs/metrics/src/render.cpp`, `tests/metrics/CMakeLists.txt`, `tests/metrics/test_registry.cpp`, `tests/metrics/test_render.cpp`
- Modify: `CMakeLists.txt` (option, library list), `libs/transport/CMakeLists.txt` (asio block)

**Interfaces:**
- Produces (namespace `pychron::metrics`), exactly as spec section 2.1:
  - `using Labels = std::vector<std::pair<std::string, std::string>>;`
  - `class Counter { void inc(double by = 1.0); void set_total(double total); double value() const; };`
  - `class Gauge { void set(double); void inc(double by = 1.0); void dec(double by = 1.0); double value() const; };`
  - `class Histogram { void observe(double); std::uint64_t count() const; double sum() const; };`
  - `class Registry` with `counter`, `gauge`, `histogram`, `remove`, `add_collector`, `render`, `names` as in the spec, plus `static constexpr std::size_t kMaxSeriesPerFamily = 1000;`
  - `class CollectorHandle` — movable, not copyable, `reset()`, unregisters on destruction; safe to outlive the registry.
  - `Registry` registers `pychron_metrics_dropped_series_total` itself at construction.
  - `using UnixClock = std::function<double()>;` (seconds since the epoch, real time) and `UnixClock system_unix_clock();` (`std::chrono::system_clock`), declared in `registry.hpp`. Tasks 3, 5 and 6 take one.

- [ ] **Step 1: Build scaffolding**

  - Move the `if(NOT TARGET asio::asio) ... endif()` blocks (and `find_package(Threads REQUIRED)`) from `libs/transport/CMakeLists.txt` into `cmake/PychronAsio.cmake`, unchanged; `include(PychronAsio)` in its place.
  - `CMakeLists.txt`: `option(PYCHRON_METRICS "Build libs/metrics (Prometheus endpoint)" ON)`; add `metrics` to `PYCHRON_LIBS` directly after `core`; skip the `libs/metrics` and `tests/metrics` subdirectories when the option is off.
  - `libs/metrics/CMakeLists.txt` modelled on `libs/transport/CMakeLists.txt`: target `pychron_metrics`, alias `pychron::metrics`, `include(PychronAsio)`, `PUBLIC pychron::core Threads::Threads`, `PRIVATE asio::asio`, the `WIN32` `NOMINMAX WIN32_LEAN_AND_MEAN` definitions, `pychron_set_warnings`.
  - `tests/metrics/CMakeLists.txt` modelled on `tests/transport/CMakeLists.txt`: target `pychron_metrics_tests`, links `pychron::metrics asio::asio GTest::gtest_main`, compile definition `PYCHRON_SOURCE_DIR="${PROJECT_SOURCE_DIR}"`.

- [ ] **Step 2: Write the failing tests** (`tests/metrics/test_registry.cpp`)

```cpp
TEST(Registry, SameNameAndLabelsIsOneSeries) {
  Registry r;
  r.counter("pychron_x_total", "h", {{"a", "1"}}).inc();
  r.counter("pychron_x_total", "h", {{"a", "1"}}).inc(2);
  EXPECT_DOUBLE_EQ(r.counter("pychron_x_total", "h", {{"a", "1"}}).value(), 3.0);
  EXPECT_DOUBLE_EQ(r.counter("pychron_x_total", "h", {{"a", "2"}}).value(), 0.0);
}
TEST(Registry, LabelOrderDoesNotMakeANewSeries)      // {{"a","1"},{"b","2"}} == {{"b","2"},{"a","1"}}
TEST(Registry, CounterIgnoresANegativeIncrement)     // inc(-1) leaves the value
TEST(Registry, SetTotalAddsTheIncrease)              // set_total(5), set_total(8) -> 8
TEST(Registry, SetTotalTreatsADropAsARestart)        // set_total(5), set_total(2) -> 7; then set_total(3) -> 8
TEST(Registry, ANameReusedAsAnotherTypeIsNotRendered)
  // counter("pychron_x_total"), then gauge("pychron_x_total").set(9): render() has no "9",
  // and pychron_metrics_dropped_series_total is 1
TEST(Registry, AnInvalidNameIsNotRendered)           // "9bad", "has space", "" ; label name "le" on a histogram, "bad-label"
TEST(Registry, HistogramBucketsAreCumulative)
  // buckets {1, 5}; observe 0.5, 3, 100 -> le="1" 1, le="5" 2, le="+Inf" 3, _count 3, _sum 103.5
TEST(Registry, AFamilyStopsAtItsCap)
  // kMaxSeriesPerFamily + 5 distinct label values: names()/render() show exactly the cap,
  // dropped_series_total is 5, and updating a dropped series does not crash
TEST(Registry, ConcurrentIncrementsSumExactly)       // 8 threads x 10000 inc() on one series and on 8 series -> exact totals
TEST(Registry, ACollectorRunsAtEachRenderUntilItsHandleIsReset)
TEST(Registry, ACollectorHandleMayOutliveTheRegistry)
TEST(Registry, SystemUnixClockIsNow)                 // within 5 s of std::time(nullptr)
TEST(Registry, RemoveHidesASeriesAndSetBringsItBack)
  // gauge g{h="a"} set(1); remove; render lacks it; gauge(...).set(2); render has 2
```

  `tests/metrics/test_render.cpp`:

```cpp
TEST(Render, GoldenText) {
  Registry r;  // compare the whole string, after dropping the registry's own pychron_metrics_ families
  r.gauge("pychron_pressure", "Gauge pressure, in the gauge's unit.", {{"gauge", "IG1"}, {"unit", "torr"}}).set(1.5e-9);
  r.counter("pychron_runs_started_total", "Runs started.").inc(3);
  // expected:
  // # HELP pychron_pressure Gauge pressure, in the gauge's unit.
  // # TYPE pychron_pressure gauge
  // pychron_pressure{gauge="IG1",unit="torr"} 1.5e-09
  // # HELP pychron_runs_started_total Runs started.
  // # TYPE pychron_runs_started_total counter
  // pychron_runs_started_total 3
}
TEST(Render, FamiliesAndSeriesAreSorted)             // registered z, a; labels b, a -> a before z, a before b
TEST(Render, HelpAndTypeAppearOncePerFamily)
TEST(Render, LabelValuesAreEscaped)
  // value  IG "bone"\x  with a newline  ->  gauge="IG \"bone\"\\x\n"   (Review Focus 2)
TEST(Render, NonAsciiLabelValuesPassThrough)         // "Fürnace" appears byte for byte
TEST(Render, HelpTextIsEscaped)                      // backslash and newline in help
TEST(Render, NonFiniteValues)                        // NaN -> "NaN", +inf -> "+Inf", -inf -> "-Inf"
TEST(Render, NumbersRoundTrip)                       // 0.1, 1e-12, 123456789012 parse back to the same double
TEST(Render, EndsWithANewline)
```

- [ ] **Step 3: Run to verify they fail**

  Run: `cmake --preset dev && cmake --build --preset dev --target pychron_metrics_tests`
  Expected: compile error, `pychron/metrics/registry.hpp` not found.

- [ ] **Step 4: Implement `registry.hpp`, `registry.cpp`, `render.cpp`**

  - A family is `{type, help, buckets, std::map<Labels, std::unique_ptr<Series>>}` with labels sorted by name as the key; series are heap-allocated so references stay valid.
  - Values are `std::atomic<std::uint64_t>` holding the bits of a double, updated by compare-exchange. A histogram holds one atomic count per bucket (non-cumulative; made cumulative at render), a count and a sum.
  - A metric name matches `[a-zA-Z_:][a-zA-Z0-9_:]*`, a label name `[a-zA-Z_][a-zA-Z0-9_]*`.
  - Each type has one static "sink" series per registry that refused lookups return.
  - `render()` runs the collectors first, outside the registry mutex (a collector calls back into the registry), then formats under it. Numbers use `std::to_chars` shortest form.
  - `CollectorHandle` holds a `std::weak_ptr` to the registry's state, as `SignalBus::Subscription` does.

- [ ] **Step 5: Run to verify they pass**

  Run: `cmake --build --preset dev --target pychron_metrics_tests pychron_transport_tests && ctest --test-dir build/dev -R "^(Registry|Render)\." --output-on-failure && ctest --test-dir build/dev -R Transport --output-on-failure`
  Expected: all pass (the transport suite proves the asio move changed nothing).

- [ ] **Step 6: Commit**

  `feat(metrics): a registry of counters, gauges and histograms in the Prometheus text format`

---

### Task 2: The HTTP endpoint

**Files:**
- Create: `libs/metrics/include/pychron/metrics/server.hpp`, `libs/metrics/src/server.cpp`, `tests/metrics/test_server.cpp`
- Modify: `libs/metrics/CMakeLists.txt`, `tests/metrics/CMakeLists.txt`

**Interfaces:**
- Consumes: `Registry::render()`, `Registry::gauge`, `Registry::counter`.
- Produces:

```cpp
class MetricsServer {
 public:
  struct Options {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 9464;                              // 0: the OS picks
    std::chrono::milliseconds read_timeout{5000};
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_connections = 8;
  };
  static Result<std::unique_ptr<MetricsServer>> start(Registry& registry, Options options);
  std::uint16_t port() const noexcept;
  ~MetricsServer();
};
```

  It registers and maintains `pychron_metrics_scrape_duration_seconds` and `pychron_metrics_bad_requests_total`.

- [ ] **Step 1: Write the failing tests** (`tests/metrics/test_server.cpp`)

  A helper `std::string http(std::uint16_t port, std::string_view request, std::chrono::milliseconds piece_delay = 0ms)` opens a blocking asio socket to `127.0.0.1`, writes the request (in two halves with `piece_delay` between them when it is non-zero), reads to end of stream and returns everything received. All servers use `Options{.bind = "127.0.0.1", .port = 0}`.

```cpp
TEST(MetricsServer, ServesTheRegistry)
  // gauge pychron_pressure{gauge="IG1",unit="torr"} = 2 ; GET /metrics HTTP/1.1
  // response starts "HTTP/1.1 200", has "Content-Type: text/plain; version=0.0.4; charset=utf-8",
  // "Connection: close", a Content-Length equal to the body size, and the body contains the series line
TEST(MetricsServer, AnswersARequestThatArrivesInPieces)      // Review Focus 1: piece_delay 50ms, with
  // "Accept: application/openmetrics-text;version=1.0.0,text/plain;version=0.0.4;q=0.5\r\nUser-Agent: Prometheus/2.53\r\n" -> 200
TEST(MetricsServer, IgnoresAQueryString)                     // GET /metrics?x=1 -> 200
TEST(MetricsServer, Healthz)                                 // GET /healthz -> 200, body "ok\n"
TEST(MetricsServer, UnknownPathIs404)
TEST(MetricsServer, OtherMethodIs405)                        // POST /metrics
TEST(MetricsServer, HeadersOverTheLimitCloseTheConnection)   // max_header_bytes 256, send 1 KiB of header: no "200", bad_requests_total 1
TEST(MetricsServer, ASilentClientIsDropped)                  // read_timeout 100ms; connect, send nothing: read returns EOF within 2 s
TEST(MetricsServer, GarbageIsABadRequest)                    // "\x00\x01\x02\r\n\r\n" -> closed, bad_requests_total 1
TEST(MetricsServer, RefusesConnectionsOverTheLimit)          // max_connections 2, read_timeout 2s: hold 2 open and silent; a third gets EOF at once; after the two close, a fourth gets 200
TEST(MetricsServer, APortInUseIsAnError)                     // Review Focus 4: start twice on the first one's port() -> the second is an Error, no abort
TEST(MetricsServer, ABadBindAddressIsAnError)                // bind "not-an-address"
TEST(MetricsServer, StopsWithAConnectionOpen)                // open a silent connection, destroy the server: returns within 2 s
TEST(MetricsServer, RecordsTheScrapeDuration)                // after one scrape, pychron_metrics_scrape_duration_seconds >= 0 and is rendered
TEST(MetricsServer, ListensOnIPv6)                           // bind "::1"; skipped only by returning early when start() reports the address family unavailable
```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev --target pychron_metrics_tests`
  Expected: compile error, `pychron/metrics/server.hpp` not found.

- [ ] **Step 3: Implement `MetricsServer`**

  - One `asio::io_context`, one thread, async accept. `start()` resolves `bind` with `asio::ip::make_address`, opens, sets `reuse_address` (not on Windows, where it allows a second bind), binds, listens, and only then starts the thread: every failure before that is returned as `Error`.
  - Per connection: `async_read_until` `"\r\n\r\n"` into a buffer capped at `max_header_bytes`, with a steady timer for `read_timeout`. Parse the request line only: method, target (cut at `?`), version. Write the response and close.
  - The destructor stops the context, closes every open socket and joins.
  - Nothing is logged above `debug`; the server takes no logger in this version (counts only).

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev --target pychron_metrics_tests && ctest --test-dir build/dev -R "^MetricsServer\." --output-on-failure --repeat until-fail:5`
  Expected: all pass five times running (timing-dependent tests must not flake).

- [ ] **Step 5: Commit**

  `feat(metrics): an HTTP endpoint serving /metrics`

---

### Task 3: Scheduler job statistics and the heartbeat

**Files:**
- Modify: `libs/core/include/pychron/core/scheduler.hpp`, `libs/core/src/scheduler.cpp`, `tests/core/test_scheduler.cpp`, `libs/metrics/CMakeLists.txt`, `tests/metrics/CMakeLists.txt`
- Create: `libs/metrics/include/pychron/metrics/scheduler_metrics.hpp`, `libs/metrics/src/scheduler_metrics.cpp`, `tests/metrics/test_scheduler_metrics.cpp`

**Interfaces:**
- Produces in `pychron`:

```cpp
struct NamedJobStats { std::string name; JobStats stats; };
// Scheduler: every(), scan() and watchdog() jobs, in name order; after() jobs are left out.
std::vector<NamedJobStats> job_stats() const;
```

- Produces in `pychron::metrics`:

```cpp
class SchedulerMetrics {
 public:
  // Registers the collector and a job "metrics.heartbeat" every 5 s. `scheduler` and
  // `registry` must outlive this object. The heartbeat gauge is set once at construction.
  SchedulerMetrics(Registry& registry, Scheduler& scheduler, UnixClock now = system_unix_clock());
  ~SchedulerMetrics();  // cancels the job, drops the collector
};
```

- [ ] **Step 1: Write the failing tests**

  `tests/core/test_scheduler.cpp`, using the file's existing `ManualClock` + `Options{.threads = 0}` + `run_pending()` style:

```cpp
TEST(Scheduler, JobStatsListsPeriodicJobsByName)
  // every("b", 1s), scan("a", 1s, ok sampler), watchdog("w", 10s), after("once", 1s);
  // advance 3 s running pending -> names {"a","b","w"}; a.runs == 3, b.runs == 3
TEST(Scheduler, JobStatsCountsFailures)              // a scan returning an error twice -> failures == 2
TEST(Scheduler, JobStatsOmitsACancelledJob)
```

  `tests/metrics/test_scheduler_metrics.cpp`:

```cpp
TEST(SchedulerMetrics, RendersEachJobsCounters)
  // job "gauges" run 3 times, 1 failure -> pychron_scheduler_job_runs_total{job="gauges"} 3,
  // ..._failures_total{job="gauges"} 1, ..._skipped_overlaps_total{job="gauges"} 0
TEST(SchedulerMetrics, HeartbeatFollowsTheScheduler)
  // now() returns 1000 then 1005: after construction the gauge is 1000; advance the ManualClock 5 s,
  // run_pending() -> pychron_scheduler_heartbeat_timestamp_seconds 1005
TEST(SchedulerMetrics, TheHeartbeatJobIsNotAmongTheJobLabelsTwice)   // job="metrics.heartbeat" appears exactly once
TEST(SchedulerMetrics, DestructionCancelsTheJob)     // job_count() returns to what it was
```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev --target pychron_core_tests pychron_metrics_tests`
  Expected: compile errors, `job_stats` and `scheduler_metrics.hpp` not found.

- [ ] **Step 3: Implement `Scheduler::job_stats()` and `SchedulerMetrics`**

  `job_stats()` copies names and stats under the scheduler's mutex. The collector calls it and `set_total`s the three counters per job.

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev --target pychron_core_tests pychron_metrics_tests && ctest --test-dir build/dev -R "^(Scheduler|SchedulerMetrics)\." --output-on-failure`
  Expected: all pass.

- [ ] **Step 5: Commit**

  `feat(metrics): scheduler job counters and a heartbeat`

---

### Task 4: The `[metrics]` table

**Files:**
- Create: `libs/core/include/pychron/core/config/metrics_config.hpp`
- Modify: `libs/core/include/pychron/core/config/system_config.hpp`, `libs/core/src/config/loader.cpp`, `tests/core/test_config_loader.cpp`

**Interfaces:**
- Produces in `pychron::config`:

```cpp
struct MetricsConfig : Located {
  bool enabled = false;
  std::string bind = "0.0.0.0";
  std::int64_t port = 9464;
};
// SystemConfig gains:  MetricsConfig metrics;
```

- [ ] **Step 1: Write the failing tests** (`tests/core/test_config_loader.cpp`, beside the `Logging` tests and in their style)

```cpp
TEST(Metrics, AbsentTableIsOff)                 // enabled false, bind "0.0.0.0", port 9464
TEST(Metrics, ParsesAllKeys)                    // enabled = true, bind = "192.168.1.20", port = 9500
TEST(Metrics, AcceptsAnIPv6Bind)                // bind = "::"
TEST(Metrics, BindMustBeAnAddress)              // bind = "labpc.local" -> one diagnostic naming metrics.bind, with its line
TEST(Metrics, PortMustBeInRange)                // 0 and 70000 -> a diagnostic each
TEST(Metrics, UnknownKeyIsADiagnostic)          // tls = true
TEST(Metrics, NotATableIsADiagnostic)           // metrics = 3
```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev --target pychron_core_tests`
  Expected: compile error, no member `metrics`.

- [ ] **Step 3: Implement**

  Add `"metrics"` to the root key list in `loader.cpp` and a `parse_metrics` beside `parse_logging`, using the same `p_` reader calls. `libs/core` does not link asio: check `bind` with a small parser of its own (dotted quad with each part 0..255, or a string of hex digits and colons containing at least two colons); the server makes the real decision when it binds.

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev --target pychron_core_tests && ctest --test-dir build/dev -R "^(Metrics|Logging)\." --output-on-failure`
  Expected: all pass.

- [ ] **Step 5: Commit**

  `feat(core): a [metrics] table in the line's configuration`

---

### Task 5: Core exporter

**Files:**
- Create: `libs/metrics/include/pychron/metrics/core_exporter.hpp`, `libs/metrics/src/core_exporter.cpp`, `tests/metrics/test_core_exporter.cpp`
- Modify: `libs/metrics/CMakeLists.txt`, `tests/metrics/CMakeLists.txt`

**Interfaces:**
- Consumes: `Registry`, `UnixClock` (Task 1), `SignalBus`, the events in `pychron/core/events.hpp`.
- Produces:

```cpp
struct BuildInfo { std::string version, os, compiler; };
BuildInfo build_info(std::string version);  // os: "macos" | "linux" | "windows"; compiler: "clang 17.0" | "gcc 14.2" | "msvc 1940"

class CoreExporter {
 public:
  // `registry` and `bus` must outlive the exporter.
  CoreExporter(Registry& registry, SignalBus& bus, BuildInfo build, UnixClock now = system_unix_clock());
  ~CoreExporter();  // unsubscribes
};
```

  Metrics: spec sections 3.1 and 3.3 (`build_info`, `process_start_time`, `log_records`, `transport_*`).

- [ ] **Step 1: Write the failing tests** (`tests/metrics/test_core_exporter.cpp`)

  Fixture order: `Registry registry; SignalBus bus; double now = 1700000000;` then the exporter with `[&]{ return now; }`. A helper `double value(const Registry&, std::string_view series_line_prefix)` finds a rendered line and parses its value; `bool has(...)`.

```cpp
TEST_F(CoreExporterTest, Pressure)
  // PressureSample{"IG1", 2.5e-9, "torr"} -> pychron_pressure{gauge="IG1",unit="torr"} 2.5e-09
  // and pychron_last_sample_timestamp_seconds{kind="pressure",source="IG1"} 1700000000
TEST_F(CoreExporterTest, ANonFinitePressureIsRendered)       // NaN -> the line ends "NaN"
TEST_F(CoreExporterTest, Temperature)                        // {"ls336","A",4.2} -> temperature_kelvin{input="A",source="ls336"} 4.2; timestamp source="ls336/A"
TEST_F(CoreExporterTest, HeaterFieldsComeAndGo)
  // readback 80, setpoint 100, enabled true -> three series (enabled 1);
  // next sample with readback nullopt -> pychron_heater_readback{heater="h1"} absent, the others present;
  // next with readback 81 -> present again
TEST_F(CoreExporterTest, ValveStateIsOneOfThree)
  // ValveChanged{"A", Open} -> state="open" 1, "closed" 0, "unknown" 0; then Closed -> open 0, closed 1
TEST_F(CoreExporterTest, ValveTransitionsCountChangesOnly)   // Open, Open, Closed -> transitions_total{valve="A"} 2
TEST_F(CoreExporterTest, SnapshotSeedsValvesWithoutCountingTransitions)
TEST_F(CoreExporterTest, ActuationFailures)
TEST_F(CoreExporterTest, AlarmsBySourceAndSeverity)          // severity "info" | "warning" | "critical"
TEST_F(CoreExporterTest, AlarmMessageIsNotALabel)            // the message text is nowhere in render()
TEST_F(CoreExporterTest, LogRecordsUseTheFirstSegmentOfTheLogger)
  // Log{Warn,"transport.serial.ig1","boom"} -> log_records_total{component="transport",level="warn"} 1; "boom" and "ig1" absent
TEST_F(CoreExporterTest, AnEmptyLoggerNameIsComponentUnknown)
TEST_F(CoreExporterTest, TransportHealth)                    // connected true -> 1; error_count 4 -> errors_total 4
TEST_F(CoreExporterTest, TransportErrorsSurviveARestartOfTheCount)   // Review Focus 3: 4, then 1 -> 5
TEST_F(CoreExporterTest, OddNamesRenderValidly)              // Review Focus 2: gauge name  IG "bone"  and heater "Fürnace"
TEST_F(CoreExporterTest, BuildInfoAndStartTime)              // build_info{compiler=..,os=..,version="0.3.0"} 1; process_start_time_seconds 1700000000
TEST_F(CoreExporterTest, StopsListeningWhenDestroyed)        // destroy, publish a sample: the value does not change
TEST(BuildInfo, NamesThisPlatform)                           // os is one of the three; compiler non-empty
```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --build --preset dev --target pychron_metrics_tests`
  Expected: compile error, `core_exporter.hpp` not found.

- [ ] **Step 3: Implement `CoreExporter`**

  One subscription per event type, kept in a vector. The last valve state per valve is kept under a small mutex to decide whether a `ValveChanged` is a transition. Level names: `trace`, `debug`, `info`, `warn`, `error`.

- [ ] **Step 4: Run to verify they pass**

  Run: `cmake --build --preset dev --target pychron_metrics_tests && ctest --test-dir build/dev -R "^(CoreExporterTest|BuildInfo)\." --output-on-failure`
  Expected: all pass.

- [ ] **Step 5: Commit**

  `feat(metrics): instrument and application health from the bus`

---

### Task 6: Experiment metrics

**Files:**
- Create: `libs/experiment/include/pychron/experiment/metrics/experiment_metrics.hpp`, `libs/experiment/src/metrics/experiment_metrics.cpp`, `tests/experiment/test_experiment_metrics.cpp`
- Modify: `libs/experiment/include/pychron/experiment/lab/session.hpp` and `libs/experiment/src/lab/session.cpp` (`QueueStarted`), `libs/experiment/CMakeLists.txt`, `tests/experiment/CMakeLists.txt`, `tests/integration/test_lab_session.cpp`

**Interfaces:**
- Consumes: `Registry`, `UnixClock`; the events `ExecutorStateChanged`, `RunStarted`, `RunFinished`, `QueueEdited`, `ExecutorWaiting` (executor.hpp), `RunStateChanged` (run/state.hpp), `BlockFinished`, `ConditionalTripped` (measurement/engine.hpp), `NotificationSent` (lab/notifier.hpp), `QueueEnded` (lab/session.hpp).
- Produces in `pychron::experiment::lab` (session.hpp, beside `QueueEnded`):

```cpp
// Published on the line's bus when a session starts a queue, before the executor runs.
struct QueueStarted {
  std::size_t rows = 0;      // QueueSpec::runs.size()
  std::size_t from_row = 0;
};
```

- Produces in `pychron::experiment::metrics`:

```cpp
class ExperimentMetrics {
 public:
  // `registry` and `bus` must outlive this object.
  ExperimentMetrics(pychron::metrics::Registry& registry, SignalBus& bus,
                    pychron::metrics::UnixClock now = pychron::metrics::system_unix_clock());
  ~ExperimentMetrics();
  std::size_t tracked_runs() const;  // runs whose state is being timed; for tests
};
```

  Metrics: spec section 3.2, buckets as given there.

- [ ] **Step 1: Build wiring**

  `libs/experiment/CMakeLists.txt`: when `TARGET pychron_metrics`, link `pychron::metrics` (public) and define `PYCHRON_EXPERIMENT_HAS_METRICS=1` (public); otherwise `list(FILTER PYCHRON_EXPERIMENT_SOURCES EXCLUDE REGEX "/src/metrics/")` before `add_library`. `tests/experiment/CMakeLists.txt`: add the test file under the same condition.

- [ ] **Step 2: Write the failing tests** (`tests/experiment/test_experiment_metrics.cpp`)

  Fixture: `Registry registry; SignalBus bus; double now = 1700000000;` then the object. Events are published by hand; `TimePoint t(std::chrono::seconds(n))` gives the event times.

```cpp
TEST_F(ExperimentMetricsTest, ExecutorStateIsOneOfSeven)
  // Idle -> Running: executor_state{state="running"} 1 and the other six 0, among them state="stopping_at_boundary"
TEST_F(ExperimentMetricsTest, QueueActiveFollowsTheQueue)    // 0 at start; QueueStarted -> 1; QueueEnded -> 0
TEST_F(ExperimentMetricsTest, QueueActiveAlsoSetByTheFirstBusyState)   // no QueueStarted (an executor driven directly): Idle->Preparing -> 1; ->Idle -> 0
TEST_F(ExperimentMetricsTest, QueueProgress)
  // QueueStarted{10, 2} -> queue_runs{status="total"} 8, done 0; two RunFinished -> done 2;
  // QueueEdited with 12 runs and frozen 4 -> total 10 (12 - from_row)
TEST_F(ExperimentMetricsTest, RunsByOutcome)
  // Success; Success truncated; Failed; Failed with save_error; Cancelled; Aborted ->
  // runs_finished_total{state="success",truncated="false"} 1, {success,true} 1, {failed,false} 2,
  // {cancelled,false} 1, {aborted,false} 1; run_save_errors_total 1; runs_started_total as published
TEST_F(ExperimentMetricsTest, StateDurations)
  // run "r1": Pending->Preparing at 0 s, ->Extracting at 4 s, ->Equilibrating at 64 s, ->Measuring at 84 s,
  // ->PostMeasuring at 684 s, ->Saving at 700 s, ->Success at 702 s
  // run_state_duration_seconds_sum{state="extracting"} 60, {state="measuring"} 600, {state="saving"} 2;
  // _count 1 each; _bucket{le="60",state="extracting"} 1 and {le="30",state="extracting"} 0
TEST_F(ExperimentMetricsTest, OverlappingRunsAreTimedSeparately)   // r1 and r2 interleaved
TEST_F(ExperimentMetricsTest, AFinishedRunIsForgotten)       // tracked_runs() is 0 after Success, Failed, Cancelled, Aborted
TEST_F(ExperimentMetricsTest, AnAbortedQueueLeavesNothingBehind)   // Review Focus 5
  // QueueStarted{5,0}; r1 reaches Measuring; one RunFinished; QueueEnded{Aborted} with r1 never terminal ->
  // tracked_runs() 0, queue_active 0, queues_ended_total{end="aborted"} 1;
  // then QueueStarted{3,0} -> total 3, done 0, queue_active 1
TEST_F(ExperimentMetricsTest, MeasurementBlocks)             // BlockFinished{Main, ok} and {BaselineAfter, !ok} -> block="main",ok="true" 1 ; block="baseline_after",ok="false" 1
TEST_F(ExperimentMetricsTest, ConditionalTrips)              // Trip kind Truncation level Queue -> conditional_trips_total{kind="truncation",level="queue"} 1
TEST_F(ExperimentMetricsTest, WaitReasonsAreAFixedSet)
  // "scheduled start" -> scheduled_start; "delay before 12345-01A" -> delay; "extraction device" -> extraction_device;
  // "minimum pump time" -> pump_time; "something new" -> other; and "12345-01A" is nowhere in render()
TEST_F(ExperimentMetricsTest, Notifications)                 // {channel "email", RunFailed, ok false} -> {channel="email",event="run_failed",ok="false"} 1
TEST_F(ExperimentMetricsTest, ANotificationWithNoChannelIsChannelNone)
TEST_F(ExperimentMetricsTest, LastRunFinishedIsRealTime)     // now = 1700000123 at RunFinished -> the gauge is 1700000123
TEST_F(ExperimentMetricsTest, RunIdsAndIdentifiersAreNeverRendered)   // after all of the above, neither "r1" as a label value nor the identifier appears
```

  And in `tests/integration/test_lab_session.cpp`, inside `RunsTheExampleQueueAndPausesTheScan` or a new test beside it: a subscriber sees exactly one `QueueStarted` whose `rows` is the example queue's run count, before the first `RunStarted`.

- [ ] **Step 3: Run to verify they fail**

  Run: `cmake --build --preset dev --target pychron_experiment_tests`
  Expected: compile error, `experiment_metrics.hpp` not found.

- [ ] **Step 4: Implement**

  - `QueueStarted`: publish in `LabSession::run` before `Executor::execute`, on the same bus `QueueEnded` uses.
  - Enumeration label values are the existing `to_string` results lower-cased with spaces and camel-case boundaries turned to `_`; write one small `label_of(std::string_view)` and test it through the cases above. Use the names the tests pin; where an existing `to_string` already gives snake case, it is used as is.
  - The per-run map (`run_id -> {state, ts}`) and the queue counters sit under one mutex; `from_row` of the current queue is remembered for `QueueEdited`.
  - Wait reasons: prefix `scheduled start` -> `scheduled_start`; prefix `delay` -> `delay`; equal to `extraction device` -> `extraction_device`; contains `pump` -> `pump_time`; else `other`.

- [ ] **Step 5: Run to verify they pass**

  Run: `cmake --build --preset dev && ctest --test-dir build/dev -R "ExperimentMetricsTest|LabSessionTest" --output-on-failure`
  Expected: all pass.

- [ ] **Step 6: Commit**

  `feat(experiment): run and queue metrics from the bus`

---

### Task 7: Wiring in `pychron-ui`

**Files:**
- Create: `apps/pychron-ui/src/metrics_wiring.hpp`, `apps/pychron-ui/src/metrics_wiring.cpp`, `tests/ui/test_metrics_wiring.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt`, `apps/pychron-ui/src/main.cpp`, `tests/ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `config::MetricsConfig` (Task 4), `Registry`, `MetricsServer::start`/`Options`, `CoreExporter`, `build_info`, `SchedulerMetrics`, `ExperimentMetrics`, `ExtractionLine::bus()`, `scheduler()`, `log_hub()`.
- Produces in `pychron::ui`:

```cpp
class MetricsWiring {
 public:
  // nullptr when `config.enabled` is false. Otherwise the registry and exporters always exist;
  // when the endpoint cannot listen, an error is logged ("metrics" logger) and an
  // Alarm{"metrics", Warning, "metrics endpoint is off: <error>"} is published on `bus`.
  static std::unique_ptr<MetricsWiring> start(const config::MetricsConfig& config, SignalBus& bus,
                                              Scheduler& scheduler, std::shared_ptr<LogHub> log_hub,
                                              std::string version);
  bool listening() const noexcept;
  std::uint16_t port() const noexcept;  // 0 when not listening
  metrics::Registry& registry() noexcept;
  ~MetricsWiring();
};
```

  Member order (destruction is the reverse): registry, core exporter, experiment metrics, scheduler metrics, server.

- [ ] **Step 1: Write the failing tests** (`tests/ui/test_metrics_wiring.cpp`; no Qt object is needed)

  Fixture order: `ManualClock clock; SignalBus bus; Scheduler scheduler(clock, &bus, {.threads = 0});`. To get a free port, start and destroy a `MetricsServer` on port 0 and reuse its port.

```cpp
TEST(MetricsWiring, DisabledBuildsNothing)           // enabled false -> nullptr; scheduler.job_count() unchanged
TEST(MetricsWiring, EnabledListensAndExportsBusEvents)
  // enabled, bind "127.0.0.1", a free port -> listening(); publish PressureSample;
  // registry().render() contains pychron_pressure and pychron_build_info{...version="9.9.9"}
TEST(MetricsWiring, APortInUseLeavesTheApplicationRunning)     // Review Focus 4
  // hold the port with a MetricsServer; start() returns non-null, listening() false,
  // exactly one Alarm with source "metrics" and severity Warning was published,
  // and a PressureSample still updates registry()
TEST(MetricsWiring, DestructionRemovesTheHeartbeatJob)
```

- [ ] **Step 2: Run to verify they fail**

  Run: `cmake --preset dev-ui && cmake --build --preset dev-ui --target pychron_ui_tests`
  Expected: compile error, `metrics_wiring.hpp` not found. (Use the test target name `tests/ui/CMakeLists.txt` defines.)

- [ ] **Step 3: Implement and wire**

  - `apps/pychron-ui/CMakeLists.txt`: when `TARGET pychron_metrics`, add `src/metrics_wiring.cpp` to `pychron_ui_lib`, link `pychron::metrics`, define `PYCHRON_UI_HAS_METRICS=1` (public).
  - `main.cpp`: after the line is created and before `MainWindow`, under `#ifdef PYCHRON_UI_HAS_METRICS`:
    `auto metrics = pychron::ui::MetricsWiring::start((*line)->config().metrics, (*line)->bus(), (*line)->scheduler(), (*line)->log_hub(), PYCHRON_VERSION);`
    declared so that it is destroyed before the line. On success it logs at `info`: `metrics: listening on <bind>:<port>`.
  - Without the library and with `enabled = true`, `main.cpp` logs one `warn`: `metrics: this build has no metrics (PYCHRON_METRICS=OFF)`.

- [ ] **Step 4: Run to verify they pass, and see it work**

  Run: `cmake --build --preset dev-ui && ctest --test-dir build/dev-ui -R "^MetricsWiring\." --output-on-failure`
  Expected: all pass.

  Then run the application against the simulated example line with `[metrics] enabled = true`, `bind = "127.0.0.1"` added to a copy of its `extraction_line.toml`, and `curl -s http://127.0.0.1:9464/metrics | grep -c '^pychron_'`.
  Expected: a count above 20, with `pychron_pressure` and `pychron_valve_state` lines among them. (How the example line is started: `docs/dev_setup.md`.)

- [ ] **Step 5: Commit**

  `feat(ui): the metrics endpoint starts with the line when [metrics] is enabled`

---

### Task 8: Box files, their guard, and the guide

**Files:**
- Create: `packaging/observability/prometheus/pychron.scrape.yml`, `packaging/observability/grafana/provisioning/dashboards/pychron.yml`, `packaging/observability/grafana/provisioning/alerting/pychron-deadman.yml`, `packaging/observability/grafana/dashboards/{instrument-health,run-operations,app-health}.json`, `docs/observability.md`, `tests/metrics/test_packaging.cpp`
- Modify: `tests/metrics/CMakeLists.txt`, `AGENTS.md`, `README.md` (one line linking the guide, where the other guides are listed)

**Interfaces:**
- Consumes: `Registry::names()`, `CoreExporter`, `SchedulerMetrics`, `MetricsServer`, and `ExperimentMetrics` when `PYCHRON_EXPERIMENT_HAS_METRICS`. `tests/metrics` links `pychron::experiment` when that target exists.

- [ ] **Step 1: Write the failing test** (`tests/metrics/test_packaging.cpp`)

```cpp
// Every pychron_ name used by a dashboard or the alert is one the exporters register.
TEST(Packaging, QueriesUseOnlyMetricsThatExist)
  // known = names() of a registry with every exporter and a server constructed against it
  // used  = regex \bpychron_[a-z0-9_]+ over every file under PYCHRON_SOURCE_DIR/packaging/observability,
  //         with a trailing _bucket | _sum | _count removed when the remainder is a known histogram
  // EXPECT each used name in known, reporting file and name; EXPECT used not empty
TEST(Packaging, EveryDashboardIsValidJsonWithATitleAndUid)     // nlohmann::json::parse; "title", "uid" non-empty, uids distinct
TEST(Packaging, DashboardsTakeTheirDatasourceFromAVariable)    // no panel datasource uid other than "${datasource}"
TEST(Packaging, EveryDashboardHasAnInstrumentVariable)
TEST(Packaging, TheAlertNamesBothConditions)
  // the alert file contains  last_over_time(pychron_queue_active[24h])  and  pychron_scheduler_heartbeat_timestamp_seconds
TEST(Packaging, EveryCatalogMetricIsOnSomeDashboard)
  // each known name except pychron_metrics_dropped_series_total and pychron_metrics_bad_requests_total is used somewhere
```

  `nlohmann-json` is found the way `cmake/PychronDependencies.cmake` already provides it; if `libs/metrics`'s tests are the first to need it without persistence, link the target that file defines.

- [ ] **Step 2: Run to verify it fails**

  Run: `cmake --build --preset dev --target pychron_metrics_tests && ctest --test-dir build/dev -R "^Packaging\." --output-on-failure`
  Expected: FAIL, no files under `packaging/observability`.

- [ ] **Step 3: Write the box files**

  - `pychron.scrape.yml`: the job of spec section 5.1, with a comment line saying where it goes (`scrape_configs:` in `prometheus.yml`).
  - `provisioning/dashboards/pychron.yml`: one file provider, folder `Pychron`, path `/var/lib/grafana/dashboards/pychron`.
  - Dashboards: schema version 39, uids `pychron-instrument-health`, `pychron-run-operations`, `pychron-app-health`; templating variables `datasource` (type datasource, query `prometheus`) and `instrument` (`label_values(pychron_build_info, instrument)`); every query filters `{instrument="$instrument"}`; refresh `15s`. Panels per spec section 5.2. Pressures: time series, log scale, one series per `gauge`. Valve and executor state: state timeline over the series whose value is 1. Sample age: `time() - pychron_last_sample_timestamp_seconds`, thresholds 60 s amber, 300 s red. Phase durations: `histogram_quantile(0.5|0.95, sum by (le, state) (rate(pychron_run_state_duration_seconds_bucket[6h])))`. Counters are shown with `increase(...[$__range])` or `rate(...[5m])`, never raw.
  - `pychron-deadman.yml`: Grafana alerting provisioning, `apiVersion: 1`, one group `pychron` in folder `Pychron`, interval `1m`, two rules (`PychronGone`, `PychronStuck`) with the expressions of spec section 5.3, `for: 2m`, `noDataState: OK`, `execErrState: Error`, labels `severity: critical`, annotations with a one-sentence summary naming `{{ $labels.instrument }}`. The datasource uid is `${PYCHRON_PROM_UID}`; the guide says how to set it.

- [ ] **Step 4: Write `docs/observability.md`**

  For the lab manager, in the manner of `docs/notifications.md` (numbered steps, what you will see, what it means when it fails). Sections: What you get (the three dashboards, one alert) · Turn metrics on (the TOML, restart, the `curl` check with expected first lines) · Tell the box (scrape job, reload, the Targets page shows `UP`) · Load the dashboards (copy two directories, restart Grafana) · The alert (set `PYCHRON_PROM_UID`, the contact point, test it by quitting during a simulated queue; what it does not catch) · Who can read this (spec section 4.4) · If something is wrong (port in use; `DOWN` target: firewall, `bind`; empty panels: `instrument` label) · Metric reference (the three tables of spec section 3).

  `AGENTS.md`, under Build and test, one bullet: `libs/metrics` (`-DPYCHRON_METRICS=OFF` skips it), the label rule (configured names and enumerations only), the two-clock rule, and that a metric added to an exporter needs a panel, or `Packaging.EveryCatalogMetricIsOnSomeDashboard` fails.

- [ ] **Step 5: Run to verify it passes**

  Run: `cmake --build --preset dev --target pychron_metrics_tests && ctest --test-dir build/dev -R "^Packaging\." --output-on-failure`
  Expected: all pass.

- [ ] **Step 6: Commit**

  `feat(metrics): Grafana dashboards, the scrape job and a dead-man alert for the lab's box`

---

### Task 9: One simulated queue, end to end

**Files:**
- Create: `tests/integration/test_metrics_session.cpp`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Consumes: the `LabSessionTest` fixture pattern of `tests/integration/test_lab_session.cpp` (copy its set-up; do not edit that fixture), `CoreExporter`, `ExperimentMetrics`, `SchedulerMetrics`, `MetricsServer`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(MetricsSessionTest, ASimulatedQueueIsCountedAndNamesNoRun)
  // registry + all three exporters attached to the line's bus and scheduler before the session starts;
  // a MetricsServer on 127.0.0.1:0; run the example queue to its end, collecting every RunStarted.
  // Scrape over the socket (not render()):
  //   pychron_runs_started_total == number of RunStarted
  //   sum of pychron_runs_finished_total == the same
  //   pychron_queues_ended_total{end="completed"} 1 ; pychron_queue_active 0
  //   pychron_queue_runs{status="done"} == {status="total"}
  //   pychron_run_state_duration_seconds_count{state="measuring"} == runs that measured
  //   at least one pychron_pressure and one pychron_valve_state line
  //   for every collected run_id and identifier: not a substring of the body
TEST_F(MetricsSessionTest, ScrapingDuringAQueueDoesNotDisturbIt)
  // a thread scrapes in a loop while the queue runs; the queue ends Completed and every scrape was a 200
```

- [ ] **Step 2: Run to verify it fails**

  Run: `cmake --build --preset dev && ctest --test-dir build/dev -R "^MetricsSessionTest\." --output-on-failure`
  Expected: fails to build until the file is added to `tests/integration/CMakeLists.txt` (under `if(TARGET pychron_metrics)`), then passes or shows what Tasks 5 to 7 missed. A failure here is fixed in the owning unit, with a unit test added there for it.

- [ ] **Step 3: Run everything**

  Run: `cmake --build --preset dev && ctest --test-dir build/dev --output-on-failure -j4`
  Expected: the whole suite passes. Then a sanitizer build: `cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DPYCHRON_SANITIZE=address,undefined && cmake --build build/asan && ctest --test-dir build/asan -R "Registry|Render|MetricsServer|SchedulerMetrics|CoreExporter|ExperimentMetrics|MetricsSession|Packaging" --output-on-failure`
  Expected: passes with no sanitizer report. And `-DPYCHRON_METRICS=OFF` configures and builds (`cmake -S . -B build/nometrics -DPYCHRON_METRICS=OFF && cmake --build build/nometrics`).

- [ ] **Step 4: Commit**

  `test(metrics): a simulated queue scraped end to end`
