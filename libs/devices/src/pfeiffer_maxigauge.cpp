#include "pychron/devices/pfeiffer_maxigauge.hpp"

#include <algorithm>

namespace pychron {

namespace mg = codec::maxigauge;

namespace {

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

Result<std::vector<int>> parse_channels(const toml::table& options) {
  const toml::node* node = options.get("channels");
  if (node == nullptr) return std::vector<int>{mg::kFirstChannel};

  const toml::array* array = node->as_array();
  if (array == nullptr) return fail(ErrorKind::Config, "channels: expected an array of integers");
  if (array->empty()) return fail(ErrorKind::Config, "channels: at least one channel is required");

  std::vector<int> channels;
  for (const auto& element : *array) {
    auto value = element.value<std::int64_t>();
    if (!value) return fail(ErrorKind::Config, "channels: expected an array of integers");
    if (*value < mg::kFirstChannel || *value > mg::kLastChannel) {
      return fail(ErrorKind::Config, "channels: " + std::to_string(*value) + " is outside " +
                                         std::to_string(mg::kFirstChannel) + ".." +
                                         std::to_string(mg::kLastChannel));
    }
    int channel = static_cast<int>(*value);
    if (std::ranges::find(channels, channel) != channels.end()) {
      return fail(ErrorKind::Config, "channels: " + std::to_string(channel) + " listed twice");
    }
    channels.push_back(channel);
  }
  return channels;
}

}  // namespace

PfeifferMaxiGauge::PfeifferMaxiGauge(std::string name, Transport& transport, std::vector<int> channels,
                                     const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      channels_(std::move(channels)),
      clock_(clock != nullptr ? clock : &steady_clock()) {}

DriverSchema PfeifferMaxiGauge::schema() {
  return {"",
          "Pfeiffer MaxiGauge TPG 256 A, six-channel gauge controller (ENQ/ACK ASCII protocol)",
          {{"channels", KeyType::IntegerArray, false,
            "gauge channels in use, 1..6; the first is read by read_pressure()/sample(); default [1]"}}};
}

Result<std::unique_ptr<PfeifferMaxiGauge>> PfeifferMaxiGauge::create(const DriverArgs& args) {
  auto channels = parse_channels(args.options);
  if (!channels) return fail(std::move(channels).error());
  return std::make_unique<PfeifferMaxiGauge>(args.name, args.transport, std::move(*channels), args.clock);
}

Result<Bytes> PfeifferMaxiGauge::query(const codec::Command& mnemonic) {
  std::lock_guard lock(query_mutex_);
  auto ack = transport_.exchange(mnemonic.tx, *mnemonic.reply);
  if (!ack) return fail(std::move(ack).error());
  if (auto ok = mg::decode_ack(*ack); !ok) return fail(std::move(ok).error());

  const codec::Command enq = mg::enquiry();
  return transport_.exchange(enq.tx, *enq.reply);
}

Result<double> PfeifferMaxiGauge::read_pressure() { return read_pressure(channels_.front()); }

Result<double> PfeifferMaxiGauge::read_pressure(int channel) {
  if (std::ranges::find(channels_, channel) == channels_.end()) {
    return observe(Result<double>(
        fail(ErrorKind::Config, "channel " + std::to_string(channel) + " is not configured")));
  }
  auto cmd = mg::read_pressure(channel);
  if (!cmd) return observe(Result<double>(fail(std::move(cmd).error())));
  auto reply = query(*cmd);
  if (!reply) return observe(Result<double>(fail(std::move(reply).error())));
  return observe(mg::decode_pressure(*reply));
}

Result<Sample> PfeifferMaxiGauge::sample() {
  auto p = read_pressure();
  if (!p) return fail(std::move(p).error());
  return Sample{name(), clock_->now(), *p};
}

Result<std::vector<mg::Reading>> PfeifferMaxiGauge::read_all() {
  auto reply = query(mg::read_all_pressures());
  if (!reply) return observe(Result<std::vector<mg::Reading>>(fail(std::move(reply).error())));
  return observe(mg::decode_all_readings(*reply));
}

Result<mg::Units> PfeifferMaxiGauge::read_units() {
  auto reply = query(mg::read_units());
  if (!reply) return observe(Result<mg::Units>(fail(std::move(reply).error())));
  return observe(mg::decode_units(*reply));
}

REGISTER_DRIVER("pfeiffer_maxigauge", PfeifferMaxiGauge);

// --- SimSystem hook -----------------------------------------------------------

SimTransport::Hook maxigauge_sim_hook(MaxiGaugeSimModel model) {
  struct State {
    MaxiGaugeSimModel model;
    std::mutex mutex;                     // in case one hook serves several transports
    std::optional<mg::Request> pending;  // last acknowledged mnemonic

    mg::Reading reading(int channel) const {
      auto p = model.pressure ? model.pressure(channel) : std::nullopt;
      if (!p) return {mg::Status::NoSensor, 0.0};
      return {mg::Status::Ok, *p};
    }

    Bytes respond(const Bytes& tx) {
      std::lock_guard lock(mutex);
      auto request = mg::decode_request(tx);
      if (!request) return mg::encode_nak();
      if (request->kind != mg::Request::Kind::Enquiry) {
        pending = *request;
        return mg::encode_ack();
      }
      if (!pending) return mg::encode_nak();
      switch (pending->kind) {
        case mg::Request::Kind::Pressure:
          return mg::encode_reading(reading(pending->channel));
        case mg::Request::Kind::AllPressures: {
          std::vector<mg::Reading> all;
          for (int ch = mg::kFirstChannel; ch <= mg::kLastChannel; ++ch) all.push_back(reading(ch));
          return mg::encode_all_readings(all);
        }
        case mg::Request::Kind::Units:
          return mg::encode_units(model.units);
        case mg::Request::Kind::Enquiry:
          break;
      }
      return mg::encode_nak();
    }
  };

  auto state = std::make_shared<State>();
  state->model = std::move(model);
  return [state](const Bytes& tx) { return state->respond(tx); };
}

}  // namespace pychron
