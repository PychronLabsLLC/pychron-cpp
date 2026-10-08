#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

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
