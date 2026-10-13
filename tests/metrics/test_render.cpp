#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "metrics_text.hpp"
#include "pychron/metrics/registry.hpp"

using namespace pychron::metrics;
using metrics_text::raw;
using metrics_text::without_own;

TEST(Render, GoldenText) {
  Registry r;
  r.gauge("pychron_pressure", "Gauge pressure, in the gauge's unit.", {{"gauge", "IG1"}, {"unit", "torr"}}).set(1.5e-9);
  r.counter("pychron_runs_started_total", "Runs started.").inc(3);
  EXPECT_EQ(without_own(r.render()),
            "# HELP pychron_pressure Gauge pressure, in the gauge's unit.\n"
            "# TYPE pychron_pressure gauge\n"
            "pychron_pressure{gauge=\"IG1\",unit=\"torr\"} 1.5e-09\n"
            "# HELP pychron_runs_started_total Runs started.\n"
            "# TYPE pychron_runs_started_total counter\n"
            "pychron_runs_started_total 3\n");
}

TEST(Render, GoldenHistogram) {
  Registry r;
  r.histogram("pychron_h_seconds", "A duration.", {1.0, 2.5}).observe(2);
  EXPECT_EQ(without_own(r.render()),
            "# HELP pychron_h_seconds A duration.\n"
            "# TYPE pychron_h_seconds histogram\n"
            "pychron_h_seconds_bucket{le=\"1\"} 0\n"
            "pychron_h_seconds_bucket{le=\"2.5\"} 1\n"
            "pychron_h_seconds_bucket{le=\"+Inf\"} 1\n"
            "pychron_h_seconds_sum 2\n"
            "pychron_h_seconds_count 1\n");
}

TEST(Render, FamiliesAndSeriesAreSorted) {
  Registry r;
  r.gauge("pychron_z", "h").set(1);
  r.gauge("pychron_a", "h", {{"k", "b"}}).set(1);
  r.gauge("pychron_a", "h", {{"k", "a"}}).set(1);
  const std::string text = without_own(r.render());
  const auto a_a = text.find("pychron_a{k=\"a\"}");
  const auto a_b = text.find("pychron_a{k=\"b\"}");
  const auto z = text.find("pychron_z 1");
  ASSERT_NE(a_a, std::string::npos);
  ASSERT_NE(a_b, std::string::npos);
  ASSERT_NE(z, std::string::npos);
  EXPECT_LT(a_a, a_b);
  EXPECT_LT(a_b, z);
}

TEST(Render, HelpAndTypeAppearOncePerFamily) {
  Registry r;
  r.gauge("pychron_a", "h", {{"k", "a"}}).set(1);
  r.gauge("pychron_a", "h", {{"k", "b"}}).set(1);
  const std::string text = r.render();
  const auto first = text.find("# TYPE pychron_a gauge");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(text.find("# TYPE pychron_a gauge", first + 1), std::string::npos);
  const auto help = text.find("# HELP pychron_a h");
  ASSERT_NE(help, std::string::npos);
  EXPECT_EQ(text.find("# HELP pychron_a h", help + 1), std::string::npos);
}

TEST(Render, AFamilyWithNoVisibleSeriesIsLeftOut) {
  Registry r;
  r.gauge("pychron_a", "h", {{"k", "a"}}).set(1);
  r.remove("pychron_a", {{"k", "a"}});
  EXPECT_EQ(without_own(r.render()), "");
}

TEST(Render, LabelValuesAreEscaped) {
  Registry r;
  r.gauge("pychron_g", "h", {{"gauge", "IG \"bone\"\\x\nend"}}).set(1);
  EXPECT_TRUE(metrics_text::has(r.render(), "pychron_g{gauge=\"IG \\\"bone\\\"\\\\x\\nend\"}"));
}

TEST(Render, NonAsciiLabelValuesPassThrough) {
  Registry r;
  r.gauge("pychron_g", "h", {{"heater", "F\xC3\xBCrnace"}}).set(1);
  EXPECT_TRUE(metrics_text::has(r.render(), "pychron_g{heater=\"F\xC3\xBCrnace\"}"));
}

TEST(Render, HelpTextIsEscaped) {
  Registry r;
  r.gauge("pychron_g", "a\\b\nc").set(1);
  EXPECT_NE(r.render().find("# HELP pychron_g a\\\\b\\nc\n"), std::string::npos);
}

TEST(Render, NonFiniteValues) {
  Registry r;
  r.gauge("pychron_g", "h", {{"v", "nan"}}).set(std::nan(""));
  r.gauge("pychron_g", "h", {{"v", "pinf"}}).set(std::numeric_limits<double>::infinity());
  r.gauge("pychron_g", "h", {{"v", "ninf"}}).set(-std::numeric_limits<double>::infinity());
  const std::string text = r.render();
  EXPECT_EQ(raw(text, "pychron_g{v=\"nan\"}"), "NaN");
  EXPECT_EQ(raw(text, "pychron_g{v=\"pinf\"}"), "+Inf");
  EXPECT_EQ(raw(text, "pychron_g{v=\"ninf\"}"), "-Inf");
}

TEST(Render, NumbersRoundTrip) {
  for (double v : {0.1, 1e-12, 123456789012.0, 1700000000.25, -3.5, 0.0}) {
    Registry r;
    r.gauge("pychron_g", "h").set(v);
    const auto text = raw(r.render(), "pychron_g");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(std::strtod(text->c_str(), nullptr), v) << *text;
  }
}

TEST(Render, EndsWithANewline) {
  Registry r;
  const std::string text = r.render();
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(text.back(), '\n');
}
