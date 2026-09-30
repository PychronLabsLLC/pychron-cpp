#include "pychron/systems/spectrometer/field_table.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace ps = pychron::spectrometer;
using pychron::ErrorKind;

namespace {

// Exact quadratic per detector so fits reproduce it: v = a + b m + c m^2.
double h1_curve(double m) { return 0.5 + 0.1 * m + 0.001 * m * m; }
double ax_curve(double m) { return 0.3 + 0.1 * m + 0.001 * m * m; }

ps::FieldTable argon_table(ps::FitKind fit = ps::FitKind::Quadratic) {
  std::vector<ps::ControlPoint> pts;
  for (auto [iso, mass] : std::vector<std::pair<std::string, double>>{
           {"Ar36", 35.967545}, {"Ar37", 36.966776}, {"Ar38", 37.962732}, {"Ar39", 38.964313}, {"Ar40", 39.962383}}) {
    pts.push_back({iso, mass, {{"H1", h1_curve(mass)}, {"AX", ax_curve(mass)}}});
  }
  return ps::FieldTable(fit, ps::TableAxis::Dac, std::move(pts));
}

}  // namespace

TEST(FieldTable, DetectorsInFirstAppearanceOrder) {
  auto t = argon_table();
  EXPECT_EQ(t.detectors(), (std::vector<std::string>{"AX", "H1"}));  // map order within a point
  EXPECT_TRUE(t.has_detector("H1"));
  EXPECT_FALSE(t.has_detector("CDD"));
}

TEST(FieldTable, QuadraticFitReproducesCurve) {
  auto t = argon_table();
  for (double m : {36.0, 38.5, 39.9624, 41.0}) {
    auto v = t.value_for(m, "H1");
    ASSERT_TRUE(v.has_value()) << pychron::to_string(v.error());
    EXPECT_NEAR(*v, h1_curve(m), 1e-9);
  }
}

TEST(FieldTable, LinearFitIsLeastSquares) {
  std::vector<ps::ControlPoint> pts{{"a", 1.0, {{"H1", 1.0}}}, {"b", 2.0, {{"H1", 2.0}}}, {"c", 3.0, {{"H1", 3.3}}}};
  ps::FieldTable t(ps::FitKind::Linear, ps::TableAxis::Dac, pts);
  // y = 0.15 * x ... slope = 1.15, intercept = -0.2
  EXPECT_NEAR(*t.value_for(2.0, "H1"), 2.1, 1e-12);
  EXPECT_NEAR(*t.value_for(4.0, "H1"), 4.4, 1e-12);
}

TEST(FieldTable, CubicFitReproducesCubic) {
  auto f = [](double m) { return 1.0 + 0.2 * m - 0.003 * m * m + 0.0001 * m * m * m; };
  std::vector<ps::ControlPoint> pts;
  for (double m : {20.0, 28.0, 36.0, 40.0, 44.0}) pts.push_back({"x" + std::to_string(m), m, {{"H1", f(m)}}});
  ps::FieldTable t(ps::FitKind::Cubic, ps::TableAxis::Field, pts);
  EXPECT_NEAR(*t.value_for(32.0, "H1"), f(32.0), 1e-8);
  EXPECT_EQ(t.axis(), ps::TableAxis::Field);
}

TEST(FieldTable, TooFewPointsForFitIsConfigError) {
  std::vector<ps::ControlPoint> pts{{"a", 1.0, {{"H1", 1.0}}}, {"b", 2.0, {{"H1", 2.0}}}};
  ps::FieldTable t(ps::FitKind::Quadratic, ps::TableAxis::Dac, pts);
  auto v = t.value_for(1.5, "H1");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().kind, ErrorKind::Config);
}

TEST(FieldTable, UnknownDetectorIsConfigError) {
  auto t = argon_table();
  auto v = t.value_for(40.0, "CDD");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().kind, ErrorKind::Config);
  EXPECT_EQ(t.mass_for(5.0, "CDD").error().kind, ErrorKind::Config);
}

TEST(FieldTable, DiscreteHitsNearestWithinTolerance) {
  auto t = argon_table(ps::FitKind::Discrete);
  EXPECT_DOUBLE_EQ(*t.value_for(39.96, "H1"), h1_curve(39.962383));
  EXPECT_DOUBLE_EQ(*t.value_for(40.10, "H1"), h1_curve(39.962383));
}

TEST(FieldTable, DiscreteMissIsConfigErrorNotFallback) {
  auto t = argon_table(ps::FitKind::Discrete);
  auto v = t.value_for(39.5, "H1");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().kind, ErrorKind::Config);
}

TEST(FieldTable, PerDetectorFitOverride) {
  auto t = argon_table(ps::FitKind::Quadratic);
  t.set_fit("AX", ps::FitKind::Discrete);
  EXPECT_EQ(t.fit("AX"), ps::FitKind::Discrete);
  EXPECT_EQ(t.fit("H1"), ps::FitKind::Quadratic);
  EXPECT_FALSE(t.value_for(39.5, "AX").has_value());
  EXPECT_TRUE(t.value_for(39.5, "H1").has_value());
}

TEST(FieldTable, InverseRoundTrip) {
  auto t = argon_table();
  for (double m : {35.5, 36.0, 38.2, 39.962383, 40.7}) {
    for (const char* det : {"H1", "AX"}) {
      auto v = t.value_for(m, det);
      ASSERT_TRUE(v.has_value());
      auto back = t.mass_for(*v, det);
      ASSERT_TRUE(back.has_value()) << pychron::to_string(back.error());
      EXPECT_NEAR(*back, m, 1e-6) << det;
    }
  }
}

TEST(FieldTable, InverseOutsideBracketIsConfigError) {
  auto t = argon_table();
  auto m = t.mass_for(h1_curve(80.0), "H1");
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().kind, ErrorKind::Config);
}

TEST(FieldTable, DiscreteInverseInterpolatesBetweenPoints) {
  auto t = argon_table(ps::FitKind::Discrete);
  EXPECT_NEAR(*t.mass_for(h1_curve(39.962383), "H1"), 39.962383, 1e-9);
  double mid = 0.5 * (h1_curve(38.964313) + h1_curve(39.962383));
  auto m = t.mass_for(mid, "H1");
  ASSERT_TRUE(m.has_value());
  EXPECT_GT(*m, 38.964313);
  EXPECT_LT(*m, 39.962383);
}

TEST(FieldTable, UpdateShiftsOnlyThatDetector) {
  auto t = argon_table();
  double ax_before = t.points()[4].values.at("AX");
  ASSERT_TRUE(t.update("H1", "Ar40", 5.0, false).has_value());
  EXPECT_DOUBLE_EQ(t.points()[4].values.at("H1"), 5.0);
  EXPECT_DOUBLE_EQ(t.points()[4].values.at("AX"), ax_before);
}

TEST(FieldTable, UpdatePropagatesOffsetToEveryDetector) {
  auto t = argon_table();
  double h1_before = t.points()[4].values.at("H1");
  double ax_before = t.points()[4].values.at("AX");
  ASSERT_TRUE(t.update("H1", "Ar40", h1_before + 0.02, true).has_value());
  EXPECT_DOUBLE_EQ(t.points()[4].values.at("H1"), h1_before + 0.02);
  EXPECT_NEAR(t.points()[4].values.at("AX"), ax_before + 0.02, 1e-12);
  // Other isotopes untouched.
  EXPECT_DOUBLE_EQ(t.points()[0].values.at("AX"), ax_curve(35.967545));
}

TEST(FieldTable, UpdateUnknownIsotopeOrDetectorIsConfigError) {
  auto t = argon_table();
  EXPECT_EQ(t.update("H1", "Kr84", 1.0, false).error().kind, ErrorKind::Config);
  EXPECT_EQ(t.update("CDD", "Ar40", 1.0, false).error().kind, ErrorKind::Config);
}

TEST(FieldTable, UpdateAddsDetectorValueMissingAtIsotope) {
  std::vector<ps::ControlPoint> pts{{"Ar40", 39.96, {{"H1", 5.0}}}, {"Ar36", 35.97, {{"H1", 4.5}, {"CDD", 4.4}}}};
  ps::FieldTable t(ps::FitKind::Discrete, ps::TableAxis::Dac, pts);
  ASSERT_TRUE(t.update("CDD", "Ar40", 4.9, false).has_value());
  EXPECT_DOUBLE_EQ(*t.value_for(39.96, "CDD"), 4.9);
}

TEST(FieldTable, RandomUpdatesKeepRoundTripWithinTolerance) {
  auto t = argon_table();
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> iso_pick(0, 4);
  std::uniform_int_distribution<int> det_pick(0, 1);
  std::uniform_real_distribution<double> nudge(-0.002, 0.002);
  std::uniform_real_distribution<double> mass(35.5, 40.5);
  std::bernoulli_distribution prop(0.5);
  const char* dets[] = {"H1", "AX"};
  for (int i = 0; i < 200; ++i) {
    const auto& p = t.points()[static_cast<size_t>(iso_pick(rng))];
    std::string det = dets[det_pick(rng)];
    std::string iso = p.isotope;
    double nv = p.values.at(det) + nudge(rng);
    ASSERT_TRUE(t.update(det, iso, nv, prop(rng)).has_value());
    double m = mass(rng);
    auto v = t.value_for(m, det);
    ASSERT_TRUE(v.has_value());
    auto back = t.mass_for(*v, det);
    ASSERT_TRUE(back.has_value()) << "iteration " << i;
    EXPECT_NEAR(*back, m, 1e-6);
  }
}

TEST(FieldTable, EnumStringsRoundTrip) {
  for (auto k : {ps::FitKind::Discrete, ps::FitKind::Linear, ps::FitKind::Quadratic, ps::FitKind::Cubic})
    EXPECT_EQ(*ps::parse_fit_kind(ps::to_string(k)), k);
  EXPECT_EQ(*ps::parse_fit_kind("parabolic"), ps::FitKind::Quadratic);
  EXPECT_EQ(ps::parse_fit_kind("spline").error().kind, ErrorKind::Config);
  for (auto a : {ps::TableAxis::Dac, ps::TableAxis::Field, ps::TableAxis::Mass})
    EXPECT_EQ(*ps::parse_table_axis(ps::to_string(a)), a);
  EXPECT_FALSE(ps::parse_table_axis("volts").has_value());
}

TEST(FieldTableToml, ParsesSpecExample) {
  auto t = ps::parse_field_table(R"(
fit = "quadratic"
axis = "dac"

[fits]
H2 = "linear"

[[points]]
isotope = "Ar40"
mass = 39.962
H2 = 5.123
H1 = 5.001

[[points]]
isotope = "Ar36"
mass = 35.968
H1 = 4.5
)");
  ASSERT_TRUE(t.has_value()) << pychron::to_string(t.error());
  EXPECT_EQ(t->default_fit(), ps::FitKind::Quadratic);
  EXPECT_EQ(t->axis(), ps::TableAxis::Dac);
  EXPECT_EQ(t->fit("H2"), ps::FitKind::Linear);
  ASSERT_EQ(t->points().size(), 2u);
  EXPECT_EQ(t->points()[0].isotope, "Ar40");
  EXPECT_DOUBLE_EQ(t->points()[0].values.at("H2"), 5.123);
  EXPECT_EQ(t->points()[1].values.count("H2"), 0u);
}

TEST(FieldTableToml, RoundTripsThroughText) {
  auto t = argon_table();
  t.set_fit("AX", ps::FitKind::Cubic);
  auto back = ps::parse_field_table(ps::to_toml(t));
  ASSERT_TRUE(back.has_value()) << pychron::to_string(back.error());
  EXPECT_EQ(*back, t);
}

TEST(FieldTableToml, RejectsMalformed) {
  EXPECT_EQ(ps::parse_field_table("fit = ").error().kind, ErrorKind::Config);
  EXPECT_EQ(ps::parse_field_table("fit = \"bogus\"\naxis=\"dac\"").error().kind, ErrorKind::Config);
  EXPECT_EQ(ps::parse_field_table("fit = \"linear\"\naxis=\"dac\"\n[[points]]\nmass = 1.0\n").error().kind,
            ErrorKind::Config);  // missing isotope
  EXPECT_EQ(ps::parse_field_table("fit = \"linear\"\naxis=\"dac\"\n[[points]]\nisotope=\"a\"\nmass=1.0\nH1=\"x\"\n")
                .error()
                .kind,
            ErrorKind::Config);  // non-numeric detector value
}
