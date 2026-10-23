#include "pychron/arar/analysis_data.hpp"

#include <memory>
#include <utility>

namespace pychron::arar {

const ArArAnalysisData* analysis_data_of(const processing::Analysis& analysis) noexcept {
  return dynamic_cast<const ArArAnalysisData*>(analysis.extension.get());
}

ArArAnalysisData& edit_analysis_data(processing::Analysis& analysis) {
  const ArArAnalysisData* now = analysis_data_of(analysis);
  auto copy = now != nullptr ? std::make_shared<ArArAnalysisData>(*now) : std::make_shared<ArArAnalysisData>();
  ArArAnalysisData& out = *copy;
  analysis.extension = std::move(copy);
  return out;
}

}  // namespace pychron::arar
