#pragma once

// An analysis with its reduction (design section 7.3): every per-isotope
// correction stage as a UFloat built from the same variables reduce() used,
// so ratios of stages carry correlated errors.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/processing/model.hpp"
#include "pychron/reduction/signal.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::processing {

// Core stages only: a domain's own stages (Ar-Ar's decay and interference
// corrected) are its DomainReduced's, by name.
enum class Stage {
  Intercept,
  Baseline,
  Blank,
  IcFactor,
  BaselineCorrected,  // intercept - baseline
  BlankCorrected,     // - blank (unless the type is not blank corrected)
  IcCorrected,        // * discrimination * IC; legacy get_intensity
};

std::string_view to_string(Stage stage) noexcept;  // "intercept", "bs_corrected", ...
std::optional<Stage> parse_stage(std::string_view text) noexcept;

struct IsotopeStages {
  std::string key;
  std::map<Stage, reduction::UFloat> values;
};

// What a domain's reduction adds to a reduced analysis (domain seam spec
// 3.2): its stages and named values, built from the analysis's signals so
// they correlate with the core stages.
class DomainReduced {
 public:
  DomainReduced() = default;
  DomainReduced(const DomainReduced&) = default;
  DomainReduced(DomainReduced&&) = default;
  DomainReduced& operator=(const DomainReduced&) = default;
  DomainReduced& operator=(DomainReduced&&) = default;
  virtual ~DomainReduced() = default;

  virtual std::string_view domain() const = 0;
  // A stage of this domain for `label` ("decay_corrected"); nullopt: not one of its.
  virtual std::optional<reduction::UFloat> stage(std::string_view label, std::string_view stage) const = 0;
  // A named value ("age", "kca"); nullopt: not one of its.
  virtual std::optional<reduction::UFloat> named(std::string_view name) const = 0;
  virtual std::vector<std::string> stages() const = 0;  // names, in order after IcCorrected
  virtual std::vector<std::string> names() const = 0;
  virtual std::string_view error() const = 0;  // why some values are missing; empty when none are
};

struct ReducedAnalysis {
  AnalysisPtr analysis;
  // One per isotope, same order as analysis->isotopes: the variables every
  // stage is built from. A domain's reduction reads these, never mints its own,
  // so its values correlate with the core stages (UFloat correlation is by
  // variable id).
  std::vector<reduction::IsotopeSignal> isotope_signals;
  std::vector<IsotopeStages> isotopes;  // same order as analysis->isotopes; core stages only
  // The domain's part, when its reduction ran and gave one; null otherwise.
  std::shared_ptr<const DomainReduced> domain;
  // Why the domain's reduction gave no values, or why some are missing (an
  // Ar-Ar unknown without production ratios: no age); such an analysis must
  // not feed group results.
  std::string reduction_error;

  const IsotopeStages* find(std::string_view key) const;
  // Stage of an isotope key, or of the first isotope with that name.
  std::optional<reduction::UFloat> stage(std::string_view key, Stage stage) const;
};

using ReducedPtr = std::shared_ptr<const ReducedAnalysis>;

// Never fails, and sets no reduction_error: an isotope whose intercept,
// baseline, blank or IC factor is unknown (Value::known() false) has NaN
// stages. The core stages of every isotope, from one signal each. Core's
// reduction has nothing to choose; a domain's options are its own units'
// (Ar-Ar: arar_reduce).
ReducedPtr reduce_analysis(AnalysisPtr analysis);

}  // namespace pychron::processing
