// CanvasView: items built from canvas.toml, painted from the bridge snapshot;
// click-to-actuate, interlock rejection feedback, network region colouring,
// gauge values and alarm colouring.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <QtTest/QtTest>

#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "theme.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::systems::SwitchOp;
using pychron::ui::CanvasView;
using pychron::ui::CoreBridge;

namespace {

// True when the pixel just outside the left edge of the valve body, rendered
// over white, is the lock colour (the border is 3 px wide, centred on the edge).
bool hasLockBorder(ui::ValveItem* item) {
  QImage image(60, 60, QImage::Format_ARGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  painter.translate(30, 30);
  item->paint(&painter, nullptr, nullptr);
  painter.end();
  const QColor px = image.pixelColor(30 - static_cast<int>(ui::ValveItem::kSize / 2) - 1, 30);
  const QColor want = ui::ValveItem::lock_color();
  return std::abs(px.red() - want.red()) < 8 && std::abs(px.green() - want.green()) < 8 &&
         std::abs(px.blue() - want.blue()) < 8;
}

}  // namespace

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

  void lockedValveDrawsBlueBorderAndUnlockClearsIt() {
    ui::ValveItem* a = view_->valve("A");
    QVERIFY(!a->locked());
    QVERIFY(!hasLockBorder(a));

    view_->set_confirm_unlock([](const QString&) { return true; });
    QVERIFY(view_->request_lock("A", true));
    QTRY_VERIFY(a->locked());
    QVERIFY(line_->is_locked("A"));
    QVERIFY(hasLockBorder(a));

    QVERIFY(view_->request_lock("A", false));
    QTRY_VERIFY(!a->locked());
    QVERIFY(!line_->is_locked("A"));
    QVERIFY(!hasLockBorder(a));
  }

  void unlockAsksForConfirmationAndCanBeDeclined() {
    view_->set_confirm_unlock([](const QString&) { return true; });
    QVERIFY(view_->request_lock("A", true));
    QTRY_VERIFY(view_->valve("A")->locked());

    QStringList asked;
    view_->set_confirm_unlock([&](const QString& name) {
      asked << name;
      return false;
    });
    QVERIFY(!view_->request_lock("A", false));
    QCOMPARE(asked, QStringList{QStringLiteral("A")});
    QVERIFY(line_->is_locked("A"));
    QVERIFY(view_->valve("A")->locked());
  }

  void lockingNeverAsksForConfirmation() {
    bool asked = false;
    view_->set_confirm_unlock([&](const QString&) {
      asked = true;
      return true;
    });
    QVERIFY(view_->request_lock("B", true));
    QVERIFY(!asked);
  }

  void lockedValveRefusesClicksAndFlashes() {
    view_->set_confirm_unlock([](const QString&) { return true; });
    QVERIFY(view_->request_lock("B", true));
    QTRY_VERIFY(view_->valve("B")->locked());
    view_->show();
    QVERIFY(QTest::qWaitForWindowExposed(view_.get()));
    ui::ValveItem* b = view_->valve("B");
    QTest::mouseClick(view_->viewport(), Qt::LeftButton, {}, view_->mapFromScene(b->scenePos()));
    QTRY_VERIFY(b->is_flashing());
    QCOMPARE(b->state(), ValveState::Closed);
    QVERIFY(b->locked());
  }

  void manualValvesCannotBeLocked() {
    QVERIFY(!view_->request_lock("M1", true));
    QVERIFY(!view_->valve("M1")->locked());
    QVERIFY(!view_->request_lock("nope", true));
  }

  void switchesCanBeLocked() {
    QVERIFY(view_->request_lock("pump_power", true));
    QTRY_VERIFY(view_->valve("pump_power")->locked());
  }

  void openValveStaysGreenByDefault() {
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("A")->state(), ValveState::Open, 5000);
    QCOMPARE(view_->valve("A")->fill_color(), ui::valve_color(ValveState::Open));
  }

  void inheritModeColoursOpenValveWithItsRegion() {
    view_->set_open_valve_color(canvas::OpenValveColor::Inherit);
    ui::ValveItem* a = view_->valve("A");
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(a->state(), ValveState::Open, 5000);
    const QColor region = view_->stage("bone")->region_color();
    QVERIFY(region != CanvasView::isolated_color());
    QCOMPARE(a->fill_color(), region);
    QVERIFY(a->fill_color() != ui::valve_color(ValveState::Open));

    bridge_->actuate("A", SwitchOp::Close);
    QTRY_COMPARE_WITH_TIMEOUT(a->state(), ValveState::Closed, 5000);
    QCOMPARE(a->fill_color(), ui::valve_color(ValveState::Closed));

    view_->set_open_valve_color(canvas::OpenValveColor::Green);
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(a->state(), ValveState::Open, 5000);
    QCOMPARE(a->fill_color(), ui::valve_color(ValveState::Open));
  }

  void inheritModeLeavesIsolatedOpenValveGreen() {
    // pump_power is a switch outside every shared region: it joins no volumes.
    view_->set_open_valve_color(canvas::OpenValveColor::Inherit);
    bridge_->actuate("pump_power", SwitchOp::Open);
    ui::ValveItem* p = view_->valve("pump_power");
    QTRY_COMPARE_WITH_TIMEOUT(p->state(), ValveState::Open, 5000);
    QCOMPARE(p->fill_color(), ui::valve_color(ValveState::Open));
  }

  void inheritModeKeepsLockBorderAndFlash() {
    view_->set_open_valve_color(canvas::OpenValveColor::Inherit);
    view_->set_confirm_unlock([](const QString&) { return true; });
    bridge_->actuate("A", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("A")->state(), ValveState::Open, 5000);
    QVERIFY(view_->request_lock("A", true));
    QTRY_VERIFY(view_->valve("A")->locked());
    QVERIFY(hasLockBorder(view_->valve("A")));
    view_->valve("A")->flash("x");
    QVERIFY(view_->valve("A")->is_flashing());
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
        QCOMPARE(pipe->region_color(), region);
        ++coloured;
      }
      // A pipe on the far side of the closed valve B never takes bone's colour.
      if (touches(pipe, "B") && touches(pipe, "spec")) {
        QVERIFY(pipe->region_color() != region);
      }
    }
    QVERIFY(coloured >= 2);
  }

  // A connection with an orientation is drawn in straight runs, never on a
  // slant (the legacy importer writes them; legacy canvases rely on them):
  // straight when the ends line up, else round one corner.
  void orientedConnectionsAreDrawnSquare() {
    const std::filesystem::path examples = PYCHRON_EXAMPLE_CONFIGS_DIR;
    QTemporaryDir tmp;
    const std::filesystem::path canvas = std::filesystem::path(tmp.path().toStdString()) / "canvas.toml";
    std::filesystem::copy_file(examples / "canvas.toml", canvas);
    // B (550, 200) and turbo (650, 300; 60 x 40) do not line up either way.
    std::ofstream(canvas, std::ios::app) << "\n[[connection]]\nstart = \"B\"\nend = \"turbo\"\norientation = \"v\"\n";
    // offsets move where a pipe meets its element: A (250, 200) to the top
    // edge of bone (100, 200; 80 x 40), 30 left of its centre
    std::ofstream(canvas, std::ios::app)
        << "\n[[connection]]\nstart = \"A\"\nend = \"bone\"\nstart_offset = [0, -15]\nend_offset = [-30, -20]\n";
    auto line = ui::test::make_example_line(canvas);
    CoreBridge bridge(*line);
    CanvasView view(bridge);
    auto route = [&](const char* a, const char* b) {
      std::vector<QPointF> points;
      for (const ui::ConnectionItem* pipe : view.pipes()) {
        if (pipe->endpoints() != std::vector<std::string>{a, b}) continue;
        const QPainterPath path = pipe->path();
        for (int i = 0; i < path.elementCount(); ++i) points.emplace_back(path.elementAt(i));
      }
      return points;
    };
    // down first, then across
    QCOMPARE(route("B", "turbo"), (std::vector<QPointF>{{550, 200}, {550, 300}, {650, 300}}));
    QCOMPARE(route("A", "bone"), (std::vector<QPointF>{{250, 185}, {70, 180}}));
    // the example's own: P1 straight under prep
    QCOMPARE(route("prep", "P1"), (std::vector<QPointF>{{400, 200}, {400, 330}}));
    line->stop();
  }

  // An elbow turns at the named corner of its ends' bounding box. Named a
  // corner one of its ends sits on, it turns level with the end instead.
  void elbowsTurnOneSquareCorner() {
    const std::filesystem::path examples = PYCHRON_EXAMPLE_CONFIGS_DIR;
    QTemporaryDir tmp;
    const std::filesystem::path canvas = std::filesystem::path(tmp.path().toStdString()) / "canvas.toml";
    std::filesystem::copy_file(examples / "canvas.toml", canvas);
    // B (550, 200), A (250, 200), turbo (650, 300), P2 (400, 510).
    std::ofstream(canvas, std::ios::app) << "\n[[elbow]]\nstart = \"B\"\nend = \"turbo\"\ncorner = \"ur\"\n"
                                         << "\n[[elbow]]\nstart = \"A\"\nend = \"turbo\"\ncorner = \"ul\"\n"
                                         << "\n[[elbow]]\nstart = \"P1\"\nend = \"P2\"\ncorner = \"ll\"\n";
    auto line = ui::test::make_example_line(canvas);
    CoreBridge bridge(*line);
    CanvasView view(bridge);
    auto route = [&](const char* a, const char* b) {
      std::vector<QPointF> points;
      for (const ui::ConnectionItem* pipe : view.pipes()) {
        if (pipe->endpoints() != std::vector<std::string>{a, b} || pipe->path().elementCount() < 2) continue;
        if (!points.empty()) points.clear();  // the last one added: the elbow
        const QPainterPath path = pipe->path();
        for (int i = 0; i < path.elementCount(); ++i) points.emplace_back(path.elementAt(i));
      }
      return points;
    };
    QCOMPARE(route("B", "turbo"), (std::vector<QPointF>{{550, 200}, {650, 200}, {650, 300}}));
    QCOMPARE(route("A", "turbo"), (std::vector<QPointF>{{250, 200}, {250, 300}, {650, 300}}));
    QCOMPARE(route("P1", "P2"), (std::vector<QPointF>{{400, 330}, {400, 510}}));  // lined up
    line->stop();
  }

  // Pipes are bordered like every other component, and where one runs into
  // a volume the volume's border is broken (legacy pychron's look).
  void pipesAreBorderedAndBreakTheBorderOfVolumesTheyEnter() {
    const ui::ConnectionItem* pipe = nullptr;
    for (const ui::ConnectionItem* p : view_->pipes()) {
      if (p->endpoints() == std::vector<std::string>{"prep", "P1"}) pipe = p;
    }
    QVERIFY(pipe != nullptr);
    // the border: the same path, a border wider each side, under every fill
    QVERIFY(pipe->outline()->scene() == pipe->scene());
    QCOMPARE(pipe->outline()->path(), pipe->path());
    QCOMPARE(pipe->outline()->pen().widthF(), pipe->pen().widthF() + 2 * ui::ConnectionItem::kBorderWidth);
    QCOMPARE(pipe->outline()->pen().color(), ui::theme().text);
    QVERIFY(pipe->outline()->zValue() < pipe->zValue());
    for (const ui::ConnectionItem* other : view_->pipes()) QVERIFY(pipe->outline()->zValue() < other->zValue());

    // one gap, at prep (a volume); none at P1 (a valve keeps its border)
    QCOMPARE(pipe->gaps().size(), std::size_t{1});
    const QGraphicsPathItem* gap = pipe->gaps().front();
    QVERIFY(gap->scene() == pipe->scene());
    QVERIFY(gap->zValue() > view_->stage("prep")->zValue());
    QVERIFY(gap->zValue() < view_->valve("P1")->zValue());
    // it straddles prep's bottom edge, on the pipe (x = 400)
    const QRectF prep = view_->stage("prep")->sceneBoundingRect().adjusted(1, 1, -1, -1);
    const QRectF across = gap->path().boundingRect();
    QCOMPARE(across.center().x(), 400.0);
    QVERIFY(across.top() < prep.bottom() && across.bottom() > prep.bottom());
    QCOMPARE(gap->pen().widthF(), pipe->pen().widthF());

    // and wears the pipe's colour as the region changes
    QCOMPARE(gap->pen().color(), pipe->region_color());
    bridge_->actuate("P1", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("P1")->state(), ValveState::Open, 5000);
    QVERIFY(pipe->region_color() != ui::ConnectionItem::default_color());
    QCOMPARE(gap->pen().color(), pipe->region_color());
  }

  // A pipe that lands in a volume's rounded corner: the border curves in
  // from the edge there, and the gap reaches in as far as it does.
  void gapReachesRoundAVolumesCorner() {
    const QRectF box(60, 180, 80, 40);  // the example's bone: radius 8
    const ui::BoxEntry mid{{140, 200}, {-1, 0}};
    QCOMPARE(ui::StageItem::border_inset(box, mid, 6), 0.0);
    // 4 below the top-right corner, 6 wide: its upper side is 1 from the corner
    const ui::BoxEntry corner{{140, 184}, {-1, 0}};
    const double inset = ui::StageItem::border_inset(box, corner, 6);
    QVERIFY(std::abs(inset - (8 - std::sqrt(64.0 - 49.0))) < 1e-9);
    // through the top edge, by the left corner
    QVERIFY(std::abs(ui::StageItem::border_inset(box, {{64, 180}, {0, 1}}, 6) - inset) < 1e-9);

    ui::ConnectionItem pipe({{250, 184}, {100, 184}}, 6, {"A", "bone"});
    const QGraphicsPathItem* gap = pipe.add_gap(corner, inset);
    const QRectF across = gap->path().boundingRect();
    QCOMPARE(across.right(), 140 + ui::ConnectionItem::kBorderWidth);
    QVERIFY(std::abs(across.left() - (140 - inset - ui::ConnectionItem::kBorderWidth)) < 1e-9);
    delete gap;
    delete pipe.outline();
  }

  // Every pipe on the NMGRL line breaks the border of each volume it joins:
  // ends that legacy offsets put on a volume's far edge (D into Bone) or a
  // fraction of a pixel short of it (J into FurnaceManifold) included.
  void everyPipeIntoAVolumeBreaksItsBorder() {
    const std::filesystem::path dir = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "nmgrl";
    systems::ExtractionLine::Options options;
    options.force_sim = true;
    options.run_scheduler = false;
    options.state_file = std::filesystem::path(QDir::tempPath().toStdString()) / "pychron-ui-test-nmgrl.state.toml";
    auto line = systems::ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", options);
    QVERIFY2(line.has_value(), line ? "" : line.error().what.c_str());
    CoreBridge bridge(**line);
    CanvasView view(bridge);
    int checked = 0;
    for (const ui::ConnectionItem* pipe : view.pipes()) {
      const auto& ends = pipe->endpoints();
      if (ends.size() != 2) continue;  // a tee's arms name all three
      const std::size_t volumes = (view.stage(ends[0]) != nullptr) + (view.stage(ends[1]) != nullptr);
      QVERIFY2(pipe->gaps().size() == volumes, (ends[0] + " - " + ends[1]).c_str());
      checked += static_cast<int>(volumes);
    }
    QVERIFY(checked > 40);
  }

  // A stage's symbol is a glyph inside its box, placed where it fits.
  void stageSymbolsFitInsideTheBox() {
    QCOMPARE(view_->stage("spec")->symbol(), canvas::StageSymbol::None);
    const QSizeF label(40, 16);
    ui::StageItem tall("Jan", "Jan", {58, 83}, Qt::white);
    QVERIFY(tall.symbol_rect(label).isEmpty());  // no symbol asked for
    tall.set_symbol(canvas::StageSymbol::Spectrometer);
    // above the name: the box's width, the height the name leaves
    QCOMPARE(tall.symbol_rect(label), QRectF(-25, -37.5, 50, 59));
    ui::StageItem wide("Quad", "Quad", {70, 31}, Qt::white);
    wide.set_symbol(canvas::StageSymbol::Spectrometer);
    // too short for both: a square beside the name
    QCOMPARE(wide.symbol_rect(label), QRectF(-31, -11.5, 22, 23));
    ui::StageItem small("x", "x", {40, 20}, Qt::white);
    small.set_symbol(canvas::StageSymbol::Laser);
    QVERIFY(small.symbol_rect(label).isEmpty());  // no room: the name alone

    // painted: dark strokes above the name that the plain box lacks
    auto dark_pixels = [&](ui::StageItem& item) {
      QImage image(58, 83, QImage::Format_ARGB32);
      image.fill(Qt::white);
      QPainter painter(&image);
      painter.translate(29, 41.5);
      item.paint(&painter, nullptr, nullptr);
      const QRectF area = tall.symbol_rect(painter.fontMetrics().size(Qt::TextSingleLine, "Jan")).translated(29, 41.5);
      painter.end();
      int dark = 0;
      // the upper part only: a plain box centres its name lower down
      for (int y = int(area.top()) + 2; y < int(area.center().y()) - 8; ++y)
        for (int x = int(area.left()) + 2; x < int(area.right()) - 2; ++x) dark += image.pixelColor(x, y).lightness() < 100;
      return dark;
    };
    ui::StageItem plain("Jan", "Jan", {58, 83}, Qt::white);
    QCOMPARE(dark_pixels(plain), 0);
    QVERIFY(dark_pixels(tall) > 10);
  }

  // A region keeps its colour when another region appears or goes away.
  void regionsKeepTheirColourAsOthersComeAndGo() {
    auto open = [&](const char* valve, bool on) {
      bridge_->actuate(valve, on ? SwitchOp::Open : SwitchOp::Close);
      QTRY_COMPARE_WITH_TIMEOUT(view_->valve(valve)->state(), on ? ValveState::Open : ValveState::Closed, 5000);
    };
    // turbo and its gauge are one region from the start; the air pipette's
    // volumes make another once P2 opens; bone + prep a third with A.
    open("P2", true);
    const QColor turbo = view_->stage("turbo")->region_color();
    const QColor tank = view_->stage("air_tank")->region_color();
    QVERIFY(turbo != CanvasView::isolated_color());
    QVERIFY(tank != CanvasView::isolated_color());
    QVERIFY(turbo != tank);

    open("A", true);
    const QColor bone = view_->stage("bone")->region_color();
    QVERIFY(bone != turbo && bone != tank && bone != CanvasView::isolated_color());
    QCOMPARE(view_->stage("turbo")->region_color(), turbo);
    QCOMPARE(view_->stage("air_tank")->region_color(), tank);

    // each goes away in turn: the others do not change
    open("P2", false);
    QCOMPARE(view_->stage("air_tank")->region_color(), CanvasView::isolated_color());
    QCOMPARE(view_->stage("bone")->region_color(), bone);
    QCOMPARE(view_->stage("turbo")->region_color(), turbo);
    open("P2", true);
    open("A", false);
    QCOMPARE(view_->stage("bone")->region_color(), CanvasView::isolated_color());
    QCOMPARE(view_->stage("turbo")->region_color(), turbo);

    // joined (prep to turbo through C): one colour, one of the two it had
    open("C", true);
    QCOMPARE(view_->stage("prep")->region_color(), view_->stage("turbo")->region_color());
    QCOMPARE(view_->stage("turbo")->region_color(), turbo);
  }

  void boxEntryFindsWhereALineCrossesIntoABox() {
    const QRectF box(80, -10, 40, 20);
    auto in = ui::box_entry({{0, 0}, {100, 0}}, box);
    QVERIFY(in.has_value());
    QCOMPARE(in->edge, QPointF(80, 0));
    QCOMPARE(in->inward, QPointF(1, 0));
    // round a corner that is already inside: the crossing is on the first run
    in = ui::box_entry({{100, 50}, {100, 5}, {110, 5}}, box);
    QVERIFY(in.has_value());
    QCOMPARE(in->edge, QPointF(100, 10));
    QCOMPARE(in->inward, QPointF(0, -1));
    // through to the far edge and a hair beyond (a rounded legacy offset)
    in = ui::box_entry({{200, 50}, {200, 0}, {79.5, 0}}, box);
    QVERIFY(in.has_value());
    QCOMPARE(in->edge, QPointF(120, 0));
    QCOMPARE(in->inward, QPointF(-1, 0));
    QVERIFY(!ui::box_entry({{0, 0}, {50, 0}}, box).has_value());       // stops short
    QVERIFY(!ui::box_entry({{0, 20}, {200, 20}}, box).has_value());    // passes by
    QVERIFY(!ui::box_entry({{90, 0}, {100, 0}}, box).has_value());     // never outside
  }

  // A gauge's reading has a small dial to its left, inside the item's bounds.
  void gaugesWearADial() {
    const ui::GaugeLabelItem* ig = view_->gauge("IG1");
    const QRectF dial = ig->dial_rect();
    QCOMPARE(dial.width(), dial.height());
    QVERIFY(dial.width() > 8);
    QVERIFY(dial.right() < 0);  // left of the text, which starts at x = 0
    QVERIFY(ig->boundingRect().contains(dial));
  }

  void gaugeLabelTurnsRedOnAlarmAndClearsInLimits() {
    std::thread([this] {
      line_->bus().publish(PressureSample{"IG1", 5e-3, "torr", {}});
      line_->bus().publish(Alarm{"IG1", AlarmSeverity::Critical, "too high", {}});
    }).join();
    ui::GaugeLabelItem* ig = view_->gauge("IG1");
    QTRY_VERIFY(ig->in_alarm());
    QCOMPARE(ig->brush().color(), ui::theme().error_text);
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
