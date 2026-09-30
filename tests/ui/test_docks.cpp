// LogDock, AlarmDock, HealthBar and the assembled MainWindow wiring.

#include <chrono>
#include <thread>

#include <QtTest/QtTest>

#include "main_window.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::systems::SwitchOp;
using namespace std::chrono_literals;
using pychron::ui::AlarmDock;
using pychron::ui::HealthBar;
using pychron::ui::LogDock;

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
