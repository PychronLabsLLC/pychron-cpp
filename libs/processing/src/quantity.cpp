#include "pychron/processing/quantity.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

struct NamedInfo {
  Named named;
  std::string_view name;
  std::string_view label;
  bool reduced;  // needs reduce()
};

constexpr NamedInfo kNamed[] = {
    {Named::Age, "age", "Age", true},
    {Named::AgeWithJ, "age_w_j", "Age (with J error)", true},
    {Named::AgeWithPosition, "age_w_position", "Age (with position error)", true},
    {Named::F, "F", "40Ar*/39ArK", true},
    {Named::KCa, "kca", "K/Ca", true},
    {Named::CaK, "cak", "Ca/K", true},
    {Named::KCl, "kcl", "K/Cl", true},
    {Named::ClK, "clk", "Cl/K", true},
    {Named::RadiogenicYield, "radiogenic_yield", "%40Ar*", true},
    {Named::Rad40, "rad40", "40Ar*", true},
    {Named::K39, "k39", "39ArK", true},
    {Named::J, "j", "J", false},
    {Named::Timestamp, "timestamp", "Time", false},
    {Named::Aliquot, "aliquot", "Aliquot", false},
    {Named::StepIndex, "step_index", "Step", false},
    {Named::ExtractValue, "extract_value", "Extract value", false},
    {Named::ExtractDuration, "extract_duration", "Extract duration (s)", false},
    {Named::CleanupDuration, "cleanup_duration", "Cleanup (s)", false},
    {Named::Weight, "weight", "Weight", false},
};

struct FamilyInfo {
  Family family;
  std::string_view name;
  std::string_view label;
};

constexpr FamilyInfo kFamilies[] = {
    {Family::Gain, "gain", "gain"},
    {Family::Deflection, "deflection", "deflection"},
    {Family::Source, "source", "source"},
    {Family::Environmental, "env", ""},
    {Family::PeakCenter, "peak_center", "peak center"},
};

const NamedInfo* named_info(Named n) {
  for (const auto& i : kNamed)
    if (i.named == n) return &i;
  return nullptr;
}

const FamilyInfo* family_info(Family f) {
  for (const auto& i : kFamilies)
    if (i.family == f) return &i;
  return nullptr;
}

std::string trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}

// [A-Z][a-z]?[0-9]{1,3}, e.g. Ar40, He4, Ne20.
bool is_isotope_name(std::string_view s) {
  std::size_t i = 0;
  if (i >= s.size() || !std::isupper(static_cast<unsigned char>(s[i]))) return false;
  ++i;
  if (i < s.size() && std::islower(static_cast<unsigned char>(s[i]))) ++i;
  const std::size_t digits = s.size() - i;
  if (digits < 1 || digits > 3) return false;
  for (; i < s.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

bool is_identifier(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
  return true;
}

// "H1:Ar40" or "Ar40".
bool is_isotope_key(std::string_view s) {
  const auto colon = s.find(':');
  if (colon == std::string_view::npos) return is_isotope_name(s);
  return is_identifier(s.substr(0, colon)) && is_isotope_name(s.substr(colon + 1));
}

Result<Term> parse_term(std::string_view raw) {
  const std::string s = trim(raw);
  if (s.empty()) return fail(ErrorKind::Config, "quantity: empty term");
  Term t;
  for (const auto& n : kNamed) {
    if (s == n.name || (n.named == Named::F && s == "f") ||
        (n.named == Named::RadiogenicYield && s == "rad40_percent")) {
      t.kind = Term::Kind::Named;
      t.named = n.named;
      return t;
    }
  }
  const auto dot = s.rfind('.');
  if (dot != std::string::npos) {
    const std::string head = s.substr(0, dot), tail = s.substr(dot + 1);
    for (const auto& f : kFamilies) {
      if (head == f.name) {
        if (!is_identifier(tail)) return fail(ErrorKind::Config, "quantity: bad key '" + tail + "' in '" + s + "'");
        t.kind = Term::Kind::Family;
        t.family = f.family;
        t.key = tail;
        return t;
      }
    }
    if (!is_isotope_key(head)) return fail(ErrorKind::Config, "quantity: unknown term '" + s + "'");
    const auto stage = parse_stage(tail);
    if (!stage) return fail(ErrorKind::Config, "quantity: unknown stage '" + tail + "' in '" + s + "'");
    t.kind = Term::Kind::Isotope;
    t.key = head;
    t.stage = *stage;
    return t;
  }
  if (!is_isotope_key(s)) return fail(ErrorKind::Config, "quantity: unknown term '" + s + "'");
  t.kind = Term::Kind::Isotope;
  t.key = s;
  t.stage = Stage::IcCorrected;
  return t;
}

std::string term_text(const Term& t) {
  switch (t.kind) {
    case Term::Kind::Named:
      return std::string(named_info(t.named)->name);
    case Term::Kind::Family:
      return std::string(family_info(t.family)->name) + "." + t.key;
    case Term::Kind::Isotope:
      if (t.stage == Stage::IcCorrected) return t.key;
      return t.key + "." + std::string(to_string(t.stage));
  }
  return {};
}

std::string stage_label(Stage s) {
  switch (s) {
    case Stage::Intercept:
      return "intercept";
    case Stage::Baseline:
      return "baseline";
    case Stage::Blank:
      return "blank";
    case Stage::IcFactor:
      return "IC factor";
    case Stage::BaselineCorrected:
      return "bs corrected";
    case Stage::BlankCorrected:
      return "bk corrected";
    case Stage::IcCorrected:
      return "";
    case Stage::DecayCorrected:
      return "decay corrected";
    case Stage::InterferenceCorrected:
      return "interference corrected";
  }
  return "";
}

std::string term_label(const Term& t) {
  switch (t.kind) {
    case Term::Kind::Named:
      return std::string(named_info(t.named)->label);
    case Term::Kind::Family: {
      const auto* f = family_info(t.family);
      if (t.family == Family::Environmental) return t.key;
      return t.key + " " + std::string(f->label);
    }
    case Term::Kind::Isotope: {
      const std::string st = stage_label(t.stage);
      return st.empty() ? t.key : t.key + " " + st;
    }
  }
  return {};
}

std::optional<r::UFloat> opt(const std::optional<r::UFloat>& v) { return v; }

std::optional<r::UFloat> exact(std::optional<double> v) {
  if (!v) return std::nullopt;
  return r::UFloat(*v);
}

std::optional<r::UFloat> from_map(const std::map<std::string, double>& m, const std::string& key) {
  auto it = m.find(key);
  if (it == m.end()) return std::nullopt;
  return r::UFloat(it->second);
}

std::optional<r::UFloat> eval_term(const Term& t, const ReducedAnalysis& ra) {
  const Analysis* a = ra.analysis.get();
  if (!a) return std::nullopt;
  switch (t.kind) {
    case Term::Kind::Isotope:
      return ra.stage(t.key, t.stage);
    case Term::Kind::Family:
      switch (t.family) {
        case Family::Gain:
          return from_map(a->gains, t.key);
        case Family::Deflection:
          return from_map(a->deflections, t.key);
        case Family::Source:
          return from_map(a->source, t.key);
        case Family::Environmental:
          return from_map(a->environmentals, t.key);
        case Family::PeakCenter:
          for (const auto& pc : a->peak_centers)
            if (pc.detector == t.key) return r::UFloat(pc.center);
          return std::nullopt;
      }
      return std::nullopt;
    case Term::Kind::Named: {
      const auto& ar = ra.arar;
      switch (t.named) {
        case Named::Age:
          if (ar && ar->ages) return ar->ages->age;
          return std::nullopt;
        case Named::AgeWithJ:
          if (ar && ar->ages) return ar->ages->age_w_j_err;
          return std::nullopt;
        case Named::AgeWithPosition:
          if (ar && ar->ages) return ar->ages->age_w_position_err;
          return std::nullopt;
        case Named::F:
          if (ar) return opt(ar->f.f);
          return std::nullopt;
        case Named::KCa:
          if (ar) return opt(ar->kca);
          return std::nullopt;
        case Named::CaK:
          if (ar) return opt(ar->cak);
          return std::nullopt;
        case Named::KCl:
          if (ar) return opt(ar->kcl);
          return std::nullopt;
        case Named::ClK:
          if (ar) return opt(ar->clk);
          return std::nullopt;
        case Named::RadiogenicYield:
          if (ar) return opt(ar->f.radiogenic_yield);
          return std::nullopt;
        case Named::Rad40:
          if (ar) return ar->f.rad40;
          return std::nullopt;
        case Named::K39:
          if (ar) return ar->f.interference_corrected[r::index(r::ArgonIsotope::Ar39)];
          return std::nullopt;
        case Named::J:
          if (!a->context.flux) return std::nullopt;
          // An unknown J or J error is NaN, not a variable (which needs a finite sigma).
          if (const auto& j = a->context.flux->j; !std::isfinite(j.value) || !std::isfinite(j.error))
            return r::UFloat(std::numeric_limits<double>::quiet_NaN());
          return r::UFloat::variable(a->context.flux->j.value, a->context.flux->j.error, "J");
        case Named::Timestamp:
          return r::UFloat(a->timestamp);
        case Named::Aliquot:
          return r::UFloat(static_cast<double>(a->aliquot));
        case Named::StepIndex:
          if (a->increment < 0) return std::nullopt;
          return r::UFloat(static_cast<double>(a->increment));
        case Named::ExtractValue:
          return exact(a->extraction.value);
        case Named::ExtractDuration:
          return exact(a->extraction.duration);
        case Named::CleanupDuration:
          return exact(a->extraction.cleanup);
        case Named::Weight:
          return exact(a->extraction.weight);
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool term_needs_reduction(const Term& t) {
  if (t.kind == Term::Kind::Named) return named_info(t.named)->reduced;
  if (t.kind == Term::Kind::Isotope)
    return t.stage == Stage::DecayCorrected || t.stage == Stage::InterferenceCorrected;
  return false;
}

std::string term_units(const Term& t) {
  if (t.kind == Term::Kind::Isotope) return t.stage == Stage::IcFactor ? "" : "fA";
  if (t.kind == Term::Kind::Named) {
    switch (t.named) {
      case Named::Age:
      case Named::AgeWithJ:
      case Named::AgeWithPosition:
        return "Ma";
      case Named::RadiogenicYield:
        return "%";
      case Named::Rad40:
      case Named::K39:
        return "fA";
      case Named::ExtractDuration:
      case Named::CleanupDuration:
        return "s";
      default:
        return "";
    }
  }
  return "";
}

}  // namespace

Result<Quantity> Quantity::parse(std::string_view text) {
  Quantity q;
  const auto slash = text.find('/');
  auto num = parse_term(text.substr(0, slash));
  if (!num) return fail(num.error());
  q.numerator_ = *num;
  if (slash != std::string_view::npos) {
    const auto rest = text.substr(slash + 1);
    if (rest.find('/') != std::string_view::npos)
      return fail(ErrorKind::Config, "quantity: at most one '/' in '" + std::string(text) + "'");
    auto den = parse_term(rest);
    if (!den) return fail(den.error());
    q.denominator_ = *den;
  }
  q.text_ = term_text(q.numerator_);
  if (q.denominator_) q.text_ += "/" + term_text(*q.denominator_);
  return q;
}

std::string Quantity::label(bool with_units) const {
  if (denominator_) return term_label(numerator_) + "/" + term_label(*denominator_);
  const std::string u = with_units ? units() : std::string();
  return u.empty() ? term_label(numerator_) : term_label(numerator_) + " (" + u + ")";
}

std::string Quantity::units() const {
  if (denominator_) return "";
  return term_units(numerator_);
}

bool Quantity::needs_reduction() const {
  return term_needs_reduction(numerator_) || (denominator_ && term_needs_reduction(*denominator_));
}

std::optional<Value> Quantity::eval(const ReducedAnalysis& a) const {
  auto n = eval_term(numerator_, a);
  if (!n) return std::nullopt;
  r::UFloat v = *n;
  if (denominator_) {
    auto d = eval_term(*denominator_, a);
    if (!d || d->nominal() == 0.0) return std::nullopt;
    v = v / *d;
  }
  if (!std::isfinite(v.nominal())) return std::nullopt;
  const double e = v.std_dev();
  return Value{v.nominal(), std::isfinite(e) ? e : 0.0};
}

std::vector<std::string> available_quantities(const std::vector<ReducedPtr>& analyses) {
  std::set<std::string> isotopes, detectors, envs, peaks;
  bool reduced = false, ages = false, extract = false;
  for (const auto& ra : analyses) {
    if (!ra || !ra->analysis) continue;
    const Analysis& a = *ra->analysis;
    for (const auto& iso : a.isotopes) isotopes.insert(iso.key);
    for (const auto& [d, _] : a.gains) detectors.insert(d);
    for (const auto& [k, _] : a.environmentals) envs.insert(k);
    for (const auto& pc : a.peak_centers) peaks.insert(pc.detector);
    if (ra->arar) reduced = true;
    if (ra->arar && ra->arar->ages) ages = true;
    if (a.extraction.value) extract = true;
  }
  std::vector<std::string> out;
  if (ages) {
    out.insert(out.end(), {"age", "age_w_j"});
  }
  if (reduced) out.insert(out.end(), {"F", "kca", "kcl", "radiogenic_yield", "rad40", "k39"});
  for (const auto& i : isotopes) out.push_back(i);
  const char* kArgon[] = {"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"};
  for (std::size_t i = 0; i < 5; ++i)
    for (std::size_t j = i + 1; j < 5; ++j)
      if (isotopes.count(kArgon[i]) && isotopes.count(kArgon[j]))
        out.push_back(std::string(kArgon[i]) + "/" + kArgon[j]);
  for (const auto& i : isotopes)
    for (const char* st : {"intercept", "baseline", "blank", "ic_factor", "bs_corrected", "bk_corrected"})
      out.push_back(i + "." + st);
  for (const auto& d : detectors) out.push_back("gain." + d);
  for (const auto& d : peaks) out.push_back("peak_center." + d);
  for (const auto& e : envs) out.push_back("env." + e);
  if (extract) out.insert(out.end(), {"extract_value", "extract_duration", "cleanup_duration"});
  out.insert(out.end(), {"aliquot", "step_index"});
  return out;
}

}  // namespace pychron::processing
