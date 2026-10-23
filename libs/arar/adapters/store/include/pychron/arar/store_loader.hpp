#pragma once

// The Ar-Ar extension loader of the store source (domain seam spec 3.3): the
// flux, production ratios and chronology of an analysis from the references
// the store resolved for it, as StoreSource read them before the domain seam
// (same references, so the input fingerprint covers them: it hashes every
// resolved reference with the heads and the reduction version).

#include "pychron/processing/store_source.hpp"

namespace pychron::arar {

// Makes an ArArAnalysisData (analysis_data.hpp) of every analysis it is given,
// empty parts and all: a J (with its unknown error kept NaN), the production
// ratios (none when the production has no ratios: reference data the source
// removed) with their reactor, and the chronology. Fails, with the analysis's
// run id, when the production's ratios cannot be read.
[[nodiscard]] processing::StoreSourceOptions::ExtensionLoader store_extension_loader();

// `options` with the Ar-Ar domain's reduction version and
// store_extension_loader(): how Ar-Ar-only code (the flux window, publishing,
// EarthBank) and its tests open a StoreSource. An application that runs on
// the active domain sets both from that domain instead.
[[nodiscard]] processing::StoreSourceOptions store_source_options(processing::StoreSourceOptions options = {});

}  // namespace pychron::arar
