// The Ar-Ar domain over today's code (domain seam spec 4): its answers are
// what the application hard-codes now. Moves to libs/arar when that library
// is split out.

#include <algorithm>
#include <string>
#include <utility>

#include "pychron/processing/derived.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/report.hpp"

namespace pychron::processing {

namespace {

FigureEntry figure(std::string kind, std::string label, std::string tip, std::string group_key = "none",
                   FigureEntry::Window window = FigureEntry::Window::Figure, bool separator_before = false) {
  FigureEntry e;
  e.icon = kind;
  e.kind = std::move(kind);
  e.label = std::move(label);
  e.tip = std::move(tip);
  e.group_key = std::move(group_key);
  e.window = window;
  e.separator_before = separator_before;
  return e;
}

// The Schaen et al. (2021) report of `groups`, as DataWorkspace writes it
// after the export pipeline ran: by aliquot when any analysis is a heating
// step, else by identifier. The reduction tag is the one of a reduce unit with
// its default options, which is what the export pipeline runs.
Result<void> write_schaen(const std::filesystem::path& path, const std::vector<std::vector<ReducedPtr>>& groups) {
  Dataset dataset;
  bool steps = false;
  for (const auto& group : groups) {
    for (const auto& reduced : group) {
      if (!reduced || !reduced->analysis) continue;
      steps = steps || reduced->analysis->increment >= 0;
      DatasetItem item;
      item.analysis = reduced;
      // The select unit's default: these tags are kept, but out of the statistics.
      const std::string& tag = reduced->analysis->tag;
      item.exclusion.tag = tag == "omit" || tag == "outlier";
      dataset.mutable_items().push_back(std::move(item));
    }
  }
  dataset.reduction_tag = std::string(reduction::kReductionVersion) + "/default/d0e0";

  const Unit* group_unit = UnitRegistry::builtin().find("group");
  if (group_unit == nullptr) return fail(ErrorKind::Config, "export: no group unit");
  Options options(group_unit->schema());
  if (auto set = options.set("key", std::string(steps ? "aliquot" : "identifier")); !set) return fail(set.error());
  RunContext context;
  auto grouped = group_unit->execute({PortValue(make_dataset(std::move(dataset)))}, options, context);
  if (!grouped) return fail(grouped.error());
  const Dataset& by_key = *std::get<DatasetPtr>(grouped->at(0));
  return save_report(make_report(by_key, ReportOptions{}), path);
}

class ArArDomain final : public Domain {
 public:
  std::string_view id() const override { return "arar"; }
  std::string_view title() const override { return "40Ar/39Ar"; }
  const UnitRegistry& units() const override { return UnitRegistry::builtin(); }
  std::vector<std::string> reduction_chain() const override { return {"reduce"}; }

  std::vector<FigureEntry> figures() const override {
    using W = FigureEntry::Window;
    return {
        figure("time_series", "Time series", "Time series of the selected analyses"),
        figure("ideogram", "Ideogram", "Ideogram of the selected analyses", "identifier"),
        figure("spectrum", "Age spectrum", "Age spectrum of the selected analyses", "aliquot"),
        figure("inverse_isochron", "Inverse isochron", "Inverse isochron of the selected analyses", "aliquot"),
        figure("spectrum_isochron", "Spectrum + isochron",
               "Age spectrum and inverse isochron of the selected analyses, side by side", "aliquot"),
        figure("isotope_evolution_fit", "Isotope evolutions...", "Refit the selected analyses' isotope evolutions",
               "none", W::IsotopeEvolution, true),
        figure("blank_fit", "Blanks...", "Blanks: fit the selected unknowns' values from reference analyses", "none",
               W::ReferenceFit),
        figure("icfactor_fit", "IC factors...",
               "IC factors: fit the selected unknowns' values from reference analyses", "none", W::ReferenceFit),
    };
  }

  // The values quantity.cpp computes from reduce() (`reduced` in its table),
  // and the two stages of it that core does not know.
  std::vector<std::string> quantities() const override {
    return {"age",     "age_w_j", "age_w_position", "F",  "kca",
            "cak",     "kcl",     "clk",            "radiogenic_yield",
            "rad40",   "k39",     "decay_corrected", "interference_corrected"};
  }
  std::string default_quantity() const override { return "age"; }

  std::vector<DerivedQuantity> derived() const override {
    return {{"age", "Age (Ma)", "Ma", true}, {"age_w_j", "Age with J error (Ma)", "Ma", false}, {"kca", "K/Ca", "", false}};
  }
  std::optional<DerivedValues> derive(const ReducedAnalysis& reduced) const override {
    return derived_age(reduced, ReductionSettings{});
  }
  std::string_view reduction_version() const override { return reduction::kReductionVersion; }

  std::vector<RecallSection> recall_sections(const ReducedAnalysis& reduced) const override {
    RecallModel model = make_recall_model(reduced);
    return {std::move(model.computed), std::move(model.ratios)};
  }
  std::vector<RecallErrorComponent> error_budget(const ReducedAnalysis& reduced) const override {
    return make_recall_model(reduced).error_budget;
  }

  std::vector<Exporter> exporters() const override {
    return {{"schaen", "Export",
             "Export: write the selected analyses (right list) as a 40Ar/39Ar data report after Schaen et al. (2021), "
             "CSV or JSON",
             "CSV report (*.csv);;JSON report (*.json)", write_schaen}};
  }
};

}  // namespace

std::unique_ptr<Domain> make_arar_domain() { return std::make_unique<ArArDomain>(); }

}  // namespace pychron::processing
