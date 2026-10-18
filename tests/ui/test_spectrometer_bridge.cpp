// SpectrometerBridge against the sim-integrated example spectrometer: bus
// events reach the main thread and the state mirror, readings are batched, and
// the blocking commands run off the main thread and report through
// commandFinished. Integration is 0.1 s (the sim's quantum) to keep it quick.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <QTemporaryDir>
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

struct Saved {
  QString name;
  Result<std::string> result;
};

struct Restored {
  QString name;
  QString version;
  Result<void> result;
};

struct Changed {
  QString name;
  QString version;
};

// The version the example `argon` table ships as current.
const QString kShippedArgon = QStringLiteral("2026-09-30T000000");

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
    for (const auto& it : std::views::reverse(finished_)) {
      if (it.what == what) {
        return &it;
      }
    }
    return nullptr;
  }

  // Rebuilds the fixture on a copy of the example tables in a temporary
  // directory, so that saves never write into the source tree.
  void use_temporary_tables() {
    bridge_.reset();
    sim_.reset();
    tables_dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(tables_dir_->isValid());
    const std::filesystem::path root = tables_dir_->path().toStdString();
    std::filesystem::copy(std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "tables", root / "tables",
                          std::filesystem::copy_options::recursive);
    sim_ = ui::test::make_sim_spectrometer([&root](spectrometer::cfg::SpectrometerData& data) { data.root = root; });
    QVERIFY(sim_->spec->data_root() == root);
    make_bridge();
  }

  void make_bridge() {
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    connect(bridge_.get(), &SpectrometerBridge::commandFinished, this,
            [this](const QString& what, const Result<void>& r) { finished_.push_back({what, r}); });
    connect(bridge_.get(), &SpectrometerBridge::readings, this, [this](const std::vector<IntensityReading>& batch) {
      batches_.push_back(batch.size());
      readings_.insert(readings_.end(), batch.begin(), batch.end());
    });
    connect(bridge_.get(), &SpectrometerBridge::tableSaved, this,
            [this](const QString& name, const Result<std::string>& r) { saved_.push_back({name, r}); });
    connect(bridge_.get(), &SpectrometerBridge::tableRestored, this,
            [this](const QString& name, const QString& version, const Result<void>& r) {
              restored_.push_back({name, version, r});
            });
    connect(bridge_.get(), &SpectrometerBridge::fieldTableChanged, this, [this](const QString& name, const QString& v) {
      // The copy is re-read before the signal.
      changed_.push_back({name, v});
      changed_had_copy_.push_back(bridge_->table_copy(name));
    });
  }

  // The active table with an Ar38 row between Ar39 and Ar36, its values
  // read off the table itself so the row keeps every check happy.
  spectrometer::FieldTable with_ar38() const {
    auto table = bridge_->table_copy(bridge_->active_table());
    const double mass = *bridge_->mass_of("Ar38");
    spectrometer::ControlPoint row{"Ar38", mass, {}};
    for (const auto& d : bridge_->detectors()) {
      auto value = table.value_for(mass, d.name);
      if (value) {
        row.values[d.name] = *value;
      }
    }
    auto& points = table.mutable_points();  // heaviest first, as shipped
    const auto at = std::ranges::find_if(points, [mass](const auto& p) { return p.mass < mass; });
    points.insert(at, row);
    return table;
  }

  std::unique_ptr<QTemporaryDir> tables_dir_;
  std::unique_ptr<ui::test::SimSpectrometer> sim_;
  std::unique_ptr<SpectrometerBridge> bridge_;
  std::vector<Finished> finished_;
  std::vector<IntensityReading> readings_;
  std::vector<std::size_t> batches_;
  std::vector<Saved> saved_;
  std::vector<Restored> restored_;
  std::vector<Changed> changed_;
  std::vector<spectrometer::FieldTable> changed_had_copy_;

 private slots:
  void init() {
    sim_ = ui::test::make_sim_spectrometer();
    make_bridge();
  }

  void cleanup() {
    bridge_.reset();
    sim_.reset();
    tables_dir_.reset();
    finished_.clear();
    readings_.clear();
    batches_.clear();
    saved_.clear();
    restored_.clear();
    changed_.clear();
    changed_had_copy_.clear();
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

  void tableDescriptionComesFromTheCore() {
    QCOMPARE(bridge_->active_table(), QStringLiteral("argon"));
    QVERIFY(bridge_->data_root() == sim_->spec->data_root());
    QCOMPARE(&bridge_->config(), &sim_->spec->config());
    QCOMPARE(&bridge_->weights(), &sim_->spec->weights());
    QVERIFY(bridge_->table_copy("argon") == sim_->spec->field_table());
    QVERIFY(bridge_->table_copy("argon_hv").points().empty());  // not the active table
    QVERIFY(!bridge_->table_held().has_value());
    auto hold = sim_->spec->hold_table("queue");
    QCOMPARE(bridge_->table_held(), std::optional<QString>(QStringLiteral("queue")));
    hold.release();
    QVERIFY(!bridge_->table_held().has_value());
  }

  void replaceTableEmitsSavedAndChanged() {
    use_temporary_tables();
    const auto table = with_ar38();
    bridge_->replace_table("argon", table, kShippedArgon);
    QTRY_COMPARE_WITH_TIMEOUT(saved_.size(), std::size_t{1}, kWaitMs);
    QCOMPARE(saved_[0].name, QStringLiteral("argon"));
    if (!saved_[0].result) {
      QFAIL(saved_[0].result.error().what.c_str());
    }
    const std::string version = *saved_[0].result;
    QCOMPARE(*sim_->spec->table_version("argon"), version);

    QTRY_COMPARE_WITH_TIMEOUT(changed_.size(), std::size_t{1}, kWaitMs);
    QCOMPARE(changed_[0].name, QStringLiteral("argon"));
    QCOMPARE(changed_[0].version, QString::fromStdString(version));
    QVERIFY(changed_had_copy_[0] == table);
    QVERIFY(bridge_->isotopes_for("H1").contains("Ar38"));
    QVERIFY(sim_->spec->field_table() == table);

    // The same base again is now stale: the store moved on.
    bridge_->replace_table("argon", table, kShippedArgon);
    QTRY_COMPARE_WITH_TIMEOUT(saved_.size(), std::size_t{2}, kWaitMs);
    QVERIFY(!saved_[1].result.has_value());
    QCOMPARE(saved_[1].result.error().code, std::string("table_conflict"));
  }

  void isotopesFollowAPeakCenterStyleSave() {
    use_temporary_tables();
    bridge_->replace_table("argon", with_ar38(), QString());
    QTRY_COMPARE_WITH_TIMEOUT(changed_.size(), std::size_t{1}, kWaitMs);
    QVERIFY(bridge_->isotopes_for("AX").contains("Ar38"));
    const auto before = bridge_->mass_on_reference_for("Ar38", "AX");
    QVERIFY(before.has_value());

    // What peak center does from the core side, on another thread than the
    // bridge's: update one value, save.
    const double old_value = bridge_->table_copy("argon").value_for(*bridge_->mass_of("Ar38"), "AX").value();
    const double new_value = old_value + 0.01;
    std::string version;
    std::thread job([&] {
      if (auto updated = sim_->spec->update_table("AX", "Ar38", new_value, false); !updated) return;
      if (auto saved = sim_->spec->save_table(sim_->spec->clock().wall_now())) version = *saved;
    });
    job.join();
    QVERIFY(!version.empty());
    QTRY_COMPARE_WITH_TIMEOUT(changed_.size(), std::size_t{2}, kWaitMs);
    QCOMPARE(changed_[1].version, QString::fromStdString(version));
    const auto& copy = changed_had_copy_[1];
    const auto row = std::ranges::find(copy.points(), std::string("Ar38"), &spectrometer::ControlPoint::isotope);
    QVERIFY(row != copy.points().end());
    QCOMPARE(row->values.at("AX"), new_value);
    QVERIFY(bridge_->isotopes_for("AX").contains("Ar38"));
    const auto after = bridge_->mass_on_reference_for("Ar38", "AX");
    QVERIFY(after.has_value());
    QVERIFY(std::abs(*after - *before) > 1e-6);  // the copy is the new table
  }

  void replaceRefusedWhileHeldReportsTheCode() {
    use_temporary_tables();
    const auto before = *sim_->spec->table_version("argon");
    {
      auto hold = sim_->spec->hold_table("queue");
      bridge_->replace_table("argon", with_ar38(), kShippedArgon);
      QTRY_COMPARE_WITH_TIMEOUT(saved_.size(), std::size_t{1}, kWaitMs);
    }
    QVERIFY(!saved_[0].result.has_value());
    QCOMPARE(saved_[0].result.error().code, std::string("table_held"));
    QCOMPARE(*sim_->spec->table_version("argon"), before);
    QVERIFY(!bridge_->isotopes_for("H1").contains("Ar38"));
    QCoreApplication::processEvents();
    QVERIFY(changed_.empty());

    // Held for a restore too.
    {
      auto hold = sim_->spec->hold_table("peak_center");
      bridge_->restore_table_version("argon", kShippedArgon);
      QTRY_COMPARE_WITH_TIMEOUT(restored_.size(), std::size_t{1}, kWaitMs);
    }
    QVERIFY(!restored_[0].result.has_value());
    QCOMPARE(restored_[0].result.error().code, std::string("table_held"));
  }

  void restoreEmitsRestored() {
    use_temporary_tables();
    bridge_->replace_table("argon", with_ar38(), kShippedArgon);
    QTRY_COMPARE_WITH_TIMEOUT(changed_.size(), std::size_t{1}, kWaitMs);
    QVERIFY(bridge_->isotopes_for("H1").contains("Ar38"));

    bridge_->restore_table_version("argon", kShippedArgon);
    QTRY_COMPARE_WITH_TIMEOUT(restored_.size(), std::size_t{1}, kWaitMs);
    QCOMPARE(restored_[0].name, QStringLiteral("argon"));
    QCOMPARE(restored_[0].version, kShippedArgon);
    QVERIFY(restored_[0].result.has_value());
    QCOMPARE(*sim_->spec->table_version("argon"), kShippedArgon.toStdString());

    QTRY_COMPARE_WITH_TIMEOUT(changed_.size(), std::size_t{2}, kWaitMs);
    QCOMPARE(changed_[1].version, kShippedArgon);
    QVERIFY(!bridge_->isotopes_for("H1").contains("Ar38"));
    QVERIFY(changed_had_copy_[1] == sim_->spec->field_table());
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
