#include "pychron/systems/line_cryo_service.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace pychron::systems {

namespace {

std::string kelvin(double k) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f K", k);
  return buf;
}

}  // namespace

LineCryoService::LineCryoService(ExtractionLine& line) : line_(line) {}

Result<ITemperatureController*> LineCryoService::controller() const {
  if (!line_.config().cryo) return fail(ErrorKind::Config, "the line has no [cryo]");
  auto* tc = line_.cryostat();
  if (tc == nullptr)
    return fail(ErrorKind::Config, "[cryo] driver '" + line_.config().cryo->driver + "' is not a temperature controller");
  return tc;
}

Result<void> LineCryoService::apply(const std::map<int, double>& setpoints) {
  auto tc = controller();
  if (!tc) return fail(std::move(tc).error());
  for (const auto& [output, k] : setpoints) {
    if (auto ok = (*tc)->set_setpoint(output, k); !ok) return ok;
  }
  std::lock_guard lock(mutex_);
  targets_ = setpoints;
  set_at_ = line_.clock().now();
  return {};
}

Result<void> LineCryoService::set_cryo(double setpoint_kelvin) { return apply({{1, setpoint_kelvin}}); }

Result<void> LineCryoService::set_cryo_named(std::string_view name) {
  if (!line_.config().cryo) return fail(ErrorKind::Config, "the line has no [cryo]");
  const auto& named = line_.config().cryo->setpoints;
  auto it = named.find(std::string(name));
  if (it == named.end()) {
    std::string known;
    for (const auto& [n, _] : named) known += (known.empty() ? "" : ", ") + n;
    return fail(ErrorKind::Config, "no cryo setpoint named '" + std::string(name) + "'" +
                                       (known.empty() ? std::string(" ([cryo.setpoints] is empty)") : " (" + known + ")"));
  }
  std::map<int, double> setpoints;
  for (std::size_t i = 0; i < it->second.size(); ++i) setpoints[static_cast<int>(i) + 1] = it->second[i];
  return apply(setpoints);
}

Result<double> LineCryoService::get_cryo_temp(int channel) {
  auto tc = controller();
  if (!tc) return fail(std::move(tc).error());
  const auto inputs = (*tc)->inputs();
  if (channel < 1 || channel > static_cast<int>(inputs.size()))
    return fail(ErrorKind::Config, "cryo channel " + std::to_string(channel) + " is not 1.." +
                                       std::to_string(inputs.size()));
  return (*tc)->read_temperature(inputs[static_cast<std::size_t>(channel - 1)]);
}

Result<bool> LineCryoService::cryo_settling() {
  std::map<int, double> targets;
  TimePoint set_at;
  {
    std::lock_guard lock(mutex_);
    targets = targets_;
    set_at = set_at_;
  }
  if (targets.empty()) return false;
  auto tc = controller();
  if (!tc) return fail(std::move(tc).error());
  const auto& cryo = *line_.config().cryo;
  const auto inputs = (*tc)->inputs();
  std::string waiting;
  for (const auto& [output, target] : targets) {
    if (output > static_cast<int>(inputs.size())) continue;  // no input to wait on
    const auto& input = inputs[static_cast<std::size_t>(output - 1)];
    auto t = (*tc)->read_temperature(input);
    if (!t) return fail(std::move(t).error());
    if (std::fabs(*t - target) > cryo.tolerance_k) {
      waiting = "input " + input + " at " + kelvin(*t) + ", output " + std::to_string(output) + " set to " +
                kelvin(target);
      break;
    }
  }
  if (waiting.empty()) return false;
  const double elapsed = std::chrono::duration<double>(line_.clock().now() - set_at).count();
  if (elapsed > cryo.timeout_s) {
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.0f", cryo.timeout_s);
    return fail(ErrorKind::Io, std::string("the cryostat did not reach its setpoint within ") + seconds + " s: " + waiting,
                cryo.driver);
  }
  return true;
}

}  // namespace pychron::systems
