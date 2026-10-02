// Decay since irradiation (spec 3.2): E6 chronology and E7 decay factors.
#include <algorithm>
#include <cmath>
#include <string>

#include "pychron/reduction/arar_reduction.hpp"

namespace pychron::reduction {

namespace {
constexpr double kSecondsPerDay = 86400.0;
constexpr double kUnitGuard = 50.0;
}  // namespace

// E6. legacy:dvc/dvc_analysis.py:370-395, dvc/meta_object.py:99-105
Irradiation irradiation_from_doses(std::span<const Dose> doses, std::int64_t analysis_utc_s,
                                   bool use_irradiation_endtime) {
  Irradiation out;
  out.segments.reserve(doses.size());
  for (const Dose& d : doses) {
    const std::int64_t ref = use_irradiation_endtime ? d.end_utc_s : d.start_utc_s;
    out.segments.push_back({d.power, static_cast<double>(d.end_utc_s - d.start_utc_s) / kSecondsPerDay,
                            static_cast<double>(analysis_utc_s - ref) / kSecondsPerDay});
  }
  if (!doses.empty()) {
    out.decay_days = static_cast<double>(analysis_utc_s - doses.front().start_utc_s) / kSecondsPerDay;
  }
  return out;
}

// E7. legacy:processing/argon_calculations.py:253-279, 341-360
Result<DecayFactors> decay_factors(double lambda37_per_day, double lambda39_per_day,
                                   std::span<const DecaySegment> segments) {
  DecayFactors out;
  if (segments.empty()) return out;
  for (const DecaySegment& s : segments) {
    const double span = std::max(std::fabs(s.duration_days), std::fabs(s.dt_days));
    for (double l : {lambda37_per_day, lambda39_per_day}) {
      if (std::fabs(l * span) > kUnitGuard) {
        return fail(ErrorKind::Config,
                    "reduction: decay constants and times must be in the same unit (days); "
                    "|lambda * t| = " + std::to_string(std::fabs(l * span)) + " > 50");
      }
    }
  }
  double p = 0.0, b = 0.0, c = 0.0;
  for (const DecaySegment& s : segments) {
    p += s.power * s.duration_days;
    b += s.power * (1 - std::exp(-lambda37_per_day * s.duration_days)) /
         (lambda37_per_day * std::exp(lambda37_per_day * s.dt_days));
    c += s.power * (1 - std::exp(-lambda39_per_day * s.duration_days)) /
         (lambda39_per_day * std::exp(lambda39_per_day * s.dt_days));
  }
  out.df37 = b != 0.0 ? p / b : 1.0;
  out.df39 = c != 0.0 ? p / c : 1.0;
  return out;
}

}  // namespace pychron::reduction
