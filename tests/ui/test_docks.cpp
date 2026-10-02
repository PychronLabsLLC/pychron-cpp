// LogDock, AlarmDock, HealthBar and the assembled MainWindow wiring.

#include <chrono>
#include <thread>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "main_window.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::systems::SwitchOp;
using namespace std::chrono_literals;
using pychron::ui::AlarmDock;
using pychron::ui::HealthBar;
using pychron::ui::LogDock;
using pychron::ui::LogModel;

class TestDocks : public QObject {
  Q_OBJECT

 private slots:
  void logDockAppendsFormattedLines() {
    LogDock dock;
    QCOMPARE(dock.line_count(), 0);
    dock.append_log(Log{LogLevel::Warn, "scanner", "IG1 read failed", {}});
    QCOMPARE(dock.line_count(), 1);
    QVERIFY(dock.text().contains(QStringLiteral("WARN [scanner] IG1 read failed")));
  }

  void pauseBuffersAndReleases() {
    LogDock dock;
    dock.append_log(Log{LogLevel::Info, "a", "before", {}});
    dock.flush_pending();
    QCOMPARE(dock.line_count(), 1);
    dock.set_paused(true);
    for (int i = 0; i < 5; ++i) dock.append_log(Log{LogLevel::Info, "a", "during", {}});
    dock.flush_pending();
    QCOMPARE(dock.line_count(), 1);
    QCOMPARE(dock.pending_count(), 5);
    dock.set_paused(false);
    dock.flush_pending();
    QCOMPARE(dock.pending_count(), 0);
    QCOMPARE(dock.line_count(), 6);
  }

  void filtersByLevelPrefixAndText() {
    LogDock dock;
    dock.append_log(Log{LogLevel::Debug, "hw.valve", "opened A", {}});
    dock.append_log(Log{LogLevel::Warn, "hw.valve", "slow close B", {}});
    dock.append_log(Log{LogLevel::Error, "scanner", "IG1 failed", {}});
    dock.append_log(Log{LogLevel::Info, "hwx", "other", {}});
    dock.flush_pending();
    QCOMPARE(dock.line_count(), 4);

    dock.set_min_level(LogLevel::Warn);
    QCOMPARE(dock.line_count(), 2);
    dock.set_min_level(LogLevel::Trace);

    dock.set_logger_filter(QStringLiteral("hw"));  // includes hw.valve, not hwx
    QCOMPARE(dock.line_count(), 2);
    dock.set_logger_filter(QStringLiteral("scanner"));
    QCOMPARE(dock.line_count(), 1);
    dock.set_logger_filter(QStringLiteral("hw.valve"));
    QCOMPARE(dock.line_count(), 2);

    dock.set_text_filter(QStringLiteral("CLOSE"));
    QCOMPARE(dock.line_count(), 1);
    QVERIFY(dock.text().contains(QStringLiteral("WARN [hw.valve] slow close B")));
    QVERIFY(!dock.text().contains(QStringLiteral("opened A")));

    dock.set_logger_filter({});
    dock.set_text_filter({});
    QCOMPARE(dock.line_count(), 4);
  }

  void appendLineParsesPrefix() {
    LogDock dock;
    dock.append_line(QStringLiteral("WARN [canvas] x"));
    dock.append_line(QStringLiteral("hello"));
    dock.append_line(QStringLiteral("ERROR [ui] A rejected: busy"));
    dock.flush_pending();
    QCOMPARE(dock.line_count(), 3);
    dock.set_logger_filter(QStringLiteral("canvas"));
    QCOMPARE(dock.line_count(), 1);
    dock.set_min_level(LogLevel::Warn);
    QCOMPARE(dock.line_count(), 1);
    QVERIFY(dock.text().contains(QStringLiteral("WARN [canvas] x")));
    dock.set_min_level(LogLevel::Trace);
    dock.set_logger_filter(QStringLiteral("ui"));
    QCOMPARE(dock.line_count(), 2);
    QVERIFY(dock.text().contains(QStringLiteral("INFO [ui] hello")));
    QVERIFY(dock.text().contains(QStringLiteral("ERROR [ui] A rejected: busy")));
  }

  void ringEvictsAtCapacity() {
    LogDock dock;
    QCOMPARE(LogDock::kMaxLines, LogModel::kCapacity);
    for (int i = 0; i < LogModel::kCapacity + 10; ++i)
      dock.append_log(Log{LogLevel::Info, "a", "m" + std::to_string(i), {}});
    dock.flush_pending();
    QCOMPARE(dock.line_count(), LogModel::kCapacity);
    QVERIFY(!dock.text().contains(QStringLiteral("] m9\n")));
    QVERIFY(dock.text().contains(QStringLiteral("] m10\n")));
    dock.clear();
    QCOMPARE(dock.line_count(), 0);
  }

  void saveVisibleWritesOnlyFilteredRows() {
    LogDock dock;
    dock.append_log(Log{LogLevel::Info, "a", "keep me", {}});
    dock.append_log(Log{LogLevel::Debug, "a", "drop me", {}});
    dock.append_log(Log{LogLevel::Error, "b", "keep too", {}});
    dock.flush_pending();
    dock.set_min_level(LogLevel::Info);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("log.txt"));
    QVERIFY(dock.save_visible(path));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    QVERIFY(content.contains(QStringLiteral("INFO [a] keep me")));
    QVERIFY(content.contains(QStringLiteral("ERROR [b] keep too")));
    QVERIFY(!content.contains(QStringLiteral("drop me")));
    QCOMPARE(content.count(QLatin1Char('\n')), 2);
    QVERIFY(!dock.save_visible(dir.filePath(QStringLiteral("no/such/dir/x.txt"))));
  }

  void floodDoesNotBlock() {
    LogDock dock;
    dock.resize(800, 400);
    dock.show();
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 50000; ++i) dock.append_log(Log{LogLevel::Trace, "flood", "line " + std::to_string(i), {}});
    dock.flush_pending();
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QStringLiteral("took %1 ms").arg(timer.elapsed())));
    QCOMPARE(dock.line_count(), LogModel::kCapacity);
  }

  void alarmDockKeepsOneRowPerSourceAndAcks() {
    AlarmDock dock;
    dock.add_alarm(Alarm{"IG1", AlarmSeverity::Critical, "high", {}});
    dock.add_alarm(Alarm{"IG1", AlarmSeverity::Critical, "still high", {}});
    dock.add_alarm(Alarm{"PG1", AlarmSeverity::Warning, "low", {}});
    QCOMPARE(dock.active_count(), 2);
    dock.tree()->topLevelItem(0)->setSelected(true);
    QCOMPARE(dock.acknowledge_selected(), 1);
    QVERIFY(!dock.is_active("IG1"));
    QVERIFY(dock.is_active("PG1"));
    dock.acknowledge_all();
    QCOMPARE(dock.active_count(), 0);
  }

  void healthBarChipsTrackStatusAndAge() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.seed({"valve_bus"});
    QCOMPARE(*bar.status("valve_bus"), HealthBar::Status::Unknown);

    bar.update_health(TransportHealth{"valve_bus", true, 0, "", now});
    QCOMPARE(*bar.status("valve_bus"), HealthBar::Status::Ok);
    now += 5s;
    bar.refresh();
    QVERIFY(bar.chip("valve_bus")->text().contains(QStringLiteral("5s")));

    bar.update_health(TransportHealth{"valve_bus", true, 2, "timeout", now});
    QCOMPARE(*bar.status("valve_bus"), HealthBar::Status::Degraded);
    bar.update_health(TransportHealth{"gauge_net", false, 3, "gone", now});
    QCOMPARE(*bar.status("gauge_net"), HealthBar::Status::Down);
    QVERIFY(bar.chip("gauge_net")->toolTip().contains(QStringLiteral("gone")));
  }

  void mainWindowRoutesBusEventsToDocks() {
    auto line = ui::test::make_example_line();
    {
      ui::MainWindow window(*line);
      QVERIFY(window.health_bar()->status("valve_bus").has_value());
      QVERIFY(window.health_bar()->status("gauge_net").has_value());
      QVERIFY(line->start().has_value());

      std::thread([&line] {
        line->bus().publish(Log{LogLevel::Info, "test", "from a worker", {}});
        line->bus().publish(Alarm{"IG1", AlarmSeverity::Critical, "high", {}});
        line->bus().publish(TransportHealth{"valve_bus", false, 1, "unplugged", {}});
      }).join();
      QTRY_VERIFY(window.log_dock()->text().contains(QStringLiteral("from a worker")));
      QTRY_VERIFY(window.alarm_dock()->is_active("IG1"));
      QTRY_COMPARE(*window.health_bar()->status("valve_bus"), HealthBar::Status::Down);

      window.bridge().actuate("C", SwitchOp::Open);
      window.bridge().drain();
      window.bridge().actuate("A", SwitchOp::Open);
      QTRY_VERIFY(window.log_dock()->text().contains(QStringLiteral("A rejected")));
      line->stop();
    }
  }
};

QTEST_MAIN(TestDocks)
#include "test_docks.moc"
