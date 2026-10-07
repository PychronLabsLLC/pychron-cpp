#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"

#include <algorithm>
#include <set>

namespace pychron::spectrometer {

namespace {

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

}  // namespace

PolledAcquirer::PolledAcquirer(std::string name, std::vector<ChannelId> channels, double sample_hz,
                               const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      channels_(std::move(channels)),
      period_(std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / sample_hz))),
      clock_(clock != nullptr ? *clock : steady_clock()) {}

Result<void> PolledAcquirer::configure(Duration integration) {
  if (integration <= Duration::zero()) {
    return observe(Result<void>(fail(ErrorKind::Config, "integration time must be positive")));
  }
  std::lock_guard lock(mutex_);
  integration_ = integration;
  return {};
}

Duration PolledAcquirer::integration() const {
  std::lock_guard lock(mutex_);
  return integration_;
}

Result<void> PolledAcquirer::start() {
  if (auto primed = prime(); !primed) return observe(std::move(primed));
  {
    std::lock_guard lock(mutex_);
    running_ = true;
    due_ = clock_.now();
  }
  clock_.notify_all(cv_);  // a next() asleep until the old due time
  return {};
}

Result<void> PolledAcquirer::stop() {
  {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
  clock_.notify_all(cv_);
  return {};
}

Result<std::optional<Frame>> PolledAcquirer::next(Duration timeout) {
  std::uint64_t seq = 0;
  {
    std::unique_lock lock(mutex_);
    if (!running_) return observe(Result<std::optional<Frame>>(fail(ErrorKind::Config, "acquirer not started")));
    // Bounded by the clock's time alone; stop() notifies.
    const TimePoint deadline = clock_.now() + timeout;
    while (running_ && clock_.now() < due_) {
      if (clock_.now() >= deadline) return std::optional<Frame>{};
      clock_.wait_until(cv_, lock, std::min(due_, deadline));
    }
    if (!running_) return std::optional<Frame>{};
    seq = ++seq_;
    due_ = std::max(due_ + period_, clock_.now());
  }

  Frame frame;
  frame.ts = clock_.now();
  frame.seq = seq;
  frame.integrated = false;
  frame.span = period_;
  auto values = sample();
  if (!values) return observe(Result<std::optional<Frame>>(fail(std::move(values).error())));
  if (values->size() != channels_.size()) {
    return observe(Result<std::optional<Frame>>(fail(ErrorKind::Protocol, "sample has " +
                                                                              std::to_string(values->size()) +
                                                                              " values for " +
                                                                              std::to_string(channels_.size()) +
                                                                              " channels")));
  }
  frame.values.reserve(channels_.size());
  for (std::size_t i = 0; i < channels_.size(); ++i) frame.values.emplace_back(channels_[i], (*values)[i]);
  return observe(Result<std::optional<Frame>>(std::optional<Frame>{std::move(frame)}));
}

namespace legacy {

Result<std::vector<ChannelId>> parse_channels(const DriverArgs& args) {
  const toml::array* array = args.options["channels"].as_array();
  if (array == nullptr || array->empty()) {
    return fail(ErrorKind::Config, "channels: at least one channel name is required");
  }
  std::vector<ChannelId> channels;
  std::set<ChannelId> seen;
  for (const auto& element : *array) {
    auto name = element.value<std::string>();
    if (!name || name->empty()) return fail(ErrorKind::Config, "channels: names must be non-empty strings");
    if (!seen.insert(*name).second) return fail(ErrorKind::Config, "channels: \"" + *name + "\" listed twice");
    channels.push_back(*name);
  }
  return channels;
}

Result<double> parse_sample_hz(const DriverArgs& args, double fallback) {
  const double hz = args.options["sample_hz"].value_or(fallback);
  if (!(hz > 0.0) || hz > 1e6) return fail(ErrorKind::Config, "sample_hz must be in (0, 1e6]");
  return hz;
}

}  // namespace legacy

}  // namespace pychron::spectrometer
