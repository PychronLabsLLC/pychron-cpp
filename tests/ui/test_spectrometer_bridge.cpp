// SpectrometerBridge against the sim-integrated example spectrometer: bus
// events reach the main thread and the state mirror, readings are batched, and
// the blocking commands run off the main thread and report through
// commandFinished. Integration is 0.1 s (the sim's quantum) to keep it quick.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <vector>

#include <QtTest/QtTest>

#include "spectrometer_bridge.hpp"
#include "spectrometer_fixture.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using pychron::spectrometer::DetectorState;
using pychron::spectrometer::IntensityReading;
using pychron::spectrometer::MagnetMoved;
using pychron::spectrometer::ScanStatus;
using pychron::ui::SpectrometerBridge;

namespace {

constexpr int kWaitMs = 10000;

struct Finished {
  QString what;
  Result<void> result;
};

}  // namespace

class TestSpectrometerBridge : public QObject {
  Q_OBJECT

 private:
  // Every commandFinished since init(), in order.
  int count(const QString& what) const {
    int n = 0;
    for (const auto& f : finished_) {
      n += f.what == what ? 1 : 0;
    }
    return n;
  }
  const Finished* last(const QString& what) const {
    for (auto it = finished_.rbegin(); it != finished_.rend(); ++it) {
      if (it->what == what) {
        return &*it;
      }
    }
    return nullptr;
  }

  std::unique_ptr<ui::test::SimSpectrometer> sim_;
  std::unique_ptr<SpectrometerBridge> bridge_;
  std::vector<Finished> finished_;
  std::vector<IntensityReading> readings_;
  std::vector<std::size_t> batches_;

 private slots:
  void init() {
    sim_ = ui::test::make_sim_spectrometer();
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    connect(bridge_.get(), &SpectrometerBridge::commandFinished, this,
            [this](const QString& what, const Result<void>& r) { finished_.push_back({what, r}); });
    connect(bridge_.get(), &SpectrometerBridge::readings, this, [this](const std::vector<IntensityReading>& batch) {
      batches_.push_back(batch.size());
      readings_.insert(readings_.end(), batch.begin(), batch.end());
    });
  }

  void cleanup() {
    bridge_.reset();
    sim_.reset();
    finished_.clear();
    readings_.clear();
    batches_.clear();
  }

  void readingsArriveOnGuiThreadInOrder() {
    QThread* seen_on = nullptr;
    connect(bridge_.get(), &SpectrometerBridge::readings, this,
            [&](const std::vector<IntensityReading>&) { seen_on = QThread::currentThread(); });
    bridge_->start_scan(0.1);
    QTRY_VERIFY_WITH_TIMEOUT(readings_.size() >= 5, kWaitMs);
    QCOMPARE(seen_on, QCoreApplication::instance()->thread());
    for (std::size_t i = 1; i < readings_.size(); ++i) {
      QVERIFY(readings_[i].reading.ts > readings_[i - 1].reading.ts);
    }
  }

  void burstIsDeliveredAsOneBatch() {
    const TimePoint t0 = sim_->clock.now();
    std::thread publisher([this, t0] {
      for (int i = 0; i < 50; ++i) {
        IntensityReading r;
        r.reading.ts = t0 + std::chrono::milliseconds(i);
        sim_->bus.publish(r);
      }
    });
    publisher.join();  // the event loop has not run: all 50 are queued
    QVERIFY(batches_.empty());
    QCoreApplication::processEvents();
    QCOMPARE(batches_.size(), std::size_t{1});
    QCOMPARE(batches_.front(), std::size_t{50});
    for (std::size_t i = 0; i < readings_.size(); ++i) {
      QCOMPARE(readings_[i].reading.ts, t0 + std::chrono::milliseconds(i));
    }

    // The next burst is delivered too (the pending flush was re-armed).
    sim_->bus.publish(IntensityReading{});
    QCoreApplication::processEvents();
    QCOMPARE(batches_.size(), std::size_t{2});
    QCOMPARE(batches_.back(), std::size_t{1});
  }

  void startAndStopReportThroughCommandFinished() {
    std::vector<ScanStatus> statuses;
    connect(bridge_.get(), &SpectrometerBridge::scanStatus, this, [&](const ScanStatus& s) {
      statuses.push_back(s);
      // The mirror is updated before the signal.
      QCOMPARE(bridge_->state().scan.running, s.running);
      QCOMPARE(bridge_->state().scan.ts, s.ts);
    });
    QVERIFY(!bridge_->state().scan.running);

    bridge_->start_scan(0.1);
    QCOMPARE(count("start"), 0);  // returned before the command completed
    QTRY_COMPARE_WITH_TIMEOUT(count("start"), 1, kWaitMs);
    QVERIFY(last("start")->result.has_value());
    QVERIFY(bridge_->state().scan.running);
    QCOMPARE(bridge_->state().scan.integration, Duration(100ms));
    QVERIFY(sim_->scan->running());
    QVERIFY(!statuses.empty());

    bridge_->stop_scan();
    QTRY_COMPARE_WITH_TIMEOUT(count("stop"), 1, kWaitMs);
    QVERIFY(last("stop")->result.has_value());
    QVERIFY(!bridge_->state().scan.running);
    QVERIFY(!sim_->scan->running());
    QVERIFY(!statuses.back().running);
  }

  void startScanSeedsMagnetPosition() {
    int reads = 0;
    connect(bridge_.get(), &SpectrometerBridge::magnetRead, this, [&](double native, std::optional<double> mass) {
      ++reads;
      // The mirror is updated before the signal.
      QCOMPARE(bridge_->state().magnet_native, std::optional<double>(native));
      QCOMPARE(bridge_->state().mass_on_reference, mass);
    });
    QVERIFY(!bridge_->state().magnet_native.has_value());

    bridge_->position("Ar40", "H1");
    bridge_->drain();
    const auto native = sim_->spec->magnet_native();
    QVERIFY(native.has_value());

    bridge_->start_scan(0.1);
    QTRY_COMPARE_WITH_TIMEOUT(count("start"), 1, kWaitMs);
    QVERIFY(last("start")->result.has_value());
    QCOMPARE(reads, 1);
    QCOMPARE(bridge_->state().magnet_native, std::optional<double>(*native));
    QVERIFY(bridge_->state().mass_on_reference.has_value());
    QVERIFY(std::abs(*bridge_->state().mass_on_reference - 39.962) < 0.05);

    // What Ar39 on AX puts on the reference detector, from the table alone.
    const auto on_reference = bridge_->mass_on_reference_for("Ar39", "AX");
    QVERIFY(on_reference.has_value());
    QVERIFY(std::abs(*on_reference - *bridge_->mass_of("Ar39")) > 0.5);
    QCOMPARE(bridge_->mass_on_reference_for("Ar39", "H1"), bridge_->mass_of("Ar39"));
    QVERIFY(!bridge_->mass_on_reference_for("Xx99", "AX").has_value());
  }

  void staleScanStatusIsIgnored() {
    int emitted = 0;
    connect(bridge_.get(), &SpectrometerBridge::scanStatus, this, [&](const ScanStatus&) { ++emitted; });
    const TimePoint now = sim_->clock.now();
    ScanStatus newer{sim_->spec->name(), true, false, 200ms, "", now};
    ScanStatus older{sim_->spec->name(), false, false, 100ms, "", now - 1s};
    sim_->bus.publish(newer);
    sim_->bus.publish(older);  // published later, stamped earlier
    QCoreApplication::processEvents();
    QCOMPARE(emitted, 1);
    QVERIFY(bridge_->state().scan.running);
    QCOMPARE(bridge_->state().scan.integration, Duration(200ms));

    ScanStatus tie = newer;  // same ts: the last received wins
    tie.integration = 300ms;
    sim_->bus.publish(tie);
    QCoreApplication::processEvents();
    QCOMPARE(emitted, 2);
    QCOMPARE(bridge_->state().scan.integration, Duration(300ms));
  }

  void setIntegrationReportsSnapped() {
    bridge_->start_scan(0.1);
    QTRY_VERIFY_WITH_TIMEOUT(!readings_.empty(), kWaitMs);

    bridge_->set_integration(0.25);
    QTRY_COMPARE_WITH_TIMEOUT(count("integration"), 1, kWaitMs);
    QVERIFY(last("integration")->result.has_value());
    // The sim snaps to its 100 ms quantum; the first reading of the restarted
    // scan reports what it chose.
    QTRY_VERIFY_WITH_TIMEOUT(bridge_->state().scan.integration == Duration(200ms) ||
                                 bridge_->state().scan.integration == Duration(300ms),
                             kWaitMs);
    QVERIFY(bridge_->state().scan.running);
  }

  void rapidIntegrationChangesEndRunning() {
    bridge_->start_scan(0.1);
    for (double s : {0.2, 0.3, 0.1, 0.2, 0.1}) {
      bridge_->set_integration(s);
    }
    QTRY_COMPARE_WITH_TIMEOUT(count("integration"), 5, kWaitMs);
    for (const auto& f : finished_) {
      QVERIFY2(f.result.has_value(), qPrintable(f.what));
    }
    // Commands ran in the order issued: the last one wins.
    QVERIFY(sim_->scan->running());
    QTRY_VERIFY_WITH_TIMEOUT(bridge_->state().scan.running && bridge_->state().scan.integration == Duration(100ms),
                             kWaitMs);
    const std::size_t before = readings_.size();
    QTRY_VERIFY_WITH_TIMEOUT(readings_.size() >= before + 2, kWaitMs);
    QCOMPARE(readings_.back().reading.integration, Duration(100ms));
  }

  void positionMovesAndUpdatesState() {
    std::vector<MagnetMoved> moves;
    std::vector<DetectorState> changes;
    connect(bridge_.get(), &SpectrometerBridge::magnetMoved, this, [&](const MagnetMoved& e) { moves.push_back(e); });
    connect(bridge_.get(), &SpectrometerBridge::detectorChanged, this,
            [&](const DetectorState& e) { changes.push_back(e); });
    QVERIFY(!bridge_->state().magnet_native.has_value());

    bridge_->position("Ar40", "H1");
    QTRY_COMPARE_WITH_TIMEOUT(count("position"), 1, kWaitMs);
    QVERIFY2(last("position")->result.has_value(), "position Ar40 on H1");
    QCOMPARE(moves.size(), std::size_t{1});
    QVERIFY(bridge_->state().magnet_native.has_value());
    QCOMPARE(*bridge_->state().magnet_native, moves.back().to);
    QVERIFY(bridge_->state().mass_on_reference.has_value());
    QVERIFY(std::abs(*bridge_->state().mass_on_reference - 39.962) < 0.05);  // H1 is the reference
    QCOMPARE(bridge_->state().isotopes.at("H1"), std::string("Ar40"));

    // H1 is configured with Ar40, so move it to something else as well.
    bridge_->position("Ar36", "H1");
    QTRY_COMPARE_WITH_TIMEOUT(count("position"), 2, kWaitMs);
    QVERIFY2(last("position")->result.has_value(), "position Ar36 on H1");
    QCOMPARE(moves.size(), std::size_t{2});
    QVERIFY(std::abs(*bridge_->state().mass_on_reference - 35.968) < 0.05);
    QCOMPARE(bridge_->state().isotopes.at("H1"), std::string("Ar36"));
    QCOMPARE(bridge_->state().isotopes.at("AX"), std::string("Ar39"));  // untouched
    const bool announced = std::any_of(changes.begin(), changes.end(), [](const DetectorState& s) {
      return s.detector == "H1" && s.isotope == "Ar36";
    });
    QVERIFY(announced);
  }

  void positionUnknownIsotopeReportsError() {
    int moves = 0;
    connect(bridge_.get(), &SpectrometerBridge::magnetMoved, this, [&](const MagnetMoved&) { ++moves; });
    bridge_->position("Xx99", "H1");
    QTRY_COMPARE_WITH_TIMEOUT(count("position"), 1, kWaitMs);
    QVERIFY(!last("position")->result.has_value());
    QVERIFY(!last("position")->result.error().what.empty());
    QCOMPARE(moves, 0);
    QVERIFY(!bridge_->state().magnet_native.has_value());
    QCOMPARE(bridge_->state().isotopes.at("H1"), std::string("Ar40"));
  }

  void isotopesForListsTableIsotopes() {
    const QStringList h1 = bridge_->isotopes_for("H1");
    QVERIFY(h1.contains("Ar40"));
    QVERIFY(h1.contains("Ar36"));
    QVERIFY(bridge_->isotopes_for("ZZ").isEmpty());

    QVERIFY(bridge_->mass_of("Ar40").has_value());
    QVERIFY(std::abs(*bridge_->mass_of("Ar40") - 39.962) < 0.01);
    QVERIFY(!bridge_->mass_of("Xx99").has_value());
  }

  void detectorsCarryConfigColors() {
    QCOMPARE(bridge_->name(), QStringLiteral("sim-integrated"));
    QCOMPARE(bridge_->reference_detector(), QStringLiteral("H1"));
    QCOMPARE(bridge_->default_integration_s(), 1.0);

    const auto detectors = bridge_->detectors();
    const auto& configured = sim_->spec->config().detectors;
    QCOMPARE(detectors.size(), configured.size());
    for (std::size_t i = 0; i < detectors.size(); ++i) {
      QCOMPARE(detectors[i].name, configured[i].name);  // config order
      QVERIFY(detectors[i].color.isValid());
      QVERIFY(detectors[i].visible);
    }
    QCOMPARE(detectors[0].name, std::string("H2"));
    QCOMPARE(detectors[0].color, QColor("#1f77b4"));
    QCOMPARE(detectors[0].units, QStringLiteral("fA"));
    QVERIFY(detectors[0].isotope.isEmpty());
    QCOMPARE(detectors[1].color, QColor("#ff7f0e"));
    QCOMPARE(detectors[1].isotope, QStringLiteral("Ar40"));
    QCOMPARE(detectors[5].units, QStringLiteral("cps"));
  }

  void destroyWithCommandInFlightIsClean() {
    std::atomic<int> moved{0};
    auto sub = sim_->bus.subscribe<MagnetMoved>([&moved](const MagnetMoved&) { ++moved; });
    bridge_->position("Ar40", "H1");
    QTest::qSleep(100);  // the executor is inside the move; no events are processed here
    QCOMPARE(moved.load(), 0);
    bridge_.reset();
    // The destructor waited for the move; its result went nowhere.
    QCOMPARE(moved.load(), 1);
    QCoreApplication::processEvents();
    QCOMPARE(count("position"), 0);
  }

  void destroyWhileScanningIsClean() {
    bridge_->start_scan(0.1);
    QTRY_VERIFY_WITH_TIMEOUT(readings_.size() >= 2, kWaitMs);
    bridge_->set_integration(0.2);
    bridge_.reset();  // readings and statuses still in flight are dropped
    QCoreApplication::processEvents();
    QCOMPARE(count("integration"), 0);
    QTest::qWait(300);  // the scan (owned by the fixture) keeps publishing; nothing listens
  }
};

QTEST_MAIN(TestSpectrometerBridge)
#include "test_spectrometer_bridge.moc"
