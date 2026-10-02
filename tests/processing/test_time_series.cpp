#include "pychron/processing/time_series.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"
#include "pychron/processing/units.hpp"

namespace pp = pychron::processing;
using pp::test::make_air;

namespace {

// Six airs, one hour apart, ratios 295..300, grouped by `groups` (index % groups).
pp::Dataset airs(int groups = 1) {
  pp::Dataset d;
  for (int i = 0; i < 6; ++i) {
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(make_air(i, 295.0 + i), {});
    item.path.group = i % groups;
    d.mutable_items().push_back(item);
  }
  for (int g = 0; g < groups; ++g) d.group_names.push_back("g" + std::to_string(g));
  return d;
}

pp::Options with_panels(std::vector<std::pair<std::string, std::string>> panels) {
  pp::Options o(pp::time_series_schema());
  std::vector<pp::Options> rows;
  for (const auto& [q, fit] : panels) {
    auto row = o.new_row("panels");
    EXPECT_TRUE(row.set("quantity", q));
    EXPECT_TRUE(row.set("fit", fit));
    rows.push_back(row);
  }
  EXPECT_TRUE(o.set_rows("panels", rows));
  return o;
}

template <class T>
std::vector<const T*> layers(const pp::Panel& p) {
  std::vector<const T*> out;
  for (const auto& l : p.layers)
    if (const auto* x = std::get_if<T>(&l)) out.push_back(x);
  return out;
}

TEST(TimeSeries, PanelsTopToBottomOnATimeAxis) {
  auto o = with_panels({{"Ar40/Ar36", "none"}, {"Ar40", "none"}, {"gain.H1", "none"}});
  auto s = pp::build_time_series(airs(), o);
  ASSERT_TRUE(s) << s.error().what;
  ASSERT_EQ(s->graphs.size(), 1u);
  const auto& g = s->graphs[0];
  ASSERT_EQ(g.panels.size(), 3u);
  EXPECT_EQ(g.panels[0].quantity, "Ar40/Ar36");
  EXPECT_EQ(g.panels[2].quantity, "gain.H1");
  EXPECT_EQ(g.x.format, pp::AxisFormat::Time);
  const auto pts = layers<pp::PointLayer>(g.panels[0]);
  ASSERT_EQ(pts.size(), 1u);
  ASSERT_EQ(pts[0]->x.size(), 6u);
  EXPECT_DOUBLE_EQ(pts[0]->x[0], 1'700'000'000.0);
  EXPECT_NEAR(pts[0]->y[5], 300.0, 1e-9);
  EXPECT_EQ(pts[0]->refs[2].analysis, "uuid-A1-2");
  EXPECT_EQ(pts[0]->y_err.size(), 6u);
  ASSERT_TRUE(g.x.min && g.x.max);
  EXPECT_LT(*g.x.min, pts[0]->x.front());
  EXPECT_GT(*g.x.max, pts[0]->x.back());
  EXPECT_EQ(s->point_count(), 18u);
  EXPECT_TRUE(s->warnings.empty());
}

TEST(TimeSeries, RelativeAndIndexAxes) {
  auto o = with_panels({{"Ar40", "none"}});
  ASSERT_TRUE(o.set("x.kind", std::string("relative")));
  auto rel = pp::build_time_series(airs(), o);
  ASSERT_TRUE(rel);
  auto pts = layers<pp::PointLayer>(rel->graphs[0].panels[0]);
  EXPECT_DOUBLE_EQ(pts[0]->x.front(), -5.0);  // hours before the last analysis
  EXPECT_DOUBLE_EQ(pts[0]->x.back(), 0.0);
  ASSERT_TRUE(o.set("x.origin", std::string("first")));
  auto first = pp::build_time_series(airs(), o);
  ASSERT_TRUE(first);
  pts = layers<pp::PointLayer>(first->graphs[0].panels[0]);
  EXPECT_DOUBLE_EQ(pts[0]->x.back(), 5.0);
  ASSERT_TRUE(o.set("x.kind", std::string("index")));
  auto index = pp::build_time_series(airs(), o);
  ASSERT_TRUE(index);
  pts = layers<pp::PointLayer>(index->graphs[0].panels[0]);
  EXPECT_DOUBLE_EQ(pts[0]->x.front(), 1.0);
  EXPECT_DOUBLE_EQ(pts[0]->x.back(), 6.0);
}

TEST(TimeSeries, WeightedMeanSkipsExcludedPoints) {
  auto d = airs();
  auto o = with_panels({{"Ar40/Ar36", "weighted_mean"}});
  auto all = pp::build_time_series(d, o);
  ASSERT_TRUE(all);
  auto lines = layers<pp::LineLayer>(all->graphs[0].panels[0]);
  ASSERT_EQ(lines.size(), 1u);
  const double mean_all = lines[0]->y[0];
  EXPECT_NEAR(mean_all, 297.5, 0.1);
  EXPECT_EQ(layers<pp::BandLayer>(all->graphs[0].panels[0]).size(), 1u);
  EXPECT_EQ(layers<pp::TextLayer>(all->graphs[0].panels[0]).size(), 1u);

  d.mutable_items()[5].exclusion.user = true;  // 300 out
  auto fewer = pp::build_time_series(d, o);
  ASSERT_TRUE(fewer);
  const double mean_fewer = layers<pp::LineLayer>(fewer->graphs[0].panels[0])[0]->y[0];
  EXPECT_LT(mean_fewer, mean_all);
  const auto pts = layers<pp::PointLayer>(fewer->graphs[0].panels[0]);
  EXPECT_TRUE(pts[0]->excluded[5]);
  EXPECT_FALSE(pts[0]->excluded[4]);
}

TEST(TimeSeries, LinearFitGoesThroughALine) {
  auto d = airs();  // ratio rises 1 per hour
  auto o = with_panels({{"Ar40/Ar36", "linear"}});
  auto s = pp::build_time_series(d, o);
  ASSERT_TRUE(s) << s.error().what;
  const auto lines = layers<pp::LineLayer>(s->graphs[0].panels[0]);
  ASSERT_EQ(lines.size(), 1u);
  const auto& l = *lines[0];
  ASSERT_GT(l.x.size(), 10u);
  for (std::size_t i = 0; i < l.x.size(); ++i) {
    const double hours = (l.x[i] - 1'700'000'000.0) / 3600.0;
    EXPECT_NEAR(l.y[i], 295.0 + hours, 1e-6);
  }
  const auto bands = layers<pp::BandLayer>(s->graphs[0].panels[0]);
  ASSERT_EQ(bands.size(), 1u);
  EXPECT_EQ(bands[0]->x.size(), l.x.size());
}

TEST(TimeSeries, GroupsGetPaletteColoursAndLegendLabels) {
  auto o = with_panels({{"Ar40", "average"}});
  auto s = pp::build_time_series(airs(2), o);
  ASSERT_TRUE(s);
  const auto pts = layers<pp::PointLayer>(s->graphs[0].panels[0]);
  ASSERT_EQ(pts.size(), 2u);
  EXPECT_EQ(pts[0]->label, "g0");
  EXPECT_EQ(pts[1]->label, "g1");
  EXPECT_EQ(pts[0]->marker.color, pp::palette_color(0));
  EXPECT_EQ(pts[1]->marker.color, pp::palette_color(1));
  EXPECT_EQ(layers<pp::LineLayer>(s->graphs[0].panels[0]).size(), 2u);

  // A group row overrides colour, marker and label.
  auto row = o.new_row("groups");
  ASSERT_TRUE(row.set("color", std::string("#ff0000")));
  ASSERT_TRUE(row.set("marker", std::string("square")));
  ASSERT_TRUE(row.set("label", std::string("first")));
  ASSERT_TRUE(o.set_rows("groups", {row}));
  auto styled_scene = pp::build_time_series(airs(2), o);
  ASSERT_TRUE(styled_scene);
  const auto styled = layers<pp::PointLayer>(styled_scene->graphs[0].panels[0]);
  EXPECT_EQ(styled[0]->marker.color, (pp::Color{255, 0, 0, 255}));
  EXPECT_EQ(styled[0]->marker.shape, pp::MarkerShape::Square);
  EXPECT_EQ(styled[0]->label, "first");
  EXPECT_EQ(styled[1]->marker.color, pp::palette_color(1));
}

TEST(TimeSeries, GraphsPerGraphIndex) {
  auto d = airs();
  for (std::size_t i = 0; i < d.size(); ++i) d.mutable_items()[i].path.graph = static_cast<int>(i % 3);
  d.graph_names = {"a", "b", "c"};
  auto s = pp::build_time_series(d, with_panels({{"Ar40", "none"}}));
  ASSERT_TRUE(s);
  ASSERT_EQ(s->graphs.size(), 3u);
  EXPECT_EQ(s->graphs[1].title, "b");
  EXPECT_EQ(s->point_count(), 6u);
}

TEST(TimeSeries, MissingValuesAreWarnedNotZero) {
  auto s = pp::build_time_series(airs(), with_panels({{"age", "none"}, {"Ar40", "none"}}));
  ASSERT_TRUE(s);
  EXPECT_TRUE(layers<pp::PointLayer>(s->graphs[0].panels[0]).empty());
  ASSERT_EQ(s->warnings.size(), 1u);
  EXPECT_NE(s->warnings[0].find("age: 6 analyses have no value"), std::string::npos);
}

TEST(TimeSeries, DeviationAndHiddenExcluded) {
  auto d = airs();
  auto o = with_panels({{"Ar40/Ar36", "none"}});
  auto rows = o.rows("panels");
  ASSERT_TRUE(rows[0].set("deviation", std::string("percent")));
  ASSERT_TRUE(o.set_rows("panels", rows));
  ASSERT_TRUE(o.set("excluded_style", std::string("hidden")));
  ASSERT_TRUE(o.set("error_bar_nsigma", std::int64_t{2}));
  auto s = pp::build_time_series(d, o);
  ASSERT_TRUE(s);
  const auto pts = layers<pp::PointLayer>(s->graphs[0].panels[0]);
  EXPECT_FALSE(pts[0]->show_excluded);
  double sum = 0;
  for (double y : pts[0]->y) sum += y;
  EXPECT_NEAR(sum / 6, 0.0, 0.01);  // percent deviations around the mean
  EXPECT_NE(s->graphs[0].panels[0].y.title.find("Δ%"), std::string::npos);
}

TEST(TimeSeries, NoPanelsIsAnError) {
  auto o = with_panels({{"Ar40", "none"}});
  auto rows = o.rows("panels");
  ASSERT_TRUE(rows[0].set("enabled", false));
  ASSERT_TRUE(o.set_rows("panels", rows));
  EXPECT_FALSE(pp::build_time_series(airs(), o));
}

}  // namespace
