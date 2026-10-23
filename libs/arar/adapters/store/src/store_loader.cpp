#include "pychron/arar/store_loader.hpp"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "pychron/arar/analysis_data.hpp"
#include "pychron/arar/domain.hpp"

namespace pychron::arar {

namespace ps = pychron::persistence;
namespace pp = pychron::processing;

namespace {

Result<std::shared_ptr<const pp::DomainAnalysisData>> load(const pp::StoreAnalysisParts& parts, const pp::Analysis& a) {
  auto data = std::make_shared<ArArAnalysisData>();
  for (const auto& ref : parts.refs) {
    if (const auto* f = std::get_if<ps::FluxValue>(&ref)) {
      // No J: no flux yet, hence no age. With a J, a NULL error is unknown
      // (NaN), not 0, and the analysis does not reduce. The position error is
      // an optional extra: NULL is none.
      if (!f->j) continue;
      constexpr double unknown = std::numeric_limits<double>::quiet_NaN();
      reduction::Flux flux;
      flux.j = {*f->j, f->j_err.value_or(unknown)};
      flux.position_jerr = f->position_jerr.value_or(0.0);
      if (f->lambda_k_total)
        flux.lambda_k_total = reduction::Measured{*f->lambda_k_total, f->lambda_k_total_err.value_or(unknown)};
      data->flux = flux;
    } else if (const auto* p = std::get_if<ps::ProductionValue>(&ref)) {
      data->reactor = p->reactor.value_or("");
      // A production without ratios is no production (reference data the
      // source removed), not nine ratios of zero.
      if (p->ratios.empty()) continue;
      std::map<std::string, reduction::Measured, std::less<>> rows;
      for (const auto& r : p->ratios) rows[r.key] = {r.value, r.error};
      auto ratios = reduction::production_from_rows(rows);
      if (!ratios) return fail(ErrorKind::Config, a.runid + ": production: " + ratios.error().what);
      data->production = *ratios;
    } else if (const auto* c = std::get_if<ps::ChronologyValue>(&ref)) {
      data->chronology.clear();
      for (const auto& dose : c->doses)
        data->chronology.push_back(
            reduction::Dose{dose.power, dose.start.micros / 1'000'000, dose.end.micros / 1'000'000});
    }
  }
  return std::shared_ptr<const pp::DomainAnalysisData>(std::move(data));
}

}  // namespace

pp::StoreSourceOptions::ExtensionLoader store_extension_loader() { return load; }

pp::StoreSourceOptions store_source_options(pp::StoreSourceOptions options) {
  options.reduction_version = std::string(shared_domain().reduction_version());
  options.extension_loader = store_extension_loader();
  return options;
}

}  // namespace pychron::arar
