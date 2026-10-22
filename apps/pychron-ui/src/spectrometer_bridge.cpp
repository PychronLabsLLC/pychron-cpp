#include "spectrometer_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>

#include "pychron/core/config/local_file.hpp"
#include "pychron/systems/spectrometer/field_table_store.hpp"

namespace pychron::ui {

bool valid_set_name(const QString& name) {
  if (name.isEmpty()) {
    return false;
  }
  return std::ranges::all_of(name, [](QChar c) {
    return (c.unicode() < 128 && c.isLetterOrNumber()) || c == QLatin1Char('_') || c == QLatin1Char('-');
  });
}

namespace {

using spectrometer::DetectorState;
using spectrometer::FieldTable;
using spectrometer::FieldTableChanged;
using spectrometer::IntensityReading;
using spectrometer::MagnetMoved;
using spectrometer::ScanStatus;

Duration to_duration(double seconds) {
  return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds));
}

}  // namespace

// Bus handlers run on scheduler/executor threads and may still be mid-call
// while the bridge is destroyed. They reach the bridge only through the Gate,
// which the destructor closes under the same lock; posting a queued call is
// thread-safe and never runs the bridge's code on the publishing thread.
struct SpectrometerBridge::Gate {
  std::mutex mutex;
  SpectrometerBridge* target = nullptr;
  // Readings waiting for the main thread, oldest first, and whether the call
  // that will deliver them is already posted.
  std::vector<IntensityReading> readings;
  bool flush_posted = false;
  // Set on the main thread before a run's thread starts, cleared when its
  // result is delivered: only events published meanwhile are relayed. They
  // wait here in the order published, as readings do.
  bool peak_center_active = false;
  using PeakCenterEvent = std::variant<jobs::PeakCenterTryStarted, jobs::PeakCenterPoint>;
  std::vector<PeakCenterEvent> peak_center_events;
  bool peak_center_flush_posted = false;
};

template <class E>
void SpectrometerBridge::relay(void (SpectrometerBridge::*apply)(const E&)) {
  subscriptions_.push_back(bus_.subscribe<E>([gate = gate_, apply](const E& event) {
    std::lock_guard lock(gate->mutex);
    if (SpectrometerBridge* self = gate->target) {
      QMetaObject::invokeMethod(self, [self, apply, event] { (self->*apply)(event); }, Qt::QueuedConnection);
    }
  }));
}

SpectrometerBridge::SpectrometerBridge(spectrometer::Spectrometer& spec, spectrometer::ScanService& scan,
                                       SignalBus& bus, QObject* parent)
    : QObject(parent), spec_(spec), scan_(scan), bus_(bus), gate_(std::make_shared<Gate>()) {
  gate_->target = this;
  subscriptions_.push_back(bus_.subscribe<IntensityReading>([gate = gate_](const IntensityReading& event) {
    std::lock_guard lock(gate->mutex);
    SpectrometerBridge* self = gate->target;
    if (self == nullptr) {
      return;
    }
    gate->readings.push_back(event);
    if (!gate->flush_posted) {
      gate->flush_posted = true;
      QMetaObject::invokeMethod(self, [self] { self->flush_readings(); }, Qt::QueuedConnection);
    }
  }));
  relay<MagnetMoved>(&SpectrometerBridge::on_magnet);
  relay<DetectorState>(&SpectrometerBridge::on_detector);
  relay<ScanStatus>(&SpectrometerBridge::on_scan);
  relay<FieldTableChanged>(&SpectrometerBridge::on_table_changed);
  // A try's start and its points go through one queue, so they arrive in
  // the order the run published them.
  const auto queue_run_event = [](const std::shared_ptr<Gate>& gate) {
    return [gate](const auto& event) {
      std::lock_guard lock(gate->mutex);
      SpectrometerBridge* self = gate->target;
      if (self == nullptr || !gate->peak_center_active) {
        return;
      }
      gate->peak_center_events.emplace_back(event);
      if (!gate->peak_center_flush_posted) {
        gate->peak_center_flush_posted = true;
        QMetaObject::invokeMethod(self, [self] { self->flush_peak_center_events(); }, Qt::QueuedConnection);
      }
    };
  };
  subscriptions_.push_back(bus_.subscribe<jobs::PeakCenterTryStarted>(queue_run_event(gate_)));
  subscriptions_.push_back(bus_.subscribe<jobs::PeakCenterPoint>(queue_run_event(gate_)));
  qRegisterMetaType<jobs::PeakCenterTryStarted>();
  qRegisterMetaType<jobs::PeakCenterPoint>();
  qRegisterMetaType<Result<jobs::PeakCenterResult>>();

  // Seeded after subscribing: anything published meanwhile is queued and
  // applied on top once the event loop runs.
  active_table_ = spec_.active_table();
  table_ = spec_.field_table();
  for (const auto& d : spec_.config().detectors) {
    auto current = spec_.detector_state(d.name);
    state_.isotopes[d.name] = current ? current->isotope : d.isotope;
  }
  state_.scan = scan_.status();
  if (state_.scan.running) {
    scan_intent_ = std::chrono::duration<double>(state_.scan.integration).count();
  }

  executor_.setObjectName(QStringLiteral("pychron-spectrometer-executor"));
  worker_ = new QObject;
  worker_->moveToThread(&executor_);
  connect(&executor_, &QThread::finished, worker_, &QObject::deleteLater);
  executor_.start();
}

SpectrometerBridge::~SpectrometerBridge() {
  // A run still going is cancelled and its thread waited for; its result,
  // posted to a bridge about to go, is dropped with it.
  cancel_peak_center();
  join_peak_center();
  {
    std::lock_guard lock(gate_->mutex);
    gate_->target = nullptr;
  }
  subscriptions_.clear();
  executor_.quit();  // finishes the command in flight; queued ones are dropped
  executor_.wait();
  // A stop asked for after the last start is never dropped: a magnet move
  // still in flight would otherwise leave the scan running with no window.
  if (stop_requested_) scan_.stop();
}

QString SpectrometerBridge::name() const { return QString::fromStdString(spec_.name()); }

std::vector<DetectorSeries> SpectrometerBridge::detectors() const {
  std::vector<DetectorSeries> out;
  const auto& configured = spec_.config().detectors;
  out.reserve(configured.size());
  for (std::size_t i = 0; i < configured.size(); ++i) {
    const auto& d = configured[i];
    QColor color(QString::fromStdString(d.color));  // invalid when empty
    std::optional<std::size_t> slot;
    if (!color.isValid()) {
      slot = i;  // none configured: the palette's, and it follows a change of it
      color = StripChartModel::palette_color(i);
    }
    auto isotope = state_.isotopes.find(d.name);
    out.push_back(DetectorSeries{d.name, color, QString::fromStdString(d.units),
                                 QString::fromStdString(isotope != state_.isotopes.end() ? isotope->second : d.isotope),
                                 true, slot});
  }
  return out;
}

QStringList SpectrometerBridge::isotopes_for(const QString& detector) const {
  const std::string det = detector.toStdString();
  QStringList out;
  for (const auto& point : table_.points()) {
    if (point.values.contains(det)) {
      out.push_back(QString::fromStdString(point.isotope));
    }
  }
  return out;
}

std::optional<double> SpectrometerBridge::mass_of(const QString& isotope) const {
  auto mass = spec_.mass_of(spectrometer::PositionTarget{spectrometer::Isotope{isotope.toStdString()}, {}});
  if (!mass) {
    return std::nullopt;
  }
  return *mass;
}

std::optional<double> SpectrometerBridge::mass_on_reference_for(const QString& isotope, const QString& detector) const {
  const auto mass = mass_of(isotope);
  const std::string det = detector.toStdString();
  const std::string& reference = spec_.reference_detector();
  if (!mass || det.empty() || det == reference) {
    return mass;
  }
  auto value = table_.value_for(*mass, det);
  if (!value) {
    return std::nullopt;
  }
  auto on_reference = table_.mass_for(*value, reference);
  if (!on_reference) {
    return std::nullopt;
  }
  return *on_reference;
}

QString SpectrometerBridge::reference_detector() const { return QString::fromStdString(spec_.reference_detector()); }

double SpectrometerBridge::default_integration_s() const { return spec_.config().system.integration_time_s; }

template <class Work, class Deliver>
void SpectrometerBridge::post(Work work, Deliver deliver) {
  QPointer<SpectrometerBridge> self(this);
  QMetaObject::invokeMethod(
      worker_,
      [self, work = std::move(work), deliver = std::move(deliver)]() mutable {
        auto result = work();
        // `self` is only dereferenced on the main thread, inside the posted call.
        QMetaObject::invokeMethod(
            self.data(),
            [self, deliver = std::move(deliver), result = std::move(result)]() mutable {
              if (self) {
                deliver(*self, std::move(result));
              }
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

template <class Command>
void SpectrometerBridge::run(const char* what, Command command) {
  post([command = std::move(command)]() -> Result<void> { return command(); },
       [name = QString::fromLatin1(what)](SpectrometerBridge& self, const Result<void>& result) {
         emit self.commandFinished(name, result);
       });
}

void SpectrometerBridge::refuse_during_peak_center(const char* what) {
  QMetaObject::invokeMethod(
      this,
      [this, name = QString::fromLatin1(what)] {
        const Result<void> refused = fail(ErrorKind::Config, "a peak center is running", "peak_center");
        emit commandFinished(name, refused);
      },
      Qt::QueuedConnection);
}

void SpectrometerBridge::start_scan(double integration_s) {
  // A scan beside the sweep would read the magnet as it moves; refused, it is
  // no scan the run's end puts back either.
  if (peak_center_running_) {
    refuse_during_peak_center("start");
    return;
  }
  stop_requested_ = false;
  scan_intent_ = integration_s;
  const std::uint64_t generation = ++start_generation_;
  ++starts_in_flight_;
  QPointer<SpectrometerBridge> self(this);
  post(
      [self, spec = &spec_, scan = &scan_, integration = to_duration(integration_s)]() -> Result<void> {
    // Posted before the command's own result, so the state is seeded by the
    // time "start" is reported.
    if (auto native = spec->magnet_native()) {
      std::optional<double> mass;
      if (auto m = spec->mass_at(*native, spec->reference_detector())) {
        mass = *m;
      }
      QMetaObject::invokeMethod(
          self.data(),
          [self, native = *native, mass] {
            if (self) {
              self->on_magnet_read(native, mass);
            }
          },
          Qt::QueuedConnection);
    }
    return scan->start(integration);
      },
      [generation](SpectrometerBridge& bridge, const Result<void>& result) {
        --bridge.starts_in_flight_;
        // A start that failed is no scan to put back after a peak center,
        // unless a newer start or stop has said otherwise since.
        if (!result && generation == bridge.start_generation_) {
          bridge.scan_intent_.reset();
        }
        emit bridge.commandFinished(QStringLiteral("start"), result);
      });
}

void SpectrometerBridge::stop_scan() {
  stop_requested_ = true;
  scan_intent_.reset();
  ++start_generation_;
  run("stop", [scan = &scan_] {
    scan->stop();
    return Result<void>{};
  });
}

void SpectrometerBridge::set_integration(double seconds) {
  if (scan_intent_) {
    scan_intent_ = seconds;
  }
  run("integration",
      [scan = &scan_, integration = to_duration(seconds)] { return scan->set_integration(integration); });
}

void SpectrometerBridge::position(const QString& isotope, const QString& detector) {
  if (peak_center_running_) {  // a move under the sweep would spoil its fit
    refuse_during_peak_center("position");
    return;
  }
  run("position", [spec = &spec_, isotope = isotope.toStdString(), detector = detector.toStdString()]() -> Result<void> {
    auto moved = spec->position(spectrometer::PositionTarget{spectrometer::Isotope{isotope}, detector});
    if (!moved) {
      return fail(moved.error());
    }
    // position() only moves the magnet; the detector's isotope is runtime
    // state the core keeps separately and publishes as DetectorState.
    return spec->set_isotope(detector.empty() ? spec->reference_detector() : detector, isotope);
  });
}

void SpectrometerBridge::drain() {
  QSemaphore done;
  QMetaObject::invokeMethod(worker_, [&done] { done.release(); }, Qt::QueuedConnection);
  done.acquire();
}

std::filesystem::path SpectrometerBridge::data_root() const { return spec_.data_root(); }

const spectrometer::cfg::SpectrometerConfig& SpectrometerBridge::config() const { return spec_.config(); }

const spectrometer::MolecularWeights& SpectrometerBridge::weights() const { return spec_.weights(); }

QString SpectrometerBridge::active_table() const { return QString::fromStdString(active_table_); }

std::optional<QString> SpectrometerBridge::table_held() const {
  auto holder = spec_.table_held();
  if (!holder) {
    return std::nullopt;
  }
  return QString::fromStdString(*holder);
}

FieldTable SpectrometerBridge::table_copy(const QString& name) const {
  return name.toStdString() == active_table_ ? table_ : FieldTable{};
}

void SpectrometerBridge::replace_table(const QString& name, FieldTable table, const QString& base_version) {
  post(
      [spec = &spec_, name = name.toStdString(), table = std::move(table),
       base = base_version.toStdString()]() mutable {
        return spec->replace_table(name, std::move(table), base, spec->clock().wall_now());
      },
      [name](SpectrometerBridge& self, const Result<std::string>& result) { emit self.tableSaved(name, result); });
}

void SpectrometerBridge::restore_table_version(const QString& name, const QString& version) {
  post([spec = &spec_, name = name.toStdString(),
        version = version.toStdString()] { return spec->restore_table_version(name, version); },
       [name, version](SpectrometerBridge& self, const Result<void>& result) {
         emit self.tableRestored(name, version, result);
       });
}

void SpectrometerBridge::apply_peak_center(const QString& detector, const QString& isotope, double table_value,
                                           bool propagate) {
  struct Applied {
    Result<void> result;
    std::vector<std::string> notes;
  };
  post(
      [spec = &spec_, detector = detector.toStdString(), isotope = isotope.toStdString(), table_value,
       propagate]() -> Applied {
        auto notes = jobs::write_peak_center_to_table(*spec, detector, isotope, table_value, propagate, true);
        if (!notes) {
          return {fail(notes.error()), {}};
        }
        return {Result<void>{}, std::move(*notes)};
      },
      [](SpectrometerBridge& self, const Applied& applied) {
        // The helper has logged a failed save already; only relayed here.
        for (const auto& note : applied.notes) {
          emit self.peakCenterNote(QString::fromStdString(note));
        }
        emit self.peakCenterApplied(applied.result);
      });
}

void SpectrometerBridge::save_peak_center_set(std::filesystem::path file, const QString& name,
                                              jobs::PeakCenterConfig config) {
  post(
      [file = std::move(file), name = name.toStdString(), valid = valid_set_name(name),
       config = std::move(config)]() -> Result<void> {
        if (!valid) {
          return fail(ErrorKind::Config, "set name must be letters, digits, _ or -", "peak_center");
        }
        if (auto ok = jobs::validate_peak_center_config(config); !ok) {
          return fail(ok.error());
        }
        const std::string body = "[" + name + "]\n" + jobs::format_peak_center_config(config);
        if (auto written = config::set_local_table(file, name, body); !written) {
          return fail(written.error());
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
          return fail(ErrorKind::Io, "saved, but " + file.string() + " cannot be read again", "peak_center");
        }
        std::ostringstream text;
        text << in.rdbuf();
        if (auto parsed = jobs::parse_peak_center_configs(text.str(), file.string()); !parsed) {
          return fail(ErrorKind::Config, "saved, but peak_center.toml does not load: " + parsed.error().what,
                      "peak_center");
        }
        return Result<void>{};
      },
      [name](SpectrometerBridge& self, const Result<void>& result) { emit self.peakCenterSetSaved(name, result); });
}

void SpectrometerBridge::peak_center(jobs::PeakCenterConfig config) {
  if (peak_center_running_) {
    QMetaObject::invokeMethod(
        this,
        [this] {
          const Result<jobs::PeakCenterResult> refused =
              fail(ErrorKind::Config, "a peak center is running", "peak_center");
          emit peakCenterFinished(refused);
        },
        Qt::QueuedConnection);
    return;
  }
  config.update_table = false;  // the window applies a result only when the operator asks
  peak_center_running_ = true;
  {
    // Before the thread starts, so that nothing it publishes is missed.
    std::lock_guard lock(gate_->mutex);
    gate_->peak_center_active = true;
    gate_->peak_center_events.clear();
  }
  peak_center_run_ = std::make_unique<PeakCenterRun>();
  resume_integration_s_.reset();
  // What the operator asked for, not what the executor has got to: a start_scan
  // just issued has not reported yet, and its queued stop below follows it.
  if (!scan_intent_) {
    start_peak_center_thread(std::move(config));
    return;
  }
  // The scan is stopped on the executor first, and the thread (which moves the
  // magnet) starts only once that has completed. A cancel meanwhile is seen by
  // the run as soon as it starts.
  resume_integration_s_ = *scan_intent_;
  post(
      [scan = &scan_]() -> Result<void> {
        scan->stop();
        return Result<void>{};
      },
      [config = std::move(config)](SpectrometerBridge& self, const Result<void>&) mutable {
        self.start_peak_center_thread(std::move(config));
      });
}

void SpectrometerBridge::start_peak_center_thread(jobs::PeakCenterConfig config) {
  // Time does not jump between the start and the thread's Participant.
  auto hold = std::make_shared<Clock::Hold>(spec_.clock());
  peak_center_thread_ = std::thread([state = peak_center_run_.get(), spec = &spec_, bus = &bus_, gate = gate_,
                                     config = std::move(config), hold]() mutable {
    Clock::Participant participant(spec->clock(), "spectrometer.peak_center");
    hold.reset();
    jobs::Progress progress;  // the bus events carry the progress
    jobs::PeakCenterOptions options;
    options.bus = bus;
    auto result = jobs::run_peak_center(*spec, config, progress, state->cancel, options);
    {
      std::lock_guard lock(gate->mutex);
      if (SpectrometerBridge* self = gate->target) {
        QMetaObject::invokeMethod(
            self, [self, result = std::move(result)] { self->on_peak_center_finished(result); },
            Qt::QueuedConnection);
      }
    }
    // Last act, still a participant.
    std::lock_guard lock(state->mutex);
    state->done = true;
    spec->clock().notify_all(state->cv);
  });
  hold.reset();
}

void SpectrometerBridge::cancel_peak_center(ScanAfter after) {
  if (peak_center_run_) {
    if (after == ScanAfter::Stopped) {
      resume_integration_s_.reset();
    }
    peak_center_run_->cancel.cancel();
  }
}

void SpectrometerBridge::cancel_peak_center_and_wait() {
  if (!peak_center_running_) {
    return;
  }
  cancel_peak_center(ScanAfter::Stopped);  // what shows the scan is about to go
  // The run's thread may not have started: it starts when the executor has
  // stopped the scan, and sees the cancel at once. Each round lets the
  // executor finish, delivers what it posted (the thread's start, or the run's
  // finish, which the thread posted before it said it was done) and joins.
  for (int round = 0; round < 100 && peak_center_running_; ++round) {
    drain();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    join_peak_center();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
  }
}

void SpectrometerBridge::join_peak_center() {
  if (!peak_center_thread_.joinable()) {
    return;
  }
  {
    std::unique_lock lock(peak_center_run_->mutex);
    while (!peak_center_run_->done) {
      spec_.clock().wait(peak_center_run_->cv, lock);
    }
  }
  peak_center_thread_.join();
  peak_center_run_.reset();
}

void SpectrometerBridge::flush_peak_center_events() {
  std::vector<Gate::PeakCenterEvent> batch;
  {
    std::lock_guard lock(gate_->mutex);
    batch.swap(gate_->peak_center_events);
    gate_->peak_center_flush_posted = false;
  }
  for (const auto& event : batch) {
    if (const auto* started = std::get_if<jobs::PeakCenterTryStarted>(&event)) {
      emit peakCenterTryStarted(*started);
    } else {
      emit peakCenterPoint(std::get<jobs::PeakCenterPoint>(event));
    }
  }
}

// Posted by the run's thread after everything it published, so its last
// points are delivered here first.
void SpectrometerBridge::on_peak_center_finished(const Result<jobs::PeakCenterResult>& result) {
  flush_peak_center_events();
  {
    std::lock_guard lock(gate_->mutex);
    gate_->peak_center_active = false;
    gate_->peak_center_events.clear();  // anything since is not the run's
  }
  join_peak_center();
  peak_center_running_ = false;
  // Whatever the outcome, the scan the run stopped goes on, unless the operator
  // pressed Stop scan meanwhile.
  const auto resume = std::exchange(resume_integration_s_, std::nullopt);
  if (resume && !stop_requested_) {
    start_scan(scan_intent_.value_or(*resume));  // an integration changed meanwhile wins
  }
  emit peakCenterFinished(result);
}

void SpectrometerBridge::flush_readings() {
  std::vector<IntensityReading> batch;
  {
    std::lock_guard lock(gate_->mutex);
    batch.swap(gate_->readings);
    gate_->flush_posted = false;
  }
  if (!batch.empty()) {
    emit readings(batch);
  }
}

void SpectrometerBridge::on_magnet(const MagnetMoved& e) {
  state_.magnet_native = e.to;
  state_.mass_on_reference = e.mass_on_reference;
  emit magnetMoved(e);
}

void SpectrometerBridge::on_magnet_read(double native, std::optional<double> mass) {
  state_.magnet_native = native;
  state_.mass_on_reference = mass;
  emit magnetRead(native, mass);
}

void SpectrometerBridge::on_detector(const DetectorState& e) {
  state_.isotopes[e.detector] = e.isotope;
  emit detectorChanged(e);
}

// The copy is re-read on the executor: the core's table lock may be held
// across a settling move, which the main thread must not wait for. Events
// are re-read in the order they came, so the last one leaves the newest copy.
// While a TableScope has another table active, the active table's version is
// read from the store instead.
void SpectrometerBridge::on_table_changed(const FieldTableChanged& e) {
  if (e.name != active_table_) {
    emit fieldTableChanged(QString::fromStdString(e.name), QString::fromStdString(e.version));
    return;
  }
  post(
      [spec = &spec_, e]() -> std::optional<FieldTable> {
        if (spec->active_table() == e.name) {
          return spec->field_table();
        }
        auto stored = spectrometer::FieldTableStore(spec->data_root() / "tables", e.name).load(e.version);
        if (!stored) {
          return std::nullopt;
        }
        return std::move(*stored);
      },
      [e](SpectrometerBridge& self, std::optional<FieldTable> table) {
        if (table) {
          self.table_ = std::move(*table);
        }
        emit self.fieldTableChanged(QString::fromStdString(e.name), QString::fromStdString(e.version));
      });
}

// Statuses are published from several threads and can arrive out of order:
// the newest timestamp wins, the last received on a tie.
void SpectrometerBridge::on_scan(const ScanStatus& e) {
  if (e.ts < state_.scan.ts) {
    return;
  }
  state_.scan = e;
  // A scan that stopped by itself (a fault, another program), with no start
  // of ours still to report, is no scan to put back after a peak center. The
  // run's own stop is not that.
  if (!e.running && starts_in_flight_ == 0 && !peak_center_running_) {
    scan_intent_.reset();
  }
  emit scanStatus(e);
}

}  // namespace pychron::ui
