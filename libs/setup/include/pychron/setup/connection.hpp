#pragma once

// Connecting to an instrument the way the programs do (installation wizard
// spec 3.5 Test connection; elctl doctor --probe): the spectrometer config is
// assembled with its real drivers, so every transport opens and every driver
// runs its connect step (NGX: banner and login; Qtegra: the
// RemoteControlServer handshake), then it is taken down again. Nothing is
// moved or acquired.

#include <filesystem>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/setup/profile.hpp"

namespace pychron::setup {

// What was reached ("spec: isotopx_ngx at 10.0.0.20:1099"), or the first
// transport or driver that failed. A simulated config connects to nothing
// and says so.
Result<std::string> connect_spectrometer(const std::filesystem::path& spectrometer_toml);

// The same for answers not installed yet: the profile is rendered into a
// scratch folder, its spectrometer.toml connected, and the folder removed.
Result<std::string> test_instrument_connection(const ProfileLibrary& library, const ResolvedProfile& profile,
                                               const Answers& answers);

}  // namespace pychron::setup
