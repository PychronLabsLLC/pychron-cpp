#include "pychron/devices/spectrometer/legacy/adc_bank.hpp"

#include <cmath>
#include <mutex>

#include "pychron/codecs/modbus_adc.hpp"

namespace pychron::spectrometer {

namespace mb = codec::modbus_adc;

AdcBank::AdcBank(std::string name, Transport& transport, std::vector<ChannelId> channels, Options options,
                 const Clock* clock)
    : PolledAcquirer(std::move(name), std::move(channels), options.sample_hz, clock),
      transport_(transport),
      options_(options) {}

DriverSchema AdcBank::schema() {
  return {"",
          "legacy Faraday ADC bank over Modbus TCP (float32 input registers); raw volt samples, host integrates",
          {legacy::kRolesKey,
           {"channels", KeyType::StringArray, true, "channel names, one float (two registers) each, in order"},
           {"sample_hz", KeyType::Float, false, "poll rate; default 100"},
           {"unit", KeyType::Integer, false, "Modbus unit id, 0..255; default 1"},
           {"start_register", KeyType::Integer, false, "first input register of channel 0; default 0"},
           {"scale", KeyType::Float, false, "multiplier applied to every sample; default 1"}}};
}

Result<std::unique_ptr<AdcBank>> AdcBank::create(const DriverArgs& args) {
  const auto& o = args.options;
  auto channels = legacy::parse_channels(args);
  if (!channels) return fail(std::move(channels).error());
  if (channels->size() * mb::kRegistersPerChannel > mb::kMaxRegisters) {
    return fail(ErrorKind::Config, "channels: at most 62 channels per bank");
  }
  auto hz = legacy::parse_sample_hz(args, 100.0);
  if (!hz) return fail(std::move(hz).error());
  const auto unit = o["unit"].value_or(std::int64_t{1});
  if (unit < 0 || unit > 255) return fail(ErrorKind::Config, "unit: " + std::to_string(unit) + " is outside 0..255");
  const auto start = o["start_register"].value_or(std::int64_t{0});
  const auto last = start + static_cast<std::int64_t>(channels->size()) * mb::kRegistersPerChannel;
  if (start < 0 || last > 0x10000) {
    return fail(ErrorKind::Config, "start_register: channels must fit in registers 0..65535");
  }
  const double scale = o["scale"].value_or(1.0);
  if (!std::isfinite(scale) || scale == 0.0) return fail(ErrorKind::Config, "scale must be finite and non-zero");

  Options options{*hz, static_cast<std::uint8_t>(unit), static_cast<std::uint16_t>(start), scale};
  return std::make_unique<AdcBank>(args.name, args.transport, std::move(*channels), options, args.clock);
}

Result<std::vector<double>> AdcBank::sample() {
  const auto tid = static_cast<std::uint16_t>(tid_.fetch_add(1) + 1);
  const auto n = channels().size();
  auto cmd = mb::read_channels(tid, options_.unit, options_.start_register, n);
  if (!cmd) return fail(std::move(cmd).error());
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return fail(std::move(reply).error());
  auto volts = mb::decode_channels(tid, options_.unit, n, *reply);
  if (!volts) return fail(std::move(volts).error());
  for (auto& v : *volts) v *= options_.scale;
  return volts;
}

SimTransport::Hook adc_sim_hook(AdcSimModel model) {
  struct State {
    AdcSimModel model;
    std::mutex mutex;

    Bytes respond(const Bytes& tx) {
      std::lock_guard lock(mutex);
      auto request = mb::decode_request(tx);
      if (!request || request->unit != model.unit) return {};
      if (request->function != mb::kReadInputRegisters || request->start < model.start_register ||
          (request->start - model.start_register) % mb::kRegistersPerChannel != 0 ||
          request->count % mb::kRegistersPerChannel != 0) {
        return mb::encode_exception(request->tid, request->unit, request->function, mb::kIllegalAddress);
      }
      const std::size_t first = (request->start - model.start_register) / mb::kRegistersPerChannel;
      const std::size_t n = request->count / mb::kRegistersPerChannel;
      std::vector<std::uint16_t> registers;
      registers.reserve(request->count);
      for (std::size_t i = first; i < first + n; ++i) {
        auto v = model.volts ? model.volts(i) : std::nullopt;
        if (!v) return mb::encode_exception(request->tid, request->unit, request->function, mb::kIllegalAddress);
        auto pair = mb::to_registers(static_cast<float>(*v));
        registers.insert(registers.end(), pair.begin(), pair.end());
      }
      return mb::encode_registers(request->tid, request->unit, registers);
    }
  };
  auto state = std::make_shared<State>();
  state->model = std::move(model);
  return [state](const Bytes& tx) { return state->respond(tx); };
}

REGISTER_DRIVER("adc_bank", AdcBank);

}  // namespace pychron::spectrometer
