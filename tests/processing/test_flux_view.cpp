#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "flux_level_inputs.hpp"
#include "pychron/processing/flux_view.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;

namespace {

pp::FluxOptions options_of(r::ModelKind kind) {
  pp::FluxOptions o;
  o.fit.kind = kind;
  o.fit.weighted = true;
  o.mean = r::MeanKind::Arithmetic;
  o.mean_error = r::MeanErrorKind::Msem;
  o.fit.error = r::MeanErrorKind::Msem;
  return o;
}

pp::LevelFit hand_fit() {
  pp::LevelFit fit;
  fit.irradiation = "NM-300";
  fit.level = "A";
  fit.options = options_of(r::ModelKind::Plane);
  fit.mswd = 1.12;
  fit.dof = 5;
  fit.min_j = 1.0012e-3;
  fit.max_j = 1.0241e-3;
  fit.delta_j_percent = 2.24;
  return fit;
}

pp::FittedPosition::UsedAnalysis analysis(const std::string& id, pp::AnalysisState state) {
  pp::FittedPosition::UsedAnalysis a;
  a.record_id = id;
  a.state = state;
  return a;
}

}  // namespace

TEST(FluxText, ModelLinePerModel) {
  EXPECT_EQ(pp::flux_model_line(options_of(r::ModelKind::Plane)), "plane, weighted; mean arithmetic (msem); fit error msem");
  auto nearest = options_of(r::ModelKind::NearestNeighbors);
  nearest.fit.n_neighbors = 3;
  EXPECT_EQ(pp::flux_model_line(nearest), "nearest, 3 neighbors; mean arithmetic (msem); fit error msem");
  auto bracketing = options_of(r::ModelKind::Bracketing);
  bracketing.fit.interpolation = r::Interpolation::Linear;
  EXPECT_EQ(pp::flux_model_line(bracketing), "bracketing, linear; mean arithmetic (msem); fit error msem");
  auto b1 = options_of(r::ModelKind::Bracketing1D);
  b1.fit.axis = r::Axis::Y;
  const std::string line = pp::flux_model_line(b1);
  EXPECT_EQ(line, "bracketing1d, axis y; mean arithmetic (msem); fit error msem");
  EXPECT_EQ(line.find("linear"), std::string::npos);
  EXPECT_EQ(line.find("weighted,"), std::string::npos);
}

TEST(FluxText, SummaryFormat) {
  EXPECT_EQ(pp::flux_summary(hand_fit()), "fit MSWD 1.12 (5 dof)   J min 1.0012e-03  max 1.0241e-03  delta 2.24 %");
}

TEST(FluxText, StatusLine) {
  EXPECT_EQ(pp::flux_status_line(hand_fit()),
            "plane, weighted \xC2\xB7 fit MSWD 1.12 (5 dof) \xC2\xB7 J 1.0012e-03 \xE2\x80\x93 1.0241e-03 (2.24 %)");
  auto fit = hand_fit();
  fit.options = options_of(r::ModelKind::NearestNeighbors);
  fit.options.fit.n_neighbors = 3;
  fit.mswd = 0;
  fit.dof = 0;
  EXPECT_EQ(pp::flux_status_line(fit),
            "nearest, 3 neighbors \xC2\xB7 J 1.0012e-03 \xE2\x80\x93 1.0241e-03 (2.24 %)");
}

TEST(FluxText, WarningsCoverEveryKind) {
  pp::LevelInputs inputs;
  inputs.monitor_set.name = "FC-2";
  inputs.saved_options = options_of(r::ModelKind::Plane);
  inputs.saved_monitor_set = "FC Min";
  inputs.saved_monitor_set_missing = true;
  inputs.saved_sd_replaced = true;

  auto fit = hand_fit();
  fit.mswd = 4.5;
  fit.mswd_outside_limits = true;
  pp::FittedPosition a;
  a.hole = 3;
  a.mean_j_mswd = 3.0;
  a.notes = {pp::PositionNote::NoUsableAnalysis, pp::PositionNote::LeftOutOfFit, pp::PositionNote::MeanMswdOutsideLimits,
             pp::PositionNote::Extrapolated, pp::PositionNote::AnalysisNotReduced, pp::PositionNote::AnalysisRejected};
  auto tagged = analysis("A-1", pp::AnalysisState::OmittedByTag);
  tagged.tag = "bad";
  auto broken = analysis("A-5", pp::AnalysisState::NotReduced);
  broken.reduction_error = "no blank";
  a.analyses = {analysis("A-0", pp::AnalysisState::Used), tagged, analysis("A-2", pp::AnalysisState::OmittedBySavedFit),
                analysis("A-3", pp::AnalysisState::OmittedByEdit), analysis("A-4", pp::AnalysisState::NoJ), broken};
  fit.positions = {a};

  const std::vector<std::string> expected = {
      "hole 3 has no usable analysis",
      "hole 3 left out of the fit",
      "hole 3: mean MSWD 3.00 is outside its limits",
      "hole 3 is extrapolated (outside the monitors)",
      "fit MSWD 4.50 is outside its limits",
      "hole 3: A-1 omitted (tag bad)",
      "hole 3: A-2 omitted (saved fit)",
      "hole 3: A-3 omitted (here)",
      "hole 3: A-4 no J",
      "hole 3: A-5 not reduced: no blank",
      "saved fit used SD, which a fitted surface does not have: using msem",
      "saved fit used monitor set 'FC Min', which the store does not have: using 'FC-2'",
  };
  EXPECT_EQ(pp::flux_warnings(inputs, fit), expected);
}

TEST(FluxText, NotReducedWithoutAnErrorHasNoTrailingColon) {
  pp::LevelInputs inputs;
  auto fit = hand_fit();
  pp::FittedPosition p;
  p.hole = 2;
  p.analyses = {analysis("B-1", pp::AnalysisState::NotReduced)};
  fit.positions = {p};
  const auto w = pp::flux_warnings(inputs, fit);
  EXPECT_NE(std::find(w.begin(), w.end(), "hole 2: B-1 not reduced"), w.end());
}

TEST(FluxText, WarningsHonourTheContext) {
  pp::LevelInputs inputs;
  inputs.monitor_set.name = "FC-2";
  inputs.saved_options = options_of(r::ModelKind::Plane);
  inputs.saved_monitor_set = "FC Min";
  inputs.saved_monitor_set_missing = true;
  inputs.saved_sd_replaced = true;
  auto fit = hand_fit();

  EXPECT_EQ(pp::flux_warnings(inputs, fit).size(), 2u);
  const auto given_set = pp::flux_warnings(inputs, fit, pp::FluxWarningContext{true, false});
  ASSERT_EQ(given_set.size(), 1u);
  EXPECT_NE(given_set[0].find("saved fit used SD"), std::string::npos);
  const auto given_error = pp::flux_warnings(inputs, fit, pp::FluxWarningContext{false, true});
  ASSERT_EQ(given_error.size(), 1u);
  EXPECT_NE(given_error[0].find("monitor set 'FC Min'"), std::string::npos);
  // Not least squares: the SD line does not apply.
  fit.options = options_of(r::ModelKind::WeightedMean);
  const auto mean_model = pp::flux_warnings(inputs, fit);
  ASSERT_EQ(mean_model.size(), 1u);
  EXPECT_NE(mean_model[0].find("monitor set 'FC Min'"), std::string::npos);
}

TEST(FluxText, CsvIsRfc4180) {
  EXPECT_EQ(pp::csv_field("plain"), "plain");
  EXPECT_EQ(pp::csv_field("a,b"), "\"a,b\"");
  EXPECT_EQ(pp::csv_field("say \"hi\""), "\"say \"\"hi\"\"\"");
  EXPECT_EQ(pp::csv_field("FC-2, \"new\""), "\"FC-2, \"\"new\"\"\"");

  auto fit = hand_fit();
  pp::FittedPosition m;
  m.hole = 1;
  m.monitor = true;
  m.identifier = "M1";
  m.sample = "FC-2, \"new\"";
  m.n = 4;
  m.j = 1.5e-3;
  m.used_in_fit = true;
  pp::FittedPosition u;
  u.hole = 2;
  u.identifier = "U1";
  u.sample = "plain";
  fit.positions = {m, u};

  const std::string header = pp::flux_csv_header();
  const std::string rows = pp::flux_csv_rows(fit);
  ASSERT_GE(header.size(), 2u);
  EXPECT_EQ(header.substr(header.size() - 2), "\r\n");
  const auto count_fields = [](const std::string& line) {  // line without CRLF; quotes honoured
    std::size_t n = 1;
    bool quoted = false;
    for (const char c : line) {
      if (c == '"') quoted = !quoted;
      else if (c == ',' && !quoted) ++n;
    }
    return n;
  };
  EXPECT_EQ(count_fields(header.substr(0, header.size() - 2)), 19u);
  std::vector<std::string> lines;
  for (std::size_t at = 0; at < rows.size();) {
    const auto end = rows.find("\r\n", at);
    ASSERT_NE(end, std::string::npos);
    lines.push_back(rows.substr(at, end - at));
    at = end + 2;
  }
  ASSERT_EQ(lines.size(), 2u);
  for (const auto& line : lines) EXPECT_EQ(count_fields(line), 19u) << line;
  EXPECT_NE(lines[0].find("\"FC-2, \"\"new\"\"\""), std::string::npos) << lines[0];
}

// ---- the options schema ------------------------------------------------------

namespace {

const std::vector<r::ModelKind> kAllKinds = {
    r::ModelKind::Plane,        r::ModelKind::Bowl,           r::ModelKind::WeightedMean,
    r::ModelKind::Matching,     r::ModelKind::NearestNeighbors, r::ModelKind::Bracketing,
    r::ModelKind::LeastSquares1D, r::ModelKind::WeightedMean1D, r::ModelKind::Bracketing1D};

}  // namespace

TEST(FluxSchema, SharedInstanceWithTheFixedIdentity) {
  const auto s = pp::flux_options_schema();
  ASSERT_TRUE(s);
  EXPECT_EQ(s, pp::flux_options_schema());
  EXPECT_EQ(s->kind, "flux");
  EXPECT_EQ(s->title, "Flux");
  EXPECT_EQ(s->version, 1);
}

TEST(FluxSchema, DefaultsAreFluxOptionsDefaults) {
  const auto o = pp::flux_options_from(pp::Options(pp::flux_options_schema()));
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(*o, pp::FluxOptions{});
}

TEST(FluxSchema, RoundTripForEveryModel) {
  for (const auto kind : kAllKinds) {
    pp::FluxOptions o;
    o.fit.kind = kind;
    o.fit.weighted = true;
    o.fit.error = r::MeanErrorKind::Sem;
    o.fit.n_neighbors = 5;
    o.fit.interpolation = r::Interpolation::Linear;
    o.fit.axis = r::Axis::Y;
    o.fit.degree = 3;
    o.mean = r::MeanKind::Weighted;
    o.mean_error = r::MeanErrorKind::Sd;
    const auto back = pp::flux_options_from(pp::to_options(o));
    ASSERT_TRUE(back.has_value()) << back.error().what;
    EXPECT_EQ(*back, o) << static_cast<int>(kind);
  }
}

TEST(FluxSchema, SdWithASurfaceIsTheMathLayersError) {
  for (const auto kind : {r::ModelKind::Plane, r::ModelKind::Bowl, r::ModelKind::LeastSquares1D}) {
    pp::FluxOptions o;
    o.fit.kind = kind;
    o.fit.error = r::MeanErrorKind::Sd;
    const auto got = pp::flux_options_from(pp::to_options(o));
    ASSERT_FALSE(got.has_value());
    EXPECT_NE(got.error().what.find("sd is not an error kind of a fitted surface"), std::string::npos);
  }
  pp::FluxOptions m;
  m.fit.kind = r::ModelKind::WeightedMean;
  m.fit.error = r::MeanErrorKind::Sd;
  EXPECT_TRUE(pp::flux_options_from(pp::to_options(m)).has_value());
}

TEST(FluxSchema, FactoryPresets) {
  const auto schema = pp::flux_options_schema();
  const auto dir = std::filesystem::temp_directory_path() / "pp_flux_schema_presets";
  std::filesystem::remove_all(dir);
  const pp::PresetStore store(dir);
  const auto list = store.list(schema);
  std::vector<std::string> names;
  for (const auto& p : list) names.push_back(p.name);
  EXPECT_NE(std::find(names.begin(), names.end(), "Default"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "Weighted plane"), names.end());
  const auto loaded = store.load(schema, "Weighted plane");
  ASSERT_TRUE(loaded.has_value());
  const auto o = pp::flux_options_from(loaded->options);
  ASSERT_TRUE(o.has_value());
  EXPECT_TRUE(o->fit.weighted);
  EXPECT_EQ(o->fit.kind, r::ModelKind::Plane);
  std::filesystem::remove_all(dir);
}

TEST(FluxSchema, EnabledWhenNamesOnlyKnownKeysAndValues) {
  const auto schema = pp::flux_options_schema();
  int checked = 0;
  for (const auto& f : schema->fields) {
    if (f.enabled_when.empty()) continue;
    const auto in = f.enabled_when.find(" in ");
    const auto eq = f.enabled_when.find(" == ");
    ASSERT_TRUE(in != std::string::npos || eq != std::string::npos) << f.enabled_when;
    const auto op = std::min(in, eq);
    const std::string key = f.enabled_when.substr(0, op);
    const std::string rest = f.enabled_when.substr(op + (op == in ? 4 : 4));
    const auto* target = schema->field(key);
    ASSERT_TRUE(target) << f.enabled_when;
    std::vector<std::string> values;
    std::size_t start = 0;
    for (;;) {
      const auto bar = rest.find('|', start);
      values.push_back(rest.substr(start, bar == std::string::npos ? bar : bar - start));
      if (bar == std::string::npos) break;
      start = bar + 1;
    }
    for (const auto& v : values)
      EXPECT_NE(std::find(target->choices.begin(), target->choices.end(), v), target->choices.end())
          << f.enabled_when;
    ++checked;
  }
  EXPECT_GE(checked, 6);
}

// ---- The scene ---------------------------------------------------------------

namespace {

using pychron::processing::flux_test::level;

const pp::PointLayer* points_labelled(const pp::Scene& scene, const std::string& label) {
  for (const auto& layer : scene.graphs.at(0).panels.at(0).layers)
    if (const auto* p = std::get_if<pp::PointLayer>(&layer))
      if (p->label == label) return p;
  return nullptr;
}
template <typename L>
const L* first_of(const pp::Scene& scene) {
  for (const auto& layer : scene.graphs.at(0).panels.at(0).layers)
    if (const auto* l = std::get_if<L>(&layer)) return l;
  return nullptr;
}
const std::vector<pp::Layer>& layers_of(const pp::Scene& scene) { return scene.graphs.at(0).panels.at(0).layers; }

pp::LevelFit fit_or_die(const pp::LevelInputs& in, const pp::FluxOptions& o, const pp::Edits& e = {}) {
  auto fit = pp::fit_level(in, o, e);
  if (!fit) throw std::runtime_error(fit.error().what);
  return *fit;
}

// The curve's y at x, by linear interpolation along the line.
double along(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
  for (std::size_t i = 1; i < xs.size(); ++i)
    if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
  return ys.back();
}

const pp::FittedPosition& hole_of(const pp::LevelFit& fit, int hole) {
  return *std::find_if(fit.positions.begin(), fit.positions.end(), [&](auto& p) { return p.hole == hole; });
}

}  // namespace

TEST(FluxScene, AbscissaPerModel) {
  using K = r::ModelKind;
  for (const auto kind : {K::Plane, K::Bowl, K::WeightedMean, K::Matching, K::NearestNeighbors, K::Bracketing})
    EXPECT_EQ(pp::flux_abscissa(options_of(kind)), pp::FluxAbscissa::Angle);
  for (const auto kind : {K::LeastSquares1D, K::WeightedMean1D, K::Bracketing1D}) {
    auto o = options_of(kind);
    o.fit.axis = r::Axis::X;
    EXPECT_EQ(pp::flux_abscissa(o), pp::FluxAbscissa::X);
    o.fit.axis = r::Axis::Y;
    EXPECT_EQ(pp::flux_abscissa(o), pp::FluxAbscissa::Y);
  }
}

TEST(FluxScene, HoleAbscissa) {
  using A = pp::FluxAbscissa;
  EXPECT_NEAR(pp::flux_hole_abscissa(A::Angle, 0, 10), 0, 1e-12);
  EXPECT_NEAR(pp::flux_hole_abscissa(A::Angle, 10, 0), 90, 1e-12);
  EXPECT_NEAR(pp::flux_hole_abscissa(A::Angle, 0, -10), 180, 1e-12);
  EXPECT_EQ(pp::flux_hole_abscissa(A::Angle, -0.0, -10), 180);  // (-180, 180]: never -180
  EXPECT_NEAR(pp::flux_hole_abscissa(A::Angle, -10, 0), -90, 1e-12);
  EXPECT_EQ(pp::flux_hole_abscissa(A::X, 3, 4), 3);
  EXPECT_EQ(pp::flux_hole_abscissa(A::Y, 3, 4), 4);
}

TEST(FluxScene, SceneShape) {
  const auto in = level();
  const auto scene = pp::flux_scene(in, fit_or_die(in, options_of(r::ModelKind::Plane)));
  ASSERT_TRUE(scene);
  EXPECT_EQ(scene->kind, "flux");
  ASSERT_EQ(scene->graphs.size(), 1u);
  ASSERT_EQ(scene->graphs[0].panels.size(), 1u);
  EXPECT_EQ(scene->graphs[0].panels[0].id, "p0");
  EXPECT_EQ(scene->graphs[0].panels[0].quantity, "J");
  EXPECT_EQ(scene->graphs[0].x.title, "Hole angle (degrees)");
  // band, line, analyses, means, unknowns
  ASSERT_EQ(layers_of(*scene).size(), 5u);
  EXPECT_TRUE(std::holds_alternative<pp::BandLayer>(layers_of(*scene)[0]));
  EXPECT_TRUE(std::holds_alternative<pp::LineLayer>(layers_of(*scene)[1]));
  EXPECT_EQ(std::get<pp::LineLayer>(layers_of(*scene)[1]).label, "Fit");
  EXPECT_EQ(std::get<pp::BandLayer>(layers_of(*scene)[0]).fill, (pp::Color{pp::palette_color(0).r, pp::palette_color(0).g,
                                                                           pp::palette_color(0).b, 48}));
  EXPECT_NE(points_labelled(*scene, "Analyses"), nullptr);
  EXPECT_NE(points_labelled(*scene, "Monitor means"), nullptr);
  EXPECT_NE(points_labelled(*scene, "Unknowns"), nullptr);
  auto o = options_of(r::ModelKind::LeastSquares1D);
  o.fit.axis = r::Axis::Y;
  const auto s1 = pp::flux_scene(in, fit_or_die(in, o));
  EXPECT_EQ(s1->graphs[0].x.title, "Y");
  EXPECT_EQ(s1->graphs[0].panels[0].y.title, "J");
}

TEST(FluxScene, OneAnalysisPointPerAnalysisWithAJ) {
  auto in = level();
  const auto o = options_of(r::ModelKind::Plane);
  auto scene = pp::flux_scene(in, fit_or_die(in, o));
  const auto* a = points_labelled(*scene, "Analyses");
  ASSERT_NE(a, nullptr);
  ASSERT_EQ(a->x.size(), 24u);
  ASSERT_EQ(a->refs.size(), 24u);
  std::vector<std::string> uuids;
  for (const auto& p : in.positions)
    for (const auto& an : p.analyses) uuids.push_back(an.uuid);
  std::vector<std::string> refs;
  for (const auto& ref : a->refs) refs.push_back(ref.analysis);
  std::sort(uuids.begin(), uuids.end());
  std::sort(refs.begin(), refs.end());
  EXPECT_EQ(refs, uuids);
  EXPECT_EQ(std::count(a->excluded.begin(), a->excluded.end(), true), 0);
  EXPECT_FALSE(a->excluded_marker.filled);
  EXPECT_EQ(a->marker.shape, pp::MarkerShape::Circle);
  EXPECT_EQ(a->marker.size, 4);
  EXPECT_TRUE(a->y_err.empty());  // no error bars on the analyses

  in.positions[0].analyses[1].f.reset();  // not reduced: no J, not drawn
  pp::Edits e;
  e.omit.insert("M2-01");
  scene = pp::flux_scene(in, fit_or_die(in, o, e));
  a = points_labelled(*scene, "Analyses");
  ASSERT_EQ(a->x.size(), 23u);
  EXPECT_EQ(std::count(a->excluded.begin(), a->excluded.end(), true), 1);
}

TEST(FluxScene, AnalysesSpreadAboutTheirHole) {
  const auto in = level();
  const auto scene = pp::flux_scene(in, fit_or_die(in, options_of(r::ModelKind::Plane)));
  const auto* a = points_labelled(*scene, "Analyses");
  std::vector<double> at_hole1;  // (10, 0): 90 degrees
  for (std::size_t i = 0; i < a->x.size(); ++i)
    if (a->refs[i].analysis.rfind("u-1-", 0) == 0) at_hole1.push_back(a->x[i]);
  ASSERT_EQ(at_hole1.size(), 3u);
  EXPECT_NEAR(at_hole1[0], 88, 1e-9);
  EXPECT_NEAR(at_hole1[1], 90, 1e-9);
  EXPECT_NEAR(at_hole1[2], 92, 1e-9);
}

TEST(FluxScene, MeansAndUnknowns) {
  const auto in = level();
  pp::Edits e;
  e.exclude_positions.insert(3);
  const auto fit = fit_or_die(in, options_of(r::ModelKind::Plane), e);
  const auto scene = pp::flux_scene(in, fit);
  const auto* m = points_labelled(*scene, "Monitor means");
  ASSERT_EQ(m->x.size(), 8u);
  ASSERT_EQ(m->y_err.size(), 8u);
  EXPECT_EQ(m->marker.shape, pp::MarkerShape::Diamond);
  EXPECT_EQ(m->marker.size, 8);
  std::size_t excluded = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const auto& p = hole_of(fit, static_cast<int>(i) + 1);
    EXPECT_EQ(m->y[i], *p.mean_j);
    EXPECT_EQ(m->y_err[i], *p.mean_j_err);
    EXPECT_EQ(bool(m->excluded[i]), !p.used_in_fit);
    excluded += m->excluded[i];
  }
  EXPECT_EQ(excluded, 1u);
  EXPECT_TRUE(m->excluded[2]);

  const auto* u = points_labelled(*scene, "Unknowns");
  ASSERT_EQ(u->x.size(), 4u);
  ASSERT_EQ(u->y_err.size(), 4u);
  EXPECT_EQ(u->marker.shape, pp::MarkerShape::Square);
  EXPECT_EQ(u->marker.size, 6);
  for (std::size_t i = 0; i < 4; ++i) {
    const auto& p = hole_of(fit, 101 + static_cast<int>(i));
    EXPECT_EQ(u->y[i], p.j);
    EXPECT_EQ(u->y_err[i], p.j_err);
  }
}

TEST(FluxScene, TheCurvePassesThroughThePredictions) {
  const auto in = level();
  const auto fit = fit_or_die(in, options_of(r::ModelKind::Plane));
  const auto scene = pp::flux_scene(in, fit);
  const auto* line = first_of<pp::LineLayer>(*scene);
  const auto* band = first_of<pp::BandLayer>(*scene);
  ASSERT_NE(line, nullptr);
  ASSERT_NE(band, nullptr);
  ASSERT_EQ(line->x.size(), 361u);
  ASSERT_EQ(band->x.size(), 361u);
  EXPECT_EQ(line->x.front(), -180);
  EXPECT_EQ(line->x.back(), 180);
  for (int hole = 1; hole <= 8; ++hole) {
    const auto& p = hole_of(fit, hole);
    const double angle = pp::flux_hole_abscissa(pp::FluxAbscissa::Angle, p.x, p.y);
    const double tol = 1e-9;  // the monitors lie at whole degrees, on the curve's points
    EXPECT_NEAR(along(line->x, line->y, angle), p.j, p.j * tol) << hole;
    EXPECT_NEAR(along(band->x, band->low, angle), p.j - p.j_err, p.j * tol) << hole;
    EXPECT_NEAR(along(band->x, band->high, angle), p.j + p.j_err, p.j * tol) << hole;
  }
}

TEST(FluxScene, WhichModelsHaveACurve) {
  using K = r::ModelKind;
  const auto ring = level();
  const auto mixed = level(flux_golden::kMixed);
  const auto has_curve = [](const pp::LevelInputs& in, K kind) {
    const auto scene = pp::flux_scene(in, fit_or_die(in, options_of(kind)));
    return std::pair{first_of<pp::LineLayer>(*scene) != nullptr, first_of<pp::BandLayer>(*scene) != nullptr};
  };
  for (const auto kind : {K::Plane, K::WeightedMean, K::LeastSquares1D, K::WeightedMean1D})
    EXPECT_EQ(has_curve(ring, kind), (std::pair{true, true})) << int(kind);
  EXPECT_EQ(has_curve(mixed, K::Bowl), (std::pair{true, true}));
  for (const auto kind : {K::Matching, K::NearestNeighbors, K::Bracketing, K::Bracketing1D})
    EXPECT_EQ(has_curve(ring, kind), (std::pair{false, false})) << int(kind);

  // A one-dimensional curve runs along the coordinate over the whole tray.
  auto o = options_of(K::LeastSquares1D);
  o.fit.axis = r::Axis::X;
  const auto scene = pp::flux_scene(ring, fit_or_die(ring, o));
  const auto* line = first_of<pp::LineLayer>(*scene);
  ASSERT_EQ(line->x.size(), 361u);
  EXPECT_NEAR(line->x.front(), -10, 1e-9);
  EXPECT_NEAR(line->x.back(), 10, 1e-9);
}

TEST(FluxScene, Highlight) {
  const auto in = level();
  const auto fit = fit_or_die(in, options_of(r::ModelKind::Plane));
  const auto plain = pp::flux_scene(in, fit);
  pp::FluxSceneOptions so;
  so.highlight_hole = 3;
  const auto scene = pp::flux_scene(in, fit, so);
  ASSERT_EQ(layers_of(*scene).size(), layers_of(*plain).size() + 2);
  const auto& a = std::get<pp::PointLayer>(layers_of(*scene)[layers_of(*plain).size()]);
  const auto& m = std::get<pp::PointLayer>(layers_of(*scene)[layers_of(*plain).size() + 1]);
  EXPECT_TRUE(a.label.empty());
  EXPECT_TRUE(m.label.empty());
  ASSERT_EQ(a.refs.size(), 3u);
  for (const auto& ref : a.refs) EXPECT_EQ(ref.analysis.rfind("u-3-", 0), 0u);
  ASSERT_EQ(m.x.size(), 1u);
  EXPECT_EQ(m.y[0], *hole_of(fit, 3).mean_j);
  EXPECT_EQ(m.marker.color, pp::palette_color(4));
}

TEST(FluxScene, WithoutAFitTheDataIsStillThere) {
  auto in = level();
  in.positions[3].analyses[0].tag = "outlier";
  const auto o = options_of(r::ModelKind::Plane);
  pp::Edits e;
  e.omit.insert("M2-02");
  const auto with = pp::flux_scene(in, fit_or_die(in, o, e));
  const auto without = pp::flux_scene(in, o, e);
  ASSERT_EQ(layers_of(*without).size(), 2u);
  for (const char* label : {"Analyses", "Monitor means"}) {
    const auto* a = points_labelled(*with, label);
    const auto* b = points_labelled(*without, label);
    ASSERT_NE(b, nullptr) << label;
    EXPECT_EQ(a->x, b->x) << label;
    EXPECT_EQ(a->y, b->y) << label;
    EXPECT_EQ(a->y_err, b->y_err) << label;
    EXPECT_EQ(a->excluded, b->excluded) << label;
    EXPECT_EQ(a->tooltips, b->tooltips) << label;
    EXPECT_EQ(a->refs, b->refs) << label;
  }
  EXPECT_EQ(without->graphs[0].x.title, "Hole angle (degrees)");
}

TEST(FluxScene, TooltipsSayWhy) {
  auto in = level();
  in.positions[1].analyses[1].tag = "outlier";  // M2-02
  in.positions[2].analyses[0].f.reset();
  pp::Edits e;
  e.omit.insert("M1-01");
  const auto fit = fit_or_die(in, options_of(r::ModelKind::Plane), e);
  const auto scene = pp::flux_scene(in, fit);
  const auto* a = points_labelled(*scene, "Analyses");
  const auto tip = [&](const std::string& uuid) {
    for (std::size_t i = 0; i < a->refs.size(); ++i)
      if (a->refs[i].analysis == uuid) return a->tooltips[i];
    return std::string("<none>");
  };
  const auto j = pp::flux_j_text(*hole_of(fit, 2).analyses[1].j);
  const auto err = pp::flux_j_text(*hole_of(fit, 2).analyses[1].j_err);
  EXPECT_EQ(tip("u-2-2"), "M2-02\nJ " + j + " \xC2\xB1 " + err + "\nomitted (tag outlier)");
  EXPECT_NE(tip("u-1-1").find("omitted (here)"), std::string::npos);
  EXPECT_EQ(tip("u-3-1"), "<none>");  // not reduced: no J, not drawn
  const std::string used = tip("u-2-1");
  EXPECT_EQ(used.find("omitted"), std::string::npos);
  EXPECT_EQ(std::count(used.begin(), used.end(), '\n'), 1);

  const auto* m = points_labelled(*scene, "Monitor means");
  const auto& p1 = hole_of(fit, 1);
  EXPECT_EQ(m->tooltips[0], "hole 1 \xC2\xB7 61\nn " + std::to_string(p1.n) + " \xC2\xB7 mean J " +
                                pp::flux_j_text(*p1.mean_j) + " \xC2\xB1 " + pp::flux_j_text(*p1.mean_j_err) +
                                "\nMSWD " + pp::flux_pct_text(*p1.mean_j_mswd));
  const auto* u = points_labelled(*scene, "Unknowns");
  const auto& u0 = hole_of(fit, 101);
  EXPECT_EQ(u->tooltips[0], "hole 101 \xC2\xB7 7101\npredicted J " + pp::flux_j_text(u0.j) + " \xC2\xB1 " +
                                pp::flux_j_text(u0.j_err));
}

TEST(FluxScene, UnknownTooltipShowsTheDeviationFromASavedJ) {
  auto in = level();
  in.positions.back().saved = pp::SavedFlux{};
  in.positions.back().saved->j = 1.0e-3;
  const auto fit = fit_or_die(in, options_of(r::ModelKind::Plane));
  const auto& p = fit.positions.back();
  ASSERT_TRUE(p.dev_percent);
  const auto scene = pp::flux_scene(in, fit);
  const auto* u = points_labelled(*scene, "Unknowns");
  EXPECT_NE(u->tooltips.back().find("\ndev " + pp::flux_pct_text(*p.dev_percent) + " %"), std::string::npos)
      << u->tooltips.back();
  EXPECT_EQ(u->tooltips.front().find("dev"), std::string::npos);
}

TEST(FluxScene, AnAnalysisTheMeanRefusedIsDrawnExcludedAndSaysWhy) {
  auto in = level();
  auto& an = in.positions[0].analyses[0];  // M1-01: F without an error, so J without one
  an.f = r::UFloat::variable(an.f->nominal(), 0.0);
  auto o = options_of(r::ModelKind::Plane);
  o.mean = r::MeanKind::Weighted;
  const auto fit = fit_or_die(in, o);
  ASSERT_EQ(hole_of(fit, 1).analyses[0].state, pp::AnalysisState::NoJ);
  ASSERT_TRUE(hole_of(fit, 1).analyses[0].j);
  const auto scene = pp::flux_scene(in, fit);
  const auto* a = points_labelled(*scene, "Analyses");
  ASSERT_EQ(a->x.size(), 24u);
  for (std::size_t i = 0; i < a->refs.size(); ++i)
    if (a->refs[i].analysis == "u-1-1") {
      EXPECT_TRUE(a->excluded[i]);
      EXPECT_NE(a->tooltips[i].find("\nnot used: J has no error"), std::string::npos) << a->tooltips[i];
      EXPECT_EQ(a->tooltips[i].find("no J"), std::string::npos);
    }
  // F that gives no J at all is not drawn.
  in.positions[0].analyses[1].f = r::UFloat::variable(0.0, 0.0);
  const auto s2 = pp::flux_scene(in, fit_or_die(in, o));
  EXPECT_EQ(points_labelled(*s2, "Analyses")->x.size(), 23u);
}

namespace {
// A fit made by hand: monitors used with a mean J, at the given places.
pp::LevelFit hand_monitors(r::ModelKind kind, const std::vector<std::pair<double, double>>& at) {
  pp::LevelFit fit;
  fit.options = options_of(kind);
  fit.options.fit.axis = r::Axis::X;
  int hole = 1;
  for (const auto& [x, y] : at) {
    pp::FittedPosition p;
    p.hole = hole++;
    p.identifier = "id";
    p.x = x;
    p.y = y;
    p.monitor = true;
    p.used_in_fit = true;
    p.n = 3;
    p.mean_j = 1.0e-3 + 1e-6 * p.hole;
    p.mean_j_err = 1e-7;
    p.j = *p.mean_j;
    p.j_err = 1e-7;
    p.analyses = {};
    fit.positions.push_back(p);
  }
  return fit;
}
}  // namespace

TEST(FluxScene, NoCurveWhenTheModelCannotBeEvaluatedAlongIt) {
  // A plane needs four monitors; three used: the data is there, the curve is not.
  const auto fit = hand_monitors(r::ModelKind::Plane, {{10, 0}, {0, 10}, {-10, 0}});
  const auto scene = pp::flux_scene(pp::LevelInputs{}, fit);
  EXPECT_EQ(first_of<pp::LineLayer>(*scene), nullptr);
  EXPECT_EQ(first_of<pp::BandLayer>(*scene), nullptr);
  EXPECT_EQ(points_labelled(*scene, "Monitor means")->x.size(), 3u);
  EXPECT_NE(points_labelled(*scene, "Analyses"), nullptr);
  EXPECT_NE(points_labelled(*scene, "Unknowns"), nullptr);
}

TEST(FluxScene, NoCurveWhenEveryPositionIsAtOneCoordinate) {
  auto fit = hand_monitors(r::ModelKind::LeastSquares1D, {{5, 0}, {5, 3}, {5, 6}, {5, 9}});
  fit.options.fit.axis = r::Axis::X;
  const auto scene = pp::flux_scene(pp::LevelInputs{}, fit);
  EXPECT_EQ(first_of<pp::LineLayer>(*scene), nullptr);
  EXPECT_EQ(first_of<pp::BandLayer>(*scene), nullptr);
  EXPECT_EQ(points_labelled(*scene, "Monitor means")->x.size(), 4u);
}
