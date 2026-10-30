#pragma once

// A spectrometer's wire emulator measuring a simulated beam (sim serve spec,
// docs/superpowers/specs/2026-10-09-sim-serve-design.md phase B): what the real
// driver sets moves the beam, what it reads is the beam's. Through the
// emulators' seams (QtegraSimModel::data_source and on_command,
// NgxSimModel::values_at and on_command), so the emulators stay what tests
// script them to be when nothing is fed.
//
// Lock order: the emulator's mutex, then the beam's (and the line's, through
// the beam's gas provider). The beam never calls an emulator.

#include <memory>
#include <string>
#include <vector>

#include "pychron/devices/spectrometer/ngx_sim.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"

namespace pychron::sim {

// SetMagnetDAC, SetHV, SetDeflection, SetGain, ProtectDetector, BlankBeam and
// SetParameter (a hardware name, through codec::qtegra::canonical_name and
// spectrometer::parse_param_id) move the beam; GetData reads every beam
// detector at the clock's now, over the integration time. The model's and the
// beam's HV start at the beam's nominal. Set before the model is used.
void feed_qtegra_from_beam(spectrometer::QtegraSimModel& model, const std::shared_ptr<BeamModel>& beam);

// SetMass moves the beam's magnet to the mass (a mass axis, as the simulated
// positioner's), SSO sets the source parameter its mnemonic names (IE the
// HV); an ACQ line carries `channels` read at its instant over one second.
// Set before the model is used.
void feed_ngx_from_beam(spectrometer::NgxSimModel& model, const std::shared_ptr<BeamModel>& beam,
                        std::vector<std::string> channels);

}  // namespace pychron::sim
