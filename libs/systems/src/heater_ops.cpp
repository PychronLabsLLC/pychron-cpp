#include "pychron/systems/heater_ops.hpp"

#include <algorithm>
#include <cmath>

#include "pychron/devices/extraction/capability.hpp"

namespace pychron::systems {

namespace {

template <class T>
Result<std::optional<T>> optional_field(Result<T> r) {
  if (r) return std::optional<T>(*r);
  if (extraction::is_not_supported(r.error())) return std::optional<T>{};
  return fail(std::move(r).error());
}

std::string on_off(bool on) { return on ? "on" : "off"; }

Result<void> set_flag(const std::string& name, const char* what, bool on, Result<void> set, Result<bool> got) {
  if (!set) return set;
  if (!got) return fail(std::move(got).error());
  if (*got != on) {
    return fail(ErrorKind::Protocol, std::string(what) + " set " + on_off(on) + " but reads back " + on_off(*got),
                name);
  }
  return {};
}

}  // namespace

Result<HeaterSample> read_heater_sample(const std::string& name, IHeater& heater, TimePoint ts) {
  auto readback = optional_field(heater.readback());
  if (!readback) return fail(std::move(readback).error());
  auto setpoint = optional_field(heater.setpoint());
  if (!setpoint) return fail(std::move(setpoint).error());
  auto enabled = optional_field(heater.enabled());
  if (!enabled) return fail(std::move(enabled).error());
  auto use_pid = optional_field(heater.use_pid());
  if (!use_pid) return fail(std::move(use_pid).error());
  return HeaterSample{name, *readback, *setpoint, *enabled, *use_pid, ts};
}

Result<void> set_heater_enabled(const std::string& name, IHeater& heater, bool on) {
  auto set = heater.set_enabled(on);
  if (!set) return set;
  return set_flag(name, "enable", on, std::move(set), heater.enabled());
}

Result<void> set_heater_pid(const std::string& name, IHeater& heater, bool on) {
  auto set = heater.set_use_pid(on);
  if (!set) return set;
  return set_flag(name, "use_pid", on, std::move(set), heater.use_pid());
}

Result<void> set_heater_setpoint(const std::string& name, IHeater& heater, double value) {
  if (auto set = heater.set_setpoint(value); !set) return set;
  auto got = heater.setpoint();
  if (!got) return fail(std::move(got).error());
  if (std::abs(value - *got) > 1e-5 * std::max(1.0, std::abs(value))) {
    return fail(ErrorKind::Protocol, "setpoint " + std::to_string(value) + " reads back as " + std::to_string(*got),
                name);
  }
  return {};
}

}  // namespace pychron::systems
