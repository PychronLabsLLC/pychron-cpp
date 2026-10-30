#pragma once

// `elctl sim serve`: the line's simulated lab on sockets, one listener per
// transport of its config, until interrupted (sim serve spec,
// docs/superpowers/specs/2026-10-09-sim-serve-design.md section 4.7). A client
// given the endpoints file it writes (--sim-endpoints) drives the lab through
// its real TCP and UDP transports.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

// `args` follow `sim serve`. `line_file` is the line the global options name
// (--config here goes over it); `install_canvas` the install's, or empty: then a
// canvas.toml beside the line, if there is one. `install_spectrometer` is
// served unless --spectrometer names another or --no-spectrometer none; empty
// or missing: none. Serves until `stop()` is true, polled every 100 ms, then
// removes the endpoints file.
int sim_serve_command(const std::vector<std::string>& args, const std::filesystem::path& line_file,
                      const std::filesystem::path& install_canvas, const std::filesystem::path& install_spectrometer,
                      Io io, const std::function<bool()>& stop);

}  // namespace elctl
