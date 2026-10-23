#include "domain_pipeline.hpp"

#include <utility>

namespace pychron::ui {

std::string add_reduction_chain(processing::Pipeline& pipeline, const processing::UnitRegistry& registry,
                                const processing::Domain& domain, const std::string& from, const std::string& suffix) {
  std::string last = from;
  for (const auto& kind : domain.reduction_chain()) {
    std::string id = kind + suffix;
    pipeline.add(registry, id, kind, {last});
    last = std::move(id);
  }
  return last;
}

}  // namespace pychron::ui
