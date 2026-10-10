#include "row_colors.hpp"

#include <algorithm>

#include <QRegularExpression>

namespace pychron::ui {

namespace {

bool tagged(const processing::AnalysisSummary& row) { return !row.tag.empty() && row.tag != "ok"; }

// The colour of a class in `colors` (TypeColors, const or not); null for no class.
template <typename Colors>
auto* member(Colors& colors, std::string_view type_class) {
  if (type_class == "unknown") return &colors.unknown;
  if (type_class == "blank") return &colors.blank;
  if (type_class == "air") return &colors.air;
  if (type_class == "cocktail") return &colors.cocktail;
  if (type_class == "detector_ic") return &colors.detector_ic;
  if (type_class == "other") return &colors.other;
  return static_cast<decltype(&colors.other)>(nullptr);
}

}  // namespace

QString to_text(ColorBy by) {
  switch (by) {
    case ColorBy::AnalysisType:
      return QStringLiteral("analysis_type");
    case ColorBy::Tag:
      return QStringLiteral("tag");
    case ColorBy::Spectrometer:
      return QStringLiteral("spectrometer");
    case ColorBy::IrradiationLevel:
      return QStringLiteral("irradiation_level");
    case ColorBy::None:
      return QStringLiteral("none");
  }
  return QStringLiteral("analysis_type");
}

ColorBy color_by_from_text(const QString& text) {
  const auto found = std::ranges::find_if(kColorByChoices, [&](ColorBy by) { return to_text(by) == text; });
  return found == kColorByChoices.end() ? ColorBy::AnalysisType : *found;
}

std::string_view type_class(std::string_view analysis_type) {
  if (analysis_type.starts_with("blank")) return "blank";
  for (const std::string_view exact : {"unknown", "air", "cocktail", "detector_ic"})
    if (analysis_type == exact) return exact;
  return "other";
}

QColor* TypeColors::find(std::string_view type_class) { return member(*this, type_class); }
const QColor* TypeColors::find(std::string_view type_class) const { return member(*this, type_class); }

TypeColors default_type_colors(const Theme& theme) {
  TypeColors c;
  c.blank = theme.row_blank;
  c.air = theme.row_air;
  c.cocktail = theme.row_cocktail;
  c.detector_ic = theme.row_detector_ic;
  return c;
}

bool is_type_color_text(const QString& text) {
  static const QRegularExpression hex(QStringLiteral("^#[0-9a-f]{6}$"));
  return text.isEmpty() || hex.match(text).hasMatch();
}

TypeColors type_colors(const TypeColorOverrides& overrides, const Theme& theme) {
  TypeColors colors = default_type_colors(theme);
  for (const auto& [name, text] : overrides) {
    QColor* color = colors.find(name);
    const QString value = QString::fromStdString(text);
    if (color == nullptr || !is_type_color_text(value)) continue;
    *color = value.isEmpty() ? QColor() : QColor::fromString(value);
  }
  return colors;
}

TypeColorOverrides type_color_overrides(const TypeColors& colors, const Theme& theme) {
  const TypeColors defaults = default_type_colors(theme);
  TypeColorOverrides out;
  for (const std::string_view name : kTypeClasses) {
    const QColor& color = *colors.find(name);
    if (color == *defaults.find(name)) continue;
    out[std::string(name)] = color.isValid() ? color.name(QColor::HexRgb).toStdString() : std::string();
  }
  return out;
}

std::string color_key(ColorBy by, const processing::AnalysisSummary& row) {
  switch (by) {
    case ColorBy::Tag:
      return tagged(row) && row.tag != "invalid" ? row.tag : std::string();
    case ColorBy::Spectrometer:
      return row.mass_spectrometer;
    case ColorBy::IrradiationLevel:
      return row.irradiation.empty() ? std::string() : row.irradiation + " " + row.level;
    case ColorBy::AnalysisType:
    case ColorBy::None:
      break;
  }
  return {};
}

QColor row_color(ColorBy by, const processing::AnalysisSummary& row, const TypeColors& types,
                 const std::vector<std::string>& keys, const Theme& theme) {
  if (by == ColorBy::Tag ? row.tag == "invalid" : tagged(row)) return theme.error_bg;
  if (by == ColorBy::None) return {};
  if (by == ColorBy::AnalysisType) {
    const QColor* c = types.find(type_class(row.analysis_type));
    return c != nullptr ? *c : QColor();
  }
  const std::string key = color_key(by, row);
  if (key.empty()) return {};
  const auto it = std::ranges::lower_bound(keys, key);
  if (it == keys.end() || *it != key) return {};
  return theme.row_category[static_cast<std::size_t>(it - keys.begin()) % theme.row_category.size()];
}

}  // namespace pychron::ui
