#pragma once

// SpectrometerBridge (spectrometer-window design section 5.2): the QObject
// between the Qt-free spectrometer core and the spectrometer window. Same
// rules as CoreBridge.
//
// State out: IntensityReading, MagnetMoved, DetectorState and ScanStatus
// events, whatever thread published them, are marshalled to the bridge's
// (main) thread with a queued call, folded into State, then re-emitted as Qt
// signals. Readings are queued under a mutex on the publishing thread and
// delivered by one pending queued call, so a burst arrives as a single
// in-order batch and a slow repaint never backs up the bus.
//
// Commands in: start_scan / stop_scan / set_integration / position return
// immediately; the blocking core call runs on one executor QThread, in the
// order issued, and its Result is posted back as commandFinished. Destruction
// finishes the command in flight and drops queued ones, except that a stop
// asked for after the last start always happens.
//
// No core object ever holds a QObject*: the bus handlers reach the bridge only
// through a shared Gate the destructor closes before the bridge goes away.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include "pychron/core/error.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "strip_chart_model.hpp"

namespace pychron::ui {

class SpectrometerBridge : public QObject {
  Q_OBJECT

 public:
  // Main-thread mirror of what the core has told the UI.
  struct State {
    std::optional<double> magnet_native;      // unknown until start_scan's read or the first move
    std::optional<double> mass_on_reference;  // amu
    std::map<std::string, std::string> isotopes;  // detector -> isotope
    spectrometer::ScanStatus scan;
  };

  // `spec`, `scan` and `bus` must outlive the bridge. The detector list and
  // the active field table are read once, here.
  SpectrometerBridge(spectrometer::Spectrometer& spec, spectrometer::ScanService& scan, SignalBus& bus,
                     QObject* parent = nullptr);
  ~SpectrometerBridge() override;
  SpectrometerBridge(const SpectrometerBridge&) = delete;
  SpectrometerBridge& operator=(const SpectrometerBridge&) = delete;

  const State& state() const noexcept { return state_; }

  // Immutable description; safe to read from the main thread at any time.
  QString name() const;
  // Config order; a detector without a configured colour gets the palette's.
  std::vector<DetectorSeries> detectors() const;
  // Isotopes of the field table's points that have a value on `detector`,
  // table order. Empty for a detector the table does not know.
  QStringList isotopes_for(const QString& detector) const;
  std::optional<double> mass_of(const QString& isotope) const;  // amu; nullopt when unknown
  // Mass (amu) the reference detector sees with `isotope` centred on
  // `detector` (empty: the reference), from the field table alone: the
  // deflection and HV corrections are left out, since applying them reads
  // hardware. nullopt when the table cannot say.
  std::optional<double> mass_on_reference_for(const QString& isotope, const QString& detector) const;
  QString reference_detector() const;
  double default_integration_s() const;  // config integration_time_s

  // Non-blocking; each reports once through commandFinished as "start",
  // "stop", "integration" or "position". start_scan also reads the magnet
  // position to seed State (announced through magnetRead); a failed read
  // leaves it unset and does not fail the start.
  void start_scan(double integration_s);
  void stop_scan();
  void set_integration(double seconds);
  // Centres `isotope` on `detector` and, on success, records it as that
  // detector's isotope (announced through detectorChanged).
  void position(const QString& isotope, const QString& detector);

  // Blocks until every queued command has finished.
  void drain();

 signals:
  void readings(const std::vector<pychron::spectrometer::IntensityReading>& batch);
  void magnetMoved(const pychron::spectrometer::MagnetMoved& event);
  void magnetRead(double native, std::optional<double> mass);  // mass on the reference detector
  void detectorChanged(const pychron::spectrometer::DetectorState& event);
  void scanStatus(const pychron::spectrometer::ScanStatus& status);
  void commandFinished(const QString& what, const pychron::Result<void>& result);

 private:
  struct Gate;

  template <class E>
  void relay(void (SpectrometerBridge::*apply)(const E&));
  // Runs `command` on the executor and posts its Result back as `what`.
  template <class Command>
  void run(const char* what, Command command);

  void flush_readings();
  void on_magnet(const spectrometer::MagnetMoved& e);
  void on_magnet_read(double native, std::optional<double> mass);
  void on_detector(const spectrometer::DetectorState& e);
  void on_scan(const spectrometer::ScanStatus& e);

  spectrometer::Spectrometer& spec_;
  spectrometer::ScanService& scan_;
  SignalBus& bus_;
  spectrometer::FieldTable table_;  // active table at construction
  State state_;
  std::shared_ptr<Gate> gate_;
  std::vector<SignalBus::Subscription> subscriptions_;
  QThread executor_;
  QObject* worker_ = nullptr;  // lives on executor_
  bool stop_requested_ = false;  // stop_scan since the last start_scan (main thread)
};

}  // namespace pychron::ui
