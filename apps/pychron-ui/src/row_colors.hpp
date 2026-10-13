#pragma once

// Row tints of the analysis table (data browser search and display design,
// section 3.4). Pure: what a row is coloured by, and the colour it gets.

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <QColor>
#include <QString>

#include "pychron/processing/source.hpp"
#include "theme.hpp"

namespace pychron::ui {

enum class ColorBy { AnalysisType, Tag, Spectrometer, IrradiationLevel, None };

inline constexpr std::array kColorByChoices = {ColorBy::AnalysisType, ColorBy::Tag, ColorBy::Spectrometer,
                                              ColorBy::IrradiationLevel, ColorBy::None};

// The name a choice is saved under; text that names none reads as AnalysisType.
QString to_text(ColorBy by);
ColorBy color_by_from_text(const QString& text);

// The classes analysis types are coloured by: "unknown", "blank" (every
// blank_*), "air", "cocktail", "detector_ic", and "other" for the rest.
inline constexpr std::array<std::string_view, 6> kTypeClasses = {"unknown",  "blank",       "air",
                                                                 "cocktail", "detector_ic", "other"};
std::string_view type_class(std::string_view analysis_type);

// One colour per class; an invalid colour is no tint.
struct TypeColors {
  QColor unknown, blank, air, cocktail, detector_ic, other;
  // Null for a name that is not a class.
  QColor* find(std::string_view type_class);
  const QColor* find(std::string_view type_class) const;
  bool operator==(const TypeColors&) const = default;
};
TypeColors default_type_colors(const Theme& theme);

// How the colours a user chose are kept (Preferences): class name to
// "#rrggbb", or to empty text for no tint; a class that is not named has the
// theme's colour.
using TypeColorOverrides = std::map<std::string, std::string>;
TypeColors type_colors(const TypeColorOverrides& overrides, const Theme& theme);
TypeColorOverrides type_color_overrides(const TypeColors& colors, const Theme& theme);
// "#rrggbb" in lower case, or empty text: what an override may hold.
bool is_type_color_text(const QString& text);

// What a row is told apart by under `by` when its colour is one of the
// theme's categories; empty for a row that takes none.
std::string color_key(ColorBy by, const processing::AnalysisSummary& row);

// The tint of one row; invalid for none. `keys`: the sorted distinct
// color_key of the rows shown.
QColor row_color(ColorBy by, const processing::AnalysisSummary& row, const TypeColors& types,
                 const std::vector<std::string>& keys, const Theme& theme);

}  // namespace pychron::ui
