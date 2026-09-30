#pragma once

// Base for legacy raw acquirers (AdcBank, PulseCounter): the host polls the
// hardware once per sample period and the host integrates. next() waits on
// the injected Clock until the next sample is due, reads one sample, and
// stamps it with host time. A failed read still consumes a seq number, so
// consumers see the gap as a dropped frame.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer {

class PolledAcquirer : public Device, public IIntensityAcquirer {
 public:
  std::vector<ChannelId> channels() const override { return channels_; }
  bool integrates() const override { return false; }
  // The host integrates: the period is validated and recorded, the sample
  // rate is unchanged.
  Result<void> configure(Duration integration) override;
  Result<void> start() override;
  Result<void> stop() override;
  // Config error when not started.
  Result<std::optional<Frame>> next(Duration timeout) override;

  Duration sample_period() const noexcept { return period_; }
  Duration integration() const;

 protected:
  // `channels` non-empty and unique, `sample_hz` > 0 (create() helpers check).
  PolledAcquirer(std::string name, std::vector<ChannelId> channels, double sample_hz, const Clock* clock);

  // One value per channel, in channels() order.
  virtual Result<std::vector<double>> sample() = 0;
  // Called by start() before the first sample (e.g. to reset counters).
  virtual Result<void> prime() { return {}; }

 private:
  std::vector<ChannelId> channels_;
  Duration period_;
  const Clock& clock_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool running_ = false;
  TimePoint due_{};
  std::uint64_t seq_ = 0;
  Duration integration_ = std::chrono::seconds(1);
};

// Shared option parsing for raw acquirer drivers.
namespace legacy {

inline const ConfigKey kRolesKey{"roles", KeyType::StringArray, false, "roles this driver plays (informational)"};

// `channels`: required, non-empty, unique, non-empty names.
Result<std::vector<ChannelId>> parse_channels(const DriverArgs& args);
// `sample_hz`: positive; `fallback` when absent.
Result<double> parse_sample_hz(const DriverArgs& args, double fallback);

}  // namespace legacy

}  // namespace pychron::spectrometer
