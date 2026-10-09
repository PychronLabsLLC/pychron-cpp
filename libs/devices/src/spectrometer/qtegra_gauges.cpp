#include "pychron/devices/spectrometer/qtegra_gauges.hpp"

#include <set>
#include <utility>

#include "pychron/codecs/thermo_qtegra.hpp"

namespace pychron::spectrometer {

namespace q = codec::qtegra;

namespace {

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

Result<q::Terminator> parse_terminator(const std::string& text) {
  if (text == "cr") return q::Terminator::CR;
  if (text == "lf") return q::Terminator::LF;
  if (text == "crlf") return q::Terminator::CRLF;
  return fail(ErrorKind::Config, "terminator must be cr, lf or crlf, not \"" + text + "\"");
}

}  // namespace

QtegraGauges::QtegraGauges(std::string name, QtegraLinkHandle link, std::vector<std::string> parameters,
                           const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      link_(std::move(link)),
      parameters_(std::move(parameters)),
      clock_(clock != nullptr ? clock : &steady_clock()) {}

DriverSchema QtegraGauges::schema() {
  return {"",
          "gauge readbacks through Thermo Qtegra RemoteControl (legacy QtegraGaugeController); gauge channel n reads "
          "parameters[n-1]; on a kind = \"link\" transport it shares thermo_qtegra's connection",
          {{"parameters", KeyType::StringArray, true,
            "Qtegra parameter names, e.g. [\"Ion Gauge MS Readback\"]; channel 1 is the first"},
           {"link", KeyType::String, false,
            "on its own tcp/udp transport: the name its connection is registered under; default: the driver name"},
           {"terminator", KeyType::String, false,
            "on its own transport: write terminator cr (default), lf or crlf; a shared link uses the owner's"}}};
}

Result<std::unique_ptr<QtegraGauges>> QtegraGauges::create(const DriverArgs& args) {
  const auto& o = args.options;
  std::vector<std::string> parameters;
  std::set<std::string> seen;
  if (const auto* array = o["parameters"].as_array()) {
    for (const auto& element : *array) {
      const std::string name = element.value_or(std::string{});
      if (auto ok = q::validate_name(name); !ok) return fail(ErrorKind::Config, "parameters: " + ok.error().what);
      if (!seen.insert(name).second) return fail(ErrorKind::Config, "parameters: \"" + name + "\" listed twice");
      parameters.push_back(name);
    }
  }
  if (parameters.empty()) return fail(ErrorKind::Config, "parameters: at least one Qtegra parameter is required");
  auto terminator = parse_terminator(o["terminator"].value_or(std::string("cr")));
  if (!terminator) return fail(std::move(terminator).error());
  auto link = make_qtegra_link(args.transport, o["link"].value_or(args.name), *terminator,
                               args.clock != nullptr ? *args.clock : steady_clock());
  if (!link) return fail(std::move(link).error());
  return std::make_unique<QtegraGauges>(args.name, std::move(*link), std::move(parameters), args.clock);
}

std::vector<int> QtegraGauges::pressure_channels() const {
  std::vector<int> out;
  for (int i = 1; std::cmp_less_equal(i, parameters_.size()); ++i) out.push_back(i);
  return out;
}

Result<double> QtegraGauges::read_pressure() { return read_pressure(1); }

Result<double> QtegraGauges::read_pressure(int channel) {
  auto value = [&]() -> Result<double> {
    if (channel < 1 || std::cmp_greater(channel, parameters_.size())) {
      return fail(ErrorKind::Config, "channel " + std::to_string(channel) + " has no parameter (parameters has " +
                                         std::to_string(parameters_.size()) + ")");
    }
    auto link = link_.get();
    if (!link) return fail(std::move(link).error());
    auto command = q::get_parameter(parameters_[static_cast<std::size_t>(channel - 1)], (*link)->terminator());
    if (!command) return fail(std::move(command).error());
    auto reply = (*link)->exchange(*command);
    if (!reply) return fail(std::move(reply).error());
    auto number = q::decode_number(*reply);
    if (!number) return fail(std::move(number).error());
    if (*number < 0) return codec::protocol_error("qtegra: a negative pressure", *reply);
    return *number;
  }();
  return observe(std::move(value));
}

Result<Sample> QtegraGauges::sample() {
  auto p = read_pressure();
  if (!p) return fail(std::move(p).error());
  return Sample{name(), clock_->now(), *p};
}

}  // namespace pychron::spectrometer

REGISTER_DRIVER("qtegra_gauges", pychron::spectrometer::QtegraGauges);
