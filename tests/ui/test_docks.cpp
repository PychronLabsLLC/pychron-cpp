// LogDock, AlarmDock, HealthBar and the assembled MainWindow wiring.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "canvas_items.hpp"
#include "gauge_popup.hpp"
#include "gauge_window.hpp"
#include "history_chart.hpp"
#include "field_table_editor_window.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "peak_center_panel.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "spectrometer_window.hpp"
#include "spectrometer_fixture.hpp"
#include "settings_guard.hpp"
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

 private:
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  // Log, Alarms, then the cryostat's and the heaters' when the line has them.
  static QList<QDockWidget*> main_docks(const ui::MainWindow& window) {
    QList<QDockWidget*> docks{window.log_dock(), window.alarm_dock()};
    if (window.cryo_dock() != nullptr) docks.append(window.cryo_dock());
    if (window.heater_dock() != nullptr) docks.append(window.heater_dock());
    return docks;
  }
  // QSettings on a file in `dir`, made afresh for each read and write.
  static std::function<std::unique_ptr<QSettings>()> settings_on(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    return [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); };
  }

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

  // An alarm's row carries the time the dock is given: real time unless told.
  void alarmDockStampsARowWithItsOwnTimeSource() {
    AlarmDock dock(nullptr, [] { return QDateTime(QDate(2001, 2, 3), QTime(4, 5, 6)); });
    dock.add_alarm(Alarm{"IG1", AlarmSeverity::Critical, "high", {}});
    QCOMPARE(dock.tree()->topLevelItem(0)->text(3), QStringLiteral("04:05:06"));

    AlarmDock real;
    const QTime before = QTime::currentTime();
    real.add_alarm(Alarm{"IG1", AlarmSeverity::Critical, "high", {}});
    const QTime shown = QTime::fromString(real.tree()->topLevelItem(0)->text(3), QStringLiteral("HH:mm:ss"));
    QVERIFY(shown.isValid());
    const int late = (before.secsTo(shown) + 86400) % 86400;  // across midnight too
    QVERIFY2(late <= 5, qPrintable(real.tree()->topLevelItem(0)->text(3)));
  }

  // On a simulated line the window tells the time by the line's clock: an
  // alarm's row has that clock's time of day, and a transport's "ok ... ago"
  // is counted on it. Here it is six hours from the real time of day and its
  // monotonic time ten hours behind the real one.
  void mainWindowTellsTheTimeByTheLinesClock() {
    VirtualClock::Options at;
    at.epoch = std::chrono::system_clock::now() + 6h;
    at.start = std::chrono::steady_clock::now() - 10h;
    VirtualClock clock(at);  // outlives the line
    auto line = ui::test::make_example_line({}, &clock);
    {
      ui::MainWindow window(*line);
      QVERIFY(line->start().has_value());

      line->bus().publish(Alarm{"IG1", AlarmSeverity::Critical, "high", clock.now()});
      QTRY_VERIFY(window.alarm_dock()->is_active("IG1"));
      const QTime shown = QTime::fromString(window.alarm_dock()->tree()->topLevelItem(0)->text(3), QStringLiteral("HH:mm:ss"));
      const QTime simulated =
          QDateTime::fromMSecsSinceEpoch(
                  std::chrono::duration_cast<std::chrono::milliseconds>(clock.wall_now().time_since_epoch()).count())
              .time();
      const int off = (shown.secsTo(simulated) + 86400) % 86400;  // across midnight too
      QVERIFY2(off <= 60, qPrintable(window.alarm_dock()->tree()->topLevelItem(0)->text(3)));

      line->bus().publish(TransportHealth{"valve_bus", true, 0, "", clock.now()});
      QTRY_COMPARE(*window.health_bar()->status("valve_bus"), HealthBar::Status::Ok);
      clock.sleep_for(5s);
      window.health_bar()->refresh();
      QVERIFY2(window.health_bar()->chip("valve_bus")->text().endsWith(QStringLiteral(" 5s")),
               qPrintable(window.health_bar()->chip("valve_bus")->text()));
      line->stop();
    }
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

  // ---- the main window's panel layout ---------------------------------------

  void mainWindowResetShowsClosedPanelsAgain() {
    auto line = ui::test::make_example_line();
    ui::MainWindow window(*line);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QList<QDockWidget*> docks = main_docks(window);
    QVERIFY(docks.size() >= 2);
    window.log_dock()->close();
    window.alarm_dock()->setFloating(true);
    for (QDockWidget* dock : docks.mid(2)) dock->close();

    window.dock_layouts()->reset();
    for (const QDockWidget* dock : docks) {
      QVERIFY2(dock->isVisible(), qPrintable(dock->objectName()));
      QVERIFY2(!dock->isFloating(), qPrintable(dock->objectName()));
    }
    QCOMPARE(window.dockWidgetArea(window.log_dock()), Qt::BottomDockWidgetArea);
    for (QDockWidget* dock : docks.mid(1)) QCOMPARE(window.dockWidgetArea(dock), Qt::RightDockWidgetArea);
  }

  void theExampleLinesWindowFitsItsInstalledSize() {
    auto line = ui::test::make_example_line();
    ui::MainWindow window(*line);
    QVERIFY(window.cryo_dock() != nullptr);
    QVERIFY(window.heater_dock() != nullptr);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY2(window.minimumSizeHint().height() <= 850,
             qPrintable(QStringLiteral("%1").arg(window.minimumSizeHint().height())));
    QCOMPARE(window.size(), QSize(1200, 850));
  }

  void mainWindowTabsTheCryostatAndHeatersTogether() {
    auto line = ui::test::make_example_line();
    ui::MainWindow window(*line);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.cryo_dock() != nullptr);
    QVERIFY(window.heater_dock() != nullptr);
    QVERIFY(window.tabifiedDockWidgets(window.cryo_dock()).contains(window.heater_dock()));
    QVERIFY(!window.cryo_dock()->visibleRegion().isEmpty());  // the cryostat's tab is the one in front

    window.heater_dock()->setFloating(true);
    window.dock_layouts()->reset();
    QVERIFY(window.tabifiedDockWidgets(window.cryo_dock()).contains(window.heater_dock()));
  }

  void mainWindowKeepsItsLayoutAcrossInstances() {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    auto line = ui::test::make_example_line();
    const QString group = QStringLiteral("main_window/%1").arg(QString::fromStdString(line->config().system.name));
    {
      ui::MainWindow window(*line, std::make_unique<QSettings>(file, QSettings::IniFormat));
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      window.log_dock()->close();
      window.close();
    }
    {
      const QSettings settings(file, QSettings::IniFormat);
      QVERIFY(settings.contains(group + QStringLiteral("/state")));
      QVERIFY(settings.contains(group + QStringLiteral("/geometry")));
    }
    ui::MainWindow again(*line, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    QVERIFY(!again.log_dock()->isVisible());
    QVERIFY(again.alarm_dock()->isVisible());
  }

  void mainWindowResetGivesThePanelsTheirSizesBack() {
    auto line = ui::test::make_example_line_without_docks();
    ui::MainWindow window(*line);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(window.log_dock()->height() > 0);
    const int log_height = window.log_dock()->height();
    const int side_width = window.alarm_dock()->width();
    window.resizeDocks({window.log_dock()}, {log_height + 200}, Qt::Vertical);
    window.resizeDocks({window.alarm_dock()}, {side_width + 250}, Qt::Horizontal);
    QTRY_VERIFY(window.log_dock()->height() > log_height + 100);
    QTRY_VERIFY(window.alarm_dock()->width() > side_width + 100);

    window.dock_layouts()->reset();
    constexpr int kSlackPx = 6;
    QTRY_VERIFY2(qAbs(window.log_dock()->height() - log_height) <= kSlackPx,
                 qPrintable(QStringLiteral("%1 vs %2").arg(window.log_dock()->height()).arg(log_height)));
    QTRY_VERIFY2(qAbs(window.alarm_dock()->width() - side_width) <= kSlackPx,
                 qPrintable(QStringLiteral("%1 vs %2").arg(window.alarm_dock()->width()).arg(side_width)));
  }

  void mainWindowHasItsSizeAndKeepsTheOneItWasClosedWith() {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    auto line = ui::test::make_example_line_without_docks();
    {
      ui::MainWindow window(*line, std::make_unique<QSettings>(file, QSettings::IniFormat));
      QCOMPARE(window.size(), QSize(1200, 850));  // as installed
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      window.resize(760, 640);  // within the offscreen screen, which restoreGeometry keeps a window on
      QTRY_COMPARE(window.size(), QSize(760, 640));
      window.close();
    }
    ui::MainWindow again(*line, std::make_unique<QSettings>(file, QSettings::IniFormat));
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    QTRY_COMPARE(again.size(), QSize(760, 640));
  }

  void mainWindowWithoutSettingsKeepsNothing() {
    auto line = ui::test::make_example_line();
    ui::MainWindow window(*line);
    QVERIFY(!window.dock_layouts()->can_save());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.log_dock()->close();
    window.close();
    ui::MainWindow again(*line);
    again.show();
    QVERIFY(QTest::qWaitForWindowExposed(&again));
    QVERIFY(again.log_dock()->isVisible());
  }

  void mainWindowLogsAnArrangementThatCannotBeApplied() {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("s.ini"));
    auto line = ui::test::make_example_line();
    {
      QSettings settings(file, QSettings::IniFormat);
      settings.setValue(QStringLiteral("main_window/%1/arrangements/bad/state")
                            .arg(QString::fromStdString(line->config().system.name)),
                        QByteArray("zz"));
    }
    ui::MainWindow window(*line, std::make_unique<QSettings>(file, QSettings::IniFormat));
    QVERIFY(!window.dock_layouts()->apply(QStringLiteral("bad")));
    window.log_dock()->set_min_level(LogLevel::Warn);
    QTRY_VERIFY(window.log_dock()->text().contains(QString::fromUtf8("arrangement “bad” not applied")));
  }

  void mainWindowPanelsListItsClosableDocks() {
    auto line = ui::test::make_example_line();
    const ui::MainWindow window(*line);
    QStringList want;
    for (const QDockWidget* dock : main_docks(window)) want.append(dock->windowTitle());
    want.sort(Qt::CaseInsensitive);  // by title
    QStringList got;
    for (const QAction* action : window.dock_layouts()->panel_actions()) got.append(action->text());
    QCOMPARE(got, want);
    QVERIFY(got.size() >= 2);
  }

  void spectrometerActionDisabledWithoutSpectrometer() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow window(*line);
    QVERIFY(!window.spectrometer_action()->isEnabled());
    QCOMPARE(window.spectrometer_action()->text(), QStringLiteral("Spectrometer"));
    QCOMPARE(window.spectrometer_action()->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    QVERIFY(window.spectrometer_window() == nullptr);
  }

  // ---- Spectrometer > Field Table… (field table editor plan, task 16) ----
  void fieldTableMenuDisabledWithoutSpectrometer() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow window(*line);
    QAction* action = window.field_table_action();
    QVERIFY(action != nullptr);
    QCOMPARE(action->text(), QStringLiteral("Field Table…"));
    QVERIFY(!action->isEnabled());
    QCOMPARE(action->toolTip(), QStringLiteral("no spectrometer configured"));
    action->trigger();
    QVERIFY(window.field_table_window() == nullptr);
  }

  void fieldTableMenuOpensOneEditor() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_spectrometer(&bridge, true);
      QAction* action = window.field_table_action();
      QVERIFY(action->isEnabled());
      QVERIFY(action->toolTip() != QStringLiteral("no spectrometer configured"));
      action->trigger();
      auto* first = window.field_table_window();
      QVERIFY(first != nullptr);
      QVERIFY(first->isVisible());
      QCOMPARE(first->open_name(), bridge.active_table());
      first->hide();
      action->trigger();
      QCOMPARE(window.field_table_window(), first);
      QVERIFY(first->isVisible());
      // Clearing the spectrometer takes the editor (it holds the bridge).
      window.set_spectrometer(nullptr, false);
      QVERIFY(window.field_table_window() == nullptr);
      QVERIFY(!action->isEnabled());
      QCOMPARE(action->toolTip(), QStringLiteral("no spectrometer configured"));
    }
  }

  void spectrometerWindowMenuOpensTheSameEditor() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_spectrometer(&bridge, true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      window.field_table_action()->trigger();
      auto* editor = window.field_table_window();
      QVERIFY(editor != nullptr);
      window.spectrometer_action()->trigger();
      auto* spectrometer = window.spectrometer_window();
      QVERIFY(spectrometer != nullptr);
      // The spectrometer window's bar has Spectrometer > Field Table…
      QMenuBar* bar = pychron::ui::MenuHub::instance().install(spectrometer);
      if (bar == nullptr) bar = pychron::ui::MenuHub::instance().bar_for(spectrometer);
      QVERIFY(bar != nullptr);
      QAction* item = nullptr;
      for (QAction* m : bar->actions()) {
        if (m->menu() == nullptr || m->text().remove(QLatin1Char('&')) != QStringLiteral("Spectrometer")) continue;
        for (QAction* a : m->menu()->actions())
          if (a->text() == QStringLiteral("Field Table…")) item = a;
      }
      QVERIFY(item != nullptr);
      QVERIFY(item->isEnabled());
      editor->hide();
      item->trigger();
      QCOMPARE(window.field_table_window(), editor);
      QVERIFY(editor->isVisible());
    }
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

  void spectrometerWindowGetsThePeakCenterFile() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    const std::filesystem::path file = std::filesystem::path(dir.path().toStdString()) / "peak_center.toml";
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_peak_center_file(file);
      window.set_spectrometer(&bridge, true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      window.spectrometer_action()->trigger();
      QVERIFY(window.spectrometer_window() != nullptr);
      QCOMPARE(window.spectrometer_window()->peak_center_panel()->file(), file);
    }
  }

  void replacingTheSpectrometerCancelsAPeakCenterWithoutAsking() {
    auto line = pychron::ui::test::make_example_line();
    auto sim = pychron::ui::test::make_sim_spectrometer();
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    const std::filesystem::path file = std::filesystem::path(dir.path().toStdString()) / "peak_center.toml";
    std::ofstream(file) << "[default]\nisotope = \"Ar40\"\nwindow = 0.06\nstep = 0.0005\nintegration_s = 0.1\n"
                           "settle_s = 0.1\n";
    {
      pychron::ui::SpectrometerBridge bridge(*sim->spec, *sim->scan, sim->bus);
      pychron::ui::MainWindow window(*line);
      window.set_peak_center_file(file);
      window.set_spectrometer(&bridge, true,
                              [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); });
      window.spectrometer_action()->trigger();
      int asked = 0;
      window.spectrometer_window()->set_ask([&](const QString&) {
        ++asked;
        return false;
      });
      QTest::mouseClick(window.spectrometer_window()->findChild<QPushButton*>(QStringLiteral("peakCenterStart")),
                        Qt::LeftButton);
      QVERIFY(bridge.peak_center_running());
      window.set_spectrometer(nullptr, false);
      QVERIFY(window.spectrometer_window() == nullptr);
      QVERIFY(!bridge.peak_center_running());
      QCOMPARE(asked, 0);
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
  // ---- a gauge's history (gauge history design, plan task 6) ----

  // Readings the bus carries go into the window's history, for the gauges
  // the line defines only.
  void readingsGoIntoTheHistory() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      // The line's gauges are kept, any other name is not; a start's
      // snapshot puts nothing in (its values have no time).
      const ui::ReadingHistory& history = window.reading_history();
      QVERIFY(history.has("IG1"));
      QVERIFY(history.has("PG1"));
      QVERIFY(!history.has("XX9"));
      QTRY_VERIFY(window.bridge().state().started);
      QVERIFY(!history.oldest("IG1").has_value());
      const TimePoint t0 = window.bridge().now();
      line->bus().publish(PressureSample{"XX9", 1e-8, "torr", t0});
      line->bus().publish(PressureSample{"IG1", 1e-8, "torr", t0});
      QTRY_VERIFY(history.oldest("IG1") == std::optional<TimePoint>(t0));
      QCOMPARE(history.revision("XX9"), 0U);
      QVERIFY(!history.oldest("XX9").has_value());
      line->stop();
    }
  }

  // A click on a gauge opens a window of its own for it, with its alarm
  // limits as guides; a second click brings back the same window.
  void clickingAGaugeOpensItsWindow() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      const TimePoint t0 = window.bridge().now();
      // Around IG1's alarm_high of 1e-4, so the guide is inside the view.
      line->bus().publish(PressureSample{"IG1", 5e-5, "torr", t0});
      line->bus().publish(PressureSample{"IG1", 2e-4, "torr", t0 + 1s});
      line->bus().publish(PressureSample{"PG1", 1e-3, "torr", t0});
      line->bus().publish(PressureSample{"PG1", 2e-3, "torr", t0 + 1s});
      QTRY_VERIFY(window.reading_history().revision("PG1") >= 2);
      QVERIFY(window.gauge_window(QStringLiteral("IG1")) == nullptr);

      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      ui::GaugeWindow* ig = window.gauge_window(QStringLiteral("IG1"));
      QVERIFY(ig != nullptr);
      QVERIFY(ig->isVisible());
      QVERIFY(ig->isWindow());
      QCOMPARE(ig->windowTitle(), QStringLiteral("IG1"));
      QCOMPARE(ig->chart()->guide_count(), 1);
      QCOMPARE(ig->chart()->point_count(), 2);

      ig->close();
      QVERIFY(!ig->isVisible());
      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      QCOMPARE(window.gauge_window(QStringLiteral("IG1")), ig);
      QVERIFY(ig->isVisible());

      emit window.canvas_view()->gauge_clicked(QStringLiteral("PG1"));
      ui::GaugeWindow* pg = window.gauge_window(QStringLiteral("PG1"));
      QVERIFY(pg != nullptr);
      QVERIFY(pg != ig);
      QCOMPARE(pg->windowTitle(), QStringLiteral("PG1"));
      QCOMPARE(pg->chart()->guide_count(), 0);

      // A name the line does not define opens nothing.
      emit window.canvas_view()->gauge_clicked(QStringLiteral("XX9"));
      QVERIFY(window.gauge_window(QStringLiteral("XX9")) == nullptr);
      line->stop();
    }
  }

  // Readings published from another thread, as the line's scan does, are
  // drawn in the window that is open.
  void aGaugeWindowFollowsNewReadings() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      ui::GaugeWindow* ig = window.gauge_window(QStringLiteral("IG1"));
      QVERIFY(ig != nullptr);
      QCOMPARE(ig->chart()->point_count(), 0);
      const TimePoint t0 = window.bridge().now();
      std::thread([&line, t0] {
        for (int i = 0; i < 5; ++i) line->bus().publish(PressureSample{"IG1", 1e-8 * (i + 1), "torr", t0 + i * 1s});
      }).join();
      QTRY_COMPARE(ig->chart()->point_count(), 5);
      std::thread([&line, t0] { line->bus().publish(PressureSample{"IG1", 7e-8, "torr", t0 + 5s}); }).join();
      QTRY_COMPARE(ig->chart()->point_count(), 6);
      line->stop();
    }
  }

  // Resting the pointer on a gauge shows the popup of its last ten minutes;
  // moving off hides it; a click hides it and opens the gauge's window.
  void restingOnAGaugeShowsThePopupAndLeavingHidesIt() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      const TimePoint t0 = window.bridge().now();
      line->bus().publish(PressureSample{"IG1", 1e-8, "torr", t0 - 2s});
      line->bus().publish(PressureSample{"IG1", 2e-8, "torr", t0});
      QTRY_VERIFY(window.reading_history().revision("IG1") >= 2);

      ui::CanvasView* canvas = window.canvas_view();
      canvas->set_gauge_rest_ms(0);
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      const ui::GaugeLabelItem* ig = canvas->gauge("IG1");
      QVERIFY(ig != nullptr);
      const QPoint on = canvas->mapFromScene(ig->mapToScene(ig->chip_rect().center()));
      const QRectF chip = ig->mapRectToScene(ig->chip_rect());
      const QPoint off = canvas->mapFromScene(QPointF(chip.center().x(), chip.bottom() + 40));
      ui::GaugePopup* popup = window.gauge_popup();
      QVERIFY(popup != nullptr);
      QVERIFY(!popup->isVisible());

      QTest::mouseMove(canvas->viewport(), off);
      QTest::mouseMove(canvas->viewport(), on);
      QTRY_VERIFY(popup->isVisible());
      QCOMPARE(popup->gauge(), QStringLiteral("IG1"));
      QCOMPARE(popup->chart()->point_count(), 2);
      // Kept current while shown.
      line->bus().publish(PressureSample{"IG1", 3e-8, "torr", t0 + 1s});
      QTRY_COMPARE(popup->chart()->point_count(), 3);

      QTest::mouseMove(canvas->viewport(), off);
      QTRY_VERIFY(!popup->isVisible());

      QTest::mouseMove(canvas->viewport(), on);
      QTRY_VERIFY(popup->isVisible());
      QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, on);
      QTRY_VERIFY(!popup->isVisible());
      QVERIFY(window.gauge_window(QStringLiteral("IG1")) != nullptr);
      QVERIFY(window.gauge_window(QStringLiteral("IG1"))->isVisible());
      line->stop();
    }
  }

  // On a simulated line the window's times are the line's clock's, not today's.
  void wallTimesAreTheLines() {
    VirtualClock clock(VirtualClock::Options{});  // outlives the line; its wall time is not today
    auto line = ui::test::make_example_line({}, &clock);
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      const TimePoint now = clock.now();
      line->bus().publish(PressureSample{"IG1", 1e-8, "torr", now - 20min});
      line->bus().publish(PressureSample{"IG1", 2e-8, "torr", now - 10min});
      line->bus().publish(PressureSample{"IG1", 3e-8, "torr", now});
      QTRY_VERIFY(window.reading_history().revision("IG1") >= 3);

      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      ui::GaugeWindow* ig = window.gauge_window(QStringLiteral("IG1"));
      QVERIFY(ig != nullptr);
      ig->range_box()->setCurrentIndex(ig->range_box()->findData(qlonglong{3600}));
      QCOMPARE(ig->range(), std::chrono::seconds(3600));

      const qint64 wall_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(clock.wall_now().time_since_epoch()).count();
      const QDateTime oldest = QDateTime::fromMSecsSinceEpoch(wall_ms).addSecs(-20 * 60);
      QVERIFY(oldest.date() != QDate::currentDate());
      QVERIFY2(ig->header().endsWith(QStringLiteral("since ") + oldest.toString(QStringLiteral("hh:mm"))),
               qPrintable(ig->header()));
      const QStringList copied = ig->copy_text().split('\n', Qt::SkipEmptyParts);
      QCOMPARE(copied.size(), 4);
      QVERIFY2(copied.at(1).startsWith(oldest.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + '\t'),
               qPrintable(copied.at(1)));
      line->stop();
    }
  }

  // Closing the main window closes the gauge windows; destroying it with one
  // open and the popup shown leaves nothing behind (ASan).
  void closingTheMainWindowClosesGaugeWindows() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      emit window.canvas_view()->gauge_clicked(QStringLiteral("PG1"));
      QVERIFY(window.gauge_window(QStringLiteral("IG1"))->isVisible());
      QVERIFY(window.close());
      QVERIFY(!window.gauge_window(QStringLiteral("IG1"))->isVisible());
      QVERIFY(!window.gauge_window(QStringLiteral("PG1"))->isVisible());
      line->stop();
    }
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      QVERIFY(line->start().has_value());
      const TimePoint t0 = window.bridge().now();
      line->bus().publish(PressureSample{"IG1", 1e-8, "torr", t0 - 1s});
      line->bus().publish(PressureSample{"IG1", 2e-8, "torr", t0});
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      const ui::GaugeLabelItem* ig = window.canvas_view()->gauge("IG1");
      const QRect local =
          window.canvas_view()->mapFromScene(ig->mapRectToScene(ig->chip_rect())).boundingRect();
      emit window.canvas_view()->gauge_rested(
          QStringLiteral("IG1"), QRect(window.canvas_view()->viewport()->mapToGlobal(local.topLeft()), local.size()));
      QVERIFY(window.gauge_popup()->isVisible());
      QVERIFY(window.gauge_window(QStringLiteral("IG1"))->isVisible());
      // Readings still on their way to the window when it goes.
      line->bus().publish(PressureSample{"IG1", 3e-8, "torr", t0 + 1s});
      line->stop();
    }
    QCoreApplication::processEvents();
  }

  // A gauge window's range is kept in the settings the main window was
  // given, and the next session's window opens on it.
  void gaugeWindowsKeepTheirRangeAcrossASession() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      ui::GaugeWindow* ig = window.gauge_window(QStringLiteral("IG1"));
      QCOMPARE(ig->range(), ui::GaugeWindow::kDefaultRange);
      ig->range_box()->setCurrentIndex(ig->range_box()->findData(qlonglong{3600}));
      QCOMPARE(ig->range(), std::chrono::seconds(3600));
    }
    {
      ui::MainWindow window(*line);
      window.set_gauge_settings(settings_on(dir));
      emit window.canvas_view()->gauge_clicked(QStringLiteral("IG1"));
      QCOMPARE(window.gauge_window(QStringLiteral("IG1"))->range(), std::chrono::seconds(3600));
      emit window.canvas_view()->gauge_clicked(QStringLiteral("PG1"));
      QCOMPARE(window.gauge_window(QStringLiteral("PG1"))->range(), ui::GaugeWindow::kDefaultRange);
    }
    const QSettings saved(dir.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QCOMPARE(saved.value(QStringLiteral("gauge_window/%1/IG1/range_s")
                             .arg(QString::fromStdString(line->config().system.name)))
                 .toLongLong(),
             qlonglong{3600});
  }

  // Every window here was given its settings: none fell back to the application's.
  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(TestDocks)
#include "test_docks.moc"
