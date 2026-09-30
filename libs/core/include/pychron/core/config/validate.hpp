#pragma once

#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/config/system_config.hpp"

namespace pychron::config {

// Semantic checks on an already-parsed config:
//  - names unique (valves + manual valves share one namespace; gauges; pipettes)
//  - cross-references resolve: driver.transport, valve.actuator, gauge.driver,
//    interlocks / positive_interlocks, pipette inner/outer
//  - valve addresses unique per actuator
//  - no self-interlock; a valve may not list the same valve as both a negative
//    and a positive interlock
//  - no positive-interlock cycles (A needs B open, B needs A open: neither can
//    ever open). Mutual negative interlocks (A blocks B, B blocks A) are the
//    normal way to express mutual exclusion and are allowed.
//  - gauge channel is one of its driver's declared `channels`, when declared
std::vector<Diagnostic> validate(const SystemConfig& config);

}  // namespace pychron::config
