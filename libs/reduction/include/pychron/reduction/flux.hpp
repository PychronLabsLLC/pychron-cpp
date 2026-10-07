// Flux (J) of monitor analyses and the mean J of an irradiation position
// (flux fitting design, sections 5.1 and 5.2). Pure functions: no I/O, no
// clocks, no state.
//
// Ported from legacy pychron:
//   processing/argon_calculations.py   calculate_flux (J from a monitor F)
//   processing/analyses/analysis_group.py  per-position mean J
// Documented deviations are marked "Deviation:".
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/reduction/stats.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction {

// The monitor's known age (years) and total 40K decay constant (1/year).
struct MonitorConstants {
  double age_a = 0;
  double lambda_k = 0;
};

// J = (exp(lambda_k t) - 1) / F. The age and the constant are plain doubles,
// so only F's uncertainty propagates: sigma_J = J sigma_F / F.
// Error (Config, "flux: ...") when F is zero, negative or not finite.
Result<UFloat> j_of(const UFloat& f, const MonitorConstants& monitor);

struct MonitorAnalysis {
  std::string record_id;
  UFloat f;  // 40Ar*/39ArK of the monitor
  bool omitted = false;
};

enum class MeanKind { Arithmetic, Weighted };

std::string_view to_string(MeanKind kind) noexcept;                       // "arithmetic", "weighted"
std::optional<MeanKind> parse_mean_kind(std::string_view text) noexcept;  // any case

struct PositionMean {
  double j = 0, j_err = 0, mswd = 0;
  bool mswd_acceptable = false;
  int n = 0;                          // analyses used
  std::vector<std::string> rejected;  // record ids whose J could not be used
};

// Mean J of the non-omitted analyses, with the requested error kind. An
// analysis with no J (see j_of) is rejected and named. Error (Config,
// "flux: ...") when none is left.
// Deviation: for MeanKind::Weighted an analysis whose J has a zero or
// non-finite error is rejected and named; weighted_mean would drop it
// silently.
Result<PositionMean> mean_j(std::span<const MonitorAnalysis> analyses, const MonitorConstants& monitor,
                            MeanKind kind, MeanErrorKind error);

}  // namespace pychron::reduction
