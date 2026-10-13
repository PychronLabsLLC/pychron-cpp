// The names an analysis gives a record's series and intercepts
// (record/names.hpp): what the processing library reads and the store keeps.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "pychron/experiment/record/names.hpp"

using namespace pychron::experiment::record;

namespace {

Trace trace(std::vector<float> t, std::vector<float> v, std::vector<float> sigma = {}) {
  return Trace{std::move(t), std::move(v), std::move(sigma)};
}

std::vector<std::string> keys(const std::vector<NamedSeries>& series) {
  std::vector<std::string> out;
  out.reserve(series.size());
  for (const auto& s : series) out.push_back(s.kind + " " + s.key + " on " + s.detector);
  return out;
}

}  // namespace

TEST(RecordNames, AnOrdinaryRunKeepsItsPlainNames) {
  Data d;
  d.series = {{"Ar40", "H1", "signal", trace({0, 1}, {5, 4})},
              {"Ar36", "CDD", "signal", trace({0, 1}, {1, 1})},
              {"Ar40", "H1", "sniff", trace({0}, {5})},
              {"", "H1", "baseline", trace({9}, {0.1f})},
              {"", "CDD", "baseline", trace({9}, {0.2f})}};
  const auto named = named_series(d);
  EXPECT_EQ(keys(named), (std::vector<std::string>{"signal Ar40 on H1", "signal Ar36 on CDD", "sniff Ar40 on H1",
                                                   "baseline H1 on H1", "baseline CDD on CDD"}));
  for (std::size_t i = 0; i < named.size(); ++i) EXPECT_EQ(named[i].trace, d.series[i].trace) << i;
  EXPECT_EQ(intercept_name(d, "Ar40"), (InterceptName{"Ar40", "Ar40", "H1"}));
  EXPECT_EQ(intercept_name(d, "Ar36"), (InterceptName{"Ar36", "Ar36", "CDD"}));
  EXPECT_FALSE(intercept_name(d, "Ar39").has_value());
}

// A detector intercalibration: the reference isotope hopped onto each Faraday.
TEST(RecordNames, AnIsotopeOnSeveralDetectorsIsNamedByDetector) {
  Data d;
  d.series = {{"Ar40", "AX", "signal", trace({1}, {5})},
              {"Ar40", "H1", "signal", trace({0}, {5})},
              {"Ar40", "L1", "signal", trace({2}, {5})},
              {"Ar36", "CDD", "signal", trace({0}, {1})},
              {"Ar40", "H1", "sniff", trace({0}, {5})},   // the sniff of the first hop
              {"Ar36", "CDD", "whiff", trace({0}, {1})}};
  EXPECT_EQ(keys(named_series(d)),
            (std::vector<std::string>{"signal AX:Ar40 on AX", "signal H1:Ar40 on H1", "signal L1:Ar40 on L1",
                                      "signal Ar36 on CDD", "sniff H1:Ar40 on H1", "whiff Ar36 on CDD"}));
  // The record's intercepts are under the acquisition's names.
  EXPECT_EQ(intercept_name(d, "Ar40:H1"), (InterceptName{"H1:Ar40", "Ar40", "H1"}));
  EXPECT_EQ(intercept_name(d, "Ar40:AX"), (InterceptName{"AX:Ar40", "Ar40", "AX"}));
  EXPECT_EQ(intercept_name(d, "Ar40:L1"), (InterceptName{"L1:Ar40", "Ar40", "L1"}));
  EXPECT_EQ(intercept_name(d, "Ar36"), (InterceptName{"Ar36", "Ar36", "CDD"}));
  EXPECT_FALSE(intercept_name(d, "Ar40:CDD").has_value());  // no such series
  EXPECT_EQ(detector_isotope_key("H1", "Ar40"), "H1:Ar40");
}

// A sniff of one isotope on two detectors has two names, whatever its signal.
TEST(RecordNames, ASeriesKindWithAnIsotopeOnSeveralDetectorsIsNamedByDetector) {
  Data d;
  d.series = {{"Ar40", "H1", "signal", trace({0}, {5})},
              {"Ar40", "H1", "sniff", trace({0}, {5})},
              {"Ar40", "AX", "sniff", trace({0}, {5})}};
  EXPECT_EQ(keys(named_series(d)),
            (std::vector<std::string>{"signal Ar40 on H1", "sniff H1:Ar40 on H1", "sniff AX:Ar40 on AX"}));
  EXPECT_EQ(intercept_name(d, "Ar40"), (InterceptName{"Ar40", "Ar40", "H1"}));
}

// A baseline block after the run and a baseline hop within it, on one detector.
TEST(RecordNames, ADetectorsBaselineSeriesAreOneInTheRecordsOrder) {
  Data d;
  d.series = {{"", "CDD", "baseline", trace({90, 91}, {0.3f, 0.4f})},      // the block
              {"", "H1", "baseline", trace({90, 91}, {0.01f, 0.02f})},
              {"Ar36", "CDD", "baseline", trace({10, 11}, {0.1f, 0.2f})},  // the hop
              {"Ar36", "CDD", "signal", trace({0}, {1})}};
  const auto named = named_series(d);
  ASSERT_EQ(keys(named),
            (std::vector<std::string>{"baseline CDD on CDD", "baseline H1 on H1", "signal Ar36 on CDD"}));
  EXPECT_EQ(named[0].trace, trace({90, 91, 10, 11}, {0.3f, 0.4f, 0.1f, 0.2f}));  // not sorted: as it was fitted
  EXPECT_EQ(named[1].trace, d.series[1].trace);
}

TEST(RecordNames, MergedBaselinesKeepTheSigmasThereAre) {
  Data d;
  d.series = {{"", "CDD", "baseline", trace({0, 1}, {1, 2}, {0.1f, 0.2f})},
              {"Ar36", "CDD", "baseline", trace({2}, {3}, {0.3f})}};
  auto named = named_series(d);
  ASSERT_EQ(named.size(), 1u);
  EXPECT_EQ(named[0].trace, trace({0, 1, 2}, {1, 2, 3}, {0.1f, 0.2f, 0.3f}));

  // Only one of the two was measured with a sigma: the other's points have none.
  d.series[0].trace.sigma.clear();
  named = named_series(d);
  ASSERT_EQ(named.size(), 1u);
  ASSERT_EQ(named[0].trace.sigma.size(), 3u);
  EXPECT_TRUE(std::isnan(named[0].trace.sigma[0]));
  EXPECT_TRUE(std::isnan(named[0].trace.sigma[1]));
  EXPECT_EQ(named[0].trace.sigma[2], 0.3f);

  d.series[0].trace.sigma = {0.1f, 0.2f};
  d.series[1].trace.sigma.clear();
  named = named_series(d);
  ASSERT_EQ(named[0].trace.sigma.size(), 3u);
  EXPECT_EQ(named[0].trace.sigma[1], 0.2f);
  EXPECT_TRUE(std::isnan(named[0].trace.sigma[2]));
}

// Hand-made: an intercept whose isotope has no signal series.
TEST(RecordNames, AnInterceptWithNoSignalSeriesTakesTheLowestDetectorOfItsOthers) {
  Data d;
  d.series = {{"Ar37", "L1", "sniff", trace({0}, {1})}, {"Ar37", "H2", "whiff", trace({0}, {1})}};
  EXPECT_EQ(intercept_name(d, "Ar37"), (InterceptName{"Ar37", "Ar37", "H2"}));
}
