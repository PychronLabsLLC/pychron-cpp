// ICanvasState: a CanvasView paints from any state, and without a bridge it
// is read-only; the CoreBridge adapter re-announces the bridge's changes.

#include <memory>
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

  CoreBridge::State state_;

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
    QVERIFY(view.valve("P2")->state() != ValveState::Open);
    fake.state_.valves["P2"] = ValveState::Open;
    emit fake.changed();
    QCOMPARE(view.valve("P2")->state(), ValveState::Open);
    fake.state_.valves["P2"] = ValveState::Closed;
    emit fake.changed();
    QCOMPARE(view.valve("P2")->state(), ValveState::Closed);
  }

  void BridgeAdapterForwardsChanges() {
    CoreBridge bridge(*line_);
    CoreBridgeCanvasState adapter(bridge, nullptr);
    QSignalSpy spy(&adapter, &ICanvasState::changed);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(spy.count() >= 1);  // start()'s Snapshot
    QCOMPARE(&adapter.state(), &bridge.state());
    QCOMPARE(adapter.canvas(), bridge.canvas());

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
