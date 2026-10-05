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
    QVERIFY(a->toolTip().contains(QStringLiteral("Last failure: ")));
    QVERIFY(a->toolTip().contains(QStringLiteral("interlocked")));
    QCOMPARE(a->state(), ValveState::Closed);
  }

  // The tooltip says what the valve is, its state and since when, and what
  // it has been asked to do.
  void tooltipShowsStateAndActuationCounts() {
    ui::ValveItem* b = view_->valve("B");
    // Counts are the session's: earlier tests have used this valve.
    const auto counts = [b] {
      const auto m = QRegularExpression(QStringLiteral("opened (\\d+), closed (\\d+), failed (\\d+)")).match(b->toolTip());
      return std::array<int, 3>{m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt()};
    };
    QVERIFY(view_->valve("P1")->toolTip().contains(QStringLiteral("Not actuated this session")));
    QVERIFY(!view_->valve("P1")->toolTip().contains(QStringLiteral("since")));
    bridge_->actuate("B", SwitchOp::Open);
    QTRY_COMPARE(b->state(), ValveState::Open);
    QTRY_VERIFY(!b->is_pending());
    bridge_->actuate("B", SwitchOp::Close);
    QTRY_VERIFY(b->toolTip().contains(QStringLiteral("Closed since ")));
    QTRY_VERIFY(!b->is_pending());
    const auto before = counts();
    bridge_->actuate("B", SwitchOp::Open);
    QTRY_VERIFY(b->toolTip().contains(QStringLiteral("Open since ")));
    QCOMPARE(counts(), (std::array<int, 3>{before[0] + 1, before[1], before[2]}));
    QVERIFY(b->toolTip().contains(QStringLiteral("Prep to spectrometer")));
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
    ui::ValveItem* b = view_->valve("B");  // joins prep to the spectrometer
    bridge_->actuate("B", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(b->state(), ValveState::Open, 5000);
    const QColor region = view_->stage("prep")->region_color();
    QCOMPARE(region, CanvasView::source_color(canvas::SourceKind::Spectrometer));
    QCOMPARE(b->fill_color(), region);
    QVERIFY(b->fill_color() != ui::valve_color(ValveState::Open));

    bridge_->actuate("B", SwitchOp::Close);
    QTRY_COMPARE_WITH_TIMEOUT(b->state(), ValveState::Closed, 5000);
    QCOMPARE(b->fill_color(), ui::valve_color(ValveState::Closed));

    view_->set_open_valve_color(canvas::OpenValveColor::Green);
    bridge_->actuate("B", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(b->state(), ValveState::Open, 5000);
    QCOMPARE(b->fill_color(), ui::valve_color(ValveState::Open));
  }

  // A valve joining volumes with no source among them has no colour to take.
  void inheritModeLeavesAValveInASourcelessRegionGreen() {
    view_->set_open_valve_color(canvas::OpenValveColor::Inherit);
    bridge_->actuate("A", SwitchOp::Open);  // bone to prep: two plain volumes
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("A")->state(), ValveState::Open, 5000);
    QCOMPARE(view_->valve("A")->fill_color(), ui::valve_color(ValveState::Open));
    QCOMPARE(view_->stage("bone")->region_color(), CanvasView::isolated_color());
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
    ui::StageItem* prep = view_->stage("prep");
    ui::StageItem* spec = view_->stage("spec");
    const QColor spectrometer = CanvasView::source_color(canvas::SourceKind::Spectrometer);
    QCOMPARE(prep->region_color(), CanvasView::isolated_color());
    QCOMPARE(spec->region_color(), spectrometer);  // a source wears its own colour, alone or not
    bridge_->actuate("B", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("B")->state(), ValveState::Open, 5000);
    QCOMPARE(prep->region_color(), spectrometer);
    QCOMPARE(spec->region_color(), spectrometer);
    QCOMPARE(view_->stage("bone")->region_color(), CanvasView::isolated_color());  // behind the closed A
  }

  void pipesInheritRegionColour() {
    auto touches = [](const ui::ConnectionItem* pipe, const char* name) {
      const auto& ends = pipe->endpoints();
      return std::find(ends.begin(), ends.end(), name) != ends.end();
    };
    // prep is connected to no source until B opens, so its pipes are neutral.
    QCOMPARE(view_->stage("prep")->region_color(), CanvasView::isolated_color());
    for (const ui::ConnectionItem* pipe : view_->pipes()) {
      if (touches(pipe, "prep")) {
        QCOMPARE(pipe->region_color(), ui::ConnectionItem::default_color());
      }
    }
    bridge_->actuate("B", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view_->valve("B")->state(), ValveState::Open, 5000);
    const QColor region = view_->stage("prep")->region_color();
    QCOMPARE(region, CanvasView::source_color(canvas::SourceKind::Spectrometer));
    int coloured = 0;
    for (const ui::ConnectionItem* pipe : view_->pipes()) {
      if (touches(pipe, "prep") || touches(pipe, "B")) {
        QCOMPARE(pipe->region_color(), region);
        ++coloured;
      }
      // A pipe on the far side of the closed valve A never takes the colour.
      if (touches(pipe, "bone")) {
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

  // On the NMGRL line: each tank has its own colour, and opening a tank's
  // valve gives the pipette (and the pipes and the valve) the tank's colour.
  void theNmgrlTanksColourWhatIsOpenToThem() {
    const std::filesystem::path dir = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "nmgrl";
    systems::ExtractionLine::Options options;
    options.force_sim = true;
    options.state_file = std::filesystem::path(QDir::tempPath().toStdString()) / "pychron-ui-test-nmgrl-tanks.state.toml";
    std::filesystem::remove(options.state_file);
    auto line = systems::ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", options);
    QVERIFY2(line.has_value(), line ? "" : line.error().what.c_str());
    QVERIFY((*line)->start());
    CoreBridge bridge(**line);
    CanvasView view(bridge);
    auto colour = [&](const char* stage) { return view.stage(stage)->region_color(); };
    const QColor air = colour("Air");
    const QColor cocktail = colour("Cocktail");
    QVERIFY(air != CanvasView::isolated_color());
    QVERIFY(cocktail != CanvasView::isolated_color());
    QVERIFY(air != cocktail);
    // A pipette holds its tank's gas whether or not the valve between them
    // is open: it wears the tank's colour either way.
    QCOMPARE(colour("AirPipette"), air);
    QCOMPARE(colour("CocktailPipette"), cocktail);

    bridge.actuate("Z", SwitchOp::Open);
    QTRY_COMPARE_WITH_TIMEOUT(view.valve("Z")->state(), ValveState::Open, 5000);
    QCOMPARE(colour("Air"), air);
    QCOMPARE(colour("AirPipette"), air);
    QCOMPARE(view.valve("Z")->fill_color(), air);  // the NMGRL canvas has open valves inherit
    QCOMPARE(colour("Cocktail"), cocktail);
    QCOMPARE(colour("CocktailPipette"), cocktail);
    for (const ui::ConnectionItem* pipe : view.pipes()) {
      const auto& ends = pipe->endpoints();
      const bool on_air = std::find(ends.begin(), ends.end(), "Air") != ends.end() ||
                          std::find(ends.begin(), ends.end(), "AirPipette") != ends.end();
      if (on_air && std::find(ends.begin(), ends.end(), "Y") == ends.end()) QCOMPARE(pipe->region_color(), air);
    }
    (*line)->stop();
    std::filesystem::remove(options.state_file);
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
    // too short for both: beside the name, in the width the name and a gap leave
    QCOMPARE(wide.symbol_rect(label), QRectF(-31, -11.5, 19, 23));
    ui::StageItem small("x", "x", {40, 20}, Qt::white);
    small.set_symbol(canvas::StageSymbol::Laser);
    QVERIFY(small.symbol_rect(label).isEmpty());  // no room: the name alone
    ui::StageItem getter("NP-10C", "NP-10C", {58, 31}, Qt::white);
    getter.set_symbol(canvas::StageSymbol::Getter);
    // a name too wide to leave room beside it: the smallest glyph, the name
    // shrunk into the rest
    QCOMPARE(getter.symbol_rect(QSizeF(45, 16)), QRectF(-25, -11.5, 14, 23));

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

  // A region takes the colour of the source connected to it with the highest
  // precedence, and of nothing else: not of the order valves were opened in,
  // nor of what other regions exist.
  void aRegionTakesTheColourOfItsHighestPrecedenceSource() {
    using canvas::SourceKind;
    auto open = [&](const char* valve, bool on) {
      bridge_->actuate(valve, on ? SwitchOp::Open : SwitchOp::Close);
      QTRY_COMPARE_WITH_TIMEOUT(view_->valve(valve)->state(), on ? ValveState::Open : ValveState::Closed, 5000);
    };
    auto colour = [&](const char* stage) { return view_->stage(stage)->region_color(); };
    const QColor none = CanvasView::isolated_color();
    const QColor pump = CanvasView::source_color(SourceKind::Pump);
    const QColor tank = CanvasView::source_color(SourceKind::Tank);
    const QColor spectrometer = CanvasView::source_color(SourceKind::Spectrometer);

    // everything closed: sources wear their own colour, plain volumes none
    QCOMPARE(colour("turbo"), pump);
    QCOMPARE(colour("air_tank"), tank);
    QCOMPARE(colour("air"), tank);  // the pipette wears its tank's colour, valve open or not
    QCOMPARE(colour("spec"), spectrometer);
    QCOMPARE(colour("bone"), none);
    QCOMPARE(colour("prep"), none);

    // tank to pipette (P2): what is open to a tank is the tank's colour
    open("P2", true);
    QCOMPARE(colour("air_tank"), tank);
    QCOMPARE(colour("air"), tank);

    // prep to the spectrometer (B), then bone joins (A): all the spectrometer's
    open("B", true);
    QCOMPARE(colour("prep"), spectrometer);
    open("A", true);
    QCOMPARE(colour("bone"), spectrometer);
    QCOMPARE(colour("prep"), spectrometer);
    QCOMPARE(colour("spec"), spectrometer);
    // other regions were not touched by that
    QCOMPARE(colour("air"), tank);
    QCOMPARE(colour("turbo"), pump);

    // prep to the turbo (C; A must be closed for it): the pump's 120 takes the
    // region, spectrometer included, and bone is cut off again
    open("A", false);
    QCOMPARE(colour("bone"), none);
    open("C", true);
    for (const char* stage : {"prep", "spec", "turbo"}) QCOMPARE(colour(stage), pump);
    QCOMPARE(colour("air"), tank);

    // and back: closing C gives the spectrometer its region again
    open("C", false);
    QCOMPARE(colour("prep"), spectrometer);
    QCOMPARE(colour("spec"), spectrometer);
    QCOMPARE(colour("turbo"), pump);

    // the same state reached in another order has the same colours
    open("B", false);
    open("P2", false);
    open("A", true);
    open("P2", true);
    open("B", true);
    QCOMPARE(colour("bone"), spectrometer);
    QCOMPARE(colour("prep"), spectrometer);
    QCOMPARE(colour("air_tank"), tank);
    QCOMPARE(colour("air"), tank);

    // the pipette into prep (P1; P2 must be closed for it): pipette 100 over
    // spectrometer 80
    open("P2", false);
    QCOMPARE(colour("air_tank"), tank);
    QCOMPARE(colour("air"), tank);
    // the aliquot let into prep is still the tank's gas, and shows as it
    open("P1", true);
    for (const char* stage : {"bone", "prep", "spec", "air"}) QCOMPARE(colour(stage), tank);
  }

  // Each tank has a colour of its own, so the gas of one is never taken for
  // another's; the other kinds have one colour each.
  void eachTankHasItsOwnColour() {
    using canvas::SourceKind;
    QSet<QRgb> seen;
    for (int i = 0; i < 4; ++i) seen.insert(CanvasView::source_color(SourceKind::Tank, i).rgb());
    QCOMPARE(seen.size(), 4);
    QCOMPARE(CanvasView::source_color(SourceKind::Tank), CanvasView::source_color(SourceKind::Tank, 0));
    QCOMPARE(CanvasView::source_color(SourceKind::Pump, 3), CanvasView::source_color(SourceKind::Pump, 0));
    for (auto kind : {SourceKind::Pump, SourceKind::Pipette, SourceKind::Laser, SourceKind::Spectrometer, SourceKind::Getter})
      QVERIFY(!seen.contains(CanvasView::source_color(kind).rgb()));
    QCOMPARE(view_->stage("air_tank")->region_color(), CanvasView::source_color(SourceKind::Tank, 0));
  }

  // A manual valve wears a handwheel on its face: nothing sticks out of the
  // body, so pipes can join any side. Any valve cuts a label too long for
  // the body short; the tooltip keeps the name whole.
  void manualValvesWearAHandwheelAndLongLabelsAreCutShort() {
    ui::ValveItem manual("MiniBoneGP5Manual", canvas::ValveKind::Manual);
    ui::ValveItem valve("A", canvas::ValveKind::Valve);
    QCOMPARE(manual.boundingRect(), valve.boundingRect());
    QVERIFY(valve.wheel_rect().isEmpty());
    const QRectF body(-ui::ValveItem::kSize / 2, -ui::ValveItem::kSize / 2, ui::ValveItem::kSize, ui::ValveItem::kSize);

    const QFontMetricsF metrics{QFont()};
    const QString shown = manual.shown_name(metrics);
    QVERIFY(shown != QStringLiteral("MiniBoneGP5Manual"));
    QVERIFY(shown.endsWith(QChar(0x2026)));  // an ellipsis
    QVERIFY(metrics.horizontalAdvance(shown) <= ui::ValveItem::kSize - 4);
    QCOMPARE(manual.toolTip(), QStringLiteral("MiniBoneGP5Manual"));
    QCOMPARE(valve.shown_name(metrics), QStringLiteral("A"));

    // labelled: a small wheel in the top-right corner, clear of the label
    const QRectF corner = manual.wheel_rect();
    QVERIFY(body.contains(corner));
    QVERIFY(corner.left() > 0 && corner.bottom() < 0);
    // blank: the wheel fills the face
    manual.set_label(QString());
    const QRectF wheel = manual.wheel_rect();
    QCOMPARE(wheel.center(), QPointF(0, 0));
    QCOMPARE(wheel.width(), 2 * ui::ValveItem::kWheelRadius);
    QVERIFY(body.contains(wheel));

    // painted: the dark hub at the centre and rim to its right, on a blank face
    QImage image(60, 60, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.translate(30, 30);
    manual.paint(&painter, nullptr, nullptr);
    painter.end();
    QVERIFY(image.pixelColor(30, 30).lightness() < 100);
    QVERIFY(image.pixelColor(30 + static_cast<int>(ui::ValveItem::kWheelRadius), 30).lightness() < 110);
    QVERIFY(image.pixelColor(30 + 4, 30 - 4).lightness() < 110);  // a spoke, on the diagonal
    QVERIFY(image.pixelColor(30 + 6, 30).lightness() > 120);      // between the spokes: the face

    // On the canvas a manual valve's face is blank unless it is given a
    // display_name; other valves show their name.
    QCOMPARE(view_->valve("M1")->label(), QString());
    QVERIFY(view_->valve("M1")->toolTip().contains(QStringLiteral(">M1<")));
    QCOMPARE(view_->valve("A")->label(), QStringLiteral("A"));
  }

  // The lock border is blue and thick enough to read at a glance.
  void lockBorderIsBlueAndThick() {
    const QColor lock = ui::ValveItem::lock_color();
    QVERIFY(lock.blue() > 200 && lock.blue() > lock.red() + 100 && lock.blue() > lock.green() + 80);
    QVERIFY(ui::ValveItem::kLockBorderWidth >= 5.0);
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
    // dial and reading sit on a chip, centred on the gauge's position
    QVERIFY(ig->chip_rect().contains(dial));
    QVERIFY(std::abs(ig->mapToScene(ig->chip_rect().center()).x() - ig->pos().x()) < 0.5);
    QVERIFY(ig->wired());
    QVERIFY(ig->text().startsWith(QStringLiteral("IG1: ")));
  }

  // A gauge the line does not define is drawn for illustration: the canvas
  // loads, and the gauge shows its name and no reading.
  void aGaugeTheLineDoesNotDefineIsDrawnWithoutAReading() {
    const std::filesystem::path examples = PYCHRON_EXAMPLE_CONFIGS_DIR;
    QTemporaryDir tmp;
    const std::filesystem::path canvas = std::filesystem::path(tmp.path().toStdString()) / "canvas.toml";
    std::filesystem::copy_file(examples / "canvas.toml", canvas);
    std::ofstream(canvas, std::ios::app) << "\n[[gauge]]\nname = \"Bone IG\"\npos = [100, 120]\n"
                                         << "\n[[connection]]\nstart = \"bone\"\nend = \"Bone IG\"\n";
    auto line = ui::test::make_example_line(canvas);
    CoreBridge bridge(*line);
    CanvasView view(bridge);
    const ui::GaugeLabelItem* gauge = view.gauge("Bone IG");
    QVERIFY(gauge != nullptr);
    QVERIFY(!gauge->wired());
    QCOMPARE(gauge->text(), QStringLiteral("Bone IG"));
    QVERIFY(view.gauge("IG1")->wired());
    line->stop();
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
