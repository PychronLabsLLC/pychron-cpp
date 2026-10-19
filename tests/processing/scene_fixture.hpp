#pragma once

// A scene of every layer kind the release figures use, built by hand so the
// committed SVG and PDF fixtures (fixtures/scene/) change only when a writer
// does, not when a figure builder does.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "pychron/core/env.hpp"
#include "pychron/processing/scene.hpp"

namespace pychron::processing::testing {

inline Scene fixture_scene() {
  Scene s;
  s.kind = "fixture";
  s.columns = 2;

  Graph ideo;
  ideo.title = "FC-1 <A&B> \"test\"";
  ideo.x.title = "Age (Ma)";
  Panel curve;
  curve.id = "p0";
  curve.y.title = "Relative probability";
  curve.y.min = 0.0;
  curve.height = 2.0;
  BandLayer band;
  band.x = {27.5, 28.0, 28.5, 29.0};
  band.low = {0.0, 0.1, 0.1, 0.0};
  band.high = {0.2, 0.9, 0.8, 0.1};
  band.fill = {31, 119, 180, 60};
  LineLayer line;
  line.x = {27.5, 28.0, 28.5, 29.0};
  line.y = {0.1, 0.5, 0.45, 0.05};
  line.style = {{31, 119, 180, 255}, 1.5, LineDash::Solid};
  GuideLayer mean;
  mean.horizontal = false;
  mean.value = 28.2;
  mean.style = {{214, 39, 40, 255}, 1.0, LineDash::Dash};
  TextLayer stats;
  stats.lines = {"Mean 28.20 \xc2\xb1 0.05 Ma (2\xcf\x83)", "MSWD 1.1  n 4/4", "caf\xc3\xa9 (x) \\ y"};
  curve.layers = {band, line, mean, stats};
  Panel values;
  values.id = "p1";
  values.y.title = "Analysis #";
  PointLayer pts;
  pts.x = {27.9, 28.1, 28.3, 28.6};
  pts.y = {1, 2, 3, 4};
  pts.x_err = {0.1, 0.12, 0.08, 0.2};
  pts.excluded = {false, false, false, true};
  pts.marker = {MarkerShape::Circle, 6.0, {31, 119, 180, 255}, true};
  pts.excluded_marker = {MarkerShape::Cross, 6.0, {150, 150, 150, 255}, false};
  pts.label = "FC-1";
  values.layers = {pts};
  ideo.panels = {curve, values};

  Graph spec;
  spec.title = "Age spectrum";
  spec.x.title = "Cumulative % 39ArK";
  spec.x.min = 0.0;
  spec.x.max = 100.0;
  Panel steps_panel;
  steps_panel.id = "p0";
  steps_panel.y.title = "Age (Ma)";
  steps_panel.y.min = 27.0;
  steps_panel.y.max = 29.5;
  SpanLayer plateau;
  plateau.x0 = 20.0;
  plateau.x1 = 90.0;
  plateau.fill = {44, 160, 44, 40};
  plateau.label = "plateau";
  StepLayer steps;
  steps.x0 = {0, 20, 45, 70, 90};
  steps.x1 = {20, 45, 70, 90, 100};
  steps.y = {27.6, 28.2, 28.25, 28.15, 28.9};
  steps.y_err = {0.3, 0.1, 0.08, 0.12, 0.4};
  steps.excluded = {false, false, false, false, true};
  steps.highlighted = {false, true, true, true, false};
  steps.labels = {"A", "B", "C", "D", "E"};
  steps.fill = {31, 119, 180, 120};
  steps.line = {31, 119, 180, 255};
  steps.dim_others = true;
  steps_panel.layers = {plateau, steps};
  spec.panels = {steps_panel};

  Graph iso;
  iso.title = "Inverse isochron";
  iso.x.title = "39Ar/40Ar";
  iso.x.min = 0.0;
  iso.x.max = 0.1;
  Panel ellipses;
  ellipses.id = "p0";
  ellipses.y.title = "36Ar/40Ar";
  ellipses.y.min = 0.0;
  ellipses.y.max = 0.004;
  EllipseLayer el;
  el.x = {0.02, 0.05, 0.08};
  el.y = {0.0026, 0.0017, 0.0007};
  el.sx = {0.002, 0.002, 0.003};
  el.sy = {0.0001, 0.00012, 0.0001};
  el.rho = {0.4, -0.2, 0.9};
  el.scale = 2.4477;
  el.line = {214, 39, 40, 255};
  el.fill = {214, 39, 40, 50};
  el.filled = true;
  LineLayer fit;
  fit.x = {0.0, 0.1};
  fit.y = {0.003384, 0.0};
  fit.style = {{0, 0, 0, 255}, 1.0, LineDash::DashDot};
  PointLayer ticks;
  ticks.x = {0.0};
  ticks.y = {0.003384};
  ticks.marker = {MarkerShape::Diamond, 7.0, {0, 0, 0, 255}, false};
  PointLayer shapes;
  shapes.x = {0.03, 0.04, 0.06, 0.07};
  shapes.y = {0.0035, 0.0035, 0.0035, 0.0035};
  shapes.marker = {MarkerShape::Square, 5.0, {148, 103, 189, 255}, true};
  PointLayer more = shapes;
  more.y = {0.0032, 0.0032, 0.0032, 0.0032};
  more.marker = {MarkerShape::Triangle, 5.0, {140, 86, 75, 255}, true};
  PointLayer stars = shapes;
  stars.y = {0.0029, 0.0029, 0.0029, 0.0029};
  stars.marker = {MarkerShape::Star, 5.0, {0, 0, 128, 255}, false};
  PointLayer plus = shapes;
  plus.y = {0.0011, 0.0011, 0.0011, 0.0011};
  plus.marker = {MarkerShape::Plus, 5.0, {0, 128, 128, 255}, false};
  TextLayer age;
  age.corner = Corner::BottomLeft;
  age.lines = {"Age 28.19 \xc2\xb1 0.07 Ma"};
  ellipses.layers = {el, fit, ticks, shapes, more, stars, plus, age};
  iso.panels = {ellipses};

  s.graphs = {ideo, spec, iso};
  return s;
}

// The committed fixture `name` under fixtures/scene/, compared with `bytes`.
// With PYCHRON_WRITE_FIXTURES=1 the file is written instead and the result
// says so: the test then fails, so a rewrite is never silent.
inline std::string compare_with_fixture(const std::string& name, const std::string& bytes) {
  const std::filesystem::path path = std::filesystem::path(PYCHRON_PROCESSING_FIXTURE_DIR) / "scene" / name;
  if (const auto write = env_var("PYCHRON_WRITE_FIXTURES"); write && *write == "1") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    return "fixture written: " + path.string();
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return "no fixture " + path.string() + " (PYCHRON_WRITE_FIXTURES=1 writes it)";
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string want = ss.str();
  if (want == bytes) return {};
  std::size_t at = 0;
  while (at < want.size() && at < bytes.size() && want[at] == bytes[at]) ++at;
  return name + " differs from the fixture at byte " + std::to_string(at) + " (fixture " +
         std::to_string(want.size()) + " bytes, written " + std::to_string(bytes.size()) + ")";
}

}  // namespace pychron::processing::testing
