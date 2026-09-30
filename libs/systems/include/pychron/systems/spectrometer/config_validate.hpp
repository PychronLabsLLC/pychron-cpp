#pragma once

// Assembler validation rules (spectrometer spec section 7.3), each a pure
// function over an already-parsed config. Every rule returns all of its
// problems; validate() concatenates every rule so a user sees the full list
// in one pass.

#include <map>
#include <string>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"

namespace pychron::spectrometer::cfg {

using Diagnostics = std::vector<config::Diagnostic>;
using Tables = std::map<std::string, TableFile>;

// Names resolve: driver.transport, system.reference_detector; detector names
// unique; no driver lists the same role twice.
Diagnostics check_references(const SpectrometerConfig& c);

// Every role referenced by [magnet].positioner, [source].driver,
// [acquisition].acquirers and [detector_control].driver exists on the named
// driver's `roles`; every driver with the acquirer role declares `channels`.
Diagnostics check_roles(const SpectrometerConfig& c);

// Each detector `channel` resolves to exactly one acquirer channel (and no two
// detectors share one); every acquirer channel is bound or listed in
// acquisition.ignored_channels; ignored channels exist. More than one acquirer
// requires host_integration.
Diagnostics check_channels(const SpectrometerConfig& c);

// magnet.field_table (and hv_table, when set) exists in `tables`, is in the
// positioner's native axis, and has a column for every active detector.
Diagnostics check_field_tables(const SpectrometerConfig& c, const Tables& tables);

// magnet.protection.detectors is a subset of the detectors with `protection`
// config; [detector_control] is present when any detector enables deflection
// control or protection.
Diagnostics check_protection(const SpectrometerConfig& c);

// native_axis = "mass" positioners cannot enable the HV correction; the HV
// correction needs source.nominal_hv.
Diagnostics check_hv_correction(const SpectrometerConfig& c);

// All of the above.
Diagnostics validate(const SpectrometerConfig& c, const Tables& tables);

}  // namespace pychron::spectrometer::cfg
