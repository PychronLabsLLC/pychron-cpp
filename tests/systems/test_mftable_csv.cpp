#include "pychron/systems/spectrometer/mftable_csv.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace ps = pychron::spectrometer;
using pychron::ErrorKind;

TEST(MftableCsv, ReadsPychronLayoutWithMassesFromWeights) {
  auto t = ps::read_mftable_csv(
      "iso,H2,H1,AX\n"
      "Ar40,5.78,5.89,6.01\n"
      "Ar39,5.63,5.74,5.86\n"
      "\n"
      "Ar36,5.35,5.45,5.56\n",
      ps::MolecularWeights::defaults());
  ASSERT_TRUE(t.has_value()) << pychron::to_string(t.error());
  EXPECT_EQ(t->axis(), ps::TableAxis::Dac);
  EXPECT_EQ(t->default_fit(), ps::FitKind::Quadratic);
  ASSERT_EQ(t->points().size(), 3u);
  EXPECT_EQ(t->points()[0].isotope, "Ar40");
  EXPECT_NEAR(t->points()[0].mass, 39.9623831, 1e-6);
  EXPECT_DOUBLE_EQ(t->points()[2].values.at("AX"), 5.56);
  EXPECT_EQ(t->detectors().size(), 3u);
}

TEST(MftableCsv, FitRowSetsPerDetectorOverrides) {
  auto t = ps::read_mftable_csv(
      "iso,H1,CDD\n"
      "#,parabolic,linear\n"
      "Ar40,5.89,6.1\n"
      "Ar36,5.45,5.6\n",
      ps::MolecularWeights::defaults());
  ASSERT_TRUE(t.has_value()) << pychron::to_string(t.error());
  EXPECT_EQ(t->fit("H1"), ps::FitKind::Quadratic);
  EXPECT_EQ(t->fit("CDD"), ps::FitKind::Linear);
  EXPECT_EQ(t->fit_overrides().size(), 1u);
}

TEST(MftableCsv, MassColumnWinsOverWeights) {
  auto t = ps::read_mftable_csv("iso, mass ,H1\nFoo,12.5, 1.0\nAr40,40.0,5.0\n", ps::MolecularWeights{},
                                ps::TableAxis::Field);
  ASSERT_TRUE(t.has_value()) << pychron::to_string(t.error());
  EXPECT_DOUBLE_EQ(t->points()[0].mass, 12.5);
  EXPECT_DOUBLE_EQ(t->points()[0].values.at("H1"), 1.0);
  EXPECT_EQ(t->axis(), ps::TableAxis::Field);
}

TEST(MftableCsv, EmptyCellsLeaveDetectorAbsent) {
  auto t = ps::read_mftable_csv("iso,H1,AX\nAr40,5.8,\nAr36,5.4,5.5\n", ps::MolecularWeights::defaults());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->points()[0].values.count("AX"), 0u);
}

TEST(MftableCsv, HandlesCrlf) {
  auto t = ps::read_mftable_csv("iso,H1\r\nAr40,5.8\r\n", ps::MolecularWeights::defaults());
  ASSERT_TRUE(t.has_value());
  EXPECT_DOUBLE_EQ(t->points()[0].values.at("H1"), 5.8);
}

TEST(MftableCsv, UnknownIsotopeIsReportedNotGuessed) {
  auto t = ps::read_mftable_csv("iso,H1\nUnobtainium,5.8\n", ps::MolecularWeights::defaults());
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error().kind, ErrorKind::Config);
  EXPECT_NE(t.error().what.find("Unobtainium"), std::string::npos);
}

TEST(MftableCsv, MalformedInputsAreConfigErrors) {
  const auto& mw = ps::MolecularWeights::defaults();
  EXPECT_EQ(ps::read_mftable_csv("", mw).error().kind, ErrorKind::Config);
  EXPECT_EQ(ps::read_mftable_csv("iso\nAr40\n", mw).error().kind, ErrorKind::Config);           // no detectors
  EXPECT_EQ(ps::read_mftable_csv("iso,H1\nAr40,abc\n", mw).error().kind, ErrorKind::Config);    // bad number
  EXPECT_EQ(ps::read_mftable_csv("iso,H1\nAr40,1,2\n", mw).error().kind, ErrorKind::Config);    // too many cells
  EXPECT_EQ(ps::read_mftable_csv("iso,H1\n#,spline\n", mw).error().kind, ErrorKind::Config);    // bad fit
  EXPECT_EQ(ps::read_mftable_csv("iso,H1\nAr40,1\nAr40,2\n", mw).error().kind, ErrorKind::Config);  // duplicate
}

namespace {

std::string read_fixture(const std::string& name) {
  std::ifstream in(std::string(PYCHRON_SYSTEMS_TEST_DATA_DIR) + "/" + name, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

ps::FitKind first_line_fit(const std::string& fit_line) {
  auto t = ps::read_mftable_csv(fit_line + "\niso,H1\nAr40,5.8\nAr36,5.4\n", ps::MolecularWeights::defaults());
  EXPECT_TRUE(t.has_value()) << pychron::to_string(t.error());
  return t ? t->default_fit() : ps::FitKind::Quadratic;
}

}  // namespace

TEST(MftableCsv, FirstLineFitNameLinear) { EXPECT_EQ(first_line_fit("linear"), ps::FitKind::Linear); }

TEST(MftableCsv, FirstLineFitNameParabolic) { EXPECT_EQ(first_line_fit("parabolic"), ps::FitKind::Quadratic); }

TEST(MftableCsv, FirstLineFitNameCubic) { EXPECT_EQ(first_line_fit("cubic"), ps::FitKind::Cubic); }

TEST(MftableCsv, FirstLineFitNameOtherIsDiscrete) {
  EXPECT_EQ(first_line_fit("weird"), ps::FitKind::Discrete);
  // Only the first non-blank line can hold the fit name.
  EXPECT_EQ(first_line_fit("\n\r\n  linear  \r"), ps::FitKind::Linear);
}

TEST(MftableCsv, ReadsTheMelbourneFileWithItsH2Column) {
  const auto text = read_fixture("mftable_melbourne.csv");
  ASSERT_FALSE(text.empty());
  ASSERT_NE(text.find("\r\n"), std::string::npos);  // fixture keeps the lab's CRLF
  auto t = ps::read_mftable_csv(text, ps::MolecularWeights::defaults());
  ASSERT_TRUE(t.has_value()) << pychron::to_string(t.error());
  EXPECT_EQ(t->default_fit(), ps::FitKind::Quadratic);
  EXPECT_TRUE(t->fit_overrides().empty());
  ASSERT_EQ(t->points().size(), 3u);
  const auto detectors = t->detectors();
  EXPECT_NE(std::find(detectors.begin(), detectors.end(), "H2"), detectors.end());
  EXPECT_EQ(detectors.size(), 6u);
  EXPECT_EQ(t->points()[0].isotope, "Ar40");
  EXPECT_DOUBLE_EQ(t->points()[0].mass, *ps::MolecularWeights::defaults().mass("Ar40"));
  for (const auto& p : t->points()) {
    for (const auto& [det, v] : p.values) {
      EXPECT_GE(v, 5.35) << p.isotope << " " << det;
      EXPECT_LE(v, 6.36) << p.isotope << " " << det;
    }
  }
}

TEST(MftableCsv, HashFitRowStillRead) {
  auto t = ps::read_mftable_csv(
      "linear\n"
      "iso,H1,CDD\n"
      "#,parabolic,cubic\n"
      "Ar40,5.89,6.1\n"
      "Ar39,5.7,5.9\n"
      "Ar36,5.45,5.6\n",
      ps::MolecularWeights::defaults());
  ASSERT_TRUE(t.has_value()) << pychron::to_string(t.error());
  EXPECT_EQ(t->default_fit(), ps::FitKind::Linear);
  EXPECT_EQ(t->fit("H1"), ps::FitKind::Quadratic);
  EXPECT_EQ(t->fit("CDD"), ps::FitKind::Cubic);
  EXPECT_EQ(t->fit_overrides().size(), 2u);
}
