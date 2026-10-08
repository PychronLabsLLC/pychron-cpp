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

std::string to_digits(double value, int digits) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.*g", digits, value);
  return buffer;
}

// A number in a message: ten significant figures say which it is without the
// tail a sum of two doubles can carry.
std::string shown(double value) { return to_digits(value, 10); }

// For a note: four significant figures are enough to recognise a number.
std::string rounded(double value) { return to_digits(value, 4); }

std::string joined(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& part : parts) out += (out.empty() ? "" : ", ") + part;
  return out;
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
  // Cut to the second, not rounded: half a second before midnight is not the next day.
  return Seconds(std::chrono::seconds(static_cast<std::int64_t>(std::floor(utc_epoch_seconds))));
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
  // The analysis measured the isotope. The reduction puts a zero in the place
  // of one it did not (reduce_analysis), and nothing made from that zero is
  // sent.
  bool measured(r::ArgonIsotope isotope) const {
    return reduced->stage(r::to_string(isotope), pr::Stage::Intercept).has_value();
  }
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

// Where the sample was taken: a latitude and a longitude, degrees.
struct Place {
  double latitude = 0.0, longitude = 0.0;
  bool operator==(const Place&) const = default;
};

// The place the analyses agree on. A place is one analysis's latitude with
// the same analysis's longitude, both finite: the analyses that carry such a
// pair must carry the same one, and half a pair is never joined to another
// analysis's other half. A lone coordinate beside whole pairs is passed over
// when it is the pair's and refused when it is not. No pair on any analysis:
// no place, and a note when there was something of one.
Result<std::optional<Place>> place_of(std::string_view sample, const Members& members, Notes& notes) {
  const auto finite = [](const std::optional<double>& v) { return v && std::isfinite(*v) ? v : std::nullopt; };
  const auto latitude = [&](const Member& m) { return finite(m.a().sample_info.latitude); };
  const auto longitude = [&](const Member& m) { return finite(m.a().sample_info.longitude); };

  const auto place = agreed(
      sample, members, "location (latitude, longitude)",
      [&](const Member& m) -> std::optional<Place> {
        const auto lat = latitude(m), lon = longitude(m);
        return lat && lon ? std::optional(Place{*lat, *lon}) : std::nullopt;
      },
      [](const Place& at) { return shown(at.latitude) + ", " + shown(at.longitude); });
  if (!place) return fail(place.error());

  if (*place) {
    const auto whole = std::find_if(members.begin(), members.end(),
                                    [&](const Member& m) { return latitude(m) && longitude(m); });
    for (const Member& m : members) {
      const auto lat = latitude(m), lon = longitude(m);
      if (lat && lon) continue;
      if (lat && *lat != (*place)->latitude) {
        return refuse(sample, "latitude differs between analyses: " + shown((*place)->latitude) + " (" +
                                  whole->a().runid + ") and " + shown(*lat) + " (" + m.a().runid + ")");
      }
      if (lon && *lon != (*place)->longitude) {
        return refuse(sample, "longitude differs between analyses: " + shown((*place)->longitude) + " (" +
                                  whole->a().runid + ") and " + shown(*lon) + " (" + m.a().runid + ")");
      }
    }
    return place;
  }

  bool any_latitude = false, any_longitude = false, bad_latitude = false, bad_longitude = false;
  for (const Member& m : members) {
    const pr::SampleInfo& info = m.a().sample_info;
    any_latitude = any_latitude || latitude(m).has_value();
    any_longitude = any_longitude || longitude(m).has_value();
    bad_latitude = bad_latitude || (info.latitude && !std::isfinite(*info.latitude));
    bad_longitude = bad_longitude || (info.longitude && !std::isfinite(*info.longitude));
  }
  const std::string of(sample);
  if (bad_latitude && !any_latitude) notes.push_back(of + ": lat is not a finite number and was left out");
  if (bad_longitude && !any_longitude) notes.push_back(of + ": lon is not a finite number and was left out");
  if (any_latitude && any_longitude) {
    notes.push_back(of + ": no analysis has both a latitude and a longitude; no location is sent");
  } else if (any_latitude || any_longitude) {
    notes.push_back(of + ": the sample has a " + (any_latitude ? "latitude" : "longitude") + " and no " +
                    (any_latitude ? "longitude" : "latitude") + "; no location is sent");
  }
  return std::optional<Place>();
}

// What the catalog says about the sample, as far as the analyses agree.
struct Facts {
  std::optional<std::string> material, lithology, unit, igsn;
  std::optional<Place> place;
  std::optional<double> elevation;
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
  auto place = place_of(sample, members, notes);
  if (!place) return fail(place.error());
  facts.place = *place;
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
  std::vector<std::string> heated;     // the names of the aliquots heated in steps, as they first appear
  bool any_step = false, any_fusion = false;

  // "2 step-heated aliquots (20001-01, 20001-02)".
  std::string heated_aliquots() const {
    return std::to_string(heated.size()) + " step-heated aliquots (" + joined(heated) + ")";
  }
};

Group group_of(const Members& dated) {
  Group g;
  g.items.reserve(dated.size());  // `all` and `schedule` point into it
  for (const Member& m : dated) {
    g.items.push_back(pr::DatasetItem{m.reduced, {}, {}});
    if (!m.is_step()) {
      g.any_fusion = true;
      continue;
    }
    g.any_step = true;
    if (std::find(g.heated.begin(), g.heated.end(), m.aliquot_name()) == g.heated.end()) {
      g.heated.push_back(m.aliquot_name());
    }
  }
  for (std::size_t i = 0; i < dated.size(); ++i) {
    g.all.push_back(&g.items[i]);
    if (g.heated.size() == 1 && dated[i].is_step()) g.schedule.push_back(&g.items[i]);
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
  pr::GroupItems used;     // the analyses the age is of
  std::string detail;      // for the note
  std::string facts;       // for the description: a plateau's steps and gas, an isochron's trapped ratio
  // A mean over analyses that do not share one J: whose relative J uncertainty
  // `with_j` holds (the largest among them), and how large it is.
  const pr::ReducedAnalysis* widest_j = nullptr;
  double j_relative = 0.0;
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

// The J of a set of analyses, each of which has one (it has an age).
struct GroupJ {
  const pr::ReducedAnalysis* first = nullptr;   // the first analysis
  const pr::ReducedAnalysis* other = nullptr;   // the first whose J, value or uncertainty, is not that of `first`
  const pr::ReducedAnalysis* widest = nullptr;  // the one with the largest sigma(J)/J; the first of equals
  double relative = 0.0;                        // that sigma(J)/J, as processing::j_relative_error forms it
};

double relative_error(const r::UFloat& j) { return j.nominal() == 0 ? 0.0 : j.std_dev() / std::abs(j.nominal()); }

GroupJ j_of(const pr::GroupItems& items) {
  GroupJ out;
  for (const pr::DatasetItem* item : items) {
    const pr::ReducedAnalysis& reduced = *item->analysis;
    if (!reduced.j) continue;
    if (out.first == nullptr) {
      out.first = out.widest = &reduced;
      out.relative = relative_error(*reduced.j);
      continue;
    }
    const bool same = reduced.j->nominal() == out.first->j->nominal() && reduced.j->std_dev() == out.first->j->std_dev();
    if (!same && out.other == nullptr) out.other = &reduced;
    if (relative_error(*reduced.j) > out.relative) {
      out.widest = &reduced;
      out.relative = relative_error(*reduced.j);
    }
  }
  return out;
}

std::string j_text(const pr::ReducedAnalysis& reduced) {
  return shown(reduced.j->nominal()) + " +/- " + shown(reduced.j->std_dev()) + " (" + reduced.analysis->runid + ")";
}

// The J term of a mean of ages: the relative J uncertainty of the analyses
// the mean is of, and of no other analysis of the sample. Each of those ages
// was made with its own J; when they do not share one, the term is the
// largest relative uncertainty among them, which the J uncertainty of their
// mean cannot exceed however the positions are weighted or correlated.
void add_j_term(SummaryAge& age, double mean, double error) {
  const GroupJ j = j_of(age.used);
  age.j_relative = j.relative;
  if (j.other != nullptr) age.widest_j = j.widest;
  age.with_j = pr::with_external_error(mean, error, j.relative);
}

// Why an age that is one pass through the age equation cannot be made over
// `items`: they do not share one J (processing::integrated_age and
// isochron_age would take the first analysis's for all of them). Empty when
// they do.
std::string j_differs(AgeKind kind, const pr::GroupItems& items) {
  const GroupJ j = j_of(items);
  if (j.other == nullptr) return {};
  return "J differs between the analyses a " + std::string(age_type_name(kind)) + " would be of: " +
         j_text(*j.first) + " and " + j_text(*j.other) +
         "; such an age is made with one J: choose weighted_mean as the preferred age (a mean of the analyses' "
         "ages, each made with its own J), or publish the irradiation positions as separate selections";
}

Candidate plateau_age(const Group& g, const PublishOptions& o) {
  if (g.heated.empty()) return none("no analysis is a heating step");
  if (g.heated.size() > 1) {
    return none(std::to_string(g.heated.size()) + " aliquots were step heated (" + joined(g.heated) +
                "), and a plateau is of one heating experiment");
  }
  const auto found = pr::group_plateau(g.schedule, o.ages.plateau, o.ages.plateau_weighting, o.ages.mean_error);
  if (!found) return none("no run of steps meets the plateau criteria (" + criteria_text(o.ages.plateau) + ")");
  const r::Mean& mean = found->mean.mean;
  SummaryAge out;
  out.kind = AgeKind::Plateau;
  out.age = mean.value;
  out.analytical = mean.error;
  out.used = found->used;
  add_j_term(out, mean.value, mean.error);
  out.n = found->mean.nsteps;
  if (out.n >= 2) {
    out.mswd = mean.mswd;
    out.probability = r::mswd_probability(mean.mswd, static_cast<int>(out.n) - 1);
  }
  const std::string first = found->steps[found->range.first]->analysis->analysis->step();
  const std::string last = found->steps[found->range.last]->analysis->analysis->step();
  const std::string gas = rounded(found->mean.gas_fraction) + "% of the 39ArK";
  out.detail = "steps " + first + "-" + last + ", " + plural(out.n, "step", "steps") + " of " +
               std::to_string(found->steps.size()) + ", " + gas;
  out.facts = (first == last ? "step " + first : "steps " + first + " to " + last) + ", " + gas;
  return out;
}

Candidate weighted_mean_age(const Group& g, const PublishOptions& o) {
  const auto mean = pr::group_weighted_mean(g.all, o.ages.mean_error);
  if (!mean) return none(reason(mean.error()));
  SummaryAge out;
  out.kind = AgeKind::WeightedMean;
  out.age = mean->value;
  out.analytical = mean->error;
  // The mean passes over an age without a usable error (reduction::weighted_mean).
  for (const pr::DatasetItem* item : g.all) {
    const r::UFloat& age = item->analysis->arar->ages->age;
    if (std::isfinite(age.nominal()) && std::isfinite(age.std_dev()) && age.std_dev() != 0.0) out.used.push_back(item);
  }
  add_j_term(out, mean->value, mean->error);
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
  pr::GroupItems used;
  for (const pr::IsochronPoint& point : points) used.push_back(point.item);
  if (std::string differs = j_differs(AgeKind::Isochron, used); !differs.empty()) return none(std::move(differs));
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
  out.used = std::move(used);
  out.detail = "of " + plural(out.n, "analysis", "analyses");
  // The trapped ratio is one over the fit's 36Ar/40Ar intercept, and there is
  // none when that intercept is exactly zero (processing::isochron_age).
  out.facts = plain->fit.intercept != 0
                  ? "trapped 40Ar/36Ar " + to_digits(plain->trapped.value, 6) + " +/- " +
                        to_digits(plain->trapped.error, 2)
                  : std::string("trapped 40Ar/36Ar not defined");
  return out;
}

Candidate integrated_age(const Group& g) {
  if (std::string differs = j_differs(AgeKind::Integrated, g.all); !differs.empty()) return none(std::move(differs));
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
  out.used = g.all;
  out.detail = "the gas of " + plural(out.n, "analysis", "analyses");
  return out;
}

Candidate candidate(AgeKind kind, const Group& g, const PublishOptions& o) {
  switch (kind) {
    case AgeKind::Plateau: return plateau_age(g, o);
    case AgeKind::WeightedMean: return weighted_mean_age(g, o);
    case AgeKind::Isochron: return isochron_age(g, o);
    case AgeKind::Integrated: return integrated_age(g);
  }
  return none("not a kind of age");
}

// The age the sample reports (spec 4.4) and a note of the choice.
Result<SummaryAge> preferred_age(std::string_view sample, const Group& g, const PublishOptions& o,
                                 const SampleOptions& so, Notes& notes) {
  const auto name = [](AgeKind kind) { return std::string(age_type_name(kind)); };
  const auto chosen = [&](const SummaryAge& age, const std::string& why) {
    notes.push_back("preferred age: " + name(age.kind) + " (" + age.detail + "): " + why);
    return age;
  };

  // Several heating experiments on one sample: a plateau and an isochron are
  // of one experiment, and of the two ages that can pool them neither is the
  // one a reader would take for granted. The lab says which.
  const bool pooled = g.heated.size() > 1;
  const std::string pooling = "choose weighted_mean or integrated as its preferred age (either pools the "
                              "aliquots), or publish the aliquots as separate selections";
  if (pooled && !so.preferred_age) {
    return refuse(sample, "it has " + g.heated_aliquots() + " and no summary age is reported for that unasked: " +
                              pooling);
  }
  if (pooled && (*so.preferred_age == AgeKind::Plateau || *so.preferred_age == AgeKind::Isochron)) {
    return refuse(sample, "the " + name(*so.preferred_age) + " asked for cannot be computed: " +
                              std::to_string(g.heated.size()) + " aliquots were step heated (" + joined(g.heated) +
                              "), and a plateau or an isochron is of one heating experiment: " + pooling);
  }

  if (so.preferred_age) {
    const Candidate asked = candidate(*so.preferred_age, g, o);
    if (!asked) return refuse(sample, "the " + name(*so.preferred_age) + " asked for cannot be computed: " + asked.error());
    return chosen(*asked, pooled ? "chosen in the options; the age pools " + g.heated_aliquots()
                                 : std::string("chosen in the options"));
  }
  if (g.any_step) {
    const Candidate plateau = plateau_age(g, o);
    if (plateau) return chosen(*plateau, "the sample was step heated and has a plateau");
    const Candidate integrated = integrated_age(g);
    if (!integrated) return refuse(sample, "no age to report: " + plateau.error() + "; " + integrated.error());
    return chosen(*integrated, "the sample was step heated and has no plateau: " + plateau.error());
  }
  const Candidate mean = weighted_mean_age(g, o);
  if (!mean) return refuse(sample, "no age to report: " + mean.error());
  return chosen(*mean, "the sample's analyses are fusions");
}

// What the reported age is and what its uncertainty holds, in one sentence of
// facts: it is the age summary's description, the only place the record can
// say which error a mean carries and whether J is in it (the uncertainty type
// is the sigma level and nothing more). `published` is the number of
// analyses sent.
std::string description_of(const SummaryAge& age, const Group& g, std::size_t published, const PublishOptions& o) {
  std::string out = std::string(age_type_name(age.kind)) + " of " + std::to_string(age.n) + " of " +
                    plural(published, "analysis", "analyses");
  if (g.heated.size() > 1) out += " of " + std::to_string(g.heated.size()) + " step-heated aliquots";
  if (!age.facts.empty()) out += " (" + age.facts + ")";
  out += "; uncertainty " + std::string(kOneSigma);
  if (age.error_kind) {
    out += " (the mean's " + std::string(r::to_string(o.ages.mean_error)) + ")";
  } else if (age.kind == AgeKind::Isochron) {
    out += " (propagated through the regression and the age equation)";
  } else {
    out += " (propagated through the age equation)";
  }
  if (!o.age_includes_j_error) {
    out += ", analytical uncertainty only";
  } else {
    out += ", J uncertainty included";
    if (age.widest_j != nullptr) {
      out += " (the largest relative J uncertainty among its analyses, " + rounded(age.j_relative * 100.0) + "% of " +
             age.widest_j->analysis->runid + ")";
    }
  }
  out += "; reduced with " + (o.software.empty() ? std::string("pychron-cpp") : o.software) + ".";
  return out;
}

// ---------------------------------------------------------------- 39ArK released

struct Released {
  double percent = 0.0, cumulative = 0.0;
};

// Each step's share of the 39ArK its aliquot released, alone and summed in
// step order, as the report's "% of group" and "cumulative" columns: a step
// without gas (39ArK not above zero) takes no part. The shares are of the
// steps published, so they are given only for an aliquot whose schedule is
// whole: when a step of it is not among the published analyses (a letter is
// missing from the run A, B, C, ..., or an analysis given was left out), the
// total is of a schedule the reader is not shown, and the two fields are left
// out for that aliquot with a note naming the steps. `given` is every
// analysis of the sample, `published` those that are sent. A fusion has no
// share.
std::map<const pr::ReducedAnalysis*, Released> released_39(const Members& given, const Members& published,
                                                           Notes& notes) {
  struct Schedule {
    int last = -1;                     // the last increment among the steps given
    std::vector<const Member*> steps;  // the steps published
  };
  std::map<std::pair<std::string, int>, Schedule> schedules;
  for (const Member& m : given) {
    if (m.is_step()) schedules[m.aliquot()].last = std::max(schedules[m.aliquot()].last, m.a().increment);
  }
  for (const Member& m : published) {
    if (m.is_step()) schedules[m.aliquot()].steps.push_back(&m);
  }
  std::map<const pr::ReducedAnalysis*, Released> out;
  for (auto& [aliquot, schedule] : schedules) {
    std::vector<const Member*>& steps = schedule.steps;
    if (steps.empty()) continue;  // nothing of the aliquot is published
    std::string missing;
    std::size_t count = 0;
    for (int increment = 0; increment <= schedule.last; ++increment) {
      const bool there = std::any_of(steps.begin(), steps.end(),
                                     [increment](const Member* m) { return m->a().increment == increment; });
      if (there) continue;
      missing += (missing.empty() ? "" : ", ") + pr::step_letters(increment);
      ++count;
    }
    if (count > 0) {
      notes.push_back(steps.front()->aliquot_name() + ": " + (count == 1 ? "step " : "steps ") + missing +
                      (count == 1 ? " is" : " are") +
                      " not among the analyses published, so ar39KReleased and cumulative39ArK were left out for "
                      "the aliquot");
      continue;
    }
    std::stable_sort(steps.begin(), steps.end(), [](const Member* x, const Member* y) {
      if (x->a().increment != y->a().increment) return x->a().increment < y->a().increment;
      return x->a().timestamp < y->a().timestamp;
    });
    // A published analysis has an age, and so a reduction.
    const auto k39 = [](const Member* m) { return m->reduced->arar->f.interference.k39.nominal(); };
    double total = 0.0;
    for (const Member* m : steps) {
      if (k39(m) > 0) total += k39(m);
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

// What the service calls an isotope "corrected": for mass discrimination, for
// decay (39Ar and 37Ar) and for interferences. For four of them that is the
// reduction's interference-corrected stage: 40Ar less the K-derived 40Ar
// (40Ar* and trapped 40Ar), 39ArK, 37Ar decay corrected, and 36Ar less the
// Ca- and Cl-derived 36Ar. That stage takes nothing from 38Ar, so 38Ar is
// formed here from the reduction's own terms: the K- and Ca-derived 38Ar are
// removed; the atmospheric and the chlorine-derived 38Ar are no interference
// and stay. Nothing for an isotope the analysis did not measure.
std::optional<r::UFloat> corrected(const Member& m, r::ArgonIsotope isotope) {
  if (!m.measured(isotope)) return std::nullopt;
  if (isotope != r::ArgonIsotope::Ar38) return m.reduced->stage(r::to_string(isotope), pr::Stage::InterferenceCorrected);
  const r::ArArResult& arar = *m.reduced->arar;
  return arar.corrected[r::index(isotope)] - arar.f.interference.k38 - arar.f.interference.ca38;
}

// The intensity the service calls "excluding interference correction": blank,
// discrimination, detector factors and abundance sensitivity applied, and, for
// 39Ar only, the decay since irradiation (its definitions say "mass
// discrimination only" of 40Ar, 38Ar, 37Ar and 36Ar, "and decay only" of
// 39Ar). For 40Ar, 39Ar, 38Ar and 36Ar that is the reduction's decay stage
// (ArArResult::corrected), which only 39Ar and 37Ar decay in; for 37Ar it is
// the same stage with the decay factor, an exact number, divided out again,
// so that the five have had the same done to them but for 39Ar's decay.
std::optional<r::UFloat> excluding_interferences(const Member& m, r::ArgonIsotope isotope) {
  if (!m.measured(isotope)) return std::nullopt;
  const auto stage = m.reduced->stage(r::to_string(isotope), pr::Stage::DecayCorrected);
  if (!stage || isotope != r::ArgonIsotope::Ar37) return stage;
  return *stage / m.reduced->arar->decay.df37;
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

// The "step included" column of the kind of age the sample reports; the
// total gas age has none.
Text Measurement::* step_column(AgeKind kind) {
  switch (kind) {
    case AgeKind::Plateau: return &Measurement::plateauStepName;
    case AgeKind::WeightedMean: return &Measurement::weightedMeanStepName;
    case AgeKind::Isochron: return &Measurement::inverseIsoStepName;
    case AgeKind::Integrated: break;
  }
  return nullptr;
}

// One analysis. `in_reported_age`: the age the sample reports used it.
// `to_ma` scales its age to Ma.
Measurement measurement_of(const Member& m, const Released* released, const SummaryAge& reported, bool in_reported_age,
                           double to_ma, Notes& notes) {
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
  // beamSize is not sent: ExtractionInfo::beam_diameter is recorded without
  // its unit (legacy pychron called it microns; a queue may give mm), and a
  // size a thousand times off is worse than none.

  // An isotope the analysis did not measure has no stage, and its fields stay empty.
  for (const IsotopeFields& f : kIsotopeFields) {
    numbers.set(out.*f.corrected, out.*f.corrected_error, f.corrected_name, corrected(m, f.isotope));
    numbers.set(out.*f.exc_int, out.*f.exc_int_error, f.exc_int_name, excluding_interferences(m, f.isotope));
    numbers.set(out.*f.blank, out.*f.blank_error, f.blank_name,
                reduced.stage(r::to_string(f.isotope), pr::Stage::Blank));
  }
  const auto k39 = corrected(m, r::ArgonIsotope::Ar39);
  for (const RatioFields& f : kRatioFields) {
    const auto top = corrected(m, f.isotope);
    if (top && k39) numbers.set(out.*f.ratio, out.*f.ratio_error, f.name, *top / *k39);
  }

  PYCHRON_EARTHBANK_SET_PM(numbers, out, ratio40Arx39ArK, arar.f.f);
  // 40Ar* in percent of the corrected 40Ar (40Ar* and trapped 40Ar), as the
  // service defines it; the reduction's own yield is of all the 40Ar, the
  // K-derived 40Ar included.
  if (const auto ar40 = corrected(m, r::ArgonIsotope::Ar40); ar40 && ar40->nominal() != 0.0) {
    PYCHRON_EARTHBANK_SET(numbers, out, radiogenicArgon, (arar.f.rad40 / *ar40 * 100.0).nominal());
  }
  // Ca/K and K/Ca are of 37Ar and Cl/K is of 38Ar: without the isotope they
  // would be of the zero put in its place.
  if (m.measured(r::ArgonIsotope::Ar37)) {
    PYCHRON_EARTHBANK_SET_PM(numbers, out, caK, arar.cak);
    PYCHRON_EARTHBANK_SET_PM(numbers, out, kCa, arar.kca);
  } else {
    notes.push_back(a.runid + ": Ar37 was not measured; its own fields, ratio37Arx39ArCorr, caK and kCa were left out "
                              "(the reduction made its Ca corrections with a 37Ar of zero)");
  }
  if (m.measured(r::ArgonIsotope::Ar38)) {
    PYCHRON_EARTHBANK_SET_PM(numbers, out, clK, arar.clk);
  } else {
    notes.push_back(a.runid + ": Ar38 was not measured; its own fields, ratio38Arx39ArCorr and clK were left out "
                              "(the reduction made its chlorine correction of 36Ar with a 38Ar of zero)");
  }
  if (released != nullptr) {
    PYCHRON_EARTHBANK_SET(numbers, out, ar39KReleased, released->percent);
    PYCHRON_EARTHBANK_SET(numbers, out, cumulative39ArK, released->cumulative);
  }

  PYCHRON_EARTHBANK_SET_PM(numbers, out, jValue, reduced.j);
  out.jvalueUncertaintyTypeName = std::string(kOneSigma);
  PYCHRON_EARTHBANK_SET_PM(numbers, out, apparentAge, arar.ages->age, to_ma);
  out.apparentAgeUncertainityTypeName = std::string(kOneSigma);

  // Whether the reported age used this analysis, in the column of that kind
  // of age; the columns of the kinds not reported stay empty.
  if (const auto column = step_column(reported.kind)) out.*column = std::string(in_reported_age ? kYes : kNo);
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

// The place the analyses agree on, or no location (place_of).
std::optional<Location> location_of(const Facts& facts) {
  if (!facts.place) return std::nullopt;
  Location out;
  out.lat = facts.place->latitude;
  out.lon = facts.place->longitude;
  return out;
}

DataPoint data_point_of(const std::string& name, const Members& dated, const Group& g, const Facts& facts,
                        Notes& notes) {
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
  // The two types are of the measurements' own numbers (the service: "quoted
  // for measurements and associated ratios", "quoted for ages"), which are
  // plain one sigma. The error kind of the sample's mean is said where the
  // mean is: in the age summary's description.
  out.analyticalUncertaintyTypeName = std::string(kOneSigma);
  out.analyticalUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  out.apparentAgeUncertaintyTypeName = std::string(kOneSigma);
  out.apparentAgeUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  out.jvalueUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  return out;
}

// One per loaded piece, in the order they first appear among the published
// analyses, with its weight in mg as its size when the analyses of the piece
// (published or not) carry one: a weight that is not above zero is no weight,
// and two analyses of one piece that give two weights are refused.
Result<std::vector<Aliquot>> aliquots_of(std::string_view sample, const Members& given, const Members& published,
                                         Notes& notes) {
  std::vector<Aliquot> out;
  std::vector<std::string> seen;
  for (const Member& m : published) {
    const std::string name = m.aliquot_name();
    if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
    seen.push_back(name);
    Aliquot& aliquot = out.emplace_back();
    aliquot.aliquotName = name;

    Members piece;
    for (const Member& other : given) {
      if (other.aliquot() == m.aliquot()) piece.push_back(other);
    }
    bool not_finite = false;
    const auto weight = agreed(
        sample, piece, "aliquot size of " + name,
        [&not_finite](const Member& of) -> std::optional<double> {
          const std::optional<double>& w = of.a().extraction.weight;
          if (w && !std::isfinite(*w)) not_finite = true;
          return w && std::isfinite(*w) && *w > 0.0 ? w : std::nullopt;
        },
        [](double w) { return shown(w); });
    if (!weight) return fail(weight.error());
    if (*weight) {
      aliquot.aliquotSize = **weight;
      aliquot.aliquotUnits = std::string(kMilligrams);
    } else if (not_finite) {
      notes.push_back(name + ": aliquotSize is not a finite number and was left out");
    }
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

AgeSummary age_summary_of(const std::string& name, const SummaryAge& age, const Group& g, std::size_t published,
                          const Constants& constants, const PublishOptions& options, const SampleOptions& so,
                          Notes& notes) {
  Numbers numbers(name, notes);
  AgeSummary out;
  PYCHRON_EARTHBANK_SET(numbers, out, age, age.age * constants.to_ma);
  const double sigma = options.age_includes_j_error ? age.with_j : age.analytical;
  PYCHRON_EARTHBANK_SET(numbers, out, ageUncertainty, sigma * constants.to_ma);
  out.ageCalcTypeName = std::string(age_type_name(age.kind));
  // The type is the sigma level. What kind of one sigma it is (a mean's error
  // kind, or propagated; with J or without) is in the description.
  out.ageUncertaintyTypeName = std::string(kOneSigma);
  out.calculatedAgeUncertaintyTypeName = std::string(kOneSigma);
  out.calculatedAgeUncertaintyTypeUnitsName = std::string(kAbsoluteUncertainty);
  if (age.mswd) PYCHRON_EARTHBANK_SET(numbers, out, mswd, *age.mswd);
  if (age.probability) PYCHRON_EARTHBANK_SET(numbers, out, agepvalue, *age.probability);
  out.nAnalysisGrouped = static_cast<std::int64_t>(age.n);
  out.preferredAge = true;
  out.dataInterpretationToolName = std::string(kInterpretationTool);
  out.interpretationName = so.interpretation.empty() ? std::string(kUnknownInterpretation) : so.interpretation;
  out.description = description_of(age, g, published, options);

  notes.push_back(std::string("the reported age's uncertainty is one sigma ") +
                  (options.age_includes_j_error ? "with the J uncertainty (" : "without the J uncertainty (") +
                  rounded(age.analytical * constants.to_ma) + " Ma analytical, " +
                  rounded(age.with_j * constants.to_ma) + " Ma with J)");
  if (options.age_includes_j_error && age.widest_j != nullptr) {
    notes.push_back("the analyses of the reported age do not share one J; the J uncertainty in the age's is the "
                    "largest relative one among them, " + rounded(age.j_relative * 100.0) + "% of " +
                    age.widest_j->analysis->runid);
  }
  notes.push_back(*out.description);
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

  // Only an analysis with an age is published, and only one that measured
  // the three isotopes an age is made of: where one is missing the reduction
  // worked with a zero in its place (without 36Ar it gives an age all the
  // same, as if there were no atmospheric argon), and that is not a result.
  Members dated;
  std::string undated, unmeasured;
  std::size_t undated_count = 0, unmeasured_count = 0;
  for (const Member& m : members) {
    std::string lacking;
    for (const r::ArgonIsotope isotope : {r::ArgonIsotope::Ar40, r::ArgonIsotope::Ar39, r::ArgonIsotope::Ar36}) {
      if (!m.measured(isotope)) lacking += (lacking.empty() ? "" : ", ") + std::string(r::to_string(isotope));
    }
    if (!lacking.empty()) {
      unmeasured += (unmeasured.empty() ? "" : ", ") + m.a().runid + " (" + lacking + " not measured)";
      ++unmeasured_count;
    } else if (!m.has_age()) {
      const std::string& why = m.reduced->reduction_error;
      undated += (undated.empty() ? "" : ", ") + m.a().runid + (why.empty() ? "" : " (" + why + ")");
      ++undated_count;
    } else {
      dated.push_back(m);
    }
  }
  const std::string of_all = plural(members.size(), "analysis", "analyses");
  if (dated.empty() && unmeasured_count == 0) return refuse(name, "none of its " + of_all + " has an age: " + undated);
  if (dated.empty()) {
    return refuse(name, "none of its " + of_all + " can be published: " + unmeasured +
                            (undated.empty() ? "" : ", " + undated) +
                            "; an analysis needs Ar40, Ar39 and Ar36 measured, and an age");
  }
  if (undated_count > 0) {
    notes.push_back(plural(undated_count, "analysis", "analyses") + " of " + std::to_string(members.size()) +
                    " without an age left out: " + undated);
  }
  if (unmeasured_count > 0) {
    notes.push_back(plural(unmeasured_count, "analysis", "analyses") + " of " + std::to_string(members.size()) +
                    " left out for want of an isotope the age is made of: " + unmeasured);
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

  out.sample = sample_of(name, *facts, options, notes);
  out.location = location_of(*facts);
  out.data_point = data_point_of(name, dated, group, *facts, notes);
  auto aliquots = aliquots_of(name, members, dated, notes);
  if (!aliquots) return fail(aliquots.error());
  out.aliquots = std::move(*aliquots);

  const auto released = released_39(members, dated, notes);
  for (const Member& m : dated) {
    const auto share = released.find(m.reduced.get());
    const bool used = std::any_of(age->used.begin(), age->used.end(), [&](const pr::DatasetItem* item) {
      return item->analysis.get() == m.reduced.get();
    });
    out.measurements.push_back(
        measurement_of(m, share == released.end() ? nullptr : &share->second, *age, used, constants->to_ma, notes));
  }

  auto age_calc = age_calc_of(name, dated, *constants, options, so, notes);
  if (!age_calc) return fail(age_calc.error());
  out.age_calc = std::move(*age_calc);
  out.age_summary = age_summary_of(name, *age, group, dated.size(), *constants, options, so, notes);

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
