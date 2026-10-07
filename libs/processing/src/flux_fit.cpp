#include "pychron/processing/flux_fit.hpp"

#include <cmath>

namespace pychron::processing {

reduction::Measured MonitorSet::lambda_k() const {
  return {lambda_ec.value + lambda_b.value, std::hypot(lambda_ec.error, lambda_b.error)};
}

reduction::MonitorConstants MonitorSet::constants() const { return {age_ma * 1e6, lambda_k().value}; }

}  // namespace pychron::processing
