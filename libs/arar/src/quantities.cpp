#include "pychron/arar/quantities.hpp"

#include <array>

namespace pychron::arar {

namespace {

struct Row {
  std::string_view name;
  std::string_view label;
  std::string_view units;
  bool stage;
  // Offered by Domain::quantities(): what reduce() computes and its stages.
  // Not J, the flux's value (a typed quantity may still use it).
  bool listed;
};

// The labels and units quantity.cpp's table gave before the domain seam:
// figure axes, SVG and PDF fixtures and presets' titles read them.
constexpr std::array<Row, 14> kRows{{
    {"age", "Age", "Ma", false, true},
    {"age_w_j", "Age (with J error)", "Ma", false, true},
    {"age_w_position", "Age (with position error)", "Ma", false, true},
    {"F", "40Ar*/39ArK", "", false, true},
    {"kca", "K/Ca", "", false, true},
    {"cak", "Ca/K", "", false, true},
    {"kcl", "K/Cl", "", false, true},
    {"clk", "Cl/K", "", false, true},
    {"radiogenic_yield", "%40Ar*", "%", false, true},
    {"rad40", "40Ar*", "fA", false, true},
    {"k39", "39ArK", "fA", false, true},
    {"j", "J", "", false, false},
    {"decay_corrected", "decay corrected", "fA", true, true},
    {"interference_corrected", "interference corrected", "fA", true, true},
}};

}  // namespace

std::string_view canonical_name(std::string_view name) noexcept {
  if (name == "f") return "F";
  if (name == "rad40_percent") return "radiogenic_yield";
  return name;
}

std::optional<processing::QuantityInfo> quantity_info(std::string_view raw) {
  const std::string_view name = canonical_name(raw);
  for (const auto& row : kRows)
    if (row.name == name)
      return processing::QuantityInfo{std::string(row.name), std::string(row.label), std::string(row.units), row.stage};
  return std::nullopt;
}

std::vector<std::string> value_names() {
  std::vector<std::string> out;
  for (const auto& row : kRows)
    if (!row.stage) out.emplace_back(row.name);
  return out;
}

std::vector<std::string> listed_quantities() {
  std::vector<std::string> out;
  for (const auto& row : kRows)
    if (row.listed) out.emplace_back(row.name);
  return out;
}

}  // namespace pychron::arar
