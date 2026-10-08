// The age spectrum and the inverse isochron of the same analyses, as one figure.
// Nothing of either is redone here: the options are split back into each
// figure's own and the two scene builders are run.

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "figure_common.hpp"
#include "pychron/processing/arar_figures.hpp"
#include "pychron/processing/units.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace {

using namespace detail;

constexpr std::string_view kSpectrum = "spectrum.";
constexpr std::string_view kIsochron = "isochron.";
// Set by the pair itself (layout), so not offered.
constexpr std::string_view kGraphColumns = "graph_columns";
constexpr std::string_view kGroups = "groups";

// A field both figures have with one meaning (title, fonts, legend, ...).
bool is_common(std::string_view key) {
  static const std::set<std::string, std::less<>> keys = [] {
    std::set<std::string, std::less<>> out;
    for (const auto& f : common_figure_fields()) out.insert(f.key);
    return out;
  }();
  return keys.contains(key);
}

// One figure's own fields and lists, under `prefix`, in sections "<name>: <section>".
void take(Schema& s, const Schema& part, std::string_view prefix, const std::string& name) {
  for (FieldSpec f : part.fields) {
    if (is_common(f.key)) continue;
    // A condition begins with the key it reads.
    if (!f.enabled_when.empty()) f.enabled_when = std::string(prefix) + f.enabled_when;
    f.key = std::string(prefix) + f.key;
    f.section = name + ": " + f.section;
    s.fields.push_back(std::move(f));
  }
  for (ListSpec l : part.lists) {
    if (l.key == kGroups) continue;
    l.key = std::string(prefix) + l.key;
    l.section = name + ": " + l.section;
    s.lists.push_back(std::move(l));
  }
}

SchemaPtr build_schema() {
  auto s = std::make_shared<Schema>();
  s->kind = "figure.spectrum_isochron";
  s->title = "Spectrum and isochron";
  take(*s, *spectrum_schema(), kSpectrum, "Spectrum");
  take(*s, *isochron_schema(), kIsochron, "Isochron");
  for (auto& f : s->fields)
    if (f.key == "isochron.exclude_non_plateau") f.help = "The plateau the spectrum shows, fixed steps included";
  s->fields.push_back(choice("layout", "Spectrum and isochron", "Layout", {"side_by_side", "stacked"}));
  for (auto& f : common_figure_fields())
    if (f.key != kGraphColumns) s->fields.push_back(std::move(f));
  s->lists.push_back(groups_list(true));
  s->factory_presets = {
      {"Default", ""},
      {"Plateau steps", "[isochron]\nexclude_non_plateau = true\n[spectrum]\ndim_non_plateau = true\n"},
      {"With K/Ca",
       "[[spectrum.panels]]\nkind = \"value\"\nquantity = \"kca\"\nscale = \"log\"\n"
       "[[spectrum.panels]]\nkind = \"age_spectrum\"\nheight = 2.5\n"},
  };
  return s;
}

// The options of one of the two figures: what is set under its prefix, and
// what is set of the shared fields and the groups. What is not set reads as
// that figure's default, which is the pair's default too.
Options part_options(const Options& o, const SchemaPtr& schema, std::string_view prefix) {
  Options out(schema);
  auto own_key = [&](const std::string& key) -> std::string {
    if (key.starts_with(prefix)) return key.substr(prefix.size());
    if (is_common(key) || key == kGroups) return key;
    return {};
  };
  for (const auto& [key, value] : o.raw_values())
    if (const std::string k = own_key(key); !k.empty() && schema->field(k)) out.raw_values()[k] = value;
  for (const auto& [key, rows] : o.raw_lists())
    if (const std::string k = own_key(key); !k.empty() && schema->list(k)) out.raw_lists()[k] = rows;
  return out;
}

Error of_part(Error e, std::string_view part) {
  e.what = std::string(part) + ": " + e.what;
  return e;
}

}  // namespace

const SchemaPtr& spectrum_isochron_schema() {
  static const SchemaPtr s = build_schema();
  return s;
}

Result<Scene> build_spectrum_isochron(const Dataset& d, const Options& o) {
  const Options so = part_options(o, spectrum_schema(), kSpectrum);
  const Options io = part_options(o, isochron_schema(), kIsochron);

  auto spectrum = build_spectrum(d, so);
  if (!spectrum) return fail(of_part(spectrum.error(), "spectrum"));

  // The plateau as drawn: the steps the age spectrum highlights.
  std::set<std::string> plateau;
  for (const auto& g : spectrum->graphs)
    for (const auto& p : g.panels)
      for (const auto& layer : p.layers)
        if (const auto* steps = std::get_if<StepLayer>(&layer))
          for (std::size_t i = 0; i < steps->refs.size() && i < steps->highlighted.size(); ++i)
            if (steps->highlighted[i]) plateau.insert(steps->refs[i].analysis);
  const auto panels = so.rows("panels");
  const bool age_panel =
      std::any_of(panels.begin(), panels.end(), [](const Options& row) { return row.get_string("kind") == "age_spectrum"; });
  const bool from_plateau = io.get_bool("exclude_non_plateau");

  auto isochron = build_isochron_scene(d, io, from_plateau && age_panel ? &plateau : nullptr);
  if (!isochron) return fail(of_part(isochron.error(), "isochron"));

  Scene scene = std::move(*spectrum);
  scene.kind = "spectrum_isochron";
  scene.columns = o.get_string("layout") == "stacked" ? 1 : 2;
  // A graph of the dataset is one graph of each figure, in the same order.
  std::vector<Graph> spectra = std::move(scene.graphs);
  scene.graphs.clear();
  for (std::size_t i = 0; i < std::max(spectra.size(), isochron->graphs.size()); ++i) {
    if (i < spectra.size()) scene.graphs.push_back(std::move(spectra[i]));
    if (i < isochron->graphs.size()) scene.graphs.push_back(std::move(isochron->graphs[i]));
  }
  if (from_plateau && !age_panel)
    scene.warnings.push_back("the spectrum has no age panel: the isochron looks for the plateau itself");
  for (auto& w : isochron->warnings)
    if (std::find(scene.warnings.begin(), scene.warnings.end(), w) == scene.warnings.end())
      scene.warnings.push_back(std::move(w));
  return scene;
}

namespace {

class SpectrumIsochronUnit final : public Unit {
 public:
  std::string_view kind() const override { return "spectrum_isochron"; }
  std::string_view title() const override { return "Spectrum and isochron"; }
  const SchemaPtr& schema() const override { return spectrum_isochron_schema(); }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"figure", PortType::Scene}}; }
  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    auto scene = build_spectrum_isochron(*std::get<DatasetPtr>(in.at(0)), o);
    if (!scene) return fail(scene.error());
    return std::vector<PortValue>{PortValue(std::make_shared<const Scene>(std::move(*scene)))};
  }
};

}  // namespace

std::unique_ptr<Unit> make_spectrum_isochron_unit() { return std::make_unique<SpectrumIsochronUnit>(); }

}  // namespace pychron::processing
