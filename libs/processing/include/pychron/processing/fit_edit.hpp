#pragma once

// Editing isotope-evolution fits in recall (data browsing and visualization
// design, section 11.3): a fit kind, error type, outlier filter and points
// the user leaves out, refitted on the raw signal. The result is a pending
// edit until a revision source saves it.

#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/reduction/fits.hpp"

namespace pychron::processing {

// One fit of a raw series with points left out. `outliers` are the points the
// filter removed, as indices into the full series (not the reduced one).
struct SeriesFit {
  reduction::Intercept intercept;
  std::vector<std::size_t> outliers;
};

// Fits `series` without the `user_excluded` indices (out-of-range ones are
// ignored). `origin` shifts time zero: the value is the fit at t = origin.
Result<SeriesFit> fit_series(const RawSeries& series, const reduction::FitSpec& spec,
                             const std::vector<std::size_t>& user_excluded, double origin = 0.0);

struct FitEdit {
  std::string key;  // isotope key
  reduction::FitSpec fit;
  std::vector<std::size_t> user_excluded;  // ascending, unique
};

struct EditedIsotope {
  std::string key;
  Value intercept;
  reduction::FitSpec fit;
  int n_points = 0;  // raw signal points
  int n_used = 0;    // points in the final fit
  std::vector<std::size_t> user_excluded;
};

struct FitEditResult {
  AnalysisPtr analysis;  // a copy with the edited intercepts, fits, n and exclusions
  std::vector<EditedIsotope> isotopes;
};

// Refits every edit on the isotope's raw signal. Fails, naming the isotope,
// when the analysis has no such isotope, there is no signal, or the fit fails.
Result<FitEditResult> apply_fit_edits(const Analysis& analysis, const RawData& raw, const std::vector<FitEdit>& edits);

// Toggles `index` in an ascending, unique index list.
void toggle_index(std::vector<std::size_t>& indices, std::size_t index);

// Evolution point references are "<isotope key>#<index>" (scene PointRef).
std::string evolution_ref(const std::string& key, std::size_t index);
std::optional<std::pair<std::string, std::size_t>> parse_evolution_ref(const std::string& ref);

// "<ISOEVO> Ar40 linear -> parabolic, Ar36 2 excluded" for a revision message.
std::string describe_fit_edits(const Analysis& before, const std::vector<EditedIsotope>& edits);

}  // namespace pychron::processing
