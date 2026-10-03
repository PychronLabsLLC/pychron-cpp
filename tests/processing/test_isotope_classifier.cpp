// The isotope classifier (legacy IsotopeClassifier): samples from sniffs in
// tenth-second bins, a 3-nearest-neighbour vote, TOML training files, and
// its use (and AUTO_N fits) in batch refits.

#include <gtest/gtest.h>

#include <filesystem>
#include <random>

#include "fixtures.hpp"
#include "pychron/processing/isotope_classifier.hpp"
#include "pychron/processing/isotope_evolution_fit.hpp"

namespace pychron::processing {
namespace {

namespace r = pychron::reduction;
namespace fs = std::filesystem;

RawData with_sniff(const std::string& key, std::vector<double> t, std::vector<double> v) {
  RawData raw;
  RawSeries s;
  s.kind = SeriesKind::Sniff;
  s.key = key;
  s.t = std::move(t);
  s.v = std::move(v);
  raw.series.push_back(s);
  return raw;
}

IsotopeSample sample(int klass, double level) {
  IsotopeSample s;
  s.klass = klass;
  s.mass = 39.9624;
  for (int b = 1; b <= 5; ++b) s.bins[b] = level + 0.01 * b;
  return s;
}

TEST(IsotopeClassifier, SamplesFollowLegacyBins) {
  EXPECT_EQ(isotope_mass("Ar40"), 39.9624);
  EXPECT_EQ(isotope_mass("Ar36"), 35.9675);
  EXPECT_FALSE(isotope_mass("Kr84"));
  // digitize(10 t, arange(12000)) = floor(10 t) + 1; past 1200 s is dropped,
  // a zero value leaves its bin empty.
  auto s = make_isotope_sample(with_sniff("Ar40", {0.0, 0.25, 2.55, 3.0, 1200.1}, {1, 2, 3, 0, 4}), "Ar40", "Ar40");
  EXPECT_EQ(s.mass, 39.9624);
  EXPECT_EQ(s.isotope, "Ar40");
  EXPECT_EQ(s.bins, (std::map<int, double>{{1, 1.0}, {3, 2.0}, {26, 3.0}}));
  auto none = make_isotope_sample(RawData{}, "H1:Ar39", "Ar39");
  EXPECT_TRUE(none.bins.empty());
  EXPECT_EQ(none.mass, 38.964);
}

TEST(IsotopeClassifier, DistanceIsEuclideanOverSparseBins) {
  IsotopeSample a, b;
  a.mass = 40;
  b.mass = 36;
  a.bins = {{1, 3.0}, {5, 1.0}};
  b.bins = {{1, 0.0}, {7, 2.0}};
  EXPECT_DOUBLE_EQ(sample_distance(a, b), std::sqrt(16.0 + 9.0 + 1.0 + 4.0));
  EXPECT_DOUBLE_EQ(sample_distance(a, a), 0.0);
}

TEST(IsotopeClassifier, NearestNeighbourVote) {
  IsotopeClassifier c;
  EXPECT_FALSE(c.classify(sample(1, 1.0)));
  for (int i = 0; i < 3; ++i) c.add(sample(1, 1.0 + 0.1 * i));
  for (int i = 0; i < 3; ++i) c.add(sample(0, 100.0 + i));
  auto good = c.classify(sample(1, 1.05));
  ASSERT_TRUE(good);
  EXPECT_EQ(good->klass, 1);
  EXPECT_DOUBLE_EQ(good->probability, 1.0);
  auto bad = c.classify(sample(1, 99.0));
  ASSERT_TRUE(bad);
  EXPECT_EQ(bad->klass, 0);
  // Between them: two good neighbours and one bad.
  IsotopeClassifier mixed;
  mixed.add(sample(1, 1.0));
  mixed.add(sample(1, 2.0));
  mixed.add(sample(0, 3.0));
  mixed.add(sample(0, 50.0));
  auto vote = mixed.classify(sample(1, 2.4));
  ASSERT_TRUE(vote);
  EXPECT_EQ(vote->klass, 1);
  EXPECT_NEAR(vote->probability, 2.0 / 3.0, 1e-12);
  // Fewer samples than k: k = n; a tie goes to the smaller class.
  IsotopeClassifier two;
  two.add(sample(1, 1.0));
  two.add(sample(0, 2.0));
  auto tie = two.classify(sample(1, 1.5));
  ASSERT_TRUE(tie);
  EXPECT_EQ(tie->klass, 0);
  EXPECT_DOUBLE_EQ(tie->probability, 0.5);
}

TEST(IsotopeClassifier, TomlRoundTripAndFiles) {
  IsotopeClassifier c;
  auto s = sample(0, 5.0);
  s.runid = "66001-01";
  s.isotope = "Ar40";
  c.add(s);
  c.add(sample(1, 1.0));
  auto back = IsotopeClassifier::from_toml(c.to_toml());
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(back->samples(), c.samples());
  EXPECT_FALSE(IsotopeClassifier::from_toml("[[sample]]\nklass = 2\n"));
  EXPECT_FALSE(IsotopeClassifier::from_toml("[[sample]]\nklass = 1\nbins = [1, 2]\nvalues = [1.0]\n"));
  EXPECT_FALSE(IsotopeClassifier::from_toml("[[sample]]\nklass = 1\nbins = [12000]\nvalues = [1.0]\n"));
  EXPECT_FALSE(IsotopeClassifier::from_toml("not toml ="));

  const fs::path dir = fs::temp_directory_path() / ("pychron_classifier_" + std::to_string(std::random_device{}()));
  const fs::path file = dir / "nested" / "isotope.toml";
  auto missing = IsotopeClassifier::load(file);
  ASSERT_TRUE(missing);
  EXPECT_TRUE(missing->empty());
  ASSERT_TRUE(c.save(file));
  auto loaded = IsotopeClassifier::load(file);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->samples(), c.samples());
  fs::remove_all(dir);
}

// Refits of make_air analyses whose Ar40 signal is a 20-point line, with a
// sniff that is flat (good) or wildly noisy (bad).
struct RefitFixture {
  Dataset dataset;
  std::map<std::string, RawData> raw;
  RawLoader loader() const {
    return [this](const std::string& uuid) -> Result<RawData> { return raw.at(uuid); };
  }
};

RefitFixture refit_fixture(bool noisy_sniff) {
  RefitFixture f;
  auto a = test::make_air(0);
  RawData raw;
  RawSeries sig;
  sig.kind = SeriesKind::Signal;
  sig.key = "Ar40";
  for (int k = 0; k < 20; ++k) {
    sig.t.push_back(k);
    sig.v.push_back(100.0 - k + (k % 2 ? 0.1 : -0.1));
  }
  raw.series.push_back(sig);
  RawSeries sniff;
  sniff.kind = SeriesKind::Sniff;
  sniff.key = "Ar40";
  for (int k = 0; k < 10; ++k) {
    sniff.t.push_back(k * 0.5);
    sniff.v.push_back(noisy_sniff ? (k % 2 ? 500.0 : -500.0) : 100.0);
  }
  raw.series.push_back(sniff);
  f.raw[a->uuid] = raw;
  f.dataset.mutable_items().push_back(DatasetItem{reduce_analysis(a, {}), {}, {}});
  return f;
}

Options one_row(std::function<void(Options&)> edit) {
  Options o(isotope_evolution_fit_schema());
  auto rows = o.rows("isotopes");
  rows.resize(1);
  edit(rows[0]);
  EXPECT_TRUE(o.set_rows("isotopes", rows));
  return o;
}

TEST(IsotopeEvolutionFits, AutoNPicksTheFitByPointCount) {
  auto f = refit_fixture(false);
  auto with = [&](std::int64_t threshold) {
    auto fig = build_isotope_evolution_fits(f.dataset, one_row([&](Options& row) {
                                              (void)row.set("fit", std::string("auto_n"));
                                              (void)row.set("n_threshold", threshold);
                                              (void)row.set("n_true", std::string("parabolic"));
                                              (void)row.set("n_false", std::string("average"));
                                            }),
                                            f.loader());
    EXPECT_TRUE(fig);
    return fig->fits;
  };
  const auto many = with(20);  // 20 points >= 20
  ASSERT_EQ(many.analyses.size(), 1u);
  EXPECT_EQ(many.analyses[0].isotopes[0].fit.fit.kind, r::FitKind::Parabolic);
  EXPECT_EQ(many.message(), "<ISOEVO> refit Ar40(parabolic)");
  const auto few = with(21);
  EXPECT_EQ(few.analyses[0].isotopes[0].fit.fit.kind, r::FitKind::Average);
}

TEST(IsotopeEvolutionFits, ClassifierFlagsBadSniffs) {
  // Train on one good and one bad flat-vs-noisy sniff of each kind.
  const fs::path file =
      fs::temp_directory_path() / ("pychron_iso_classifier_" + std::to_string(std::random_device{}()) + ".toml");
  IsotopeClassifier c;
  for (bool noisy : {false, false, true, true}) {
    auto f = refit_fixture(noisy);
    auto s = make_isotope_sample(f.raw.begin()->second, "Ar40", "Ar40");
    s.klass = noisy ? 0 : 1;
    c.add(s);
  }
  ASSERT_TRUE(c.save(file));
  auto options = [&](bool use) {
    Options o = one_row([&](Options& row) { (void)row.set("isotope", std::string("Ar40")); });
    EXPECT_TRUE(o.set("use_classifier", use));
    EXPECT_TRUE(o.set("classifier_file", file.string()));
    return o;
  };
  for (bool noisy : {false, true}) {
    auto f = refit_fixture(noisy);
    Options o = options(true);
    auto fig = build_isotope_evolution_fits(f.dataset, o, f.loader());
    ASSERT_TRUE(fig) << fig.error().what;
    const auto& refit = fig->fits.analyses.at(0);
    ASSERT_TRUE(refit.isotopes[0].classification);
    EXPECT_EQ(refit.isotopes[0].classification->klass, noisy ? 0 : 1);
    EXPECT_EQ(refit.good(), !noisy);
    if (noisy) {
      EXPECT_EQ(refit.flags.at(0).check, "classifier");
      EXPECT_NEAR(refit.flags.at(0).value, 2.0 / 3.0, 1e-12);
    }
  }
  // Off: no classification. Untrained: a warning.
  auto f = refit_fixture(true);
  auto off = build_isotope_evolution_fits(f.dataset, options(false), f.loader());
  ASSERT_TRUE(off);
  EXPECT_FALSE(off->fits.analyses.at(0).isotopes[0].classification);
  Options empty = options(true);
  ASSERT_TRUE(empty.set("classifier_file", (file.string() + ".missing")));
  auto untrained = build_isotope_evolution_fits(f.dataset, empty, f.loader());
  ASSERT_TRUE(untrained);
  EXPECT_EQ(untrained->fits.warnings.at(0), "classifier: no training samples yet");
  fs::remove(file);
}

}  // namespace
}  // namespace pychron::processing
