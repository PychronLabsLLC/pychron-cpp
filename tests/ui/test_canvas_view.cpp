// CanvasView: items built from canvas.toml, painted from the bridge snapshot;
// click-to-actuate, interlock rejection feedback, network region colouring,
// gauge values and alarm colouring.

#include <algorithm>
#include <thread>

#include <QtTest/QtTest>

#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::systems::SwitchOp;
using pychron::ui::CanvasView;
using pychron::ui::CoreBridge;

class TestCanvasView : public QObject {
  Q_OBJECT

 private slots:
  void init() {
    line_ = ui::test::make_example_line();
    bridge_ = std::make_unique<CoreBridge>(*line_);
    view_ = std::make_unique<CanvasView>(*bridge_);
    view_->resize(1100, 800);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(bridge_->state().pressures.count("IG1") == 1);
  }

  void cleanup() {
    line_->stop();
    view_.reset();
    bridge_.reset();
    line_.reset();
  }

  void buildsItemsFromCanvas() {
    for (const char* name : {"A", "B", "C", "P1", "P2", "M1", "pump_power"}) {
      QVERIFY2(view_->valve(name) != nullptr, name);
    }
    for (const char* name : {"bone", "prep", "spec", "turbo", "rough", "air_tank", "air"}) {
      QVERIFY2(view_->stage(name) != nullptr, name);
    }
    QVERIFY(view_->gauge("IG1") != nullptr);
    QVERIFY(view_->gauge("PG1") != nullptr);
    QVERIFY(view_->connection_count() >= 10);
    QCOMPARE(view_->valve("A")->kind(), canvas::ValveKind::Valve);
    QCOMPARE(view_->valve("M1")->kind(), canvas::ValveKind::Manual);
  }

  void snapshotPaintsValvesAndGauges() {
    QCOMPARE(view_->valve("A")->state(), bridge_->state().valves.at("A"));
    QVERIFY(view_->gauge("IG1")->text().contains(QStringLiteral("torr")));
  }

  void clickingAValveActuatesIt() {
    ui::ValveItem* b = view_->valve("B");
    QCOMPARE(b->state(), ValveState::Closed);
    view_->show();
    QVERIFY(QTest::qWaitForWindowExposed(view_.get()));
    const QPoint at = view_->mapFromScene(b->scenePos());
    QTest::mouseClick(view_->viewport(), Qt::LeftButton, {}, at);
    QVERIFY(b->is_pending());
    QTRY_COMPARE(b->state(), ValveState::Open);
    QTRY_VERIFY(!b->is_pending());
  }

  void interlockRejectionFlashesWithReason() {
    bridge_->actuate("C", SwitchOp::Open);
    QTRY_COMPARE(view_->valve("C")->state(), ValveState::Open);
    bridge_->actuate("A", SwitchOp::Open);
    ui::ValveItem* a = view_->valve("A");
    QTRY_VERIFY(a->is_flashing());
    QVERIFY(a->toolTip().contains(QStringLiteral("A: ")));
    QVERIFY(a->toolTip().size() > 3);
    QCOMPARE(a->state(), ValveState::Closed);
  }

  void openValveJoinsRegionColours() {
    ui::StageItem* bone = view_->stage("bone");
    ui::StageItem* prep = view_->stage("prep");
    QVERIFY(bone->region_color() != prep->region_color() || bone->region_color() == CanvasView::isolated_color());
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("A")->state(), ValveState::Open, 5000);
    QVERIFY(bone->region_color() != CanvasView::isolated_color());
    QCOMPARE(bone->region_color(), prep->region_color());
    QVERIFY(view_->stage("spec")->region_color() != bone->region_color());
  }

  void pipesInheritRegionColour() {
    auto touches = [](const ui::ConnectionItem* pipe, const char* name) {
      const auto& ends = pipe->endpoints();
      return std::find(ends.begin(), ends.end(), name) != ends.end();
    };
    // bone is isolated until A opens, so its pipes are neutral.
    QCOMPARE(view_->stage("bone")->region_color(), CanvasView::isolated_color());
    for (const ui::ConnectionItem* pipe : view_->pipes()) {
      if (touches(pipe, "bone")) {
        QCOMPARE(pipe->region_color(), ui::ConnectionItem::default_color());
      }
    }
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("A")->state(), ValveState::Open, 5000);
    const QColor region = view_->stage("bone")->region_color();
    QVERIFY(region != CanvasView::isolated_color());
    int coloured = 0;
    for (const ui::ConnectionItem* pipe : view_->pipes()) {
      if (touches(pipe, "bone") || touches(pipe, "A")) {
        QCOMPARE(pipe->region_color(), region.darker(120));
        ++coloured;
      }
      // A pipe on the far side of the closed valve B never takes bone's colour.
      if (touches(pipe, "B") && touches(pipe, "spec")) {
        QVERIFY(pipe->region_color() != region.darker(120));
      }
    }
    QVERIFY(coloured >= 2);
  }

  void gaugeLabelTurnsRedOnAlarmAndClearsInLimits() {
    std::thread([this] {
      line_->bus().publish(PressureSample{"IG1", 5e-3, "torr", {}});
      line_->bus().publish(Alarm{"IG1", AlarmSeverity::Critical, "too high", {}});
    }).join();
    ui::GaugeLabelItem* ig = view_->gauge("IG1");
    QTRY_VERIFY(ig->in_alarm());
    QCOMPARE(ig->brush().color(), QColor(Qt::red));
    QVERIFY(ig->text().contains(QStringLiteral("5.00e-03")));
    std::thread([this] { line_->bus().publish(PressureSample{"IG1", 1e-8, "torr", {}}); }).join();
    QTRY_VERIFY(!ig->in_alarm());
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<CoreBridge> bridge_;
  std::unique_ptr<CanvasView> view_;
};

QTEST_MAIN(TestCanvasView)
#include "test_canvas_view.moc"
