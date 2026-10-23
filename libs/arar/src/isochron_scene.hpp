#pragma once

// The inverse isochron's scene builder, shared by the isochron figure and the
// spectrum-isochron pair (private to libs/arar).

#include <set>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"

namespace pychron::processing::detail {

// The inverse isochron (arar_figures.hpp). With `plateau_steps` (analysis
// uuids), "exclude_non_plateau" leaves out every step not among them, in
// place of the plateau the isochron would look for itself.
Result<Scene> build_isochron_scene(const Dataset& dataset, const Options& options,
                                   const std::set<std::string>* plateau_steps);

}  // namespace pychron::processing::detail
