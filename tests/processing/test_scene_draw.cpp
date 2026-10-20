// A scene as a draw list: what each layer places, where, and what is left out.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "pychron/processing/scene_draw.hpp"

namespace pychron::processing {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

Scene one_panel(std::vector<Layer> layers) {
  Scene s;
  Graph g;
  g.x.min = 0.0;
  g.x.max = 10.0;
  Panel p;
  p.y.min = 0.0;
  p.y.max = 10.0;
  p.layers = std::move(layers);
  g.panels.push_back(std::move(p));
  s.graphs.push_back(std::move(g));
  return s;
}

std::vector<const DrawPath*> paths(const DrawList& l, DrawRole role) {
  std::vector<const DrawPath*> out;
  for (const auto& item : l.items)
    if (const auto* p = std::get_if<DrawPath>(&item); p && p->role == role) out.push_back(p);
  return out;
}

std::vector<const DrawText*> texts(const DrawList& l, TextRole role) {
  std::vector<const DrawText*> out;
  for (const auto& item : l.items)
    if (const auto* t = std::get_if<DrawText>(&item); t && t->role == role) out.push_back(t);
  return out;
}

Pt centre(const DrawPath& p) {
  double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
  for (const auto& op : p.ops) {
    const int n = op.kind == PathOp::Kind::Curve ? 3 : op.kind == PathOp::Kind::Close ? 0 : 1;
    for (int i = 0; i < n; ++i) {
      x0 = std::min(x0, op.p[i].x);
      x1 = std::max(x1, op.p[i].x);
      y0 = std::min(y0, op.p[i].y);
      y1 = std::max(y1, op.p[i].y);
    }
  }
  return {(x0 + x1) / 2, (y0 + y1) / 2};
}

TEST(SceneDraw, EmptySceneSaysNoData) {
  const DrawList l = draw_scene(Scene{});
  const auto t = texts(l, TextRole::Empty);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0]->text, "No data");
  EXPECT_GT(l.width, 0);
}

TEST(SceneDraw, OneMarkerAndTwoErrorBarsPerShownPoint) {
  PointLayer pts;
  pts.x = {1, 2, 3};
  pts.y = {1, 2, 3};
  pts.x_err = {0.1, 0.1, 0.1};
  pts.y_err = {0.2, 0.2, 0.2};
  pts.excluded = {false, true, false};
  pts.show_excluded = false;
  const DrawList l = draw_scene(one_panel({pts}));
  EXPECT_EQ(paths(l, DrawRole::Marker).size(), 2u);
  EXPECT_EQ(paths(l, DrawRole::ErrorBar).size(), 4u);
  pts.show_excluded = true;
  EXPECT_EQ(paths(draw_scene(one_panel({pts})), DrawRole::Marker).size(), 3u);
}

TEST(SceneDraw, PointsMapIntoThePlotBox) {
  PointLayer pts;
  pts.x = {5};
  pts.y = {5};
  const Scene s = one_panel({pts});
  const DrawList l = draw_scene(s);
  const SceneLayout layout = layout_scene(s);
  const Box& plot = layout.graphs[0].panels[0].plot;
  const auto m = paths(l, DrawRole::Marker);
  ASSERT_EQ(m.size(), 1u);
  const Pt c = centre(*m[0]);
  EXPECT_NEAR(c.x, plot.x + plot.w / 2, 1e-9);
  EXPECT_NEAR(c.y, plot.y + plot.h / 2, 1e-9);
  ASSERT_GE(m[0]->clip, 0);
  EXPECT_DOUBLE_EQ(l.clips[static_cast<std::size_t>(m[0]->clip)].x, plot.x);
}

TEST(SceneDraw, NonFiniteValuesAreSkipped) {
  PointLayer pts;
  pts.x = {1, kNaN, 3};
  pts.y = {1, 2, kInf};
  LineLayer line;
  line.x = {1, 2, 3, 4, 5};
  line.y = {1, 2, kNaN, 4, 5};
  const DrawList l = draw_scene(one_panel({pts, line}));
  EXPECT_EQ(paths(l, DrawRole::Marker).size(), 1u);
  const auto lines = paths(l, DrawRole::Data);
  ASSERT_EQ(lines.size(), 1u);
  const auto moves = std::count_if(lines[0]->ops.begin(), lines[0]->ops.end(),
                                   [](const PathOp& op) { return op.kind == PathOp::Kind::Move; });
  EXPECT_EQ(moves, 2);
  for (const auto& item : l.items)
    if (const auto* p = std::get_if<DrawPath>(&item))
      for (const auto& op : p->ops)
        for (const auto& pt : op.p) {
          EXPECT_TRUE(std::isfinite(pt.x));
          EXPECT_TRUE(std::isfinite(pt.y));
        }
}

TEST(SceneDraw, LogAxisLeavesOutNonPositiveValues) {
  PointLayer pts;
  pts.x = {1, 2, 3};
  pts.y = {-1, 0, 10};
  Scene s = one_panel({pts});
  s.graphs[0].panels[0].y.scale = AxisScale::Log;
  s.graphs[0].panels[0].y.min = 1.0;
  s.graphs[0].panels[0].y.max = 100.0;
  EXPECT_EQ(paths(draw_scene(s), DrawRole::Marker).size(), 1u);
}

TEST(SceneDraw, StepsAreBoxesWithLabels) {
  StepLayer steps;
  steps.x0 = {0, 3, 6};
  steps.x1 = {3, 6, 9};
  steps.y = {5, 5, 5};
  steps.y_err = {1, 1, 1};
  steps.labels = {"A", "", "C"};
  steps.fill = {100, 100, 200, 150};
  steps.highlighted = {false, true, false};
  steps.dim_others = true;
  const DrawList l = draw_scene(one_panel({steps}));
  const auto boxes = paths(l, DrawRole::Step);
  ASSERT_EQ(boxes.size(), 3u);
  EXPECT_EQ(texts(l, TextRole::StepLabel).size(), 2u);
  ASSERT_TRUE(boxes[0]->fill);
  EXPECT_EQ(boxes[0]->fill->a, 150 / 3);
  EXPECT_EQ(boxes[1]->fill->a, 150);
}

TEST(SceneDraw, ExcludedStepsAreDashed) {
  StepLayer steps;
  steps.x0 = {0};
  steps.x1 = {3};
  steps.y = {5};
  steps.y_err = {1};
  steps.excluded = {true};
  const DrawList l = draw_scene(one_panel({steps}));
  const auto boxes = paths(l, DrawRole::Step);
  ASSERT_EQ(boxes.size(), 1u);
  EXPECT_EQ(boxes[0]->dash, LineDash::Dash);
  EXPECT_EQ(*boxes[0]->fill, steps.excluded_fill);
}

TEST(SceneDraw, EllipseIsFourCurves) {
  EllipseLayer el;
  el.x = {5};
  el.y = {5};
  el.sx = {1};
  el.sy = {1};
  el.rho = {0.5};
  const DrawList l = draw_scene(one_panel({el}));
  const auto e = paths(l, DrawRole::Ellipse);
  ASSERT_EQ(e.size(), 1u);
  ASSERT_EQ(e[0]->ops.size(), 6u);
  EXPECT_EQ(e[0]->ops[0].kind, PathOp::Kind::Move);
  for (int i = 1; i <= 4; ++i) EXPECT_EQ(e[0]->ops[static_cast<std::size_t>(i)].kind, PathOp::Kind::Curve);
  EXPECT_EQ(e[0]->ops[5].kind, PathOp::Kind::Close);
}

TEST(SceneDraw, CornerTextIsOneBoxPerCorner) {
  TextLayer a;
  a.lines = {"one", "two"};
  TextLayer b;
  b.lines = {"three"};
  TextLayer c;
  c.corner = Corner::BottomRight;
  c.lines = {"four"};
  const DrawList l = draw_scene(one_panel({a, c, b}));
  EXPECT_EQ(paths(l, DrawRole::TextBox).size(), 2u);
  const auto t = texts(l, TextRole::Annotation);
  ASSERT_EQ(t.size(), 4u);
  EXPECT_EQ(t[0]->text, "one");
  EXPECT_EQ(t[1]->text, "two");
  EXPECT_EQ(t[2]->text, "three");
  EXPECT_EQ(t[3]->text, "four");
  EXPECT_GT(t[3]->at.x, t[0]->at.x);
  EXPECT_GT(t[3]->at.y, t[0]->at.y);
}

// A corner whose lines have markers draws each before its line, in the
// space the lines are indented by; none of them is a data marker.
TEST(SceneDraw, CornerTextMarkersGoBeforeTheirLines) {
  TextLayer plain;
  plain.lines = {"one"};
  TextLayer marked;
  marked.lines = {"one", "two"};
  MarkerStyle red;
  red.color = {255, 0, 0, 255};
  MarkerStyle blue = red;
  blue.color = {0, 0, 255, 255};
  marked.markers = {red, blue};
  const DrawList without = draw_scene(one_panel({plain}));
  EXPECT_TRUE(paths(without, DrawRole::Legend).empty());
  const DrawList with = draw_scene(one_panel({marked}));
  const auto swatches = paths(with, DrawRole::Legend);
  ASSERT_EQ(swatches.size(), 2u);
  EXPECT_TRUE(paths(with, DrawRole::Marker).empty());
  EXPECT_EQ(swatches[0]->fill, red.color);
  EXPECT_EQ(swatches[1]->fill, blue.color);
  const auto t = texts(with, TextRole::Annotation);
  ASSERT_EQ(t.size(), 2u);
  EXPECT_GT(t[0]->at.x, texts(without, TextRole::Annotation)[0]->at.x);  // indented
  EXPECT_LT(swatches[0]->ops.front().p[0].y, swatches[1]->ops.front().p[0].y);  // one per line, in order
  EXPECT_EQ(paths(with, DrawRole::TextBox).size(), 1u);
}

TEST(SceneDraw, LegendListsLabelledPointLayers) {
  PointLayer a;
  a.x = {1};
  a.y = {1};
  a.label = "a";
  PointLayer b = a;
  b.label = "b";
  PointLayer c = a;
  c.label.clear();
  Scene s = one_panel({a, b, c});
  EXPECT_EQ(texts(draw_scene(s), TextRole::Legend).size(), 2u);
  s.style.legend = false;
  EXPECT_TRUE(texts(draw_scene(s), TextRole::Legend).empty());
  EXPECT_EQ(paths(draw_scene(s), DrawRole::Marker).size(), 3u);  // legend swatches are not markers
}

TEST(SceneDraw, OnlyTheBottomPanelHasTickLabels) {
  Scene s = one_panel({});
  s.graphs[0].panels.push_back(s.graphs[0].panels[0]);
  s.graphs[0].x.title = "Age (Ma)";
  s.graphs[0].panels[0].y.title = "top";
  s.graphs[0].panels[1].y.title = "bottom";
  const DrawList l = draw_scene(s);
  const SceneLayout layout = layout_scene(s);
  const double top_bottom = layout.graphs[0].panels[0].plot.y + layout.graphs[0].panels[0].plot.h;
  const double low_bottom = layout.graphs[0].panels[1].plot.y + layout.graphs[0].panels[1].plot.h;
  int below_top = 0, below_low = 0;
  for (const auto* t : texts(l, TextRole::Tick)) {
    if (t->anchor != TextAnchor::Middle) continue;  // x tick labels
    if (t->at.y > top_bottom && t->at.y < layout.graphs[0].panels[1].plot.y) ++below_top;
    if (t->at.y > low_bottom) ++below_low;
  }
  EXPECT_EQ(below_top, 0);
  EXPECT_GT(below_low, 0);
  const auto titles = texts(l, TextRole::AxisTitle);
  ASSERT_EQ(titles.size(), 3u);
  EXPECT_TRUE(titles[0]->vertical);
  EXPECT_FALSE(titles[2]->vertical);
  EXPECT_EQ(titles[2]->text, "Age (Ma)");
}

TEST(SceneDraw, GraphTitleIsBoldAndCentred) {
  Scene s = one_panel({});
  s.graphs[0].title = "FC-1";
  const DrawList l = draw_scene(s);
  const auto t = texts(l, TextRole::Title);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(t[0]->bold);
  EXPECT_EQ(t[0]->anchor, TextAnchor::Middle);
  EXPECT_DOUBLE_EQ(t[0]->at.x, 432.0 / 2);
}

TEST(SceneDraw, FormatCoord) {
  EXPECT_EQ(format_coord(2.50), "2.5");
  EXPECT_EQ(format_coord(100.0), "100");
  EXPECT_EQ(format_coord(-0.001), "0");
  EXPECT_EQ(format_coord(0.125), "0.12");  // to_chars rounds the double half to even
  EXPECT_EQ(format_coord(-3.14159), "-3.14");
  EXPECT_EQ(format_coord(kNaN), "0");
}

TEST(SceneDraw, AGuideOrSpanThatMapsOffEveryScaleIsLeftOut) {
  GuideLayer guide;
  guide.value = 1e308;
  SpanLayer span;
  span.y0 = 0.0;
  span.y1 = 1e308;
  Scene s = one_panel({guide, span});
  s.graphs[0].panels[0].y.min = 0.0;
  s.graphs[0].panels[0].y.max = 1e-300;
  const DrawList l = draw_scene(s);
  EXPECT_TRUE(paths(l, DrawRole::Guide).empty());
  EXPECT_TRUE(paths(l, DrawRole::Span).empty());
}

TEST(SceneDraw, ALogErrorBarBelowZeroIsClippedNotDropped) {
  PointLayer pts;
  pts.x = {5};
  pts.y = {1};
  pts.y_err = {5};
  Scene s = one_panel({pts});
  s.graphs[0].panels[0].y.scale = AxisScale::Log;
  s.graphs[0].panels[0].y.min = 0.1;
  s.graphs[0].panels[0].y.max = 100.0;
  const DrawList l = draw_scene(s);
  const auto bars = paths(l, DrawRole::ErrorBar);
  ASSERT_EQ(bars.size(), 1u);
  const SceneLayout layout = layout_scene(s);
  const Box& plot = layout.graphs[0].panels[0].plot;
  EXPECT_NEAR(std::max(bars[0]->ops[0].p[0].y, bars[0]->ops[1].p[0].y), plot.y + plot.h, 1e-9);
}

TEST(SceneDraw, ALogStepBelowZeroIsClippedNotDropped) {
  StepLayer steps;
  steps.x0 = {0};
  steps.x1 = {5};
  steps.y = {1};
  steps.y_err = {5};
  Scene s = one_panel({steps});
  s.graphs[0].panels[0].y.scale = AxisScale::Log;
  s.graphs[0].panels[0].y.min = 0.1;
  s.graphs[0].panels[0].y.max = 100.0;
  const DrawList l = draw_scene(s);
  EXPECT_EQ(paths(l, DrawRole::Step).size(), 1u);
}

}  // namespace
}  // namespace pychron::processing
