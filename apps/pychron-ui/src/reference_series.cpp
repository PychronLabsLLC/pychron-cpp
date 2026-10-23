#include "reference_series.hpp"

#include <algorithm>
#include <cstdint>

namespace pychron::ui {

namespace pp = pychron::processing;

std::vector<std::string> analysis_types(ReferenceKind kind) {
  if (kind == ReferenceKind::Airs) return {"air"};
  // The bare "blank" is what imported data carries.
  return {"blank_unknown", "blank_air", "blank_cocktail", "blank_extractionline", "blank"};
}

std::string_view starting_preset(ReferenceKind kind) { return kind == ReferenceKind::Airs ? "Air monitor" : "Blanks"; }

pp::Pipeline reference_pipeline(const pp::UnitRegistry& reg, const ReferenceQuery& query, const pp::Options& figure) {
  pp::Pipeline pipeline;
  pipeline.name = query.kind == ReferenceKind::Airs ? "reference series: airs" : "reference series: blanks";

  // The query, never a list of uuids: an empty list would make select run an
  // unbounded query instead. remove_tags and omit_tags stay at their
  // defaults, so an invalid reference is not drawn.
  auto& select = pipeline.add(reg, "select", "select");
  (void)select.options.set("analysis_types", analysis_types(query.kind));
  if (!query.mass_spectrometer.empty())
    (void)select.options.set("mass_spectrometers", std::vector<std::string>{query.mass_spectrometer});
  if (query.since) (void)select.options.set("from", *query.since);
  (void)select.options.set("limit", static_cast<std::int64_t>(std::clamp(query.last_n, 1, 500)));

  pipeline.add(reg, "reduce", "reduce", {"select"});
  auto& group = pipeline.add(reg, "group", "group", {"reduce"});
  (void)group.options.set("key", std::string("analysis_type"));
  auto& fig = pipeline.add(reg, kReferenceFigure, "time_series", {"group"});
  fig.options = figure;
  return pipeline;
}

}  // namespace pychron::ui
