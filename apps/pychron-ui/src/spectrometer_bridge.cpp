#include "spectrometer_bridge.hpp"

#include <chrono>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>

#include "pychron/systems/spectrometer/field_table_store.hpp"

namespace pychron::ui {

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

  // Seeded after subscribing: anything published meanwhile is queued and
  // applied on top once the event loop runs.
  active_table_ = spec_.active_table();
  table_ = spec_.field_table();
  for (const auto& d : spec_.config().detectors) {
    auto current = spec_.detector_state(d.name);
    state_.isotopes[d.name] = current ? current->isotope : d.isotope;
  }
  state_.scan = scan_.status();

  executor_.setObjectName(QStringLiteral("pychron-spectrometer-executor"));
  worker_ = new QObject;
  worker_->moveToThread(&executor_);
  connect(&executor_, &QThread::finished, worker_, &QObject::deleteLater);
  executor_.start();
}

SpectrometerBridge::~SpectrometerBridge() {
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

void SpectrometerBridge::start_scan(double integration_s) {
  stop_requested_ = false;
  QPointer<SpectrometerBridge> self(this);
  run("start", [self, spec = &spec_, scan = &scan_, integration = to_duration(integration_s)] {
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
  });
}

void SpectrometerBridge::stop_scan() {
  stop_requested_ = true;
  run("stop", [scan = &scan_] {
    scan->stop();
    return Result<void>{};
  });
}

void SpectrometerBridge::set_integration(double seconds) {
  run("integration",
      [scan = &scan_, integration = to_duration(seconds)] { return scan->set_integration(integration); });
}

void SpectrometerBridge::position(const QString& isotope, const QString& detector) {
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
  emit scanStatus(e);
}

}  // namespace pychron::ui
