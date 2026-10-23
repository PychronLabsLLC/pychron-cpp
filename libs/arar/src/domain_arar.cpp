// The Ar-Ar domain over today's code (domain seam spec 4): its answers are
// what the application hard-codes now.

#include "pychron/arar/domain.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include "pychron/arar/arar_figures.hpp"
#include "pychron/arar/derived.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/arar/report.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/units.hpp"

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
      // Same list as `omit_tags` in units_builtin.cpp (SelectUnit): change both together.
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

namespace r = pychron::reduction;

std::optional<Value> val(const std::optional<r::UFloat>& u) {
  if (!u || !std::isfinite(u->nominal())) return std::nullopt;
  return Value{u->nominal(), u->std_dev()};
}

RecallRow text_row(std::string name, std::string text) {
  RecallRow row;
  row.name = std::move(name);
  row.text = std::move(text);
  return row;
}

RecallRow value_row(std::string name, std::optional<Value> v, std::string units = {}, std::string note = {}) {
  RecallRow row;
  row.name = std::move(name);
  row.value = v;
  row.units = std::move(units);
  row.note = std::move(note);
  if (!v) row.text = "—";
  return row;
}

// The recall window's Summary sections: the computed values of the Ar-Ar
// result, and the corrected argon ratios from the decay-corrected stages.
std::vector<RecallSection> arar_sections(const ReducedAnalysis& ra) {
  RecallSection computed;
  computed.title = "Computed";
  const arar::ArArReduced* reduced = arar::reduced_of(ra);
  if (reduced) {
    const r::ArArResult& ar = reduced->arar;
    if (ar.ages) {
      computed.rows.push_back(value_row("Age", val(ar.ages->age_w_j_err), "Ma", "with J error"));
      computed.rows.push_back(value_row("Age w/o J", val(ar.ages->age), "Ma", "analytical error only"));
      RecallRow wo;
      wo.name = "Age w/o J, irrad.";
      wo.value = Value{ar.ages->age.nominal(), ar.ages->age_err_wo_irrad};
      wo.units = "Ma";
      computed.rows.push_back(wo);
    } else {
      computed.rows.push_back(value_row("Age", std::nullopt, "Ma",
                                        ra.reduction_error.empty() ? "no J for this analysis" : ra.reduction_error));
    }
    computed.rows.push_back(value_row("F (40Ar*/39ArK)", val(ar.f.f)));
    computed.rows.push_back(value_row("K/Ca", val(ar.kca)));
    computed.rows.push_back(value_row("K/Cl", val(ar.kcl)));
    computed.rows.push_back(value_row("40Ar*", val(ar.f.rad40), "fA"));
    computed.rows.push_back(value_row("%40Ar*", val(ar.f.radiogenic_yield), "%"));
    computed.rows.push_back(value_row("39ArK", val(ar.f.interference_corrected[r::index(r::ArgonIsotope::Ar39)]), "fA"));
    for (const auto d : ar.diagnostics) computed.rows.push_back(text_row("note", std::string(r::to_string(d))));
  }

  // Corrected ratios from the IC-corrected (and decay-corrected) stages.
  RecallSection ratios;
  ratios.title = "Corrected ratios";
  auto stage = [&](const char* iso) -> std::optional<r::UFloat> {
    auto d = reduced ? reduced->stage(iso, "decay_corrected") : std::nullopt;
    return d ? d : ra.stage(iso, Stage::IcCorrected);
  };
  for (auto [n, d] : {std::pair{"Ar40", "Ar39"}, {"Ar40", "Ar36"}, {"Ar40", "Ar38"}, {"Ar38", "Ar39"}, {"Ar37", "Ar39"},
                      {"Ar36", "Ar39"}}) {
    auto num = stage(n), den = stage(d);
    if (!num || !den || den->nominal() == 0) continue;
    ratios.rows.push_back(value_row(std::string(n) + "/" + d, val(*num / *den)));
  }
  return {std::move(computed), std::move(ratios)};
}

// Error budget of the age with J error, by variable tag (legacy E20), largest first.
std::vector<RecallErrorComponent> arar_budget(const ReducedAnalysis& ra) {
  std::vector<RecallErrorComponent> out;
  const arar::ArArReduced* reduced = arar::reduced_of(ra);
  if (!reduced || !reduced->arar.ages) return out;
  const r::UFloat& age = reduced->arar.ages->age_w_j_err;
  const double var = age.variance();
  if (var > 0) {
    std::map<std::string, double> by;
    for (const auto& [t, sigma] : r::error_components(age)) by[std::string(r::tag_name(t))] += sigma * sigma / var * 100.0;
    for (const auto& [tag, pct] : by)
      if (pct >= 1e-6) out.push_back({tag.empty() ? "other" : tag, pct});
  }
  std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.percent > y.percent; });
  return out;
}

class ArArDomain final : public Domain {
 public:
  std::string_view id() const override { return "arar"; }
  std::string_view title() const override { return "40Ar/39Ar"; }
  // Core's units and the four Ar-Ar figures, built once.
  const UnitRegistry& units() const override {
    static const UnitRegistry registry = [] {
      UnitRegistry r;
      add_builtin_units(r);
      r.add(arar::make_arar_reduce_unit());
      r.add(make_ideogram_unit());
      r.add(make_spectrum_unit());
      r.add(make_isochron_unit());
      r.add(make_spectrum_isochron_unit());
      return r;
    }();
    return registry;
  }
  std::vector<std::string> reduction_chain() const override { return {"reduce", "arar_reduce"}; }

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
    return derived_age(reduced);
  }
  std::string_view reduction_version() const override { return reduction::kReductionVersion; }

  std::vector<RecallSection> recall_sections(const ReducedAnalysis& reduced) const override {
    return arar_sections(reduced);
  }
  std::vector<RecallErrorComponent> error_budget(const ReducedAnalysis& reduced) const override {
    return arar_budget(reduced);
  }

  std::vector<Exporter> exporters() const override {
    return {{"schaen", "Export",
             "Export: write the selected analyses (right list) as a 40Ar/39Ar data report after Schaen et al. (2021), "
             "CSV or JSON",
             "CSV report (*.csv);;JSON report (*.json)", write_schaen}};
  }
};

}  // namespace

}  // namespace pychron::processing

namespace pychron::arar {

std::unique_ptr<processing::Domain> make_domain() { return std::make_unique<processing::ArArDomain>(); }

}  // namespace pychron::arar
