#include <pychron/update/update.hpp>

namespace pychron::update {

bool due(const CheckPolicy& policy, std::chrono::system_clock::time_point now) {
  if (!policy.enabled) return false;
  if (!policy.last_check) return true;
  const auto last = *policy.last_check;
  return last > now || now - last >= kCheckInterval;
}

std::optional<Release> worth_showing(const Release& release, Version running, const CheckPolicy& policy) {
  if (release.version <= running) return std::nullopt;
  if (policy.skipped && *policy.skipped == release.version) return std::nullopt;
  return release;
}

}  // namespace pychron::update
