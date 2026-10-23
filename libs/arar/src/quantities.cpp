#include "pychron/arar/quantities.hpp"

#include <array>

namespace pychron::arar {

namespace {

struct Row {
  std::string_view name;
  std::string_view label;
  std::string_view units;
  bool stage;
};

// The labels and units quantity.cpp's table gave before the domain seam:
// figure axes, SVG and PDF fixtures and presets' titles read them.
constexpr std::array<Row, 14> kRows{{
    {"age", "Age", "Ma", false},
    {"age_w_j", "Age (with J error)", "Ma", false},
    {"age_w_position", "Age (with position error)", "Ma", false},
    {"F", "40Ar*/39ArK", "", false},
    {"kca", "K/Ca", "", false},
    {"cak", "Ca/K", "", false},
    {"kcl", "K/Cl", "", false},
    {"clk", "Cl/K", "", false},
    {"radiogenic_yield", "%40Ar*", "%", false},
    {"rad40", "40Ar*", "fA", false},
    {"k39", "39ArK", "fA", false},
    {"j", "J", "", false},
    {"decay_corrected", "decay corrected", "fA", true},
    {"interference_corrected", "interference corrected", "fA", true},
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

}  // namespace pychron::arar
