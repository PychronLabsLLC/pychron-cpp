// The figures of a release (data publishing design, section 5.6).

#include "pychron/publish/figures.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

#include "pychron/arar/arar_figures.hpp"
#include "pychron/processing/scene_pdf.hpp"
#include "pychron/processing/scene_svg.hpp"
#include "pychron/publish/tables.hpp"

namespace pychron::publish {

namespace pp = processing;

namespace {

pp::Dataset subset(const pp::Dataset& d, std::vector<pp::DatasetItem> items) {
  pp::Dataset out(std::move(items));
  out.group_names = d.group_names;
  out.graph_names = d.graph_names;
  out.reduction_tag = d.reduction_tag;
  return out;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string two_digits(int v) { return (v >= 0 && v < 10 ? "0" : "") + std::to_string(v); }

struct Figures {
  const std::string& dir;
  std::int64_t created;
  std::vector<std::string>& warnings;
  std::vector<FigureFile> files;

  void add(const std::string& name, const char* kind, const Result<pp::Scene>& scene) {
    if (!scene) {
      warnings.push_back(dir + ": no " + kind + ": " + scene.error().what);
      return;
    }
    const std::string base = "figures/" + dir + "/" + name;
    files.push_back({base + ".svg", pp::scene_svg(*scene)});
    files.push_back({base + ".pdf", pp::scene_pdf(*scene, {created, dir + " " + name})});
  }
};

}  // namespace

pp::Options figure_options(const pp::SchemaPtr& schema, const ReleaseOptions& options,
                           std::vector<std::string>* warnings) {
  pp::Options o(schema);
  // A key the schema does not have is left out: not every figure has a plateau.
  const auto set = [&](std::string_view key, pp::OptionValue value) {
    if (!schema || !schema->field(key)) return;
    if (auto ok = o.set(key, std::move(value)); !ok && warnings)
      warnings->push_back(schema->kind + ": " + std::string(key) + " kept its default: " + ok.error().what);
  };
  set("error_bar_nsigma", static_cast<std::int64_t>(options.sigma));
  set("plateau.method", options.plateau);
  set("plateau.nsteps", static_cast<std::int64_t>(options.plateau_steps));
  set("plateau.gas_fraction", options.plateau_gas);
  return o;
}

std::vector<FigureFile> sample_figures(const pp::Dataset& sample, const std::string& sample_dir,
                                       const ReleaseOptions& options, std::int64_t created_unix,
                                       std::vector<std::string>& warnings) {
  Figures out{sample_dir, created_unix, warnings, {}};
  if (sample.empty()) return {};
  out.add("ideogram", "ideogram", pp::build_ideogram(sample, figure_options(pp::ideogram_schema(), options)));

  std::map<std::pair<std::string, int>, std::vector<pp::DatasetItem>> aliquots;
  for (const auto& it : sample.items())
    if (it.analysis->analysis->increment >= 0)
      aliquots[{it.analysis->analysis->identifier, it.analysis->analysis->aliquot}].push_back(it);
  const pp::Options spectrum = figure_options(pp::spectrum_schema(), options, &warnings);
  std::set<std::string> taken;  // folded to lower case: a case-insensitive disk sees one file
  for (auto& [key, items] : aliquots) {
    const std::string base =
        aliquots.size() == 1 ? "spectrum" : "spectrum-" + file_safe(key.first + "-" + two_digits(key.second));
    std::string name = base;
    for (int n = 2; !taken.insert(lower(name)).second; ++n) name = base + "-" + std::to_string(n);
    out.add(name, "spectrum", pp::build_spectrum(subset(sample, std::move(items)), spectrum));
  }

  const auto included = std::count_if(sample.items().begin(), sample.items().end(),
                                      [](const pp::DatasetItem& it) { return it.exclusion.included(); });
  if (included >= 3)
    out.add("isochron", "isochron", pp::build_isochron(sample, figure_options(pp::isochron_schema(), options)));
  return std::move(out.files);
}

}  // namespace pychron::publish
