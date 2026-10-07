#pragma once

// Flux fitting (flux fitting design): the monitor standard a lab irradiates
// beside its samples. Qt-free and JSON-free; the store adapter reads and
// writes the document.

#include <string>

#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/flux.hpp"

namespace pychron::processing {

// One monitor standard: its age and the decay constants it is quoted with.
struct MonitorSet {
  std::string name, sample, material;
  double age_ma = 0, age_err_ma = 0;
  reduction::Measured lambda_ec, lambda_b;  // 1/a, 1 sigma
  reduction::Measured lambda_k() const;     // sum, errors in quadrature
  reduction::MonitorConstants constants() const;  // {age_ma * 1e6, lambda_k().value}

  friend bool operator==(const MonitorSet& a, const MonitorSet& b) {
    return a.name == b.name && a.sample == b.sample && a.material == b.material && a.age_ma == b.age_ma &&
           a.age_err_ma == b.age_err_ma && a.lambda_ec.value == b.lambda_ec.value &&
           a.lambda_ec.error == b.lambda_ec.error && a.lambda_b.value == b.lambda_b.value &&
           a.lambda_b.error == b.lambda_b.error;
  }
};

}  // namespace pychron::processing
