#include "pychron/devices/spectrometer/legacy/serial_hv.hpp"

#include <cmath>
#include <mutex>

#include "pychron/codecs/hv_supply.hpp"
#include "pychron/devices/spectrometer/legacy/polled_acquirer.hpp"

namespace pychron::spectrometer {

namespace hv = codec::hv_supply;

namespace {

const ParamId kHv{SourceParam::HV};

bool is_hv(const ParamId& id) { return id == kHv; }

Error unsupported_param(const ParamId& id) {
  return Error{ErrorKind::Config, "serial_hv: param " + to_string(id) + " not supported (HV only)", {}};
}

}  // namespace

SerialHv::SerialHv(std::string name, Transport& transport, double max_hv, const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      params_{ParamSpec{kHv, Unit::Volts, {0.0, max_hv}, true, true, "HV"}} {}

DriverSchema SerialHv::schema() {
  return {"",
          "legacy serial ion-source HV supply (VSET/VOUT ASCII); IBeamSource with HV only",
          {legacy::kRolesKey, {"max_hv", KeyType::Float, false, "highest accepted setpoint in volts; default 10000"}}};
}

Result<std::unique_ptr<SerialHv>> SerialHv::create(const DriverArgs& args) {
  const double max_hv = args.options["max_hv"].value_or(10000.0);
  if (!(max_hv > 0.0) || !std::isfinite(max_hv)) return fail(ErrorKind::Config, "max_hv must be positive");
  return std::make_unique<SerialHv>(args.name, args.transport, max_hv, args.clock);
}

Result<void> SerialHv::set_hv(double volts) {
  const Range range = params_.front().range;
  if (!std::isfinite(volts) || !range.contains(volts)) {
    return observe(Result<void>(fail(ErrorKind::Config, "serial_hv: " + std::to_string(volts) +
                                                            " V is outside 0.." + std::to_string(range.max))));
  }
  auto cmd = hv::set_voltage(volts);
  if (!cmd) return observe(Result<void>(fail(std::move(cmd).error())));
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(hv::decode_ok(*reply));
}

Result<double> SerialHv::read_hv() {
  const auto cmd = hv::read_output();
  auto reply = transport_.exchange(cmd.tx, *cmd.reply);
  if (!reply) return observe(Result<double>(fail(std::move(reply).error())));
  return observe(hv::decode_voltage(*reply));
}

Result<void> SerialHv::set_param(const ParamId& id, double value) {
  if (!is_hv(id)) return observe(Result<void>(fail(unsupported_param(id))));
  return set_hv(value);
}

Result<Readback> SerialHv::read_param(const ParamId& id) {
  if (!is_hv(id)) return observe(Result<Readback>(fail(unsupported_param(id))));
  // Setpoint and output as one bus step so no other command interleaves.
  auto rb = transact(transport_, [&]() -> Result<Readback> {
    const auto set_cmd = hv::read_setpoint();
    auto set_reply = transport_.exchange(set_cmd.tx, *set_cmd.reply);
    if (!set_reply) return fail(std::move(set_reply).error());
    auto setpoint = hv::decode_voltage(*set_reply);
    if (!setpoint) return fail(std::move(setpoint).error());
    const auto out_cmd = hv::read_output();
    auto out_reply = transport_.exchange(out_cmd.tx, *out_cmd.reply);
    if (!out_reply) return fail(std::move(out_reply).error());
    auto output = hv::decode_voltage(*out_reply);
    if (!output) return fail(std::move(output).error());
    return Readback{*setpoint, *output};
  });
  return observe(std::move(rb));
}

SimTransport::Hook hv_sim_hook(HvSimModel model) {
  struct State {
    HvSimModel model;
    std::mutex mutex;
    double setpoint = 0.0;

    Bytes respond(const Bytes& tx) {
      std::lock_guard lock(mutex);
      auto request = hv::decode_request(tx);
      if (!request) return hv::encode_error("SYNTAX");
      switch (request->kind) {
        case hv::Request::Kind::Set:
          if (request->volts > model.max_hv) return hv::encode_error("RANGE");
          setpoint = request->volts;
          if (model.on_set) model.on_set(setpoint);
          return hv::encode_ok();
        case hv::Request::Kind::ReadSetpoint:
          return hv::encode_voltage(setpoint);
        case hv::Request::Kind::ReadOutput:
          return hv::encode_voltage(model.output ? model.output(setpoint) : setpoint);
      }
      return hv::encode_error("SYNTAX");
    }
  };
  auto state = std::make_shared<State>();
  state->setpoint = model.initial;
  state->model = std::move(model);
  return [state](const Bytes& tx) { return state->respond(tx); };
}

REGISTER_DRIVER("serial_hv", SerialHv);

}  // namespace pychron::spectrometer
