#pragma once

// The Ar-Ar domain (domain seam spec 4): core's units plus the Ar-Ar figures,
// the age cache, the recall rows and the Schaen et al. (2021) export.

#include <memory>

#include "pychron/processing/domain.hpp"

namespace pychron::arar {

std::unique_ptr<processing::Domain> make_domain();

// One Ar-Ar domain for the whole process: what libs/arar parses its own
// quantities with (a figure built outside a run, as publish builds them), so
// they read as the domain's whatever the run context names.
[[nodiscard]] const processing::Domain& shared_domain();

}  // namespace pychron::arar
