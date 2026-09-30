#include "pychron/devices/spectrometer/legacy/pulse_counter.hpp"

#include <mutex>

#include "pychron/codecs/pulse_counter.hpp"

namespace pychron::spectrometer {

namespace pc = codec::pulse_counter;

PulseCounter::PulseCounter(std::string name, Transport& transport, std::vector<ChannelId> channels, double sample_hz,
                           const Clock* clock)
    : PolledAcquirer(std::move(name), std::move(channels), sample_hz, clock), transport_(transport) {}

DriverSchema PulseCounter::schema() {
  return {"",
          "legacy serial pulse counter (ion counting); raw count frames, host integrates",
          {legacy::kRolesKey,
           {"channels", KeyType::StringArray, true, "channel names, in the counter's reply order"},
           {"sample_hz", KeyType::Float, false, "poll rate; default 10"}}};
}

Result<std::unique_ptr<PulseCounter>> PulseCounter::create(const DriverArgs& args) {
  auto channels = legacy::parse_channels(args);
  if (!channels) return fail(std::move(channels).error());
  auto hz = legacy::parse_sample_hz(args, 10.0);
  if (!hz) return fail(std::move(hz).error());
  return std::make_unique<PulseCounter>(args.name, args.transport, std::move(*channels), *hz, args.clock);
}

Result<std::vector<double>> PulseCounter::sample() {
  const auto cmd = pc::read_counts();
  auto reply = transport_.exchange(cmd.tx, *cmd.reply);
  if (!reply) return fail(std::move(reply).error());
  auto counts = pc::decode_counts(channels().size(), *reply);
  if (!counts) return fail(std::move(counts).error());
  std::vector<double> values;
  values.reserve(counts->size());
  for (auto n : *counts) values.push_back(static_cast<double>(n));
  return values;
}

// A read resets the counter; the counts accumulated while stopped are
// discarded.
Result<void> PulseCounter::prime() {
  auto discarded = sample();
  if (!discarded) return fail(std::move(discarded).error());
  return {};
}

SimTransport::Hook counter_sim_hook(CounterSimModel model) {
  struct State {
    CounterSimModel model;
    std::mutex mutex;

    Bytes respond(const Bytes& tx) {
      std::lock_guard lock(mutex);
      if (!pc::is_read_request(tx)) return pc::encode_error("SYNTAX");
      std::vector<std::uint64_t> counts(model.channels, 0);
      for (std::size_t i = 0; i < counts.size(); ++i) counts[i] = model.counts ? model.counts(i) : 0;
      return pc::encode_counts(counts);
    }
  };
  auto state = std::make_shared<State>();
  state->model = std::move(model);
  return [state](const Bytes& tx) { return state->respond(tx); };
}

REGISTER_DRIVER("pulse_counter", PulseCounter);

}  // namespace pychron::spectrometer
