#include "pychron/earthbank/mapping.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/expected.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/processing/arar_groups.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/group_ages.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/reduction/arar_reduction.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/stats.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::earthbank {

namespace pr = processing;
namespace r = reduction;

namespace {

using Notes = std::vector<std::string>;

// ---------------------------------------------------------------- text

std::string in_quotes(std::string_view text) { return "\"" + std::string(text) + "\""; }

// A number in a message: ten significant figures say which it is without the
// tail a sum of two doubles can carry.
std::string shown(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.10g", value);
  return buffer;
}

// For a note: four significant figures are enough to recognise a number.
std::string rounded(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.4g", value);
  return buffer;
}

std::string plural(std::size_t n, std::string_view one, std::string_view many) {
  return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

Unexpected<Error> refuse(std::string_view sample, const std::string& what) {
  return fail(ErrorKind::Config, "earthbank: sample " + in_quotes(sample) + ": " + what);
}

void set_text(Text& field, std::string_view value) {
  if (!value.empty()) field = std::string(value);
}

// ---------------------------------------------------------------- numbers

// Sets the numeric fields of one record. A value that is not a finite number
// is not set: the field stays empty and a note says which, of what.
class Numbers {
 public:
  Numbers(std::string of, Notes& notes) : of_(std::move(of)), notes_(notes) {}

  bool set(Number& field, std::string_view name, double value) {
    if (std::isfinite(value)) {
      field = value;
      return true;
    }
    notes_.push_back(of_ + ": " + std::string(name) + " is not a finite number and was left out");
    return false;
  }

  // A value and its one-sigma uncertainty, both times `scale`. An uncertainty
  // is not sent without its value.
  void set(Number& value, Number& uncertainty, std::string_view name, const r::UFloat& u, double scale = 1.0) {
    if (!set(value, name, u.nominal() * scale)) return;
    set(uncertainty, std::string(name) + "Uncertainty", u.std_dev() * std::abs(scale));
  }
  void set(Number& value, Number& uncertainty, std::string_view name, const std::optional<r::UFloat>& u,
           double scale = 1.0) {
    if (u) set(value, uncertainty, name, *u, scale);
  }

 private:
  std::string of_;
  Notes& notes_;
};

// `record.field` with the field's own name, and the same with its
// `<field>Uncertainty` neighbour.
#define PYCHRON_EARTHBANK_SET(numbers, record, field, value) (numbers).set((record).field, #field, value)
#define PYCHRON_EARTHBANK_SET_PM(numbers, record, field, ...) \
  (numbers).set((record).field, (record).field##Uncertainty, #field, __VA_ARGS__)

// ---------------------------------------------------------------- time

using Seconds = std::chrono::sys_seconds;

// An analysis's time, when it has one: zero is "not recorded".
std::optional<Seconds> known_time(double utc_epoch_seconds) {
  if (!std::isfinite(utc_epoch_seconds) || utc_epoch_seconds <= 0.0 || utc_epoch_seconds >= 1e11) return std::nullopt;
  return Seconds(std::chrono::seconds(std::llround(utc_epoch_seconds)));
}

// UTC, `YYYY-MM-DD`.
std::string utc_date(Seconds t) {
  const std::chrono::year_month_day day(std::chrono::floor<std::chrono::days>(t));
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u", static_cast<int>(day.year()),
                static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
  return buffer;
}

// UTC, `YYYY-MM-DDTHH:MM:SSZ`.
std::string utc_time(Seconds t) {
  const auto midnight = std::chrono::floor<std::chrono::days>(t);
  const std::chrono::hh_mm_ss clock(t - midnight);
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "T%02d:%02d:%02dZ", static_cast<int>(clock.hours().count()),
                static_cast<int>(clock.minutes().count()), static_cast<int>(clock.seconds().count()));
  return utc_date(t) + buffer;
}

// ---------------------------------------------------------------- a sample's analyses

// One analysis of a sample with its reduction.
struct Member {
  pr::AnalysisPtr analysis;
  pr::ReducedPtr reduced;

  const pr::Analysis& a() const { return *analysis; }
  bool has_age() const { return reduced->arar && reduced->arar->ages; }
  bool is_step() const { return analysis->increment >= 0; }
  // The loaded piece it was measured on.
  std::pair<std::string, int> aliquot() const { return {analysis->identifier, analysis->aliquot}; }
  std::string aliquot_name() const { return pr::make_runid(analysis->identifier, analysis->aliquot, -1); }
};

using Members = std::vector<Member>;

// Time order; analyses at one time by run id, so the order given does not
// show in the result.
void sort_by_time(Members& members) {
  std::stable_sort(members.begin(), members.end(), [](const Member& x, const Member& y) {
    if (x.a().timestamp != y.a().timestamp) return x.a().timestamp < y.a().timestamp;
    if (x.a().runid != y.a().runid) return x.a().runid < y.a().runid;
    return x.a().uuid < y.a().uuid;
  });
}

// ---------------------------------------------------------------- agreement

// What the analyses say about something that is one fact of their sample.
// Analyses that say nothing are passed over; two that say different things
// are refused with both. `read` gives an optional value; `show` prints one.
template <typename Read, typename Show>
auto agreed(std::string_view sample, const Members& members, std::string_view what, Read read, Show show)
    -> Result<decltype(read(members.front()))> {
  decltype(read(members.front())) found;
  const Member* from = nullptr;
  for (const Member& m : members) {
    auto value = read(m);
    if (!value) continue;
    if (!found) {
      found = std::move(value);
      from = &m;
    } else if (!(*found == *value)) {
      return refuse(sample, std::string(what) + " differs between analyses: " + show(*found) + " (" + from->a().runid +
                                ") and " + show(*value) + " (" + m.a().runid + ")");
    }
  }
  return found;
}

Result<std::optional<std::string>> agreed_text(std::string_view sample, const Members& members, std::string_view what,
                                               std::string pr::Analysis::*field) {
  return agreed(
      sample, members, what,
      [field](const Member& m) { return (m.a().*field).empty() ? std::nullopt : std::optional((m.a().*field)); },
      [](const std::string& s) { return in_quotes(s); });
}

Result<std::optional<std::string>> agreed_text(std::string_view sample, const Members& members, std::string_view what,
                                               std::string pr::SampleInfo::*field) {
  return agreed(
      sample, members, what,
      [field](const Member& m) {
        const std::string& s = m.a().sample_info.*field;
        return s.empty() ? std::nullopt : std::optional(s);
      },
      [](const std::string& s) { return in_quotes(s); });
}

// A number of the catalog entry. One that is not finite is not a value: it is
// passed over, and when no analysis has a finite one a note says the field
// was left out.
Result<std::optional<double>> agreed_number(std::string_view sample, const Members& members, std::string_view what,
                                            std::optional<double> pr::SampleInfo::*field, std::string_view left_out,
                                            Notes& notes) {
  bool not_finite = false;
  auto found = agreed(
      sample, members, what,
      [field, &not_finite](const Member& m) -> std::optional<double> {
        const std::optional<double>& v = m.a().sample_info.*field;
        if (v && !std::isfinite(*v)) not_finite = true;
        return v && std::isfinite(*v) ? v : std::nullopt;
      },
      [](double v) { return shown(v); });
  if (found && !*found && not_finite) {
    notes.push_back(std::string(sample) + ": " + std::string(left_out) + " is not a finite number and was left out");
  }
  return found;
}

// What the catalog says about the sample, as far as the analyses agree.
struct Facts {
  std::optional<std::string> material, lithology, unit, igsn;
  std::optional<double> latitude, longitude, elevation;
};

Result<Facts> facts_of(std::string_view sample, const Members& members, Notes& notes) {
  Facts facts;
  auto material = agreed_text(sample, members, "material", &pr::Analysis::material);
  if (!material) return fail(material.error());
  facts.material = *material;
  auto lithology = agreed_text(sample, members, "lithology", &pr::SampleInfo::lithology);
  if (!lithology) return fail(lithology.error());
  facts.lithology = *lithology;
  auto unit = agreed_text(sample, members, "stratigraphic unit", &pr::SampleInfo::unit);
  if (!unit) return fail(unit.error());
  facts.unit = *unit;
  auto igsn = agreed_text(sample, members, "IGSN", &pr::SampleInfo::igsn);
  if (!igsn) return fail(igsn.error());
  facts.igsn = *igsn;
  auto latitude = agreed_number(sample, members, "latitude", &pr::SampleInfo::latitude, "lat", notes);
  if (!latitude) return fail(latitude.error());
  facts.latitude = *latitude;
  auto longitude = agreed_number(sample, members, "longitude", &pr::SampleInfo::longitude, "lon", notes);
  if (!longitude) return fail(longitude.error());
  facts.longitude = *longitude;
  auto elevation =
      agreed_number(sample, members, "elevation", &pr::SampleInfo::elevation, "referenceElevation", notes);
  if (!elevation) return fail(elevation.error());
  facts.elevation = *elevation;
  return facts;
}

// ---------------------------------------------------------------- constants

std::string_view units_name(r::AgeUnits units) {
  switch (units) {
    case r::AgeUnits::a: return "a";
    case r::AgeUnits::ka: return "ka";
    case r::AgeUnits::Ma: return "Ma";
    case r::AgeUnits::Ga: return "Ga";
  }
  return "Ma";
}

// The total 40K decay constant an analysis's age used, per year: the flux's
// own when it carries one (not exactly 0 +- 0, as reduction::age_equation
// reads it), otherwise lambda_b + lambda_e.
double decay_constant_of(const pr::ReducedAnalysis& reduced) {
  if (const auto& k = reduced.lambda_k_total; k && !(k->value == 0.0 && k->error == 0.0)) return k->value;
  return r::lambda_k(*reduced.constants).value;
}

// What the ages of the sample rest on. Every analysis with an age has
// constants (reduce_analysis sets them before it reduces).
struct Constants {
  double decay_constant = 0.0;  // per year
  double atm4036 = 0.0;
  r::AgeUnits units = r::AgeUnits::Ma;
  double to_ma = 1.0;  // an age in `units`, times this, is in Ma
};

Result<Constants> constants_of(std::string_view sample, const Members& dated) {
  const auto show = [](double v) { return shown(v); };
  auto decay = agreed(
      sample, dated, "decay constant",
      [](const Member& m) { return std::optional(decay_constant_of(*m.reduced)); }, show);
  if (!decay) return fail(decay.error());
  auto air = agreed(
      sample, dated, "atmospheric 40Ar/36Ar",
      [](const Member& m) { return std::optional(m.reduced->constants->atm4036.value); }, show);
  if (!air) return fail(air.error());
  auto units = agreed(
      sample, dated, "age unit", [](const Member& m) { return std::optional(m.reduced->constants->age_units); },
      [](r::AgeUnits u) { return std::string(units_name(u)); });
  if (!units) return fail(units.error());
  Constants out;
  out.decay_constant = **decay;
  out.atm4036 = **air;
  out.units = **units;
  out.to_ma = r::age_scale(out.units, r::AgeUnits::Ma);
  return out;
}

// ---------------------------------------------------------------- the reported age

// The analyses with an age as the group functions take them.
struct Group {
  Group() = default;
  Group(Group&&) = default;  // `all` and `schedule` point into `items`: moved, never copied
  Group& operator=(Group&&) = default;

  std::vector<pr::DatasetItem> items;  // one per dated analysis, in time order
  pr::GroupItems all;                  // every one, in step order (as the report orders a group)
  pr::GroupItems schedule;             // the steps of the heated aliquot, in step order; empty unless there is exactly one
  std::size_t heated_aliquots = 0;
  bool any_step = false, any_fusion = false;
};

Group group_of(const Members& dated) {
  Group g;
  g.items.reserve(dated.size());  // `all` and `schedule` point into it
  std::vector<std::pair<std::string, int>> heated;
  for (const Member& m : dated) {
    g.items.push_back(pr::DatasetItem{m.reduced, {}, {}});
    if (!m.is_step()) {
      g.any_fusion = true;
      continue;
    }
    g.any_step = true;
    if (std::find(heated.begin(), heated.end(), m.aliquot()) == heated.end()) heated.push_back(m.aliquot());
  }
  g.heated_aliquots = heated.size();
  for (std::size_t i = 0; i < dated.size(); ++i) {
    g.all.push_back(&g.items[i]);
    if (g.heated_aliquots == 1 && dated[i].is_step()) g.schedule.push_back(&g.items[i]);
  }
  g.all = pr::step_ordered(std::move(g.all));
  g.schedule = pr::step_ordered(std::move(g.schedule));
  return g;
}

// One candidate for the sample's age, in the reduction's age units.
struct SummaryAge {
  AgeKind kind = AgeKind::WeightedMean;
  double age = 0.0;
  double analytical = 0.0;  // one sigma, without the J uncertainty
  double with_j = 0.0;      // one sigma, with it
  std::optional<double> mswd, probability;
  std::size_t n = 0;       // the analyses the age used
  bool error_kind = true;  // the uncertainty is PublishOptions::ages.mean_error's; false: propagated
  pr::GroupItems plateau;  // the steps of the plateau, when it is one
  std::string detail;      // for the note
};

// A candidate, or why there is none.
using Candidate = Expected<SummaryAge, std::string>;

Unexpected<std::string> none(std::string why) { return Unexpected<std::string>(std::move(why)); }

// The message of a group function's error without its own prefix.
std::string reason(const Error& error) { return error.what; }

std::string criteria_text(const r::PlateauCriteria& c) {
  return "at least " + plural(static_cast<std::size_t>(std::max(c.nsteps, 0)), "step", "steps") + " and " +
         rounded(c.gas_fraction) + "% of the 39ArK";
}

Candidate plateau_age(const Group& g, const PublishOptions& o, double j_relative) {
  if (g.heated_aliquots == 0) return none("no analysis is a heating step");
  if (g.heated_aliquots > 1) {
    return none(std::to_string(g.heated_aliquots) + " aliquots were step heated, and a plateau is of one heating schedule");
  }
  const auto found = pr::group_plateau(g.schedule, o.ages.plateau, o.ages.plateau_weighting, o.ages.mean_error);
  if (!found) return none("no run of steps meets the plateau criteria (" + criteria_text(o.ages.plateau) + ")");
  const r::Mean& mean = found->mean.mean;
  SummaryAge out;
  out.kind = AgeKind::Plateau;
  out.age = mean.value;
  out.analytical = mean.error;
  out.with_j = pr::with_external_error(mean.value, mean.error, j_relative);
  out.n = found->mean.nsteps;
  if (out.n >= 2) {
    out.mswd = mean.mswd;
    out.probability = r::mswd_probability(mean.mswd, static_cast<int>(out.n) - 1);
  }
  out.plateau = found->used;
  out.detail = "steps " + found->steps[found->range.first]->analysis->analysis->step() + "-" +
               found->steps[found->range.last]->analysis->analysis->step() + ", " +
               plural(out.n, "step", "steps") + " of " + std::to_string(found->steps.size()) + ", " +
               rounded(found->mean.gas_fraction) + "% of the 39ArK";
  return out;
}

Candidate weighted_mean_age(const Group& g, const PublishOptions& o, double j_relative) {
  const auto mean = pr::group_weighted_mean(g.all, o.ages.mean_error);
  if (!mean) return none(reason(mean.error()));
  SummaryAge out;
  out.kind = AgeKind::WeightedMean;
  out.age = mean->value;
  out.analytical = mean->error;
  out.with_j = pr::with_external_error(mean->value, mean->error, j_relative);
  out.n = mean->n;
  if (out.n >= 2) {
    out.mswd = mean->mswd;
    out.probability = r::mswd_probability(mean->mswd, static_cast<int>(out.n) - 1);
  }
  out.detail = "of " + plural(out.n, "analysis", "analyses");
  return out;
}

Candidate isochron_age(const Group& g, const PublishOptions& o) {
  const auto points = pr::isochron_points(g.all);
  const auto plain = pr::isochron_age(points, o.ages.york, false, false);
  if (!plain) return none(reason(plain.error()));
  const auto with_j = pr::isochron_age(points, o.ages.york, false, true);
  if (!plain->age || !with_j || !with_j->age) return none("isochron: the fit gives no age");
  SummaryAge out;
  out.kind = AgeKind::Isochron;
  out.age = plain->age->nominal();
  out.analytical = plain->age->std_dev();
  out.with_j = with_j->age->std_dev();
  out.mswd = plain->fit.mswd;
  out.probability = plain->fit.probability;
  out.n = plain->fit.n;
  out.error_kind = false;
  out.detail = "of " + plural(out.n, "analysis", "analyses");
  return out;
}

Candidate integrated_age(const Group& g) {
  const auto plain = pr::integrated_age(g.all, false);
  if (!plain) return none(reason(plain.error()));
  const auto with_j = pr::integrated_age(g.all, true);
  if (!with_j) return none(reason(with_j.error()));
  SummaryAge out;
  out.kind = AgeKind::Integrated;
  out.age = plain->nominal();
  out.analytical = plain->std_dev();
  out.with_j = with_j->std_dev();
  // Every analysis here has an age, so the sum took each of them: an analysis
  // that reduced with something missing has no age (reduce_analysis).
  out.n = g.all.size();
  out.error_kind = false;
  out.detail = "the gas of " + plural(out.n, "analysis", "analyses");
  return out;
}

Candidate candidate(AgeKind kind, const Group& g, const PublishOptions& o, double j_relative) {
  switch (kind) {
    case AgeKind::Plateau: return plateau_age(g, o, j_relative);
    case AgeKind::WeightedMean: return weighted_mean_age(g, o, j_relative);
    case AgeKind::Isochron: return isochron_age(g, o);
    case AgeKind::Integrated: return integrated_age(g);
  }
  return none("not a kind of age");
}

// The age the sample reports (spec 4.4) and a note of the choice.
Result<SummaryAge> preferred_age(std::string_view sample, const Group& g, const PublishOptions& o,
                                 const SampleOptions& so, Notes& notes) {
  const double j_relative = pr::j_relative_error(g.all);
  const auto name = [](AgeKind kind) { return std::string(age_type_name(kind)); };
  const auto chosen = [&](const SummaryAge& age, const std::string& why) {
    notes.push_back("preferred age: " + name(age.kind) + " (" + age.detail + "): " + why);
    return age;
  };

  if (so.preferred_age) {
    const Candidate asked = candidate(*so.preferred_age, g, o, j_relative);
    if (!asked) return refuse(sample, "the " + name(*so.preferred_age) + " asked for cannot be computed: " + asked.error());
    return chosen(*asked, "chosen in the options");
  }
  if (g.any_step) {
    const Candidate plateau = plateau_age(g, o, j_relative);
    if (plateau) return chosen(*plateau, "the sample was step heated and has a plateau");
    const Candidate integrated = integrated_age(g);
    if (!integrated) return refuse(sample, "no age to report: " + plateau.error() + "; " + integrated.error());
    return chosen(*integrated, "the sample was step heated and has no plateau: " + plateau.error());
  }
  const Candidate mean = weighted_mean_age(g, o, j_relative);
  if (!mean) return refuse(sample, "no age to report: " + mean.error());
  return chosen(*mean, "the sample's analyses are fusions");
}

// The group functions take J from the first analysis that has one. When the
// analyses do not share a J (two irradiation positions), say whose was used.
void note_mixed_j(const Group& g, const SummaryAge& age, bool with_j, Notes& notes) {
  const pr::ReducedAnalysis* first = nullptr;
  bool differs = false;
  for (const pr::DatasetItem* item : g.all) {
    const pr::ReducedAnalysis& reduced = *item->analysis;
    if (!reduced.j) continue;
    if (first == nullptr) {
      first = &reduced;
    } else if (reduced.j->nominal() != first->j->nominal() || reduced.j->std_dev() != first->j->std_dev()) {
      differs = true;
    }
  }
  if (!differs) return;
  const bool in_age = age.kind == AgeKind::Isochron || age.kind == AgeKind::Integrated;
  if (!in_age && !with_j) return;
  notes.push_back("the analyses do not share one J; the J of " + first->analysis->runid + " (" +
                  shown(first->j->nominal()) + ") is the one in the reported " +
                  (in_age ? "age" : "age's uncertainty"));
}

// ---------------------------------------------------------------- 39ArK released

struct Released {
  double percent = 0.0, cumulative = 0.0;
};

// Each step's share of the 39ArK its aliquot released, alone and summed in
// step order, as the report's "% of group" and "cumulative" columns: a step
// without gas (39ArK not above zero) takes no part. The schedule is all of
// the aliquot's steps, published or not, so a step left out for want of an
// age still counts as gas released; when the 39ArK of a step is not known at
// all (it did not reduce) the shares of that aliquot are not known either,
// and they are left out with a note. A fusion has none.
std::map<const pr::ReducedAnalysis*, Released> released_39(const Members& members, Notes& notes) {
  std::map<std::pair<std::string, int>, std::vector<const Member*>> schedules;
  for (const Member& m : members) {
    if (m.is_step()) schedules[m.aliquot()].push_back(&m);
  }
  std::map<const pr::ReducedAnalysis*, Released> out;
  for (auto& [aliquot, steps] : schedules) {
    std::stable_sort(steps.begin(), steps.end(), [](const Member* x, const Member* y) {
      if (x->a().increment != y->a().increment) return x->a().increment < y->a().increment;
      return x->a().timestamp < y->a().timestamp;
    });
    const auto k39 = [](const Member* m) {
      return m->reduced->arar ? m->reduced->arar->f.interference.k39.nominal() : std::nan("");
    };
    std::string unknown;
    double total = 0.0;
    for (const Member* m : steps) {
      const double k = k39(m);
      if (!std::isfinite(k)) unknown += (unknown.empty() ? "" : ", ") + m->a().runid;
      if (k > 0) total += k;
    }
    if (!unknown.empty()) {
      notes.push_back(steps.front()->aliquot_name() + ": the 39ArK of " + unknown +
                      " is not known, so ar39KReleased and cumulative39ArK were left out for the aliquot");
      continue;
    }
    if (!(total > 0)) continue;
    double sum = 0.0;
    for (const Member* m : steps) {
      const double k = k39(m);
      if (!(k > 0)) continue;
      sum += k;
      out[m->reduced.get()] = {k / total * 100.0, sum / total * 100.0};
    }
  }
  return out;
}

// ---------------------------------------------------------------- records

// The fields of one isotope in a measurement: corrected, excluding the
// interference correction, and the blank.
struct IsotopeFields {
  r::ArgonIsotope isotope;
  Number Measurement::*corrected, Measurement::*corrected_error;
  Number Measurement::*exc_int, Measurement::*exc_int_error;
  Number Measurement::*blank, Measurement::*blank_error;
  const char* corrected_name;
  const char* exc_int_name;
  const char* blank_name;
};

#define PYCHRON_EARTHBANK_ISOTOPE(mass)                                                              \
  IsotopeFields {                                                                                    \
    r::ArgonIsotope::Ar##mass, &Measurement::ar##mass##Corr, &Measurement::ar##mass##CorrUncertainty, \
        &Measurement::ar##mass##ExcInt, &Measurement::ar##mass##ExcIntUncertainty,                   \
        &Measurement::ar##mass##blank, &Measurement::ar##mass##blankUncertainty, "ar" #mass "Corr",  \
        "ar" #mass "ExcInt", "ar" #mass "blank"                                                      \
  }

constexpr std::array kIsotopeFields = {PYCHRON_EARTHBANK_ISOTOPE(40), PYCHRON_EARTHBANK_ISOTOPE(39),
                                       PYCHRON_EARTHBANK_ISOTOPE(38), PYCHRON_EARTHBANK_ISOTOPE(37),
                                       PYCHRON_EARTHBANK_ISOTOPE(36)};

#undef PYCHRON_EARTHBANK_ISOTOPE

// A corrected isotope over 39ArK.
struct RatioFields {
  r::ArgonIsotope isotope;
  Number Measurement::*ratio, Measurement::*ratio_error;
  const char* name;
};

#define PYCHRON_EARTHBANK_RATIO(mass)                                           \
  RatioFields {                                                                 \
    r::ArgonIsotope::Ar##mass, &Measurement::ratio##mass##Arx39ArCorr,          \
        &Measurement::ratio##mass##Arx39ArCorrUncertainty, "ratio" #mass "Arx39ArCorr" \
  }

constexpr std::array kRatioFields = {PYCHRON_EARTHBANK_RATIO(36), PYCHRON_EARTHBANK_RATIO(37),
                                     PYCHRON_EARTHBANK_RATIO(38), PYCHRON_EARTHBANK_RATIO(40)};

#undef PYCHRON_EARTHBANK_RATIO

// The intensity the service calls "excluding interference correction": blank,
// discrimination and detector factors applied, and, for 39Ar only, the decay
// since irradiation (its definitions say "mass discrimination only" of 40Ar,
// 38Ar, 37Ar and 36Ar, "and decay only" of 39Ar). The decay stage is the
// discrimination stage for 40Ar, 38Ar and 36Ar.
pr::Stage exc_int_stage(r::ArgonIsotope isotope) {
  return isotope == r::ArgonIsotope::Ar37 ? pr::Stage::IcCorrected : pr::Stage::DecayCorrected;
}

// Laser power with its units, or the furnace's temperature: units with a `C`
// in them are degrees.
void extraction(Measurement& out, Numbers& numbers, const pr::ExtractionInfo& info) {
  if (!info.value) return;
  if (info.units.find('C') != std::string::npos) {
    PYCHRON_EARTHBANK_SET(numbers, out, furnaceTemperature, *info.value);
  } else if (PYCHRON_EARTHBANK_SET(numbers, out, laserPower, *info.value)) {
    set_text(out.laserPowerUnits, info.units);
  }
}

// One analysis. `to_ma` scales its age to Ma.
Measurement measurement_of(const Member& m, const Released* released, bool on_reported_plateau, double to_ma,
                           Notes& notes) {
  const pr::Analysis& a = m.a();
  const pr::ReducedAnalysis& reduced = *m.reduced;
  const r::ArArResult& arar = *reduced.arar;
  Numbers numbers(a.runid, notes);
  Measurement out;

  out.aliquotName = m.aliquot_name();
  out.analysisUnits = std::string(kIntensityUnits);
  if (const auto t = known_time(a.timestamp)) {
    out.analysisTime = utc_time(*t);
  } else {
    notes.push_back(a.runid + ": the analysis has no time; analysisTime was left out");
  }
  if (m.is_step()) PYCHRON_EARTHBANK_SET(numbers, out, stepNumber, static_cast<double>(a.increment + 1));
  extraction(out, numbers, a.extraction);

  // An isotope the analysis did not measure has no stage, and its fields stay empty.
  for (const IsotopeFields& f : kIsotopeFields) {
    const std::string_view isotope = r::to_string(f.isotope);
    numbers.set(out.*f.corrected, out.*f.corrected_error, f.corrected_name,
                reduced.stage(isotope, pr::Stage::InterferenceCorrected));
    numbers.set(out.*f.exc_int, out.*f.exc_int_error, f.exc_int_name, reduced.stage(isotope, exc_int_stage(f.isotope)));
    numbers.set(out.*f.blank, out.*f.blank_error, f.blank_name, reduced.stage(isotope, pr::Stage::Blank));
  }
  const auto k39 = reduced.stage(r::to_string(r::ArgonIsotope::Ar39), pr::Stage::InterferenceCorrected);
  for (const RatioFields& f : kRatioFields) {
    const auto top = reduced.stage(r::to_string(f.isotope), pr::Stage::InterferenceCorrected);
    if (top && k39) numbers.set(out.*f.ratio, out.*f.ratio_error, f.name, *top / *k39);
  }

  PYCHRON_EARTHBANK_SET_PM(numbers, out, ratio40Arx39ArK, arar.f.f);
  if (arar.f.radiogenic_yield) PYCHRON_EARTHBANK_SET(numbers, out, radiogenicArgon, arar.f.radiogenic_yield->nominal());
  PYCHRON_EARTHBANK_SET_PM(numbers, out, caK, arar.cak);
  PYCHRON_EARTHBANK_SET_PM(numbers, out, kCa, arar.kca);
  PYCHRON_EARTHBANK_SET_PM(numbers, out, clK, arar.clk);
  if (released != nullptr) {
    PYCHRON_EARTHBANK_SET(numbers, out, ar39KReleased, released->percent);
    PYCHRON_EARTHBANK_SET(numbers, out, cumulative39ArK, released->cumulative);
  }

  PYCHRON_EARTHBANK_SET_PM(numbers, out, jValue, reduced.j);
  out.jvalueUncertaintyTypeName = std::string(kOneSigma);
  PYCHRON_EARTHBANK_SET_PM(numbers, out, apparentAge, arar.ages->age, to_ma);
  out.apparentAgeUncertainityTypeName = std::string(kOneSigma);

  if (on_reported_plateau) out.plateauStepName = std::string(kYes);
  return out;
}

Sample sample_of(const std::string& name, const Facts& facts, const PublishOptions& options, Notes& notes) {
  Sample out;
  out.name = name;
  out.sampleID = name;
  out.materialName = facts.material;
  out.igsn = facts.igsn;
  out.stratographicUnitName = facts.unit;
  // A value that reached here is finite (agreed_number).
  if (facts.elevation) Numbers(name, notes).set(out.referenceElevation, "referenceElevation", *facts.elevation);
  out.description = "Uploaded from " + options.software;
  return out;
}

// Both coordinates or no location.
std::optional<Location> location_of(const std::string& name, const Facts& facts, Notes& notes) {
  if (facts.latitude && facts.longitude) {
    Location out;
    out.lat = facts.latitude;
    out.lon = facts.longitude;
    return out;
  }
  if (facts.latitude || facts.longitude) {
    notes.push_back(name + ": the sample has a " + (facts.latitude ? "latitude" : "longitude") + " and no " +
                    (facts.latitude ? "longitude" : "latitude") + "; no location is sent");
  }
  return std::nullopt;
}

DataPoint data_point_of(const std::string& name, const Members& dated, const Group& g, const Facts& facts,
                        const PublishOptions& options, Notes& notes) {
  DataPoint out;
  std::optional<Seconds> earliest;
  std::optional<double> hottest;
  for (const Member& m : dated) {
    if (const auto t = known_time(m.a().timestamp); t && (!earliest || *t < *earliest)) earliest = t;
    if (const auto& v = m.a().extraction.value; v && std::isfinite(*v) && (!hottest || *v > *hottest)) hottest = v;
  }
  if (earliest) {
    out.analysisDate = utc_date(*earliest);
  } else {
    notes.push_back(name + ": no analysis has a time; analysisDate was left out");
  }
  out.analysisUnits = std::string(kIntensityUnits);
  out.arMethodName = ar_method(g.any_step, hottest);
  out.mineralName = facts.material;
  out.lithologyName = facts.lithology;
  const std::string error_kind(r::to_string(options.ages.mean_error));
  out.analyticalUncertaintyTypeName = error_kind;
  out.analyticalUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  out.apparentAgeUncertaintyTypeName = error_kind;
  out.apparentAgeUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  out.jvalueUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  return out;
}

// One per loaded piece, in the order they first appear.
std::vector<Aliquot> aliquots_of(const Members& dated) {
  std::vector<Aliquot> out;
  std::vector<std::string> seen;
  for (const Member& m : dated) {
    std::string name = m.aliquot_name();
    if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
    seen.push_back(name);
    out.emplace_back().aliquotName = std::move(name);
  }
  return out;
}

// The monitor the analyses name, as the query it is looked up by.
struct Monitor {
  std::string name;
  std::optional<double> age;  // Ma
  bool operator==(const Monitor&) const = default;
  std::string query() const { return flux_monitor_query(name, age); }
};

Result<AgeCalc> age_calc_of(const std::string& name, const Members& dated, const Constants& constants,
                            const PublishOptions& options, const SampleOptions& so, Notes& notes) {
  AgeCalc out;

  if (!options.decay_constant.empty()) {
    out.decayConstantName = normalize_citation(options.decay_constant, true);
  } else if (std::string found = decay_constant_name(constants.decay_constant); !found.empty()) {
    out.decayConstantName = std::move(found);
  } else {
    notes.push_back(name + ": the decay constant the ages used (" + shown(constants.decay_constant) +
                    " /a) is not one EarthBank names; decayConstantName was left out, to be named in the options");
  }
  if (!options.air_ratio.empty()) {
    out.airRatioName = normalize_citation(options.air_ratio, false);
  } else if (std::string found = air_ratio_name(constants.atm4036); !found.empty()) {
    out.airRatioName = std::move(found);
  } else {
    notes.push_back(name + ": the atmospheric 40Ar/36Ar the ages used (" + shown(constants.atm4036) +
                    ") is not one EarthBank names; airRatioName was left out, to be named in the options");
  }

  if (!so.flux_monitor.empty()) {
    out.fluxMonitorName = so.flux_monitor;
    return out;
  }
  bool age_not_finite = false;
  auto monitor = agreed(
      name, dated, "flux monitor",
      [&age_not_finite](const Member& m) -> std::optional<Monitor> {
        const pr::MonitorInfo& info = m.a().monitor;
        Monitor found{info.name, std::nullopt};
        if (info.age && std::isfinite(info.age->value)) found.age = info.age->value;
        if (info.age && !std::isfinite(info.age->value)) age_not_finite = true;
        if (found.name.empty() && !found.age) return std::nullopt;
        return found;
      },
      [](const Monitor& found) { return in_quotes(found.query()); });
  if (!monitor) return fail(monitor.error());
  if (age_not_finite) notes.push_back(name + ": the flux monitor's age is not a finite number and was left out");
  if (*monitor) {
    out.fluxMonitorName = (*monitor)->query();
  } else {
    notes.push_back(name + ": the analyses name no flux monitor; fluxMonitorName was left out");
  }
  return out;
}

AgeSummary age_summary_of(const std::string& name, const SummaryAge& age, const Constants& constants,
                          const PublishOptions& options, const SampleOptions& so, Notes& notes) {
  Numbers numbers(name, notes);
  AgeSummary out;
  PYCHRON_EARTHBANK_SET(numbers, out, age, age.age * constants.to_ma);
  const double sigma = options.age_includes_j_error ? age.with_j : age.analytical;
  PYCHRON_EARTHBANK_SET(numbers, out, ageUncertainty, sigma * constants.to_ma);
  out.ageCalcTypeName = std::string(age_type_name(age.kind));
  // A mean's uncertainty is of the kind asked for; an age from the age
  // equation (integrated, isochron) carries a propagated one sigma.
  const std::string type = age.error_kind ? std::string(r::to_string(options.ages.mean_error)) : std::string(kOneSigma);
  out.ageUncertaintyTypeName = type;
  out.calculatedAgeUncertaintyTypeName = type;
  out.calculatedAgeUncertaintyTypeUnitsName = std::string(kAbsoluteUncertainty);
  if (age.mswd) PYCHRON_EARTHBANK_SET(numbers, out, mswd, *age.mswd);
  if (age.probability) PYCHRON_EARTHBANK_SET(numbers, out, agepvalue, *age.probability);
  out.nAnalysisGrouped = static_cast<std::int64_t>(age.n);
  out.preferredAge = true;
  out.dataInterpretationToolName = std::string(kInterpretationTool);
  out.interpretationName = so.interpretation.empty() ? std::string(kUnknownInterpretation) : so.interpretation;

  notes.push_back(std::string("the reported age's uncertainty is one sigma ") +
                  (options.age_includes_j_error ? "with the J uncertainty (" : "without the J uncertainty (") +
                  rounded(age.analytical * constants.to_ma) + " Ma analytical, " +
                  rounded(age.with_j * constants.to_ma) + " Ma with J)");
  return out;
}

// ---------------------------------------------------------------- one sample

Result<Submission> submission_of(const std::string& name, Members members, const PublishOptions& options) {
  static const SampleOptions kNoOptions;
  const auto found = options.samples.find(name);
  const SampleOptions& so = found == options.samples.end() ? kNoOptions : found->second;

  Submission out;
  Notes& notes = out.notes;
  sort_by_time(members);

  // Only an analysis with an age is published.
  Members dated;
  std::string undated;
  for (const Member& m : members) {
    if (m.has_age()) {
      dated.push_back(m);
      continue;
    }
    const std::string& why = m.reduced->reduction_error;
    undated += (undated.empty() ? "" : ", ") + m.a().runid + (why.empty() ? "" : " (" + why + ")");
  }
  if (dated.empty()) return refuse(name, "none of its " + plural(members.size(), "analysis", "analyses") + " has an age: " + undated);
  if (dated.size() < members.size()) {
    notes.push_back(plural(members.size() - dated.size(), "analysis", "analyses") + " of " +
                    std::to_string(members.size()) + " without an age left out: " + undated);
  }

  const auto facts = facts_of(name, members, notes);
  if (!facts) return fail(facts.error());
  const auto constants = constants_of(name, dated);
  if (!constants) return fail(constants.error());

  const Group group = group_of(dated);
  if (group.any_step && group.any_fusion) {
    notes.push_back("the sample has both heating steps and fusion analyses; it is published as step heated");
  }
  const auto age = preferred_age(name, group, options, so, notes);
  if (!age) return fail(age.error());
  note_mixed_j(group, *age, options.age_includes_j_error, notes);

  out.sample = sample_of(name, *facts, options, notes);
  out.location = location_of(name, *facts, notes);
  out.data_point = data_point_of(name, dated, group, *facts, options, notes);
  out.aliquots = aliquots_of(dated);

  const auto released = released_39(members, notes);
  for (const Member& m : dated) {
    const auto share = released.find(m.reduced.get());
    const bool on_plateau = std::any_of(age->plateau.begin(), age->plateau.end(), [&](const pr::DatasetItem* step) {
      return step->analysis.get() == m.reduced.get();
    });
    out.measurements.push_back(
        measurement_of(m, share == released.end() ? nullptr : &share->second, on_plateau, constants->to_ma, notes));
  }

  auto age_calc = age_calc_of(name, dated, *constants, options, so, notes);
  if (!age_calc) return fail(age_calc.error());
  out.age_calc = std::move(*age_calc);
  out.age_summary = age_summary_of(name, *age, *constants, options, so, notes);

  if (!so.datapoint_name.empty()) {
    out.datapoint_name = so.datapoint_name;
  } else {
    out.datapoint_name = facts->material ? name + " " + *facts->material : name;
  }
  return out;
}

}  // namespace

#undef PYCHRON_EARTHBANK_SET
#undef PYCHRON_EARTHBANK_SET_PM

Result<std::vector<Submission>> build_submissions(const std::vector<processing::AnalysisPtr>& analyses,
                                                  const PublishOptions& options) {
  std::map<std::string, Members> by_sample;  // in name order
  for (const pr::AnalysisPtr& analysis : analyses) {
    if (!analysis) return fail(ErrorKind::Config, "earthbank: an analysis is missing");
    if (analysis->sample.empty()) {
      return fail(ErrorKind::Config, "earthbank: analysis " + analysis->runid + " has no sample name");
    }
    by_sample[analysis->sample].push_back({analysis, pr::reduce_analysis(analysis, options.reduction)});
  }

  std::vector<Submission> out;
  std::map<std::string, std::string> named;  // data point name -> sample
  for (auto& [name, members] : by_sample) {
    auto submission = submission_of(name, std::move(members), options);
    if (!submission) return fail(submission.error());
    const auto [first, fresh] = named.emplace(submission->datapoint_name, name);
    if (!fresh) {
      return fail(ErrorKind::Config, "earthbank: samples " + in_quotes(first->second) + " and " + in_quotes(name) +
                                         " have the same data point name " + in_quotes(submission->datapoint_name));
    }
    out.push_back(std::move(*submission));
  }
  return out;
}

}  // namespace pychron::earthbank
