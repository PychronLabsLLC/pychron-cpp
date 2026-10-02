#include "pychron/experiment/conditionals/validate.hpp"

namespace pychron::experiment {

namespace {

bool known(const std::set<std::string>& names, const std::string& n) { return names.empty() || names.contains(n); }

}  // namespace

std::vector<ConditionalDiagnostic> validate_conditionals(const ConditionalSet& set, const MetricCatalog& catalog) {
  using K = MetricRef::Kind;
  std::vector<ConditionalDiagnostic> out;
  for (const auto& c : set.items) {
    auto add = [&](std::string m, bool error = true) { out.push_back({c.name, std::move(m), error}); };
    if (!c.expr) {
      add("check was not compiled");
      continue;
    }
    const bool between_runs = c.kind == ConditionalKind::PreRun;
    for (const auto& m : metrics_of(*c.expr)) {
      const auto text = to_string(m);
      switch (m.kind) {
        case K::Isotope:
        case K::IsotopeField:
          if (!known(catalog.isotopes, m.a)) add("unknown isotope '" + m.a + "' in " + text);
          if (between_runs) add("pre-run checks have no isotope data: " + text);
          break;
        case K::Ratio:
          if (!known(catalog.isotopes, m.a)) add("unknown isotope '" + m.a + "' in " + text);
          if (!known(catalog.isotopes, m.b)) add("unknown isotope '" + m.b + "' in " + text);
          if (between_runs) add("pre-run checks have no isotope data: " + text);
          break;
        case K::DetectorField:
          if (!known(catalog.detectors, m.a)) add("unknown detector '" + m.a + "' in " + text);
          if (m.field == "intensity" && between_runs) add("pre-run checks have no intensities: " + text);
          break;
        case K::Gauge:
          if (!known(catalog.gauges, m.a)) add("unknown gauge '" + m.a + "' in " + text);
          break;
        case K::Device:
          if (!known(catalog.devices, m.a)) add("unknown device '" + m.a + "' in " + text);
          break;
        case K::Computed:
          if (m.a == "kcl" || m.a == "clk" || m.a == "cl36") {
            add("'" + m.a + "' needs chlorine corrections, which are not available");
          } else if (!catalog.computed) {
            add("'" + m.a + "' needs Ar-Ar constants (J, production ratios); none are configured");
          } else if (between_runs) {
            add("pre-run checks have no isotope data: " + text);
          }
          break;
        case K::Param: break;
      }
    }
    if (!catalog.variables.empty())
      for (const auto& v : variables_of(*c.expr))
        if (!catalog.variables.contains(v)) add("variable $" + v + " is not defined", false);
  }
  return out;
}

}  // namespace pychron::experiment
