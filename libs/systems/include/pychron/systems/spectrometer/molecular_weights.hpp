#pragma once

#include <map>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"

namespace pychron::spectrometer {

// Isotope (or molecule) name -> mass in amu. Loaded from
// molecular_weights.toml as flat `Ar40 = 39.9623831` entries; a default
// table covering the noble gases and common interferences ships built in.
class MolecularWeights {
 public:
  MolecularWeights() = default;
  explicit MolecularWeights(std::map<std::string, double, std::less<>> masses);

  static const MolecularWeights& defaults();

  Result<double> mass(std::string_view isotope) const;
  bool contains(std::string_view isotope) const;
  void set(std::string isotope, double mass);
  const std::map<std::string, double, std::less<>>& entries() const noexcept { return masses_; }

  // Entries of `overrides` replace or extend this table.
  MolecularWeights merged(const MolecularWeights& overrides) const;

 private:
  std::map<std::string, double, std::less<>> masses_;
};

Result<MolecularWeights> parse_molecular_weights(std::string_view toml_text);
std::string to_toml(const MolecularWeights& weights);

}  // namespace pychron::spectrometer
