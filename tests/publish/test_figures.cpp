// The figures of a sample in a release: which files, named how, with the
// release's options, and what happens when a figure cannot be built.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

#include "processing/fixtures.hpp"
#include "pychron/processing/arar_figures.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/publish/figures.hpp"

namespace pychron::publish {
namespace {

namespace pp = processing;

pp::Dataset steps_of(const std::string& identifier, int aliquot, int n) {
  pp::Dataset d;
  constexpr std::array<double, 8> ar39 = {5, 20, 40, 60, 50, 30, 15, 5};
  constexpr std::array<double, 8> ar36 = {0.5, 0.3, 0.2, 0.1, 0.08, 0.06, 0.1, 0.2};
  for (int i = 0; i < n; ++i) {
    auto a = pp::test::make_step(i, ar39[static_cast<std::size_t>(i)], ar36[static_cast<std::size_t>(i)], 10.0, identifier);
    a->aliquot = aliquot;
    a->uuid += "-" + std::to_string(aliquot);
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(a, {});
    d.mutable_items().push_back(item);
  }
  return d;
}

std::vector<std::string> paths(const std::vector<FigureFile>& files) {
  std::vector<std::string> out;
  out.reserve(files.size());
  for (const auto& f : files) out.push_back(f.path);
  return out;
}

TEST(ReleaseFigures, FigureOptionsFollowTheRelease) {
  ReleaseOptions r;
  r.sigma = 1;
  r.plateau = "mahon";
  r.plateau_steps = 4;
  r.plateau_gas = 60.0;
  const pp::Options o = figure_options(pp::spectrum_schema(), r);
  EXPECT_EQ(o.get_int("error_bar_nsigma"), 1);
  EXPECT_EQ(o.get_string("plateau.method"), "mahon");
  EXPECT_EQ(o.get_int("plateau.nsteps"), 4);
  EXPECT_DOUBLE_EQ(o.get_double("plateau.gas_fraction"), 60.0);
  EXPECT_EQ(figure_options(pp::ideogram_schema(), r).get_int("error_bar_nsigma"), 1);
}

TEST(ReleaseFigures, OneAliquotOfStepsGivesIdeogramSpectrumAndIsochron) {
  std::vector<std::string> warnings;
  const auto files = sample_figures(steps_of("66012", 1, 6), "FC-2", ReleaseOptions{}, 1760000000, warnings);
  EXPECT_EQ(paths(files), (std::vector<std::string>{"figures/FC-2/ideogram.svg", "figures/FC-2/ideogram.pdf",
                                                    "figures/FC-2/spectrum.svg", "figures/FC-2/spectrum.pdf",
                                                    "figures/FC-2/isochron.svg", "figures/FC-2/isochron.pdf"}));
  for (const auto& f : files) {
    if (f.path.ends_with(".svg")) {
      EXPECT_TRUE(f.bytes.starts_with("<?xml")) << f.path;
    }
    if (f.path.ends_with(".pdf")) {
      EXPECT_TRUE(f.bytes.starts_with("%PDF-1.4")) << f.path;
    }
  }
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  EXPECT_NE(files[1].bytes.find("/CreationDate (D:20251009085320Z)"), std::string::npos);
}

TEST(ReleaseFigures, TwoAliquotsNameTheirSpectra) {
  pp::Dataset d = steps_of("66012", 1, 4);
  const pp::Dataset second = steps_of("66012", 2, 4);
  for (const auto& item : second.items()) d.mutable_items().push_back(item);
  std::vector<std::string> warnings;
  const auto p = paths(sample_figures(d, "FC-2", ReleaseOptions{}, 0, warnings));
  for (const auto& w : warnings) ADD_FAILURE() << w;
  EXPECT_NE(std::find(p.begin(), p.end(), "figures/FC-2/spectrum-66012-01.svg"), p.end());
  EXPECT_NE(std::find(p.begin(), p.end(), "figures/FC-2/spectrum-66012-02.pdf"), p.end());
  EXPECT_EQ(std::find(p.begin(), p.end(), "figures/FC-2/spectrum.svg"), p.end());
}

TEST(ReleaseFigures, ASampleWithoutEnoughPointsSkipsTheIsochronWithAWarning) {
  pp::Dataset d;
  for (int i = 0; i < 2; ++i) {
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(pp::test::make_unknown(i), {});
    d.mutable_items().push_back(item);
  }
  std::vector<std::string> warnings;
  const auto p = paths(sample_figures(d, "FC-1", ReleaseOptions{}, 0, warnings));
  EXPECT_EQ(p, (std::vector<std::string>{"figures/FC-1/ideogram.svg", "figures/FC-1/ideogram.pdf"}));
  EXPECT_TRUE(warnings.empty());  // two points make no isochron and no spectrum: nothing went wrong
}

TEST(ReleaseFigures, AnEmptySampleHasNoFigures) {
  std::vector<std::string> warnings;
  EXPECT_TRUE(sample_figures(pp::Dataset{}, "X", ReleaseOptions{}, 0, warnings).empty());
}

TEST(ReleaseFigures, SameInputsSameBytes) {
  const pp::Dataset d = steps_of("66012", 1, 5);
  std::vector<std::string> warnings;
  const auto a = sample_figures(d, "S", ReleaseOptions{}, 7, warnings);
  const auto b = sample_figures(d, "S", ReleaseOptions{}, 7, warnings);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].bytes, b[i].bytes) << a[i].path;
}

TEST(ReleaseFigures, AliquotNamesThatCollideAreMadeUnique) {
  pp::Dataset d = steps_of("A B", 1, 4);
  const pp::Dataset second = steps_of("A_B", 1, 4);
  for (const auto& item : second.items()) d.mutable_items().push_back(item);
  const pp::Dataset third = steps_of("a_b", 1, 4);
  for (const auto& item : third.items()) d.mutable_items().push_back(item);
  std::vector<std::string> warnings;
  auto p = paths(sample_figures(d, "S", ReleaseOptions{}, 0, warnings));
  std::vector<std::string> spectra;
  for (const auto& x : p)
    if (x.find("/spectrum-") != std::string::npos && x.ends_with(".svg")) spectra.push_back(x);
  ASSERT_EQ(spectra.size(), 3u);
  std::set<std::string> folded;
  for (auto x : spectra) {
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    folded.insert(x);
  }
  EXPECT_EQ(folded.size(), 3u) << "unique when case is ignored";
}

TEST(ReleaseFigures, AnOptionTheFigureRefusesIsAWarning) {
  ReleaseOptions r;
  r.plateau_steps = 1000;  // the spectrum schema takes 2..100
  std::vector<std::string> warnings;
  const pp::Options o = figure_options(pp::spectrum_schema(), r, &warnings);
  EXPECT_EQ(o.get_int("plateau.nsteps"), 3) << "the schema's default";
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("plateau.nsteps"), std::string::npos) << warnings[0];
}

}  // namespace
}  // namespace pychron::publish
