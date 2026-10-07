// CoreBridge: bus events reach the main thread and the snapshot; actuate()
// never blocks the caller and reports its Result back on the main thread.

#include <cmath>
#include <optional>
#include <thread>

#include <QtTest/QtTest>

#include "core_bridge.hpp"
#include "pychron/sim/sim_system.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::systems::SwitchOp;
using pychron::ui::CoreBridge;

class TestCoreBridge : public QObject {
  Q_OBJECT

 private slots:
  void init() {
    line_ = ui::test::make_example_line();
    bridge_ = std::make_unique<CoreBridge>(*line_);
  }

  void cleanup() {
    line_->stop();
    bridge_.reset();
    line_.reset();
  }

  void startSnapshotFillsStateBeforeAnyScan() {
    int snapshots = 0;
    connect(bridge_.get(), &CoreBridge::snapshot, this, [&](const Snapshot&) { ++snapshots; });
    QVERIFY(line_->start().has_value());
    QTRY_COMPARE(snapshots, 1);
    const auto& state = bridge_->state();
    QCOMPARE(state.valves.size(), std::size_t{7});  // 5 valves, M1, pump_power
    QVERIFY(state.valves.count("A") == 1);
    QVERIFY(state.pressures.count("IG1") == 1);
    QVERIFY(state.switches.count("A") == 1);
    // The fixture's own numbers, not the example sim.toml's: prep starts at
    // 1e-8 mbar (the file's 1e-10 is a hundred times less).
    QVERIFY(line_->sim() != nullptr);
    QVERIFY(line_->sim()->settings().file.empty());
    const auto prep = line_->sim()->pressure("prep");
    QVERIFY(prep.has_value());
    QVERIFY2(std::abs(*prep - 1e-8) < 1e-10, qPrintable(QString::number(*prep)));
  }

  void eventsFromOtherThreadsArriveOnMainThread() {
    QThread* seen_on = nullptr;
    QString message;
    connect(bridge_.get(), &CoreBridge::logLine, this, [&](const Log& e) {
      seen_on = QThread::currentThread();
      message = QString::fromStdString(e.message);
    });
    std::thread publisher([this] { line_->bus().publish(Log{LogLevel::Warn, "test", "hello", {}}); });
    publisher.join();
    QTRY_COMPARE(message, QStringLiteral("hello"));
    QCOMPARE(seen_on, QCoreApplication::instance()->thread());
  }

  void pressureAndHealthAreMirroredInState() {
    std::thread publisher([this] {
      line_->bus().publish(PressureSample{"IG1", 2.5e-7, "torr", {}});
      line_->bus().publish(TransportHealth{"valve_bus", true, 0, "", {}});
    });
    publisher.join();
    QTRY_VERIFY(bridge_->state().health.count("valve_bus") == 1);
    QCOMPARE(bridge_->state().pressures.at("IG1"), 2.5e-7);
    QCOMPARE(bridge_->state().units.at("IG1"), std::string("torr"));
  }

  void actuateRunsOffTheMainThreadAndReportsSuccess() {
    QVERIFY(line_->start().has_value());
    std::optional<Result<void>> finished;
    connect(bridge_.get(), &CoreBridge::actuationFinished, this,
            [&](const QString& name, const Result<void>& r) {
              QCOMPARE(name, QStringLiteral("B"));
              finished = r;
            });
    bridge_->actuate("B", SwitchOp::Open);
    QVERIFY(bridge_->pending("B"));  // returned before the command completed
    QTRY_VERIFY(finished.has_value());
    QVERIFY(finished->has_value());
    QVERIFY(!bridge_->pending("B"));
    QTRY_COMPARE(bridge_->state().valves.at("B"), ValveState::Open);
  }

  void interlockRejectionIsReported() {
    QVERIFY(line_->start().has_value());
    bridge_->actuate("C", SwitchOp::Open);
    bridge_->drain();
    QTRY_COMPARE(bridge_->state().valves.at("C"), ValveState::Open);

    std::optional<Result<void>> finished;
    connect(bridge_.get(), &CoreBridge::actuationFinished, this,
            [&](const QString&, const Result<void>& r) { finished = r; });
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_VERIFY(finished.has_value());
    QVERIFY(!finished->has_value());
    QCOMPARE(finished->error().kind, ErrorKind::Interlock);
    QCOMPARE(bridge_->state().valves.at("A"), ValveState::Closed);
  }

  void duplicateRequestWhilePendingIsCancelled() {
    QVERIFY(line_->start().has_value());
    std::vector<Result<void>> results;
    connect(bridge_.get(), &CoreBridge::actuationFinished, this,
            [&](const QString&, const Result<void>& r) { results.push_back(r); });
    bridge_->actuate("A", SwitchOp::Open);  // settles for 500 ms
    bridge_->actuate("A", SwitchOp::Open);
    QCOMPARE(results.size(), std::size_t{1});
    QCOMPARE(results[0].error().kind, ErrorKind::Cancelled);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), std::size_t{2}, 5000);
    QVERIFY(results[1].has_value());
  }

  void lockChangesAreRelayedAndMirrored() {
    QVERIFY(line_->start().has_value());
    std::vector<std::pair<QString, bool>> seen;
    connect(bridge_.get(), &CoreBridge::lockChanged, this,
            [&](const QString& name, bool locked) { seen.emplace_back(name, locked); });
    QVERIFY(bridge_->set_locked("A", true).has_value());
    QTRY_VERIFY(bridge_->state().switches.at("A").locked);
    QCOMPARE(seen.size(), std::size_t{1});
    QCOMPARE(seen[0].first, QStringLiteral("A"));
    QVERIFY(seen[0].second);
    QVERIFY(bridge_->set_locked("A", false).has_value());
    QTRY_VERIFY(!bridge_->state().switches.at("A").locked);
  }

  void setLockedReportsErrors() {
    auto r = bridge_->set_locked("M1", true);
    QVERIFY(!r.has_value());
    QCOMPARE(r.error().kind, ErrorKind::Config);
  }

  void snapshotSeedsLocksFromPersistedState() {
    QVERIFY(line_->set_locked("B", true).has_value());
    bridge_.reset();
    bridge_ = std::make_unique<CoreBridge>(*line_);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(bridge_->state().switches.at("B").locked);
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<CoreBridge> bridge_;
};

QTEST_MAIN(TestCoreBridge)
#include "test_core_bridge.moc"
