// ExperimentBridge on the sim lab: the example queue runs end to end and every
// event reaches the GUI thread, in order.

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QThread>
#include <QtTest/QtTest>

#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"

using pychron::ui::ExperimentBridge;
namespace exec = pychron::experiment::executor;
namespace meas = pychron::experiment::measurement;

class TestExperimentBridge : public QObject {
  Q_OBJECT

 private slots:
  void runsTheExampleQueueAndRelaysEverything() {
    pychron::ui::test::SimLab sim;
    auto bridge = std::make_unique<ExperimentBridge>(*sim.session, sim.line->bus());

    bool off_thread = false;
    auto on_gui = [&] { off_thread |= QThread::currentThread() != QCoreApplication::instance()->thread(); };
    std::vector<std::size_t> started_rows, finished_rows;
    std::vector<std::string> log;
    int states = 0, blocks = 0, counts = 0, peak_centers = 0, batches = 0, multi = 0;
    std::size_t points = 0;
    std::optional<exec::QueueResult> ended;
    connect(bridge.get(), &ExperimentBridge::runStarted, this, [&](const exec::RunStarted& e) {
      on_gui();
      started_rows.push_back(e.row);
      log.push_back("start " + e.identifier);
    });
    connect(bridge.get(), &ExperimentBridge::runFinished, this, [&](const exec::RunFinished& e) {
      on_gui();
      finished_rows.push_back(e.summary.row);
      log.push_back("finish " + e.summary.identifier);
    });
    connect(bridge.get(), &ExperimentBridge::executorStateChanged, this, [&] { on_gui(); ++states; });
    connect(bridge.get(), &ExperimentBridge::blockStarted, this, [&] { on_gui(); ++blocks; });
    connect(bridge.get(), &ExperimentBridge::countsProgress, this, [&] { on_gui(); ++counts; });
    connect(bridge.get(), &ExperimentBridge::peakCenterDone, this, [&](const pychron::jobs::PeakCenterDone& e) {
      on_gui();
      peak_centers += e.result.ok ? 1 : 0;
    });
    connect(bridge.get(), &ExperimentBridge::seriesUpdated, this,
            [&](const std::vector<pychron::experiment::collect::SeriesUpdated>& batch) {
              on_gui();
              ++batches;
              multi += batch.size() > 1 ? 1 : 0;
              for (const auto& u : batch) points += u.values.size();
            });
    std::set<double> ar40_values;  // distinct live Ar40 intercepts seen
    int fit_batches = 0;
    connect(bridge.get(), &ExperimentBridge::fitsUpdated, this,
            [&](const std::vector<pychron::experiment::collect::FitsUpdated>& batch) {
              on_gui();
              ++fit_batches;
              for (const auto& f : batch)
                for (const auto& sf : f.fits)
                  if (sf.key.isotope == "Ar40" && sf.key.kind == pychron::experiment::collect::SeriesKind::Signal)
                    ar40_values.insert(sf.fit.value);
            });
    connect(bridge.get(), &ExperimentBridge::queueEnded, this, [&](const pychron::experiment::lab::QueueEnded& e) {
      on_gui();
      ended = e.result;
      log.push_back("ended");
    });

    const auto queue = sim.queue();
    QVERIFY(bridge->check(queue).ok());
    QVERIFY(bridge->start(queue, 0));
    QVERIFY(bridge->running());
    QVERIFY(!bridge->start(queue, 0));  // already running
    QTRY_VERIFY_WITH_TIMEOUT(ended.has_value(), 60000);

    QVERIFY(!off_thread);
    QCOMPARE(ended->end, exec::QueueEnd::Completed);
    QCOMPARE(started_rows, (std::vector<std::size_t>{0, 1, 2}));
    QCOMPARE(finished_rows, (std::vector<std::size_t>{0, 1, 2}));
    QCOMPARE(log.front(), std::string("start bu"));
    QCOMPARE(log.back(), std::string("ended"));
    QVERIFY(states >= 2);
    QVERIFY(blocks > 0);
    QVERIFY(counts > 0);
    QCOMPARE(peak_centers, 3);
    QVERIFY(points > 0);
    QVERIFY(batches > 0);
    QVERIFY(fit_batches > 0);
    QVERIFY2(ar40_values.size() > 10, qPrintable(QString::number(ar40_values.size())));  // it moves with each reading
    QVERIFY(!bridge->running());
    bridge.reset();
  }

  void aQueueWithErrorsIsRefused() {
    pychron::ui::test::SimLab sim;
    ExperimentBridge bridge(*sim.session, sim.line->bus());
    auto queue = sim.queue();
    queue.runs[0].measurement.plan = "nope";
    QVERIFY(!bridge.check(queue).ok());
    auto r = bridge.start(queue, 0);
    QVERIFY(!r);
    QVERIFY(QString::fromStdString(r.error().what).contains(QStringLiteral("runs[0]")));
    QVERIFY(!bridge.running());
  }
};

QTEST_MAIN(TestExperimentBridge)
#include "test_experiment_bridge.moc"
