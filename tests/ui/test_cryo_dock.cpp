// CryoDock (plan 2026-10-05, C6): temperatures and setpoints through the
// CoreBridge, and the MainWindow showing it only for a line with a cryostat.

#include <memory>
#include <optional>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "core_bridge.hpp"
#include "cryo_dock.hpp"
#include "main_window.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::ui::CoreBridge;
using pychron::ui::CryoDock;

class TestCryoDock : public QObject {
  Q_OBJECT

 private slots:
  void init() {
    line_ = ui::test::make_cryo_line();
    bridge_ = std::make_unique<CoreBridge>(*line_);
  }

  void cleanup() {
    line_->stop();
    bridge_.reset();
    line_.reset();
  }

  void showsEveryInputAndOutput() {
    CryoDock dock(*bridge_);
    QCOMPARE(bridge_->cryo_inputs(), (std::vector<std::string>{"A", "B"}));
    QCOMPARE(bridge_->cryo_outputs(), 2);
    QCOMPARE(dock.temperature_text("A"), QStringLiteral("—"));
    QVERIFY(dock.setpoint_field(1) != nullptr);
    QVERIFY(dock.setpoint_field(2) != nullptr);
    QVERIFY(dock.setpoint_field(3) == nullptr);
    QCOMPARE(dock.chart_model().detectors().size(), std::size_t{2});
  }

  void temperaturesAndSetpointsArriveOnceTheLineStarts() {
    CryoDock dock(*bridge_);
    QVERIFY(line_->start().has_value());
    // start() reads every input once and publishes it (the sim starts near
    // room temperature and drifts on the wall clock).
    QTRY_COMPARE(bridge_->state().temperatures.size(), std::size_t{2});
    for (const char* input : {"A", "B"}) {
      const double kelvin = bridge_->state().temperatures.at(input);
      QVERIFY(qAbs(kelvin - 293.15) < 0.5);
      QCOMPARE(dock.temperature_text(input), QStringLiteral("%1 K").arg(kelvin, 0, 'f', 2));
    }
    QCOMPARE(dock.chart_model().series(0).size(), std::size_t{1});
    QCOMPARE(dock.chart()->graph_count(), 2);
    // ...and the Snapshot reads every setpoint back, which seeds the fields.
    QTRY_VERIFY(!dock.setpoint_text(1).isEmpty() && dock.setpoint_text(1) != QStringLiteral("—"));
    QTRY_VERIFY(dock.setpoint_text(2) != QStringLiteral("—"));
    QCOMPARE(QStringLiteral("%1 K").arg(dock.setpoint_field(1)->value(), 0, 'f', 2), dock.setpoint_text(1));
  }

  void aSampleLaterMovesTheLabelAndTheChart() {
    CryoDock dock(*bridge_);
    QVERIFY(line_->start().has_value());
    QTRY_COMPARE(dock.chart_model().series(1).size(), std::size_t{1});
    line_->bus().publish(TemperatureSample{"cryostat", "B", 80.5, line_->clock().now()});
    QTRY_COMPARE(dock.temperature_text("B"), QStringLiteral("80.50 K"));
    QCOMPARE(dock.chart_model().series(1).size(), std::size_t{2});
    QCOMPARE(dock.chart_model().series(0).size(), std::size_t{1});  // A is not touched
    // An input the dock does not show is ignored.
    line_->bus().publish(TemperatureSample{"cryostat", "C", 4.0, line_->clock().now()});
    QTest::qWait(20);
    QCOMPARE(dock.temperature_text("C"), QString());
  }

  void setSendsTheFieldAndShowsTheReadback() {
    CryoDock dock(*bridge_);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(dock.setpoint_text(1) != QStringLiteral("—"));
    dock.setpoint_field(1)->setValue(77.0);
    dock.set_button(1)->click();
    QVERIFY(!dock.set_button(1)->isEnabled());  // until the controller answers
    QTRY_COMPARE(dock.setpoint_text(1), QStringLiteral("77.00 K"));
    QVERIFY(dock.set_button(1)->isEnabled());
    QCOMPARE(*line_->cryostat()->setpoint(1), 77.0);
    QVERIFY(dock.status_text().isEmpty());
  }

  void aRefusedSetIsReportedAndTheReadbackKept() {
    CryoDock dock(*bridge_);
    QSignalSpy failed(&dock, &CryoDock::cryoFailed);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(dock.setpoint_text(2) != QStringLiteral("—"));
    const QString before = dock.setpoint_text(2);
    dock.setpoint_field(2)->setValue(350.0);  // output 2 has no range above 300 K
    dock.apply_setpoint(2);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(dock.status_text().contains(QStringLiteral("setpoint 2 not set")));
    QCOMPARE(dock.setpoint_text(2), before);
    QVERIFY(dock.set_button(2)->isEnabled());
  }

  void aLineWithoutACryostatAnswersEveryCommandWithAnError() {
    auto plain = ui::test::make_example_line();
    CoreBridge bridge(*plain);
    QVERIFY(bridge.cryo_inputs().empty());
    QCOMPARE(bridge.cryo_outputs(), 0);
    std::optional<Result<double>> answer;
    connect(&bridge, &CoreBridge::cryoSetpoint, this,
            [&](int output, bool set, const Result<double>& r) {
              QCOMPARE(output, 1);
              QVERIFY(set);
              answer = r;
            });
    bridge.set_cryo_setpoint(1, 77.0);
    QTRY_VERIFY(answer.has_value());
    QVERIFY(!*answer);
    QCOMPARE(answer->error().kind, ErrorKind::Config);
  }

  void theMainWindowHasTheDockOnlyWithACryostat() {
    {
      ui::MainWindow window(*line_);
      QVERIFY(window.cryo_dock() != nullptr);
    }
    auto plain = ui::test::make_example_line();
    ui::MainWindow window(*plain);
    QVERIFY(window.cryo_dock() == nullptr);
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<CoreBridge> bridge_;
};

QTEST_MAIN(TestCryoDock)
#include "test_cryo_dock.moc"
