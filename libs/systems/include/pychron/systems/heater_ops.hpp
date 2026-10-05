#pragma once

// What every client does to a heater (plan 2026-10-05, E1): read all of it
// at once, and write a field then read the same field back. ExtractionLine
// and elctl both go through these.

#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/devices/heater.hpp"

namespace pychron::systems {

// Every field; one the driver does not support is nullopt, any other
// failure fails the read. `name` is the [[heaters]] name.
Result<HeaterSample> read_heater_sample(const std::string& name, IHeater& heater, TimePoint ts);

// Write, then read back the same field: a different value is a Protocol
// error naming `name` (legacy never checked). A setpoint reads back equal
// within 1e-5 relative, as it may come back through a float32 register.
Result<void> set_heater_enabled(const std::string& name, IHeater& heater, bool on);
Result<void> set_heater_setpoint(const std::string& name, IHeater& heater, double value);
Result<void> set_heater_pid(const std::string& name, IHeater& heater, bool on);

}  // namespace pychron::systems
