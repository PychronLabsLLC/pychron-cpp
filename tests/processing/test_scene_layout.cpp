// The Qt-free layout of a scene: axis ranges as the figure view takes them,
// ticks, text widths and the geometry of graphs and panels.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <utility>

#include "pychron/processing/scene_layout.hpp"

namespace pychron::processing {
namespace {

Panel panel_of(std::vector<double> y, std::vector<double> y_err = {}) {
  Panel p;
  PointLayer pts;
  pts.x.resize(y.size());
  for (std::size_t i = 0; i < y.size(); ++i) pts.x[i] = static_cast<double>(i);
  pts.y = std::move(y);
  pts.y_err = std::move(y_err);
  p.layers.emplace_back(std::move(pts));
  return p;
}

std::vector<double> values(const std::vector<Tick>& ticks) {
  std::vector<double> out;
  out.reserve(ticks.size());
  for (const auto& t : ticks) out.push_back(t.value);
  return out;
}

std::vector<std::string> labels(const std::vector<Tick>& ticks) {
  std::vector<std::string> out;
  out.reserve(ticks.size());
  for (const auto& t : ticks) out.push_back(t.label);
  return out;
}

TEST(SceneLayout, XRangeIsTheAxisLimitsWhenBothAreSet) {
  Graph g;
  g.x.min = 0.0;
  g.x.max = 100.0;
  g.panels.push_back(panel_of({1.0, 2.0}));
  EXPECT_EQ(graph_x_range(g), (Range{0.0, 100.0}));
}

TEST(SceneLayout, XRangeIsTheDataWithoutPadding) {
  Graph g;
  Panel p;
  PointLayer pts;
  pts.x = {1.0, 3.0};
  pts.y = {0.0, 0.0};
  pts.x_err = {0.5, 0.5};
  p.layers.emplace_back(pts);
  g.panels.push_back(p);
  g.x.min = 0.0;  // one side alone is not used
  EXPECT_EQ(graph_x_range(g), (Range{0.5, 3.5}));
}

TEST(SceneLayout, YRangeIsPaddedEightPercent) {
  const Range r = panel_y_range(panel_of({10.0, 20.0}));
  EXPECT_DOUBLE_EQ(r.min, 9.2);
  EXPECT_DOUBLE_EQ(r.max, 20.8);
}

TEST(SceneLayout, ErrorsAreData) {
  const Range r = panel_y_range(panel_of({10.0, 20.0}, {1.0, 1.0}));
  EXPECT_DOUBLE_EQ(r.min, 9.0 - 12.0 * 0.08);
  EXPECT_DOUBLE_EQ(r.max, 21.0 + 12.0 * 0.08);
}

TEST(SceneLayout, ZeroWidthRangeIsPaddedAsTheView) {
  const Range r = panel_y_range(panel_of({5.0, 5.0}));
  EXPECT_DOUBLE_EQ(r.min, 4.75);
  EXPECT_DOUBLE_EQ(r.max, 5.25);
}

TEST(SceneLayout, AxisMinOverridesOneSide) {
  Panel p = panel_of({10.0, 20.0});
  p.y.min = 0.0;
  const Range r = panel_y_range(p);
  EXPECT_DOUBLE_EQ(r.min, 0.0);
  EXPECT_DOUBLE_EQ(r.max, 20.8);
}

TEST(SceneLayout, LogRangeIgnoresNonPositiveValues) {
  Panel p = panel_of({-1.0, 0.0, 1.0, 100.0});
  p.y.scale = AxisScale::Log;
  const Range r = panel_y_range(p);
  EXPECT_DOUBLE_EQ(r.min, 1.0 / 1.2);
  EXPECT_DOUBLE_EQ(r.max, 120.0);
}

TEST(SceneLayout, StepsGuidesAndSpansAreNotData) {
  Panel p;
  StepLayer steps;
  steps.x0 = {0.0};
  steps.x1 = {50.0};
  steps.y = {10.0};
  steps.y_err = {1.0};
  p.layers.emplace_back(steps);
  GuideLayer guide;
  guide.value = 3.0;
  p.layers.emplace_back(guide);
  SpanLayer span;
  span.y0 = 2.0;
  span.y1 = 7.0;
  p.layers.emplace_back(span);
  EXPECT_EQ(panel_y_range(p), (Range{0.0, 1.0}));
  Graph g;
  g.panels.push_back(p);
  EXPECT_EQ(graph_x_range(g), (Range{0.0, 1.0}));
}

TEST(SceneLayout, NonFiniteValuesAreNotData) {
  const Range r = panel_y_range(panel_of({10.0, std::numeric_limits<double>::quiet_NaN(),
                                          std::numeric_limits<double>::infinity(), 20.0}));
  EXPECT_DOUBLE_EQ(r.min, 9.2);
  EXPECT_DOUBLE_EQ(r.max, 20.8);
}

TEST(SceneLayout, LinearTicksAreOneTwoFive) {
  const Axis a;
  const auto t = axis_ticks(a, {0.0, 10.0}, 5);
  EXPECT_EQ(values(t), (std::vector<double>{0, 2, 4, 6, 8, 10}));
  EXPECT_EQ(labels(t), (std::vector<std::string>{"0", "2", "4", "6", "8", "10"}));
  const auto small = axis_ticks(a, {0.0021, 0.0049}, 5);
  EXPECT_EQ(labels(small), (std::vector<std::string>{"0.0025", "0.003", "0.0035", "0.004", "0.0045"}));
  const auto negative = axis_ticks(a, {-1.0, 1.0}, 4);
  EXPECT_EQ(labels(negative), (std::vector<std::string>{"-1", "-0.5", "0", "0.5", "1"}));
}

TEST(SceneLayout, LogTicksPerDecade) {
  Axis a;
  a.scale = AxisScale::Log;
  const auto t = axis_ticks(a, {0.5, 2000.0}, 5);
  EXPECT_EQ(labels(t), (std::vector<std::string>{"1", "10", "100", "1000"}));
  const auto wide = axis_ticks(a, {1e-6, 1e6}, 5);
  ASSERT_FALSE(wide.empty());
  EXPECT_EQ(wide.front().label, "1e-6");
  EXPECT_LE(wide.size(), 22u);
}

TEST(SceneLayout, TimeTicksUseTheFormat) {
  Axis a;
  a.format = AxisFormat::Time;
  a.time_format = "%H:%M";
  const auto t = axis_ticks(a, {0.0, 86400.0}, 5);
  ASSERT_GE(t.size(), 2u);
  EXPECT_EQ(t.front().label, "00:00");
  const double step = t[1].value - t[0].value;
  for (const auto& tick : t) EXPECT_DOUBLE_EQ(std::fmod(tick.value, step), 0.0);
  Axis d;
  d.format = AxisFormat::Time;  // default: date, then time on a second line
  const auto day = axis_ticks(d, {1760000000.0, 1760000000.0 + 3 * 86400.0}, 3);
  ASSERT_FALSE(day.empty());
  EXPECT_EQ(day.front().label, "2025-10-10\n00:00");
}

TEST(SceneLayout, CategoryTicksNameTheirValues) {
  Axis a;
  a.format = AxisFormat::Category;
  a.categories = {"a", "b", "c"};
  EXPECT_EQ(labels(axis_ticks(a, {-0.5, 2.5}, 5)), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(SceneLayout, TicksAreBounded) {
  const Axis a;
  for (const Range r : {Range{0.0, 1e-300}, Range{0.0, 1e300}, Range{-1e300, 1e300}, Range{1.0, 1.0}, Range{2.0, 1.0},
                        Range{std::numeric_limits<double>::quiet_NaN(), 1.0},
                        Range{0.0, std::numeric_limits<double>::infinity()}}) {
    const auto t = axis_ticks(a, r, 5);
    EXPECT_LE(t.size(), 22u) << r.min << " " << r.max;
    for (const auto& tick : t) EXPECT_TRUE(std::isfinite(tick.value));
  }
  Axis log;
  log.scale = AxisScale::Log;
  EXPECT_LE(axis_ticks(log, {1e-300, 1e300}, 5).size(), 22u);
  EXPECT_TRUE(axis_ticks(log, {-1.0, 0.0}, 5).empty());
}

TEST(SceneLayout, HelveticaWidths) {
  EXPECT_DOUBLE_EQ(text_width("0", 10), 5.56);
  EXPECT_DOUBLE_EQ(text_width("i", 10), 2.22);
  EXPECT_DOUBLE_EQ(text_width("m", 10, true), 8.89);
  EXPECT_DOUBLE_EQ(text_width("\xcf\x83", 10), 6.03);  // sigma
  EXPECT_DOUBLE_EQ(text_width("\xc2\xb1", 10), 5.84);  // plus-minus
  EXPECT_DOUBLE_EQ(text_width("", 10), 0.0);
}

TEST(SceneLayout, TwoColumnsMakeAGrid) {
  Scene s;
  s.columns = 2;
  for (int i = 0; i < 3; ++i) {
    Graph g;
    g.panels.push_back(panel_of({1.0, 2.0}));
    s.graphs.push_back(g);
  }
  const SceneLayout l = layout_scene(s);
  EXPECT_DOUBLE_EQ(l.width, 2 * 432.0 + 5);
  EXPECT_DOUBLE_EQ(l.height, 2 * 324.0 + 5);
  ASSERT_EQ(l.graphs.size(), 3u);
  EXPECT_DOUBLE_EQ(l.graphs[1].cell.x, 437.0);
  EXPECT_DOUBLE_EQ(l.graphs[2].cell.x, 0.0);
  EXPECT_DOUBLE_EQ(l.graphs[2].cell.y, 329.0);
}

TEST(SceneLayout, OneGraphIsOneCell) {
  Scene s;
  Graph g;
  g.panels.push_back(panel_of({1.0, 2.0}));
  s.graphs.push_back(g);
  const SceneLayout l = layout_scene(s, {300.0, 200.0});
  EXPECT_DOUBLE_EQ(l.width, 300.0);
  EXPECT_DOUBLE_EQ(l.height, 200.0);
}

TEST(SceneLayout, PanelsShareTheLeftMarginAndStackByHeight) {
  Scene s;
  Graph g;
  g.title = "Ideogram";
  Panel a = panel_of({1.0, 2.0});
  a.height = 1.0;
  Panel b = panel_of({100000.0, 200000.0});  // wider tick labels
  b.height = 3.0;
  g.panels = {a, b};
  s.graphs.push_back(g);
  const SceneLayout l = layout_scene(s);
  ASSERT_EQ(l.graphs.size(), 1u);
  const auto& p = l.graphs[0].panels;
  ASSERT_EQ(p.size(), 2u);
  EXPECT_NEAR(p[1].plot.h / p[0].plot.h, 3.0, 1e-9);
  EXPECT_DOUBLE_EQ(p[0].plot.x, p[1].plot.x);
  EXPECT_DOUBLE_EQ(p[0].plot.w, p[1].plot.w);
  EXPECT_DOUBLE_EQ(p[1].plot.y, p[0].plot.y + p[0].plot.h + s.style.panel_spacing);
  EXPECT_FALSE(p[0].x_tick_labels);
  EXPECT_TRUE(p[1].x_tick_labels);
  EXPECT_EQ(l.graphs[0].title, "Ideogram");
  EXPECT_GT(p[0].plot.y, s.style.fonts.title);  // below the title
}

TEST(SceneLayout, MapsIntoThePlotBox) {
  PanelLayout p;
  p.plot = {10.0, 20.0, 100.0, 50.0};
  p.x = {0.0, 10.0};
  p.y = {0.0, 1.0};
  EXPECT_DOUBLE_EQ(map_x(p, 5.0), 60.0);
  EXPECT_DOUBLE_EQ(map_y(p, 0.0), 70.0);
  EXPECT_DOUBLE_EQ(map_y(p, 1.0), 20.0);
  p.log_y = true;
  p.y = {1.0, 100.0};
  EXPECT_DOUBLE_EQ(map_y(p, 10.0), 45.0);
}

TEST(SceneLayout, XRangeHoldsAPointWhoseErrorIsNotFinite) {
  Graph g;
  Panel p;
  PointLayer pts;
  pts.x = {1.0, 3.0};
  pts.y = {0.0, 0.0};
  pts.x_err = {0.5, std::numeric_limits<double>::quiet_NaN()};
  p.layers.emplace_back(pts);
  g.panels.push_back(p);
  EXPECT_EQ(graph_x_range(g), (Range{0.5, 3.0}));
}

TEST(SceneLayout, MalformedUtf8IsTheReplacementCharacter) {
  const auto first = [](std::string_view s) {
    std::size_t i = 0;
    const char32_t c = next_code_point(s, i);
    return std::pair{c, i};
  };
  EXPECT_EQ(first("\xb0"), (std::pair<char32_t, std::size_t>{0xFFFD, 1}));          // lone continuation
  EXPECT_EQ(first("\xc0\x80"), (std::pair<char32_t, std::size_t>{0xFFFD, 1}));      // overlong
  EXPECT_EQ(first("\xe0\x80\x80"), (std::pair<char32_t, std::size_t>{0xFFFD, 1}));  // overlong, three bytes
  EXPECT_EQ(first("\xed\xa0\x80"), (std::pair<char32_t, std::size_t>{0xFFFD, 1}));  // surrogate
  EXPECT_EQ(first("\xf4\x90\x80\x80"), (std::pair<char32_t, std::size_t>{0xFFFD, 1}));  // above U+10FFFF
  EXPECT_EQ(first("\xff"), (std::pair<char32_t, std::size_t>{0xFFFD, 1}));
  EXPECT_EQ(first("\xcf\x83"), (std::pair<char32_t, std::size_t>{0x3C3, 2}));
  EXPECT_EQ(first("\xf0\x9f\x98\x80"), (std::pair<char32_t, std::size_t>{0x1F600, 4}));
}

TEST(SceneLayout, TicksAtPowersOfTenAreExact) {
  const Axis a;
  // 1000 / 5 = 200 exactly: the step is 200 whatever log10 rounds to.
  EXPECT_EQ(values(axis_ticks(a, {0.0, 1000.0}, 5)), (std::vector<double>{0, 200, 400, 600, 800, 1000}));
  EXPECT_EQ(labels(axis_ticks(a, {0.0, 0.001}, 5)),
            (std::vector<std::string>{"0", "0.0002", "0.0004", "0.0006", "0.0008", "0.001"}));
  Axis log;
  log.scale = AxisScale::Log;
  EXPECT_EQ(labels(axis_ticks(log, {1e-3, 1e3}, 5)),
            (std::vector<std::string>{"0.001", "0.01", "0.1", "1", "10", "100", "1000"}));
}

TEST(SceneLayout, ExtremeTimeRangesHaveNoTicks) {
  Axis a;
  a.format = AxisFormat::Time;
  EXPECT_TRUE(axis_ticks(a, {0.0, 1e300}, 5).empty());
  EXPECT_TRUE(axis_ticks(a, {-1e20, 1e20}, 5).empty());
  EXPECT_EQ(format_utc(1e300, "%Y"), "");
}

TEST(SceneLayout, YearsArePaddedToFourDigits) {
  EXPECT_EQ(format_utc(-62135596800.0, "%Y-%m-%d"), "0001-01-01");
  EXPECT_EQ(format_utc(0.0, "%Y%m%d%H%M%S"), "19700101000000");
}

TEST(SceneLayout, HugeColumnCountIsOneRow) {
  Scene s;
  s.columns = std::numeric_limits<int>::max();
  for (int i = 0; i < 2; ++i) {
    Graph g;
    g.panels.push_back(panel_of({1.0, 2.0}));
    s.graphs.push_back(g);
  }
  const SceneLayout l = layout_scene(s);
  EXPECT_DOUBLE_EQ(l.width, 2 * 432.0 + 5);
  EXPECT_DOUBLE_EQ(l.height, 324.0);
}

}  // namespace
}  // namespace pychron::processing
