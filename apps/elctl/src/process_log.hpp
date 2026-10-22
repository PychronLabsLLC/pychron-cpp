#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/log_hub.hpp"

namespace elctl {

// Reads --install / --config / --sim from the global options before the command
// (as run() does), resolves the install (resolve_install; none is fine), loads the
// logging config, sets file_name = "elctl.log", resolves the folder, creates the hub,
// logs the notes and begin_session. Never fails: on error, one stderr line, null hub.
// A usage error in the options is not reported here: run() reports it.
std::shared_ptr<pychron::LogHub> open_process_log(const std::vector<std::string>& args,
                                                  const pychron::Clock& clock, pychron::HostOs os,
                                                  const pychron::EnvLookup& env, std::ostream& err);

}  // namespace elctl
