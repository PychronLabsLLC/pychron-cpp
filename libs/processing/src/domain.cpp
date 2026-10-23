#include "pychron/processing/domain.hpp"

#include <string>
#include <utility>

namespace pychron::processing {

void DomainRegistry::add(std::unique_ptr<Domain> domain) {
  if (!domain) return;
  std::string id(domain->id());
  domains_[std::move(id)] = std::move(domain);
}

Result<DatasetPtr> run_reduction_chain(const Domain& domain, DatasetPtr dataset, RunContext& context) {
  for (const auto& kind : domain.reduction_chain()) {
    const Unit* unit = domain.units().find(kind);
    if (unit == nullptr) return fail(ErrorKind::Config, "reduction chain: no unit '" + kind + "' in domain " + std::string(domain.id()));
    auto out = unit->execute({PortValue(std::move(dataset))}, Options(unit->schema()), context);
    if (!out) return fail(out.error());
    dataset = std::get<DatasetPtr>(out->at(0));
  }
  return dataset;
}

const Domain* DomainRegistry::find(std::string_view id) const {
  const auto it = domains_.find(id);
  return it == domains_.end() ? nullptr : it->second.get();
}

std::vector<std::string> DomainRegistry::ids() const {
  std::vector<std::string> out;
  out.reserve(domains_.size());
  for (const auto& entry : domains_) out.push_back(entry.first);
  return out;
}

namespace {

// Core only: the reduction every analysis gets, the figures that name no
// isotope system, and nothing cached, recalled or exported beyond the neutral.
class NobleGasDomain final : public Domain {
 public:
  std::string_view id() const override { return "noble_gas"; }
  std::string_view title() const override { return "Noble gas"; }
  const UnitRegistry& units() const override { return UnitRegistry::builtin(); }
  std::vector<std::string> reduction_chain() const override { return {"reduce"}; }

  std::vector<FigureEntry> figures() const override {
    std::vector<FigureEntry> out;
    FigureEntry time_series;
    time_series.kind = "time_series";
    time_series.label = "Time series";
    time_series.icon = "time_series";
    time_series.tip = "Time series of the selected analyses";
    out.push_back(std::move(time_series));

    FigureEntry evolution;
    evolution.kind = "isotope_evolution_fit";
    evolution.label = "Isotope evolutions...";
    evolution.icon = "isotope_evolution_fit";
    evolution.tip = "Refit the selected analyses' isotope evolutions";
    evolution.window = FigureEntry::Window::IsotopeEvolution;
    evolution.separator_before = true;
    out.push_back(std::move(evolution));

    FigureEntry blank;
    blank.kind = "blank_fit";
    blank.label = "Blanks...";
    blank.icon = "blank_fit";
    blank.tip = "Blanks: fit the selected unknowns' values from reference analyses";
    blank.window = FigureEntry::Window::ReferenceFit;
    out.push_back(std::move(blank));

    FigureEntry ic;
    ic.kind = "icfactor_fit";
    ic.label = "IC factors...";
    ic.icon = "icfactor_fit";
    ic.tip = "IC factors: fit the selected unknowns' values from reference analyses";
    ic.window = FigureEntry::Window::ReferenceFit;
    out.push_back(std::move(ic));
    return out;
  }

  std::vector<std::string> quantities() const override { return {}; }
  // A placeholder until the noble-gas spec names its quantities; group_stats needs a string.
  std::string default_quantity() const override { return "Ar40"; }
  std::vector<DerivedQuantity> derived() const override { return {}; }
  std::optional<DerivedValues> derive(const ReducedAnalysis&) const override { return std::nullopt; }
  std::string_view reduction_version() const override { return "noble-1"; }
  std::vector<RecallSection> recall_sections(const ReducedAnalysis&) const override { return {}; }
  std::vector<RecallErrorComponent> error_budget(const ReducedAnalysis&) const override { return {}; }
  std::vector<Exporter> exporters() const override { return {}; }
};

}  // namespace

std::unique_ptr<Domain> make_noble_gas_domain() { return std::make_unique<NobleGasDomain>(); }

}  // namespace pychron::processing
