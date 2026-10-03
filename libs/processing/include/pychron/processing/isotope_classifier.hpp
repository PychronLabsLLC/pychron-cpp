#pragma once

// The isotope classifier (legacy pychron/classifier/isotope_classifier.py): a
// k-nearest-neighbours vote (k = 3, uniform weights, Euclidean distance, as
// sklearn's KNeighborsClassifier) on an isotope's sniff, trained by people
// marking isotopes good (1) or bad (0).
//
// A sample is legacy's make_sample: [mass, -9999, v_0 .. v_11999], where the
// sniff value measured at t seconds is placed at bin digitize(10 t) = floor(10 t)
// + 1 of 12000 tenth-second bins and every other bin is 0. Samples are kept
// sparse; distances treat missing bins as 0.
//
// Training samples are stored in a TOML file of [[sample]] tables (klass,
// mass, bins, values, and the run id and isotope they came from). Legacy's
// pickled sklearn models cannot be read; isotopes have to be marked again.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/model.hpp"

namespace pychron::processing {

inline constexpr int kClassifierBins = 1200 * 10;
inline constexpr double kClassifierPlaceholder = -9999.0;

// Legacy MOLECULAR_WEIGHTS (Ar36..Ar40, Ar41, Ar35, Ar33); nullopt for others.
std::optional<double> isotope_mass(std::string_view isotope);

struct IsotopeSample {
  int klass = 1;  // 1 good, 0 bad
  double mass = 0.0;
  std::map<int, double> bins;  // non-zero sniff bins
  std::string runid, isotope;  // provenance
  friend bool operator==(const IsotopeSample&, const IsotopeSample&) = default;
};

// The sample of `isotope_key` of an analysis from its raw sniff (no sniff:
// every bin 0). Points past 1200 s are dropped, as legacy's array ends there.
IsotopeSample make_isotope_sample(const RawData& raw, const std::string& isotope_key, const std::string& isotope_name);

struct Classification {
  int klass = 1;
  double probability = 0.0;  // of `klass` among the neighbours
};

class IsotopeClassifier {
 public:
  static constexpr int kNeighbours = 3;

  bool empty() const noexcept { return samples_.empty(); }
  const std::vector<IsotopeSample>& samples() const noexcept { return samples_; }
  void add(IsotopeSample sample) { samples_.push_back(std::move(sample)); }
  void clear() { samples_.clear(); }

  // The majority class of the min(k, n) nearest samples (a tie goes to the
  // smaller class, as scipy's mode) and its share of them; nullopt without
  // training samples.
  std::optional<Classification> classify(const IsotopeSample& sample) const;

  // TOML [[sample]] tables. A missing file loads as an empty classifier.
  static Result<IsotopeClassifier> load(const std::filesystem::path& path);
  Result<void> save(const std::filesystem::path& path) const;
  std::string to_toml() const;
  static Result<IsotopeClassifier> from_toml(std::string_view text);

 private:
  std::vector<IsotopeSample> samples_;
};

double sample_distance(const IsotopeSample& a, const IsotopeSample& b);

}  // namespace pychron::processing
