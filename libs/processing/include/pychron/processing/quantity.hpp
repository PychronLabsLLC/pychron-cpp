#pragma once

// Quantities (design section 10): one vocabulary for figure panels, filter
// rules, browser columns and statistics.
//
//   quantity := term ( "/" term )?
//   term     := isotope ( "." stage )?         Ar40, Ar40.bs_corrected, H1:Ar40.intercept
//             | name                           extract_value, timestamp, ..., or a domain's
//             | family "." key                 gain.H1, deflection.H1, source.trap,
//                                              env.lab_temperature, peak_center.H1
//
// An isotope without a stage is ic_corrected (legacy get_intensity). A name
// core does not have, and an isotope's stage core does not have, are the
// domain's (domain seam spec 3.4): they evaluate through the analysis's
// DomainReduced, and have no value without one. Ratios divide UFloats, so
// correlated stages propagate correctly. eval() returns nullopt when the
// analysis lacks the quantity; never 0.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/reduced.hpp"

namespace pychron::processing {

class Domain;  // domain.hpp

// Core's names: what every analysis has, whatever its domain.
enum class Named {
  Timestamp,
  Aliquot,
  StepIndex,
  ExtractValue,
  ExtractDuration,
  CleanupDuration,
  Weight,
};

enum class Family { Gain, Deflection, Source, Environmental, PeakCenter };

struct Term {
  // Isotope: `key` at core's `stage`. Named: core's `named`. Family: `family`
  // at `key`. Domain: the domain's value `name` (`key` empty), or isotope
  // `key` at the domain's stage `domain_stage`.
  enum class Kind { Isotope, Named, Family, Domain } kind = Kind::Isotope;
  std::string key;  // isotope key, or the family key ("H1")
  Stage stage = Stage::IcCorrected;
  std::string name;          // Domain: a bare name
  std::string domain_stage;  // Domain: a stage of the domain's, after the isotope key
  Named named = Named::Timestamp;
  Family family = Family::Gain;
  // Domain: what the domain at parse said (QuantityInfo); without one, the
  // name or stage as written and no units.
  std::string label, units;
  friend bool operator==(const Term&, const Term&) = default;
};

class Quantity {
 public:
  // Fails (Config, "quantity: ...") naming the bad part: malformed text, an
  // isotope's stage that is not a name (or, given `domain`, neither core's nor
  // the domain's), a family key that is not one. A bare name never fails: one
  // that is not core's is the domain's, and has no value where the domain has
  // none. `domain` (may be null) gives a domain term its canonical spelling,
  // label and units.
  [[nodiscard]] static Result<Quantity> parse(std::string_view text, const Domain* domain = nullptr);

  const std::string& text() const noexcept { return text_; }  // canonical form
  // "Ar40/Ar36", "Ar40 intercept (fA)"; without units: "Ar40 intercept". A
  // domain's term reads as the domain at parse said (QuantityInfo).
  std::string label(bool with_units = true) const;
  std::string units() const;   // "fA", "s", a domain's; "" for ratios
  bool is_ratio() const noexcept { return denominator_.has_value(); }
  bool needs_reduction() const;  // uses a domain's stages or values
  const Term& numerator() const noexcept { return numerator_; }
  const std::optional<Term>& denominator() const noexcept { return denominator_; }

  std::optional<Value> eval(const ReducedAnalysis& a) const;

  friend bool operator==(const Quantity& a, const Quantity& b) { return a.text_ == b.text_; }

 private:
  std::string text_;
  Term numerator_;
  std::optional<Term> denominator_;
};

// Quantities the dataset can evaluate, for completion lists: the domain's
// names (`domain_quantities`, Domain::quantities()) some analysis has a value
// for, isotopes (and their stages' common forms, core's and the domain's),
// ratios of isotopes of one element (heavier over lighter), gains, peak
// centers and environmentals present, extraction.
std::vector<std::string> available_quantities(const std::vector<ReducedPtr>& analyses,
                                              const std::vector<std::string>& domain_quantities);

}  // namespace pychron::processing
