#include "pychron/processing/arar_figures.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "fixtures.hpp"
#include "pychron/processing/arar_groups.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/units.hpp"
#include "pychron/reduction/arar_reduction.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using pp::test::make_step;

namespace {

// Eight steps of one aliquot with varying gas and atmospheric content.
pp::Dataset steps(int n = 8, double f = 10.0) {
  pp::Dataset d;
  const double ar39[] = {5, 20, 40, 60, 50, 30, 15, 5, 8, 12};
  const double ar36[] = {0.5, 0.3, 0.2, 0.1, 0.08, 0.06, 0.1, 0.2, 0.3, 0.4};
  for (int i = 0; i < n; ++i) {
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(make_step(i, ar39[i], ar36[i], f), {});
    d.mutable_items().push_back(item);
  }
  return d;
}

pp::GroupItems all(const pp::Dataset& d) {
  pp::GroupItems out;
  for (const auto& it : d.items()) out.push_back(&it);
  return out;
}

double age_of_f(double f) {
  // The age every step has (no J error), from the first step's reduction.
  auto d = steps(1, f);
  return pp::Quantity::parse("age")->eval(*d.items()[0].analysis)->value;
}

template <class T>
std::vector<const T*> layers(const pp::Panel& p) {
  std::vector<const T*> out;
  for (const auto& l : p.layers)
    if (const auto* x = std::get_if<T>(&l)) out.push_back(x);
  return out;
}

std::string all_text(const pp::Panel& p) {
  std::string s;
  for (const auto* t : layers<pp::TextLayer>(p))
    for (const auto& l : t->lines) s += l + "\n";
  return s;
}

TEST(ArArGroups, StepsShareOneAge) {
  const auto d = steps();
  const double age = age_of_f(10.0);
  for (const auto& it : d.items()) EXPECT_NEAR(pp::Quantity::parse("age")->eval(*it.analysis)->value, age, 1e-9);
  EXPECT_NEAR(pp::Quantity::parse("F")->eval(*d.items()[3].analysis)->value, 10.0, 1e-9);
}

TEST(ArArGroups, IntegratedAgeOfConcordantStepsIsTheirAge) {
  const auto d = steps();
  auto ia = pp::integrated_age(all(d), false);
  ASSERT_TRUE(ia) << ia.error().what;
  EXPECT_NEAR(ia->nominal(), age_of_f(10.0), 1e-9);
  auto with_j = pp::integrated_age(all(d), true);
  ASSERT_TRUE(with_j);
  EXPECT_GT(with_j->std_dev(), ia->std_dev());
  EXPECT_FALSE(pp::integrated_age({}, false));
}

TEST(ArArGroups, IsochronRecoversFAndTrapped) {
  const auto d = steps();
  const auto pts = pp::isochron_points(all(d));
  ASSERT_EQ(pts.size(), 8u);
  for (const auto& p : pts) {
    EXPECT_GT(p.rho, -1.0);
    EXPECT_LT(p.rho, 1.0);
    // 36/40 = (1 - F 39/40) / 298.56 exactly.
    EXPECT_NEAR(p.y, (1.0 - 10.0 * p.x) / 298.56, 1e-12);
  }
  auto iso = pp::isochron_age(pts, r::YorkMethod::NewYork, false, true);
  ASSERT_TRUE(iso) << iso.error().what;
  EXPECT_NEAR(iso->trapped.value, 298.56, 1e-6);
  ASSERT_TRUE(iso->f);
  EXPECT_NEAR(iso->f->nominal(), 10.0, 1e-8);
  ASSERT_TRUE(iso->age);
  EXPECT_NEAR(iso->age->nominal(), age_of_f(10.0), 1e-6);
  EXPECT_NEAR(iso->fit.mswd, 0.0, 1e-9);
  EXPECT_FALSE(pp::isochron_age({pts[0], pts[1]}, r::YorkMethod::NewYork, false, true));
}

TEST(ArArGroups, ExternalError) {
  EXPECT_DOUBLE_EQ(pp::with_external_error(100.0, 1.0, 0.0), 1.0);
  EXPECT_NEAR(pp::with_external_error(100.0, 3.0, 0.04), 5.0, 1e-12);
  const auto d = steps(2);
  EXPECT_NEAR(pp::j_relative_error(all(d)), 1e-3, 1e-12);
}

TEST(KernelDensity, UnitAreaAroundTheValues) {
  const std::vector<double> v{10.0, 10.5, 11.0, 12.0};
  auto c = r::kernel_density(v, 0.0, 22.0, 2201);
  double area = 0;
  for (std::size_t i = 1; i < c.x.size(); ++i) area += 0.5 * (c.y[i] + c.y[i - 1]) * (c.x[i] - c.x[i - 1]);
  EXPECT_NEAR(area, 1.0, 1e-6);
  EXPECT_TRUE(r::kernel_density(std::vector<double>{1.0}, 0, 2).x.empty());
}

// ---------------------------------------------------------------- ideogram

TEST(Ideogram, DefaultPanelsCurveAndMean) {
  auto d = steps();
  pp::Options o(pp::ideogram_schema());
  auto s = pp::build_ideogram(d, o);
  ASSERT_TRUE(s) << s.error().what;
  ASSERT_EQ(s->graphs.size(), 1u);
  const auto& g = s->graphs[0];
  ASSERT_EQ(g.panels.size(), 2u);
  EXPECT_EQ(g.panels[0].quantity, "analysis_number");
  EXPECT_EQ(g.panels[1].quantity, "probability");
  EXPECT_EQ(g.x.title, "Age (Ma)");
  const double age = age_of_f(10.0);
  ASSERT_TRUE(g.x.min && g.x.max);
  EXPECT_LT(*g.x.min, age);
  EXPECT_GT(*g.x.max, age);

  const auto numbers = layers<pp::PointLayer>(g.panels[0]);
  ASSERT_EQ(numbers.size(), 1u);
  EXPECT_EQ(numbers[0]->x.size(), 8u);
  EXPECT_EQ(numbers[0]->x_err.size(), 8u);
  EXPECT_DOUBLE_EQ(numbers[0]->y.back(), 8.0);

  const auto curves = layers<pp::LineLayer>(g.panels[1]);
  ASSERT_EQ(curves.size(), 1u);
  const auto peak = std::max_element(curves[0]->y.begin(), curves[0]->y.end()) - curves[0]->y.begin();
  EXPECT_NEAR(curves[0]->x[static_cast<std::size_t>(peak)], age, (*g.x.max - *g.x.min) / 400);
  const auto indicator = layers<pp::PointLayer>(g.panels[1]);
  ASSERT_EQ(indicator.size(), 1u);
  EXPECT_NEAR(indicator[0]->x[0], age, 1e-6);
  EXPECT_NE(all_text(g.panels[1]).find("wtd mean"), std::string::npos);
}

TEST(Ideogram, ExclusionDashesTheOriginalCurveAndMovesTheMean) {
  auto d = steps();
  // A stray analysis: F = 12.
  pp::DatasetItem odd;
  odd.analysis = pp::reduce_analysis(make_step(9, 30, 0.1, 12.0, "S2"), {});
  d.mutable_items().push_back(odd);
  pp::Options o(pp::ideogram_schema());
  auto with = pp::build_ideogram(d, o);
  ASSERT_TRUE(with);
  const double mean_with = layers<pp::PointLayer>(with->graphs[0].panels[1])[0]->x[0];
  d.mutable_items().back().exclusion.user = true;
  auto without = pp::build_ideogram(d, o);
  ASSERT_TRUE(without);
  const auto& p = without->graphs[0].panels[1];
  const auto lines = layers<pp::LineLayer>(p);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]->style.dash, pp::LineDash::Dash);
  EXPECT_NEAR(layers<pp::PointLayer>(p)[0]->x[0], age_of_f(10.0), 1e-6);
  EXPECT_GT(mean_with, age_of_f(10.0));
}

TEST(Ideogram, JErrorInTheMean) {
  auto d = steps();
  pp::Options o(pp::ideogram_schema());
  auto with = pp::build_ideogram(d, o);
  ASSERT_TRUE(o.set("j_error_in_mean", false));
  auto without = pp::build_ideogram(d, o);
  ASSERT_TRUE(with && without);
  const double e_with = layers<pp::PointLayer>(with->graphs[0].panels[1])[0]->x_err[0];
  const double e_without = layers<pp::PointLayer>(without->graphs[0].panels[1])[0]->x_err[0];
  EXPECT_GT(e_with, e_without);
}

TEST(Ideogram, ValuePanelKernelAndLimits) {
  auto d = steps();
  auto o = pp::PresetStore("/nonexistent").factory(pp::ideogram_schema(), "With K/Ca");
  ASSERT_TRUE(o);
  ASSERT_TRUE(o->options.set("probability", std::string("kernel")));
  ASSERT_TRUE(o->options.set("x.limits", std::string("centered")));
  ASSERT_TRUE(o->options.set("x.centered_range", 0.5));
  auto s = pp::build_ideogram(d, o->options);
  ASSERT_TRUE(s) << s.error().what;
  const auto& g = s->graphs[0];
  ASSERT_EQ(g.panels.size(), 3u);
  EXPECT_EQ(g.panels[0].quantity, "kca");
  EXPECT_EQ(g.panels[0].y.scale, pp::AxisScale::Log);
  EXPECT_EQ(g.panels[2].y.title, "Kernel density");
  EXPECT_NEAR(*g.x.max - *g.x.min, 1.0, 1e-9);
  ASSERT_TRUE(o->options.set("x.limits", std::string("asymptotic")));
  ASSERT_TRUE(o->options.set("probability", std::string("cumulative")));
  auto a = pp::build_ideogram(d, o->options);
  ASSERT_TRUE(a);
  const auto curve = layers<pp::LineLayer>(a->graphs[0].panels[2]);
  ASSERT_FALSE(curve.empty());
  const double peak = *std::max_element(curve[0]->y.begin(), curve[0]->y.end());
  EXPECT_LE(curve[0]->y.front(), 0.1 * peak + 1e-12);
  EXPECT_LE(curve[0]->y.back(), 0.1 * peak + 1e-12);
}

// ---------------------------------------------------------------- spectrum

TEST(Spectrum, StepsPlateauAndIntegrated) {
  auto d = steps();
  pp::Options o(pp::spectrum_schema());
  auto s = pp::build_spectrum(d, o);
  ASSERT_TRUE(s) << s.error().what;
  const auto& g = s->graphs[0];
  ASSERT_EQ(g.panels.size(), 1u);
  EXPECT_EQ(*g.x.min, 0.0);
  EXPECT_EQ(*g.x.max, 100.0);
  const auto st = layers<pp::StepLayer>(g.panels[0]);
  ASSERT_EQ(st.size(), 1u);
  ASSERT_EQ(st[0]->x0.size(), 8u);
  EXPECT_DOUBLE_EQ(st[0]->x0[0], 0.0);
  EXPECT_NEAR(st[0]->x1.back(), 100.0, 1e-9);
  EXPECT_NEAR(st[0]->x1[0], 5.0 / 225.0 * 100.0, 1e-9);  // widths by 39ArK
  for (std::size_t i = 0; i < 8; ++i) EXPECT_TRUE(st[0]->highlighted[i]);  // all concordant
  const std::string text = all_text(g.panels[0]);
  EXPECT_NE(text.find("plateau A-H"), std::string::npos) << text;
  EXPECT_NE(text.find("integrated"), std::string::npos) << text;
  const auto bars = layers<pp::LineLayer>(g.panels[0]);
  ASSERT_EQ(bars.size(), 2u);  // centre line, plateau bar
  EXPECT_NEAR(bars[1]->y[0], age_of_f(10.0), 1e-6);
}

TEST(Spectrum, DiscordantStepsAndFixedPlateau) {
  pp::Dataset d;
  const double ar39[] = {5, 10, 40, 60, 50, 30, 15, 5};
  for (int i = 0; i < 8; ++i) {
    pp::DatasetItem item;
    // Two young low-temperature steps.
    item.analysis = pp::reduce_analysis(make_step(i, ar39[i], 0.1, i < 2 ? 7.0 : 10.0), {});
    d.mutable_items().push_back(item);
  }
  pp::Options o(pp::spectrum_schema());
  auto s = pp::build_spectrum(d, o);
  ASSERT_TRUE(s);
  auto st = layers<pp::StepLayer>(s->graphs[0].panels[0]);
  EXPECT_FALSE(st[0]->highlighted[0]);
  EXPECT_FALSE(st[0]->highlighted[1]);
  EXPECT_TRUE(st[0]->highlighted[2]);
  EXPECT_NE(all_text(s->graphs[0].panels[0]).find("plateau C-H"), std::string::npos);

  // Fixed steps from the group row.
  auto row = o.new_row("groups");
  ASSERT_TRUE(row.set("fixed_start", std::string("A")));
  ASSERT_TRUE(row.set("fixed_end", std::string("D")));
  ASSERT_TRUE(o.set_rows("groups", {row}));
  auto fixed = pp::build_spectrum(d, o);
  ASSERT_TRUE(fixed);
  st = layers<pp::StepLayer>(fixed->graphs[0].panels[0]);
  EXPECT_TRUE(st[0]->highlighted[0]);
  EXPECT_FALSE(st[0]->highlighted[4]);
  EXPECT_NE(all_text(fixed->graphs[0].panels[0]).find("plateau A-D"), std::string::npos);

  // No plateau: the weighted mean is reported instead.
  pp::Options strict(pp::spectrum_schema());
  ASSERT_TRUE(strict.set("plateau.gas_fraction", 99.0));
  auto none = pp::build_spectrum(d, strict);
  ASSERT_TRUE(none);
  EXPECT_NE(all_text(none->graphs[0].panels[0]).find("no plateau"), std::string::npos);
}

TEST(Spectrum, ExcludedStepsLeaveThePlateau) {
  auto d = steps();
  d.mutable_items()[3].exclusion.user = true;
  auto s = pp::build_spectrum(d, pp::Options(pp::spectrum_schema()));
  ASSERT_TRUE(s);
  const auto st = layers<pp::StepLayer>(s->graphs[0].panels[0]);
  EXPECT_TRUE(st[0]->excluded[3]);
  EXPECT_FALSE(st[0]->highlighted[3]);
  EXPECT_TRUE(st[0]->highlighted[4]);
}

TEST(Spectrum, ValueSpectrumPanel) {
  auto o = pp::PresetStore("/nonexistent").factory(pp::spectrum_schema(), "With %40Ar*");
  ASSERT_TRUE(o);
  auto s = pp::build_spectrum(steps(), o->options);
  ASSERT_TRUE(s);
  ASSERT_EQ(s->graphs[0].panels.size(), 2u);
  EXPECT_EQ(s->graphs[0].panels[0].quantity, "radiogenic_yield");
  EXPECT_EQ(layers<pp::StepLayer>(s->graphs[0].panels[0])[0]->x0.size(), 8u);
}

// ---------------------------------------------------------------- isochron

TEST(Isochron, EllipsesFitAndResults) {
  auto d = steps();
  pp::Options o(pp::isochron_schema());
  auto s = pp::build_isochron(d, o);
  ASSERT_TRUE(s) << s.error().what;
  const auto& p = s->graphs[0].panels[0];
  const auto el = layers<pp::EllipseLayer>(p);
  ASSERT_EQ(el.size(), 1u);
  EXPECT_EQ(el[0]->x.size(), 8u);
  EXPECT_DOUBLE_EQ(el[0]->scale, 1.0);
  const auto lines = layers<pp::LineLayer>(p);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NEAR(lines[0]->y.front(), 1.0 / 298.56, 1e-9);  // y-intercept at x = 0
  EXPECT_EQ(layers<pp::BandLayer>(p).size(), 1u);
  const std::string text = all_text(p);
  EXPECT_NE(text.find("(40/36)trapped 298.6"), std::string::npos) << text;
  EXPECT_NE(text.find("age "), std::string::npos);
  EXPECT_EQ(*s->graphs[0].x.min, 0.0);

  d.mutable_items()[0].exclusion.user = true;
  auto fewer = pp::build_isochron(d, o);
  ASSERT_TRUE(fewer);
  EXPECT_NE(all_text(fewer->graphs[0].panels[0]).find("n 7/8"), std::string::npos);

  ASSERT_TRUE(o.set("ellipse", std::string("95%")));
  auto e95 = pp::build_isochron(d, o);
  EXPECT_NEAR(layers<pp::EllipseLayer>(e95->graphs[0].panels[0])[0]->scale, 2.4477, 1e-9);
}

TEST(Isochron, ExcludeNonPlateau) {
  pp::Dataset d;
  const double ar39[] = {5, 10, 40, 60, 50, 30, 15, 5};
  for (int i = 0; i < 8; ++i) {
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(make_step(i, ar39[i], 0.1 + 0.02 * i, i < 2 ? 7.0 : 10.0), {});
    d.mutable_items().push_back(item);
  }
  pp::Options o(pp::isochron_schema());
  ASSERT_TRUE(o.set("exclude_non_plateau", true));
  auto s = pp::build_isochron(d, o);
  ASSERT_TRUE(s);
  const auto& p = s->graphs[0].panels[0];
  EXPECT_NE(all_text(p).find("n 6/8"), std::string::npos) << all_text(p);
  const auto el = layers<pp::EllipseLayer>(p);
  EXPECT_TRUE(el[0]->excluded[0]);
  EXPECT_FALSE(el[0]->excluded[2]);
}

TEST(ArArFigures, UnitsRegisteredAndFactoryPresetsClean) {
  pp::PresetStore store("/nonexistent");
  for (const char* kind : {"ideogram", "spectrum", "inverse_isochron"}) {
    const auto* u = pp::UnitRegistry::builtin().find(kind);
    ASSERT_NE(u, nullptr) << kind;
    for (const auto& [name, _] : u->schema()->factory_presets) {
      auto f = store.factory(u->schema(), name);
      ASSERT_TRUE(f) << kind << " " << name;
      EXPECT_TRUE(f->warnings.empty()) << kind << " " << name;
    }
  }
}

TEST(ArArFigures, PipelineRunsEachFigure) {
  pp::MemorySource src;
  for (int i = 0; i < 6; ++i) src.add(make_step(i, 10.0 + 5 * i, 0.1));
  pp::Runner runner(pp::UnitRegistry::builtin(), &src);
  for (const char* kind : {"ideogram", "spectrum", "inverse_isochron"}) {
    pp::Pipeline p;
    p.add(pp::UnitRegistry::builtin(), "select", "select");
    p.add(pp::UnitRegistry::builtin(), "reduce", "reduce", {"select"});
    auto& g = p.add(pp::UnitRegistry::builtin(), "group", "group", {"reduce"});
    ASSERT_TRUE(g.options.set("key", std::string("aliquot")));
    p.add(pp::UnitRegistry::builtin(), "figure", kind, {"group"});
    auto out = runner.run(p, "figure");
    ASSERT_TRUE(out) << kind << ": " << out.error().what;
    const auto& scene = *std::get<pp::ScenePtr>(out->at(0));
    EXPECT_EQ(scene.graphs.size(), 1u) << kind;
    EXPECT_TRUE(scene.warnings.empty()) << kind << ": " << scene.warnings.front();
  }
}

}  // namespace
