#include "pychron/devices/gp_microion.hpp"

#include <algorithm>
#include <mutex>

namespace pychron {

namespace mi = codec::microion;

namespace {

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

Result<int> parse_address(const toml::table& options) {
  auto value = options["address"].value<std::int64_t>();
  if (!value) return 1;
  if (*value < mi::kMinAddress || *value > mi::kMaxAddress) {
    return fail(ErrorKind::Config, "address: " + std::to_string(*value) + " is outside 0..255");
  }
  return static_cast<int>(*value);
}

Result<std::vector<int>> parse_channels(const toml::table& options) {
  const toml::node* node = options.get("channels");
  if (node == nullptr) return std::vector<int>{static_cast<int>(mi::Sensor::IonGauge)};

  const toml::array* array = node->as_array();
  if (array == nullptr) return fail(ErrorKind::Config, "channels: expected an array of integers");
  if (array->empty()) return fail(ErrorKind::Config, "channels: at least one channel is required");

  std::vector<int> channels;
  for (const auto& element : *array) {
    auto value = element.value<std::int64_t>();
    if (!value) return fail(ErrorKind::Config, "channels: expected an array of integers");
    if (*value < mi::kFirstChannel || *value > mi::kLastChannel) {
      return fail(ErrorKind::Config, "channels: " + std::to_string(*value) + " is outside " +
                                         std::to_string(mi::kFirstChannel) + ".." +
                                         std::to_string(mi::kLastChannel));
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

GpMicroIon::GpMicroIon(std::string name, Transport& transport, int address, std::vector<int> channels,
                       const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      address_(address),
      channels_(std::move(channels)),
      clock_(clock != nullptr ? clock : &steady_clock()) {}

DriverSchema GpMicroIon::schema() {
  return {"",
          "Granville-Phillips Micro-Ion gauge controller: ion gauge + two Convectrons (RS-485 ASCII)",
          {{"address", KeyType::Integer, false, "RS-485 address, 0..255 (sent as two hex digits); default 1"},
           {"channels", KeyType::IntegerArray, false,
            "sensors in use: 1 = ion gauge, 2 = Convectron A, 3 = Convectron B; the first is read by "
            "read_pressure()/sample(); default [1]"}}};
}

Result<std::unique_ptr<GpMicroIon>> GpMicroIon::create(const DriverArgs& args) {
  auto address = parse_address(args.options);
  if (!address) return fail(std::move(address).error());
  auto channels = parse_channels(args.options);
  if (!channels) return fail(std::move(channels).error());
  return std::make_unique<GpMicroIon>(args.name, args.transport, *address, std::move(*channels), args.clock);
}

Result<double> GpMicroIon::read_pressure() { return read_pressure(channels_.front()); }

Result<double> GpMicroIon::read_pressure(int channel) {
  if (std::ranges::find(channels_, channel) == channels_.end()) {
    return observe(Result<double>(
        fail(ErrorKind::Config, "channel " + std::to_string(channel) + " is not configured")));
  }
  auto cmd = mi::read_pressure(address_, channel);
  if (!cmd) return observe(Result<double>(fail(std::move(cmd).error())));
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return observe(Result<double>(fail(std::move(reply).error())));
  return observe(mi::decode_pressure(address_, *reply));
}

Result<Sample> GpMicroIon::sample() {
  auto p = read_pressure();
  if (!p) return fail(std::move(p).error());
  return Sample{name(), clock_->now(), *p};
}

Result<void> GpMicroIon::set_ion_gauge(bool on) {
  auto cmd = mi::set_ion_gauge(address_, on);
  if (!cmd) return observe(Result<void>(fail(std::move(cmd).error())));
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(mi::decode_ok(address_, *reply));
}

REGISTER_DRIVER("gp_microion", GpMicroIon);

// --- SimSystem hook -----------------------------------------------------------

SimTransport::Hook microion_sim_hook(MicroIonSimModel model) {
  struct State {
    MicroIonSimModel model;
    std::mutex mutex;  // in case one hook serves several transports
    bool ion_gauge_on = true;

    double reading(int channel) const {
      if (channel == static_cast<int>(mi::Sensor::IonGauge) && !ion_gauge_on) return mi::kGaugeOff;
      auto p = model.pressure ? model.pressure(channel) : std::nullopt;
      return p ? *p : mi::kGaugeOff;
    }

    Bytes respond(const Bytes& tx) {
      std::lock_guard lock(mutex);
      auto request = mi::decode_request(tx);
      if (!request) {
        return mi::addressee(tx) == model.address ? mi::encode_error(model.address, "SYNTX ER") : Bytes{};
      }
      if (request->address != model.address) return Bytes{};
      switch (request->kind) {
        case mi::Request::Kind::Pressure:
          return mi::encode_pressure(model.address, reading(request->channel));
        case mi::Request::Kind::IonGaugeOn:
          ion_gauge_on = true;
          return mi::encode_ok(model.address);
        case mi::Request::Kind::IonGaugeOff:
          ion_gauge_on = false;
          return mi::encode_ok(model.address);
      }
      return mi::encode_error(model.address, "SYNTX ER");
    }
  };

  auto state = std::make_shared<State>();
  state->ion_gauge_on = model.ion_gauge_on;
  state->model = std::move(model);
  return [state](const Bytes& tx) { return state->respond(tx); };
}

}  // namespace pychron
