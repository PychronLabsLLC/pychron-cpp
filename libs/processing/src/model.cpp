#include "pychron/processing/model.hpp"

#include <cstdio>

namespace pychron::processing {

const IsotopeData* Analysis::find_isotope(std::string_view key) const {
  for (const auto& iso : isotopes)
    if (iso.key == key) return &iso;
  return nullptr;
}

const IsotopeData* Analysis::find_by_isotope(std::string_view isotope) const {
  if (const auto* exact = find_isotope(isotope)) return exact;
  for (const auto& iso : isotopes)
    if (iso.isotope == isotope) return &iso;
  return nullptr;
}

std::string Analysis::step() const { return step_letters(increment); }

std::string_view to_string(SeriesKind kind) noexcept {
  switch (kind) {
    case SeriesKind::Signal:
      return "signal";
    case SeriesKind::Baseline:
      return "baseline";
    case SeriesKind::Sniff:
      return "sniff";
  }
  return "signal";
}

const RawSeries* RawData::find(SeriesKind kind, std::string_view key) const {
  for (const auto& s : series)
    if (s.kind == kind && s.key == key) return &s;
  return nullptr;
}

std::string step_letters(int increment) {
  if (increment < 0) return {};
  std::string out;
  int n = increment;
  while (true) {
    out.insert(out.begin(), static_cast<char>('A' + n % 26));
    n = n / 26 - 1;
    if (n < 0) break;
  }
  return out;
}

std::string make_runid(const std::string& identifier, int aliquot, int increment) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d", aliquot);
  return identifier + "-" + buf + step_letters(increment);
}

}  // namespace pychron::processing
