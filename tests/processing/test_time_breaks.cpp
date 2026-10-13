#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vector>

#include "pychron/processing/time_breaks.hpp"

namespace pp = pychron::processing;

namespace {

constexpr double kHour = 3600.0;

pp::AnalysisSummary run(const char* spectrometer, double hours) {
  pp::AnalysisSummary s;
  s.mass_spectrometer = spectrometer;
  s.timestamp = 1'700'000'000.0 + hours * kHour;
  return s;
}

double at(double hours) { return 1'700'000'000.0 + hours * kHour; }

}  // namespace

TEST(TimeBreaks, NothingToBreak) {
  EXPECT_TRUE(pp::time_breaks({}, kHour).empty());
  const std::vector<pp::AnalysisSummary> one{run("jan", 0)};
  EXPECT_TRUE(pp::time_breaks(one, kHour).empty());
}

TEST(TimeBreaks, OnlyAGapLongerThanTheThresholdBreaks) {
  // Newest first: 10 h, 9 h, 7 h (2 h gap), 1 h (6 h gap).
  const std::vector<pp::AnalysisSummary> rows{run("jan", 10), run("jan", 9), run("jan", 7), run("jan", 1)};
  EXPECT_TRUE(pp::time_breaks(rows, 6 * kHour).empty());  // equal to the threshold: no break
  const auto breaks = pp::time_breaks(rows, 6 * kHour - 1);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_EQ(breaks[0].above, 3u);
  EXPECT_DOUBLE_EQ(breaks[0].gap_seconds, 6 * kHour);
  EXPECT_EQ(breaks[0].session_runs, 3u);
  EXPECT_DOUBLE_EQ(breaks[0].session_start, at(7));
  EXPECT_DOUBLE_EQ(breaks[0].session_end, at(10));
}

TEST(TimeBreaks, ASessionCountsBackToThePreviousBreak) {
  const std::vector<pp::AnalysisSummary> rows{run("jan", 30), run("jan", 20), run("jan", 19), run("jan", 5)};
  const auto breaks = pp::time_breaks(rows, 4 * kHour);
  ASSERT_EQ(breaks.size(), 2u);
  EXPECT_EQ(breaks[0].above, 1u);
  EXPECT_EQ(breaks[0].session_runs, 1u);
  EXPECT_EQ(breaks[1].above, 3u);
  EXPECT_EQ(breaks[1].session_runs, 2u);
  EXPECT_DOUBLE_EQ(breaks[1].session_start, at(19));
  EXPECT_DOUBLE_EQ(breaks[1].session_end, at(20));
}

TEST(TimeBreaks, ASpectrometerNeitherMakesNorHidesAnothersBreak) {
  // obama runs every hour; jan stops between 2 h and 9 h.
  const std::vector<pp::AnalysisSummary> rows{run("obama", 10), run("jan", 9),   run("obama", 8), run("obama", 6),
                                              run("obama", 4),  run("jan", 2),   run("obama", 2), run("jan", 1)};
  const auto breaks = pp::time_breaks(rows, 3 * kHour);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_EQ(breaks[0].above, 5u);  // above jan's run at 2 h
  EXPECT_DOUBLE_EQ(breaks[0].gap_seconds, 7 * kHour);
  EXPECT_EQ(breaks[0].session_runs, 1u);
}

TEST(TimeBreaks, NoThresholdNoBreaks) {
  const std::vector<pp::AnalysisSummary> rows{run("jan", 100), run("jan", 0)};
  EXPECT_TRUE(pp::time_breaks(rows, 0.0).empty());
  EXPECT_TRUE(pp::time_breaks(rows, -1.0).empty());
}

TEST(TimeBreaks, EqualTimestampsAreNoBreak) {
  const std::vector<pp::AnalysisSummary> rows{run("jan", 5), run("jan", 5), run("jan", 5)};
  EXPECT_TRUE(pp::time_breaks(rows, 1.0).empty());
}

// What the browser's paging relies on: older rows appended never change the
// breaks among the rows already there.
TEST(TimeBreaks, TheBreaksOfAPrefixAreThoseOfTheWholeThatLieInIt) {
  const std::array<double, 12> hours{90, 89, 80, 79, 78, 60, 59, 40, 39, 38, 10, 9};
  std::vector<pp::AnalysisSummary> rows;
  rows.reserve(hours.size());
  for (std::size_t i = 0; i < hours.size(); ++i) rows.push_back(run(i % 3 == 0 ? "obama" : "jan", hours[i]));
  const auto all = pp::time_breaks(rows, 5 * kHour);
  ASSERT_FALSE(all.empty());
  for (std::size_t k = 0; k <= rows.size(); ++k) {
    std::vector<pp::TimeBreak> expected;
    for (const auto& b : all)
      if (b.above < k) expected.push_back(b);
    EXPECT_EQ(pp::time_breaks(std::span(rows).first(k), 5 * kHour), expected) << "k = " << k;
  }
}

TEST(TimeBreaks, GapText) {
  EXPECT_EQ(pp::gap_text(45 * 60), "45 min");
  EXPECT_EQ(pp::gap_text(14 * kHour + 20 * 60), "14 h 20 min");
  EXPECT_EQ(pp::gap_text(6 * kHour), "6 h");
  EXPECT_EQ(pp::gap_text(76 * kHour + 10 * 60), "3 d 4 h");
  EXPECT_EQ(pp::gap_text(48 * kHour), "2 d");
  EXPECT_EQ(pp::gap_text(20), "0 min");
}
