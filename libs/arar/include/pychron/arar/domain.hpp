#pragma once

// The Ar-Ar domain (domain seam spec 4): core's units plus the Ar-Ar figures,
// the age cache, the recall rows and the Schaen et al. (2021) export.

#include <memory>

#include "pychron/processing/domain.hpp"

namespace pychron::arar {

std::unique_ptr<processing::Domain> make_domain();

}  // namespace pychron::arar
