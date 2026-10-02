#pragma once

// Quantities (design section 10): one vocabulary for figure panels, filter
// rules, browser columns and statistics.
//
//   quantity := term ( "/" term )?
//   term     := isotope ( "." stage )?         Ar40, Ar40.bs_corrected, H1:Ar40.intercept
//             | name                           age, kca, extract_value, timestamp, ...
//             | family "." key                 gain.H1, deflection.H1, source.trap,
//                                              env.lab_temperature, peak_center.H1
//
// An isotope without a stage is ic_corrected (legacy get_intensity). Ratios
// divide UFloats, so correlated stages propagate correctly. eval() returns
// nullopt when the analysis lacks the quantity; never 0.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/reduced.hpp"

namespace pychron::processing {

enum class Named {
  Age,
  AgeWithJ,
  AgeWithPosition,
  F,
  KCa,
  CaK,
  KCl,
  ClK,
  RadiogenicYield,
  Rad40,
  K39,
  J,
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
  enum class Kind { Isotope, Named, Family } kind = Kind::Isotope;
  std::string key;  // isotope key, or the family key ("H1")
  Stage stage = Stage::IcCorrected;
  Named named = Named::Age;
  Family family = Family::Gain;
  friend bool operator==(const Term&, const Term&) = default;
};

class Quantity {
 public:
  // Fails (Config, "quantity: ...") naming the bad part.
  static Result<Quantity> parse(std::string_view text);

  const std::string& text() const noexcept { return text_; }  // canonical form
  std::string label() const;   // "40Ar/36Ar", "Age (Ma)", "Ar40 intercept"
  std::string units() const;   // "fA", "Ma", "" for ratios
  bool is_ratio() const noexcept { return denominator_.has_value(); }
  bool needs_reduction() const;  // uses decay/interference stages or reduced values

  std::optional<Value> eval(const ReducedAnalysis& a) const;

  friend bool operator==(const Quantity& a, const Quantity& b) { return a.text_ == b.text_; }

 private:
  std::string text_;
  Term numerator_;
  std::optional<Term> denominator_;
};

// Quantities the dataset can evaluate, for completion lists: isotopes (and
// their stages' common forms), ratios of argon isotopes, reduced values when
// any analysis reduced, gains/deflections/peak centers present, extraction.
std::vector<std::string> available_quantities(const std::vector<ReducedPtr>& analyses);

}  // namespace pychron::processing
