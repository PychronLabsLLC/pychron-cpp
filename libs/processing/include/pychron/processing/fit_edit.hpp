#pragma once

// Editing isotope-evolution and baseline fits in recall (data browsing and
// visualization design, section 11.3): a fit kind, error type, outlier
// filter and points the user leaves out, refitted on the raw series. The
// result is a pending edit until a revision source saves it.
//
// Signal fits are keyed by isotope key ("Ar40"); baseline fits by detector
// ("H1"), and a baseline edit applies to every isotope on that detector.

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

// What an analysis holds for one fit: the isotope's intercept, or the
// baseline of the first isotope on the detector.
struct StoredFit {
  std::optional<reduction::FitSpec> fit;
  std::vector<std::size_t> user_excluded;
  Value value;
};
std::optional<StoredFit> stored_fit(const Analysis& analysis, SeriesKind kind, const std::string& key);

struct FitEdit {
  SeriesKind kind = SeriesKind::Signal;  // Signal or Baseline
  std::string key;                       // isotope key, or detector for baselines
  reduction::FitSpec fit;
  std::vector<std::size_t> user_excluded;  // ascending, unique
};

struct EditedFit {
  SeriesKind kind = SeriesKind::Signal;
  std::string key;
  Value value;  // the refitted intercept or baseline
  reduction::FitSpec fit;
  int n_points = 0;  // raw points
  int n_used = 0;    // points in the final fit
  std::vector<std::size_t> user_excluded;
};

struct FitEditResult {
  AnalysisPtr analysis;  // a copy with the edited values, fits, n and exclusions
  std::vector<EditedFit> fits;
};

// Refits every edit on its raw series. Fails, naming the series, when the
// analysis has no such isotope or detector, there is no raw series, the kind
// is not Signal or Baseline, or the fit fails.
Result<FitEditResult> apply_fit_edits(const Analysis& analysis, const RawData& raw, const std::vector<FitEdit>& edits);

// True when `edit` would change nothing of what `analysis` stores.
bool same_as_stored(const Analysis& analysis, const FitEdit& edit);

// Toggles `index` in an ascending, unique index list.
void toggle_index(std::vector<std::size_t>& indices, std::size_t index);

// Evolution point references are "<key>#<index>" (scene PointRef); the key
// is an isotope key on signal panels and a detector on baseline panels.
std::string evolution_ref(const std::string& key, std::size_t index);
std::optional<std::pair<std::string, std::size_t>> parse_evolution_ref(const std::string& ref);

// "<ISOEVO> Ar40 linear -> parabolic, H1 baseline 2 excluded" for a
// revision message.
std::string describe_fit_edits(const Analysis& before, const std::vector<EditedFit>& edits);

}  // namespace pychron::processing
