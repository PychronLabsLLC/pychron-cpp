#include "pychron/devices/varian_xgs600.hpp"

#include <set>

namespace pychron {

namespace xgs = codec::varian_xgs600;

namespace {

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

}  // namespace

VarianXgs600::VarianXgs600(std::string name, Transport& transport, std::string address,
                           std::vector<std::string> labels, const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      address_(std::move(address)),
      labels_(std::move(labels)),
      clock_(clock != nullptr ? clock : &steady_clock()) {}

DriverSchema VarianXgs600::schema() {
  return {"",
          "Varian/Agilent XGS-600 gauge controller (legacy XGS600GaugeController); gauge channel n reads "
          "labels[n-1]",
          {{"labels", KeyType::StringArray, true,
            "the sensors' user labels, e.g. [\"CNV1\", \"IMG1\"]; channel 1 is the first"},
           {"address", KeyType::String, false, "two hex digits; default \"00\" (RS-232)"}}};
}

Result<std::unique_ptr<VarianXgs600>> VarianXgs600::create(const DriverArgs& args) {
  const std::string address = args.options["address"].value_or(std::string("00"));
  if (auto ok = xgs::validate_address(address); !ok) return fail(std::move(ok).error());
  std::vector<std::string> labels;
  std::set<std::string> seen;
  if (const auto* array = args.options["labels"].as_array()) {
    for (const auto& element : *array) {
      const std::string label = element.value_or(std::string{});
      if (auto ok = xgs::validate_label(label); !ok) return fail(std::move(ok).error());
      if (!seen.insert(label).second) return fail(ErrorKind::Config, "labels: \"" + label + "\" listed twice");
      labels.push_back(label);
    }
  }
  if (labels.empty()) return fail(ErrorKind::Config, "labels: at least one sensor label is required");
  return std::make_unique<VarianXgs600>(args.name, args.transport, address, std::move(labels), args.clock);
}

std::vector<int> VarianXgs600::pressure_channels() const {
  std::vector<int> out;
  for (int i = 1; i <= static_cast<int>(labels_.size()); ++i) out.push_back(i);
  return out;
}

Result<double> VarianXgs600::read_pressure() { return read_pressure(1); }

Result<double> VarianXgs600::read_pressure(int channel) {
  if (channel < 1 || channel > static_cast<int>(labels_.size())) {
    return observe(Result<double>(fail(ErrorKind::Config, "channel " + std::to_string(channel) +
                                                              " has no label (labels has " +
                                                              std::to_string(labels_.size()) + ")")));
  }
  auto cmd = xgs::read_pressure(address_, labels_[static_cast<std::size_t>(channel - 1)]);
  if (!cmd) return observe(Result<double>(fail(std::move(cmd).error())));
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return observe(Result<double>(fail(std::move(reply).error())));
  return observe(xgs::decode_pressure(*reply));
}

Result<Sample> VarianXgs600::sample() {
  auto p = read_pressure();
  if (!p) return fail(std::move(p).error());
  return Sample{name(), clock_->now(), *p};
}

SimTransport::Hook xgs600_sim_hook(Xgs600SimModel model) {
  return [model = std::move(model)](const Bytes& tx) -> Bytes {
    const std::string text = to_string(tx);
    if (!text.starts_with("#" + model.address)) return {};  // another controller on the bus
    auto label = xgs::requested_label(model.address, tx);
    if (!label) return xgs::encode_rejected();
    auto p = model.pressure ? model.pressure(*label) : std::nullopt;
    return p ? xgs::encode_pressure(*p) : xgs::encode_off();
  };
}

}  // namespace pychron

REGISTER_DRIVER("varian_xgs600", pychron::VarianXgs600);
