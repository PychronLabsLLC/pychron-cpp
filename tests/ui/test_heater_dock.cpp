// HeaterDock (plan 2026-10-05, E3): heater rows through the CoreBridge, the
// confirmation before switching, and the MainWindow showing the dock only
// for a line with heaters.

#include <memory>
#include <optional>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "core_bridge.hpp"
#include "heater_dock.hpp"
#include "main_window.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::ui::CoreBridge;
using pychron::ui::HeaterDock;

class TestHeaterDock : public QObject {
  Q_OBJECT

 private slots:
  void init() {
    line_ = ui::test::make_heater_line();
    bridge_ = std::make_unique<CoreBridge>(*line_);
  }

  void cleanup() {
    line_->stop();
    bridge_.reset();
    line_.reset();
  }

  void aRowPerHeaterFilledByTheFirstScan() {
    HeaterDock dock(*bridge_);
    QVERIFY(dock.power_button("furnace") && dock.power_button("bake"));
    QVERIFY(dock.power_button("nope") == nullptr);
    QCOMPARE(dock.chart_model().detectors().size(), std::size_t{2});
    QVERIFY(line_->start().has_value());
    QTRY_COMPARE(bridge_->state().heaters.size(), std::size_t{2});
    QCOMPARE(dock.power_button("furnace")->text(), QStringLiteral("Off"));
    QCOMPARE(dock.setpoint_field("furnace")->text(), QStringLiteral("0.00"));
    QVERIFY(qAbs(dock.readback("furnace")->value() - 25.0) < 0.01);
    QCOMPARE(dock.chart_model().series(0).size(), std::size_t{1});
    // bake has no setpoint or PID: those controls are off.
    QVERIFY(dock.power_button("bake")->isEnabled());
    QVERIFY(!dock.pid_box("bake")->isEnabled());
    QVERIFY(!dock.setpoint_field("bake")->isEnabled());
    QVERIFY(dock.pid_box("furnace")->isEnabled());
  }

  void switchingAsksFirstAndShowsTheHeatersAnswer() {
    HeaterDock dock(*bridge_);
    QVERIFY(line_->start().has_value());
    QTRY_COMPARE(bridge_->state().heaters.size(), std::size_t{2});
    QString asked;
    bool answer = false;
    dock.set_confirm([&](const QString& q) {
      asked = q;
      return answer;
    });
    dock.power_button("furnace")->click();
    QCOMPARE(asked, QStringLiteral("Turn furnace on?"));
    QVERIFY(!dock.power_button("furnace")->isChecked());  // declined: nothing sent
    bridge_->drain();
    QTest::qWait(10);
    QCOMPARE(*bridge_->state().heaters.at("furnace").enabled, false);

    answer = true;
    dock.power_button("furnace")->click();
    QVERIFY(!dock.power_button("furnace")->isEnabled());  // until the PLC answers
    QTRY_COMPARE(dock.power_button("furnace")->text(), QStringLiteral("On"));
    QVERIFY(dock.power_button("furnace")->isChecked());
    QVERIFY(dock.power_button("furnace")->isEnabled());
    QVERIFY(line_->sim()->heater("furnace_plc")->enabled());
  }

  void setpointGoesOnEnterAndPidOnClick() {
    HeaterDock dock(*bridge_);
    QVERIFY(line_->start().has_value());
    QTRY_COMPARE(bridge_->state().heaters.size(), std::size_t{2});
    auto* field = dock.setpoint_field("furnace");
    field->setText(QStringLiteral("450"));
    field->setModified(true);
    QTest::keyClick(field, Qt::Key_Return);
    QTRY_COMPARE(*bridge_->state().heaters.at("furnace").setpoint, 450.0);
    QCOMPARE(line_->sim()->heater("furnace_plc")->setpoint(), 450.0);
    dock.pid_box("furnace")->click();
    QTRY_COMPARE(*bridge_->state().heaters.at("furnace").use_pid, true);
    QVERIFY(dock.pid_box("furnace")->isChecked());
    QVERIFY(dock.status_text().isEmpty());
  }

  void aRefusalIsReportedAndTheFieldGoesBack() {
    HeaterDock dock(*bridge_);
    QSignalSpy failed(&dock, &HeaterDock::heaterFailed);
    QVERIFY(line_->start().has_value());
    QTRY_COMPARE(bridge_->state().heaters.size(), std::size_t{2});
    auto* field = dock.setpoint_field("furnace");
    field->setText(QStringLiteral("450.5"));  // the PLC takes whole numbers
    field->setModified(true);
    QTest::keyClick(field, Qt::Key_Return);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(dock.status_text().contains(QStringLiteral("whole number")));
    QCOMPARE(field->text(), QStringLiteral("0.00"));
    QVERIFY(field->isEnabled());
  }

  void theMainWindowHasTheDockOnlyWithHeaters() {
    {
      ui::MainWindow window(*line_);
      QVERIFY(window.heater_dock() != nullptr);
    }
    auto plain = ui::test::make_example_line();
    ui::MainWindow window(*plain);
    QVERIFY(window.heater_dock() == nullptr);
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<CoreBridge> bridge_;
};

QTEST_MAIN(TestHeaterDock)
#include "test_heater_dock.moc"
