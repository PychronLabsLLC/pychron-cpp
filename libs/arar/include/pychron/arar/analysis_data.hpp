#pragma once

// The Ar-Ar part of an analysis (domain seam spec 3.3): what reduce() needs
// beyond the stored values, carried as the analysis's extension. The Ar-Ar
// loaders make it: the store's from the resolved references
// (store_loader.hpp), a records directory's from references.toml
// (record_loader.hpp). Any part may be absent: a records directory without a
// flux for the identifier gives no J, so its unknowns have no age.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/arar/arar_types.hpp"
#include "pychron/processing/model.hpp"

namespace pychron::arar {

struct ArArAnalysisData final : processing::DomainAnalysisData {
  std::optional<reduction::Flux> flux;
  std::optional<reduction::ProductionRatios> production;
  std::vector<reduction::Dose> chronology;
  // Where `production` was measured (production.reactor); informational.
  std::string reactor;
  std::optional<reduction::ReductionConstants> constants;  // absent: the run's settings decide
  std::optional<reduction::Measured> fixed_k3739;

  [[nodiscard]] std::string_view domain() const override { return "arar"; }
};

// The Ar-Ar data of `analysis`, or null when it has none (no Ar-Ar loader was
// wired, or another domain's).
[[nodiscard]] const ArArAnalysisData* analysis_data_of(const processing::Analysis& analysis) noexcept;

// The Ar-Ar data of `analysis` to change in place: a copy of what it has (or
// empty data), now the analysis's own extension. Extensions are shared between
// copies of an analysis, so this never changes another copy's.
[[nodiscard]] ArArAnalysisData& edit_analysis_data(processing::Analysis& analysis);

}  // namespace pychron::arar
