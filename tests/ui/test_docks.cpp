// LogDock, AlarmDock, HealthBar and the assembled MainWindow wiring.

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "main_window.hpp"
#include "spectrometer_fixture.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::systems::SwitchOp;
using namespace std::chrono_literals;
using pychron::ui::AlarmDock;
using pychron::ui::HealthBar;
using pychron::ui::LogDock;
using pychron::ui::LogModel;
using pychron::ui::LogRecord;

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

  void pausedPendingIsCappedAtCapacity() {
    LogDock dock;
    dock.set_paused(true);
    constexpr int k = 7;
    for (int i = 0; i < LogModel::kCapacity + k; ++i)
      dock.append_log(Log{LogLevel::Info, "a", "m" + std::to_string(i), {}});
    dock.flush_pending();
    QCOMPARE(dock.line_count(), 0);
    QCOMPARE(dock.pending_count(), LogModel::kCapacity);
    dock.set_paused(false);
    dock.flush_pending();
    QCOMPARE(dock.pending_count(), 0);
    QCOMPARE(dock.line_count(), LogModel::kCapacity);
    const QString text = dock.text();
    QVERIFY(text.section(QLatin1Char('\n'), 0, 0).endsWith(QStringLiteral("] m7")));
    QVERIFY(!text.contains(QStringLiteral("] m6\n")));
  }

  void levelActionCallsCallback() {
    LogDock dock;
    QVERIFY(dock.level_action() != nullptr);
    QVERIFY(!dock.level_action()->isVisible());
    std::vector<std::pair<std::string, LogLevel>> calls;
    dock.set_level_callback([&calls](std::string pattern, LogLevel level) {
      calls.emplace_back(std::move(pattern), level);
    });
    QVERIFY(dock.level_action()->isVisible());
    dock.apply_level(QStringLiteral("transport.serial.*"), LogLevel::Trace);
    QCOMPARE(calls.size(), std::size_t{1});
    QCOMPARE(calls[0].first, std::string("transport.serial.*"));
    QCOMPARE(calls[0].second, LogLevel::Trace);
  }

  void levelActionRejectsEmptyPattern() {
    LogDock dock;
    int calls = 0;
    dock.set_level_callback([&calls](std::string, LogLevel) { ++calls; });
    dock.apply_level(QString{}, LogLevel::Debug);
    dock.apply_level(QStringLiteral("   "), LogLevel::Debug);
    QCOMPARE(calls, 0);
  }

  void loadHistoryReadsTailAsHistory() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("pychron.log"));
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      for (int i = 0; i < 1500; ++i)
        f.write(QStringLiteral("2026-10-01T14:03:22.481Z [warn] transport.serial.ig1: line %1\n").arg(i).toUtf8());
    }
    LogDock dock;
    QCOMPARE(dock.load_history(path.toStdString()), 1000);
    QCOMPARE(dock.line_count(), 1000);
    const LogRecord& first = dock.model()->record(0);
    QCOMPARE(first.message, QStringLiteral("line 500"));
    const LogRecord& last = dock.model()->record(999);
    QVERIFY(last.history);
    QCOMPARE(last.level, LogLevel::Warn);
    QCOMPARE(last.logger, QStringLiteral("transport.serial.ig1"));
    QCOMPARE(last.message, QStringLiteral("line 1499"));
    // UTC in the file, shown as the same instant in local wall time.
    QCOMPARE(pychron::ui::log_wall_time(last.ts).toUTC(),
             QDateTime(QDate(2026, 10, 1), QTime(14, 3, 22, 481), QTimeZone::utc()));
    const QModelIndex idx = dock.model()->index(999, LogModel::MessageCol);
    QVERIFY(dock.model()->data(idx, Qt::ForegroundRole).isValid());
    QCOMPARE(dock.load_history(path.toStdString(), 10), 10);
  }

  void loadHistoryMissingFileIsNoop() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LogDock dock;
    QCOMPARE(dock.load_history((dir.path() + QStringLiteral("/nope/pychron.log")).toStdString()), 0);
    QCOMPARE(dock.load_history(dir.path().toStdString()), 0);  // a directory
    QCOMPARE(dock.load_history({}), 0);
    QCOMPARE(dock.line_count(), 0);
  }

  void loadHistoryHandlesUnparseableAndLongLines() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("pychron.log"));
    const QByteArray long_msg(20000, 'x');
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("2026-10-01T14:03:22.481Z [error] a.b: first\n");
      f.write("garbage without format\r\n");
      f.write("2026-10-01T14:03:22.482Z [debug] long: " + long_msg + "\n");
      f.write("2026-10-01T14:03:22.483Z [info] tail: no trailing newline");
    }
    LogDock dock;
    QCOMPARE(dock.load_history(path.toStdString()), 4);
    const LogModel* m = dock.model();
    QCOMPARE(m->record(0).level, LogLevel::Error);
    QCOMPARE(m->record(0).message, QStringLiteral("first"));
    QCOMPARE(m->record(1).level, LogLevel::Info);
    QCOMPARE(m->record(1).logger, QStringLiteral("file"));
    QCOMPARE(m->record(1).message, QStringLiteral("garbage without format"));
    QVERIFY(m->record(1).history);
    QVERIFY(m->record(2).message.size() < 4096);
    QVERIFY(m->record(2).message.startsWith(QStringLiteral("xxxx")));
    QCOMPARE(m->record(3).logger, QStringLiteral("tail"));
    QCOMPARE(m->record(3).message, QStringLiteral("no trailing newline"));

    // A single line longer than the whole scan budget: bounded, not loaded.
    const QString huge = dir.filePath(QStringLiteral("huge.log"));
    {
      QFile f(huge);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(QByteArray(200000, 'y'));
      f.write("\n2026-10-01T14:03:22.481Z [info] ok: end\n");
    }
    LogDock dock2;
    QCOMPARE(dock2.load_history(huge.toStdString(), 2), 1);
    QCOMPARE(dock2.model()->record(0).message, QStringLiteral("end"));
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

  void spectrometerActionDisabledWithoutSpectrometer() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow window(*line);
    QVERIFY(!window.spectrometer_action()->isEnabled());
    QCOMPARE(window.spectrometer_action()->text(), QStringLiteral("Spectrometer"));
    QCOMPARE(window.spectrometer_action()->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    QVERIFY(window.spectrometer_window() == nullptr);
  }

  void spectrometerActionOpensWindowOnce() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_spectrometer(&bridge, true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      QVERIFY(window.spectrometer_action()->isEnabled());

      window.spectrometer_action()->trigger();
      auto* first = window.spectrometer_window();
      QVERIFY(first != nullptr);
      QVERIFY(first->isVisible());
      QTRY_VERIFY_WITH_TIMEOUT(sim->scan->running(), 10000);
      window.spectrometer_action()->trigger();
      QCOMPARE(window.spectrometer_window(), first);
      QVERIFY(first->isVisible());
    }
  }

  void closingMainWindowClosesSpectrometerWindowAndStopsScan() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_spectrometer(&bridge, true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      window.show();
      window.spectrometer_action()->trigger();
      QTRY_VERIFY_WITH_TIMEOUT(sim->scan->running(), 10000);

      QVERIFY(window.close());
      QVERIFY(!window.spectrometer_window()->isVisible());
      bridge.drain();
      QTRY_VERIFY_WITH_TIMEOUT(!sim->scan->running(), 10000);
    }
  }
  void clearingSpectrometerSavesSettingsAndDisablesAction() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_spectrometer(&bridge, true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      window.spectrometer_action()->trigger();
      QTRY_VERIFY_WITH_TIMEOUT(sim->scan->running(), 10000);

      window.set_spectrometer(nullptr, false);
      QVERIFY(window.spectrometer_window() == nullptr);
      QVERIFY(!window.spectrometer_action()->isEnabled());
      bridge.drain();  // the stop the closing window queued
      QVERIFY(!sim->scan->running());
      QSettings saved(path, QSettings::IniFormat);
      saved.beginGroup(QStringLiteral("spectrometer_window"));
      QVERIFY(!saved.childGroups().isEmpty());
    }
  }

  // The app-quit path: the spectrometer is cleared and the bridge destroyed
  // while a magnet move is still on the bridge's executor.
  void clearingSpectrometerWithMoveInFlightStopsScan() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    {
      auto bridge = std::make_unique<pychron::ui::SpectrometerBridge>(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_spectrometer(bridge.get(), true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      window.spectrometer_action()->trigger();
      QTRY_VERIFY_WITH_TIMEOUT(sim->scan->running(), 10000);

      auto* spectrometer = window.spectrometer_window();
      spectrometer->set_confirm_move([](double) { return true; });
      spectrometer->select_target(QStringLiteral("H1"), QStringLiteral("Ar36"));
      spectrometer->apply_position();
      QVERIFY(!spectrometer->apply_enabled());  // the move is pending

      window.set_spectrometer(nullptr, false);
      QVERIFY(window.spectrometer_window() == nullptr);
      QVERIFY(!window.spectrometer_action()->isEnabled());
      bridge.reset();
      QVERIFY(!sim->scan->running());
    }
  }
};

QTEST_MAIN(TestDocks)
#include "test_docks.moc"
