#include "pychron/systems/spectrometer/molecular_weights.hpp"

#include <sstream>
#include <utility>

#include <toml++/toml.hpp>

namespace pychron::spectrometer {

MolecularWeights::MolecularWeights(std::map<std::string, double, std::less<>> masses) : masses_(std::move(masses)) {}

const MolecularWeights& MolecularWeights::defaults() {
  // Atomic masses in amu (AME2016 / NIST); molecules are sums of the most
  // abundant isotopes.
  static const MolecularWeights table({
      {"He3", 3.0160293},    {"He4", 4.0026032},    {"HD", 3.0219268},     {"Ne20", 19.9924402},
      {"Ne21", 20.9938467},  {"Ne22", 21.9913851},  {"H2O", 18.0105647},   {"N2", 28.0061480},
      {"O2", 31.9898292},    {"CO2", 43.9898292},   {"Cl35", 34.9688527},  {"Cl36", 35.9683069},
      {"Cl37", 36.9659026},  {"HCl", 35.9766777},   {"Ar36", 35.9675451},  {"Ar37", 36.9667759},
      {"Ar38", 37.9627322},  {"Ar39", 38.9643130},  {"Ar40", 39.9623831},  {"K39", 38.9637065},
      {"K40", 39.9639982},   {"K41", 40.9618253},   {"Ca40", 39.9625909},  {"Ca42", 41.9586183},
      {"Ca43", 42.9587666},  {"Ca44", 43.9554818},  {"Kr78", 77.9203648},  {"Kr80", 79.9163790},
      {"Kr82", 81.9134836},  {"Kr83", 82.9141361},  {"Kr84", 83.9114977},  {"Kr86", 85.9106106},
      {"Xe124", 123.905893}, {"Xe126", 125.904274}, {"Xe128", 127.903531}, {"Xe129", 128.904779},
      {"Xe130", 129.903508}, {"Xe131", 130.905082}, {"Xe132", 131.904154}, {"Xe134", 133.905395},
      {"Xe136", 135.907219},
  });
  return table;
}

Result<double> MolecularWeights::mass(std::string_view isotope) const {
  auto it = masses_.find(isotope);
  if (it == masses_.end()) return fail(ErrorKind::Config, "no molecular weight for '" + std::string(isotope) + "'");
  return it->second;
}

bool MolecularWeights::contains(std::string_view isotope) const { return masses_.contains(isotope); }

void MolecularWeights::set(std::string isotope, double mass) { masses_[std::move(isotope)] = mass; }

MolecularWeights MolecularWeights::merged(const MolecularWeights& overrides) const {
  MolecularWeights out = *this;
  for (const auto& [k, v] : overrides.masses_) out.masses_[k] = v;
  return out;
}

Result<MolecularWeights> parse_molecular_weights(std::string_view toml_text) {
  auto parsed = toml::parse(toml_text);
  if (!parsed) return fail(ErrorKind::Config, "molecular weights: " + std::string(parsed.error().description()));
  MolecularWeights out;
  for (const auto& [k, v] : parsed.table()) {
    auto m = v.value<double>();
    if (!m) return fail(ErrorKind::Config, "molecular weights: '" + std::string(k.str()) + "' is not a number");
    if (!(*m > 0.0)) return fail(ErrorKind::Config, "molecular weights: '" + std::string(k.str()) + "' must be > 0");
    out.set(std::string(k.str()), *m);
  }
  return out;
}

std::string to_toml(const MolecularWeights& weights) {
  toml::table root;
  for (const auto& [k, v] : weights.entries()) root.insert(k, v);
  std::ostringstream out;
  out << root;
  return out.str();
}

}  // namespace pychron::spectrometer
