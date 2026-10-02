#include "spectrometer_bridge.hpp"

#include <chrono>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>

namespace pychron::ui {

namespace {

using spectrometer::DetectorState;
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

  // Seeded after subscribing: anything published meanwhile is queued and
  // applied on top once the event loop runs.
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
}

QString SpectrometerBridge::name() const { return QString::fromStdString(spec_.name()); }

std::vector<DetectorSeries> SpectrometerBridge::detectors() const {
  std::vector<DetectorSeries> out;
  const auto& configured = spec_.config().detectors;
  out.reserve(configured.size());
  for (std::size_t i = 0; i < configured.size(); ++i) {
    const auto& d = configured[i];
    QColor color(QString::fromStdString(d.color));  // invalid when empty
    if (!color.isValid()) {
      color = StripChartModel::palette_color(i);
    }
    auto isotope = state_.isotopes.find(d.name);
    out.push_back(DetectorSeries{d.name, color, QString::fromStdString(d.units),
                                 QString::fromStdString(isotope != state_.isotopes.end() ? isotope->second : d.isotope),
                                 true});
  }
  return out;
}

QStringList SpectrometerBridge::isotopes_for(const QString& detector) const {
  const std::string det = detector.toStdString();
  QStringList out;
  for (const auto& point : table_.points()) {
    if (point.values.count(det) != 0) {
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

QString SpectrometerBridge::reference_detector() const { return QString::fromStdString(spec_.reference_detector()); }

double SpectrometerBridge::default_integration_s() const { return spec_.config().system.integration_time_s; }

template <class Command>
void SpectrometerBridge::run(const char* what, Command command) {
  const QString name = QString::fromLatin1(what);
  QPointer<SpectrometerBridge> self(this);
  QMetaObject::invokeMethod(
      worker_,
      [self, name, command = std::move(command)] {
        Result<void> result = command();
        // `self` is only dereferenced on the main thread, inside the posted call.
        QMetaObject::invokeMethod(
            self.data(),
            [self, name, result] {
              if (self) {
                emit self->commandFinished(name, result);
              }
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

void SpectrometerBridge::start_scan(double integration_s) {
  run("start", [scan = &scan_, integration = to_duration(integration_s)] { return scan->start(integration); });
}

void SpectrometerBridge::stop_scan() {
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

void SpectrometerBridge::on_detector(const DetectorState& e) {
  state_.isotopes[e.detector] = e.isotope;
  emit detectorChanged(e);
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
