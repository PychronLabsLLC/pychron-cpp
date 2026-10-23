#include "pychron/processing/quantity.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "pychron/processing/domain.hpp"

namespace pychron::processing {

namespace r = pychron::reduction;

namespace {

struct NamedInfo {
  Named named;
  std::string_view name;
  std::string_view label;
  std::string_view units;
};

constexpr NamedInfo kNamed[] = {
    {Named::Timestamp, "timestamp", "Time", ""},
    {Named::Aliquot, "aliquot", "Aliquot", ""},
    {Named::StepIndex, "step_index", "Step", ""},
    {Named::ExtractValue, "extract_value", "Extract value", ""},
    {Named::ExtractDuration, "extract_duration", "Extract duration (s)", "s"},
    {Named::CleanupDuration, "cleanup_duration", "Cleanup (s)", "s"},
    {Named::Weight, "weight", "Weight", ""},
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

// A domain's name: [A-Za-z_][A-Za-z0-9_]*.
bool is_name(std::string_view s) {
  if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s.front())) && s.front() != '_')) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// `key` at a stage core does not have: the domain's, when it knows it.
Result<Term> domain_stage_term(const std::string& s, const std::string& key, const std::string& stage,
                               const Domain* domain) {
  if (!is_name(stage)) return fail(ErrorKind::Config, "quantity: unknown stage '" + stage + "' in '" + s + "'");
  Term t;
  t.kind = Term::Kind::Domain;
  t.key = key;
  t.domain_stage = stage;
  t.label = stage;
  if (domain) {
    const auto info = domain->quantity_info(stage);
    if (!info || !info->stage)
      return fail(ErrorKind::Config, "quantity: unknown stage '" + stage + "' in '" + s + "' (domain " +
                                         std::string(domain->id()) + ")");
    t.domain_stage = info->name;
    t.label = info->label;
    t.units = info->units;
  }
  return t;
}

// A bare name core does not have: the domain's value of that name.
Term domain_name_term(const std::string& name, const Domain* domain) {
  Term t;
  t.kind = Term::Kind::Domain;
  t.name = name;
  t.label = name;
  if (domain) {
    if (const auto info = domain->quantity_info(name); info && !info->stage) {
      t.name = info->name;
      t.label = info->label;
      t.units = info->units;
    }
  }
  return t;
}

Result<Term> parse_term(std::string_view raw, const Domain* domain) {
  const std::string s = trim(raw);
  if (s.empty()) return fail(ErrorKind::Config, "quantity: empty term");
  Term t;
  for (const auto& n : kNamed) {
    if (s == n.name) {
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
    if (!stage) return domain_stage_term(s, head, tail, domain);
    t.kind = Term::Kind::Isotope;
    t.key = head;
    t.stage = *stage;
    return t;
  }
  if (is_isotope_key(s)) {
    t.kind = Term::Kind::Isotope;
    t.key = s;
    t.stage = Stage::IcCorrected;
    return t;
  }
  if (!is_name(s)) return fail(ErrorKind::Config, "quantity: unknown term '" + s + "'");
  return domain_name_term(s, domain);
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
    case Term::Kind::Domain:
      return t.key.empty() ? t.name : t.key + "." + t.domain_stage;
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
    case Term::Kind::Domain:
      return t.key.empty() ? t.label : t.key + " " + t.label;
  }
  return {};
}

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
    case Term::Kind::Domain:
      // The domain's values: what its reduction made, under the quantity's name.
      if (!ra.domain) return std::nullopt;
      return t.key.empty() ? ra.domain->named(t.name) : ra.domain->stage(t.key, t.domain_stage);
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
    case Term::Kind::Named:
      switch (t.named) {
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
  return std::nullopt;
}

bool term_needs_reduction(const Term& t) { return t.kind == Term::Kind::Domain; }

std::string term_units(const Term& t) {
  switch (t.kind) {
    case Term::Kind::Isotope:
      return t.stage == Stage::IcFactor ? "" : "fA";
    case Term::Kind::Named:
      return std::string(named_info(t.named)->units);
    case Term::Kind::Domain:
      return t.units;
    case Term::Kind::Family:
      return "";
  }
  return "";
}

}  // namespace

Result<Quantity> Quantity::parse(std::string_view text, const Domain* domain) {
  Quantity q;
  const auto slash = text.find('/');
  auto num = parse_term(text.substr(0, slash), domain);
  if (!num) return fail(num.error());
  q.numerator_ = *num;
  if (slash != std::string_view::npos) {
    const auto rest = text.substr(slash + 1);
    if (rest.find('/') != std::string_view::npos)
      return fail(ErrorKind::Config, "quantity: at most one '/' in '" + std::string(text) + "'");
    auto den = parse_term(rest, domain);
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

namespace {

struct IsotopeName {
  std::string element;
  int mass = 0;
  std::string text;
};

// "Ar40" -> {"Ar", 40}; nullopt for a detector key ("H1:Ar40") or a non-isotope.
std::optional<IsotopeName> isotope_name(const std::string& key) {
  if (!is_isotope_name(key)) return std::nullopt;
  const auto digits = key.find_first_of("0123456789");
  IsotopeName out{key.substr(0, digits), 0, key};
  if (std::from_chars(key.data() + digits, key.data() + key.size(), out.mass).ec != std::errc{}) return std::nullopt;
  return out;
}

}  // namespace

std::vector<std::string> available_quantities(const std::vector<ReducedPtr>& analyses,
                                              const std::vector<std::string>& domain_quantities) {
  std::set<std::string> isotopes, detectors, envs, peaks;
  bool extract = false;
  std::vector<const DomainReduced*> domains;
  for (const auto& ra : analyses) {
    if (!ra || !ra->analysis) continue;
    const Analysis& a = *ra->analysis;
    for (const auto& iso : a.isotopes) isotopes.insert(iso.key);
    for (const auto& [d, _] : a.gains) detectors.insert(d);
    for (const auto& [k, _] : a.environmentals) envs.insert(k);
    for (const auto& pc : a.peak_centers) peaks.insert(pc.detector);
    if (ra->domain) domains.push_back(ra->domain.get());
    if (a.extraction.value) extract = true;
  }
  std::vector<std::string> out;
  // The domain's names some analysis has a value for (its stages below).
  for (const auto& name : domain_quantities)
    if (std::any_of(domains.begin(), domains.end(), [&](const DomainReduced* d) { return d->named(name).has_value(); }))
      out.push_back(name);
  for (const auto& i : isotopes) out.push_back(i);
  // Ratios within an element, heavier over lighter.
  std::map<std::string, std::vector<IsotopeName>> by_element;
  for (const auto& i : isotopes)
    if (auto n = isotope_name(i)) by_element[n->element].push_back(std::move(*n));
  for (auto& [element, names] : by_element) {
    std::sort(names.begin(), names.end(), [](const IsotopeName& x, const IsotopeName& y) { return x.mass > y.mass; });
    for (std::size_t i = 0; i < names.size(); ++i)
      for (std::size_t j = i + 1; j < names.size(); ++j) {
        std::string ratio = names[i].text;
        ratio.append("/").append(names[j].text);
        out.push_back(std::move(ratio));
      }
  }
  for (const auto& i : isotopes)
    for (const char* st : {"intercept", "baseline", "blank", "ic_factor", "bs_corrected", "bk_corrected"})
      out.push_back(i + "." + st);
  for (const auto& i : isotopes)
    for (const auto& stage : domain_quantities)
      if (std::any_of(domains.begin(), domains.end(),
                      [&](const DomainReduced* d) { return d->stage(i, stage).has_value(); })) {
        std::string text = i;
        text.append(".").append(stage);
        out.push_back(std::move(text));
      }
  for (const auto& d : detectors) out.push_back("gain." + d);
  for (const auto& d : peaks) out.push_back("peak_center." + d);
  for (const auto& e : envs) out.push_back("env." + e);
  if (extract) out.insert(out.end(), {"extract_value", "extract_duration", "cleanup_duration"});
  out.insert(out.end(), {"aliquot", "step_index"});
  return out;
}

}  // namespace pychron::processing
