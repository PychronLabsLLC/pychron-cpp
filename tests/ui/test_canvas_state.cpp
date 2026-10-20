// ICanvasState: a CanvasView paints from any state, and without a bridge it
// is read-only; the CoreBridge adapter re-announces the bridge's changes.

#include <memory>
#include <set>
#include <string>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "canvas_state.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::ui::CanvasView;
using pychron::ui::CoreBridge;
using pychron::ui::CoreBridgeCanvasState;
using pychron::ui::ICanvasState;

namespace {

class FakeCanvasState final : public ICanvasState {
 public:
  explicit FakeCanvasState(const canvas::Canvas* canvas) : canvas_(canvas) {}

  [[nodiscard]] const CoreBridge::State& state() const override { return state_; }
  [[nodiscard]] bool pending(const std::string& name) const override { return state_.pending.contains(name); }
  [[nodiscard]] QDateTime wall_now() const override { return QDateTime::fromSecsSinceEpoch(1'000'000); }
  [[nodiscard]] const canvas::Canvas* canvas() const override { return canvas_; }
  [[nodiscard]] bool gauge_defined(const std::string& name) const override { return defined_.contains(name); }

  CoreBridge::State state_;
  std::set<std::string> defined_;

 private:
  const canvas::Canvas* canvas_;
};

}  // namespace

class TestCanvasState : public QObject {
  Q_OBJECT

 private slots:
  void init() { line_ = ui::test::make_example_line(); }

  void ReadOnlyClickDoesNothing() {
    FakeCanvasState fake(line_->canvas());
    CanvasView view(fake, nullptr);
    view.resize(1100, 800);
    QVERIFY(view.read_only());
    ui::ValveItem* p2 = view.valve("P2");
    QVERIFY(p2 != nullptr);
    const auto before = fake.state().valves;
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, view.mapFromScene(p2->scenePos()));
    QVERIFY(!p2->is_pending());
    QCOMPARE(fake.state().valves, before);
    QVERIFY(fake.state().pending.empty());
    view.set_confirm_unlock([](const QString&) { return true; });
    QVERIFY(!view.request_lock("P2", true));
    QVERIFY(!view.request_lock("P2", false));
    QVERIFY(fake.state().switches.empty());
  }

  void PaintsFromState() {
    FakeCanvasState fake(line_->canvas());
    CanvasView view(fake, nullptr);
    fake.state_.valves["P2"] = ValveState::Closed;
    emit fake.changed();
    QCOMPARE(view.valve("P2")->state(), ValveState::Closed);
    fake.state_.valves["P2"] = ValveState::Open;
    emit fake.changed();
    QCOMPARE(view.valve("P2")->state(), ValveState::Open);
    fake.state_.valves["P2"] = ValveState::Closed;
    emit fake.changed();
    QCOMPARE(view.valve("P2")->state(), ValveState::Closed);
  }

  void ReadOnlyGaugeShowsItsValueAndIsInert() {
    FakeCanvasState fake(line_->canvas());
    fake.defined_.insert("IG1");
    CanvasView view(fake, nullptr);
    view.resize(1100, 800);
    view.set_gauge_rest_ms(0);
    ui::GaugeLabelItem* ig = view.gauge("IG1");
    QVERIFY(ig != nullptr);
    fake.state_.pressures["IG1"] = 1.5e-8;
    fake.state_.units["IG1"] = "torr";
    emit fake.changed();
    QVERIFY(ig->wired());
    QVERIFY2(ig->text().contains(QStringLiteral("torr")), qPrintable(ig->text()));

    QSignalSpy clicked(&view, &CanvasView::gauge_clicked);
    QSignalSpy rested(&view, &CanvasView::gauge_rested);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QPoint at = view.mapFromScene(ig->mapToScene(ig->chip_rect().center()));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, at);
    QTest::mouseMove(view.viewport(), at);
    QTest::qWait(50);
    QCOMPARE(clicked.count(), 0);
    QCOMPARE(rested.count(), 0);
  }

  void BridgeAdapterForwardsChanges() {
    CoreBridge bridge(*line_);
    CoreBridgeCanvasState adapter(bridge, nullptr);
    QSignalSpy spy(&adapter, &ICanvasState::changed);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(spy.count() >= 1);  // start()'s Snapshot
    QCOMPARE(&adapter.state(), &bridge.state());
    QCOMPARE(adapter.canvas(), bridge.canvas());
    QVERIFY(adapter.gauge_defined("IG1"));
    QVERIFY(!adapter.gauge_defined("nonesuch"));

    const int before = spy.count();
    CanvasView view(adapter, &bridge);
    QVERIFY(!view.read_only());
    bridge.actuate(QStringLiteral("B"), systems::SwitchOp::Open);
    QTRY_VERIFY(spy.count() > before);
    QTRY_COMPARE(view.valve("B")->state(), ValveState::Open);
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
};

QTEST_MAIN(TestCanvasState)
#include "test_canvas_state.moc"
