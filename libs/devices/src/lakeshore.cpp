#include "pychron/devices/lakeshore.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace pychron {

namespace ls = codec::lakeshore;

namespace {

constexpr double kZeroCelsius = 273.15;

const Clock& steady_clock() {
  static const SteadyClock clock;
  return clock;
}

int outputs_of(const std::string& model) { return model == "336" ? 4 : 2; }
std::string allowed_inputs(const std::string& model) { return model == "336" ? "ABCD" : "AB"; }

}  // namespace

Lakeshore::Lakeshore(std::string name, Transport& transport, LakeshoreOptions options, const Clock* clock)
    : Device(std::move(name), DeviceOptions{clock}),
      transport_(transport),
      options_(std::move(options)),
      clock_(clock != nullptr ? clock : &steady_clock()) {}

DriverSchema Lakeshore::schema() {
  return {"",
          "Lake Shore 325/331/335/336 temperature controller (legacy Model335TemperatureController); kelvin at the "
          "interface",
          {{"model", KeyType::String, false, "325, 331, 335 (default) or 336"},
           {"inputs", KeyType::StringArray, false, "sensor inputs in use; default [\"A\", \"B\"] (C and D on a 336)"},
           {"units", KeyType::String, false, "K (default) or C: what the unit reports and takes"},
           {"setpoint_tolerance", KeyType::Float, false, "kelvin a setpoint may read back off by; default 0.01"},
           {"verify_retries", KeyType::Integer, false, "times a setpoint that reads back wrong is resent; default 3"},
           {"ranges", KeyType::TableArray, false,
            "array of tables {output, range, min, max}: heater range for setpoints in [min, max) kelvin"}}};
}

Result<std::unique_ptr<Lakeshore>> Lakeshore::create(const DriverArgs& args) {
  const auto& o = args.options;
  LakeshoreOptions options;
  options.model = o["model"].value_or(std::string("335"));
  if (options.model != "325" && options.model != "331" && options.model != "335" && options.model != "336")
    return fail(ErrorKind::Config, "model: \"" + options.model + "\" is not 325, 331, 335 or 336");
  if (const auto* array = o["inputs"].as_array()) {
    options.inputs.clear();
    std::set<std::string> seen;
    for (const auto& e : *array) {
      const std::string input = e.value_or(std::string{});
      if (input.size() != 1 || allowed_inputs(options.model).find(input[0]) == std::string::npos)
        return fail(ErrorKind::Config, "inputs: \"" + input + "\" is not an input of a model " + options.model +
                                           " (" + allowed_inputs(options.model) + ")");
      if (!seen.insert(input).second) return fail(ErrorKind::Config, "inputs: \"" + input + "\" listed twice");
      options.inputs.push_back(input);
    }
    if (options.inputs.empty()) return fail(ErrorKind::Config, "inputs: at least one input is required");
  }
  const std::string units = o["units"].value_or(std::string("K"));
  if (units != "K" && units != "C") return fail(ErrorKind::Config, "units: \"" + units + "\" is not K or C");
  options.units = units == "K" ? ls::Units::Kelvin : ls::Units::Celsius;
  options.setpoint_tolerance = o["setpoint_tolerance"].value_or(options.setpoint_tolerance);
  if (!(options.setpoint_tolerance > 0)) return fail(ErrorKind::Config, "setpoint_tolerance must be above 0");
  options.verify_retries = static_cast<int>(o["verify_retries"].value_or(std::int64_t{3}));
  if (options.verify_retries < 0 || options.verify_retries > 10)
    return fail(ErrorKind::Config, "verify_retries must be 0..10");
  if (const auto* bands = o["ranges"].as_array()) {
    for (const auto& b : *bands) {
      const auto* t = b.as_table();
      if (t == nullptr) return fail(ErrorKind::Config, "ranges: expected tables {output, range, min, max}");
      LakeshoreBand band;
      band.output = static_cast<int>((*t)["output"].value_or(std::int64_t{1}));
      band.range = static_cast<int>((*t)["range"].value_or(std::int64_t{-1}));
      auto min = (*t)["min"].value<double>();
      auto max = (*t)["max"].value<double>();
      if (band.output < 1 || band.output > outputs_of(options.model))
        return fail(ErrorKind::Config, "ranges: output " + std::to_string(band.output) + " is not on a model " +
                                           options.model);
      if (band.range < 0 || band.range > 5) return fail(ErrorKind::Config, "ranges: range must be 0..5");
      if (!min || !max || !(*min < *max) || *min < 0)
        return fail(ErrorKind::Config, "ranges: each band needs 0 <= min < max (kelvin)");
      band.min_k = *min;
      band.max_k = *max;
      options.bands.push_back(band);
    }
    // Per output, bands must not overlap.
    auto sorted = options.bands;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return std::tie(a.output, a.min_k) < std::tie(b.output, b.min_k); });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i].output == sorted[i - 1].output && sorted[i].min_k < sorted[i - 1].max_k)
        return fail(ErrorKind::Config, "ranges: bands for output " + std::to_string(sorted[i].output) + " overlap");
    }
  }
  return std::make_unique<Lakeshore>(args.name, args.transport, std::move(options), args.clock);
}

int Lakeshore::outputs() const { return outputs_of(options_.model); }

Result<void> Lakeshore::check_output(int output) const {
  if (output < 1 || output > outputs())
    return fail(ErrorKind::Config, "output " + std::to_string(output) + " is not on a model " + options_.model);
  return {};
}

double Lakeshore::to_wire(double kelvin) const {
  return options_.units == ls::Units::Kelvin ? kelvin : kelvin - kZeroCelsius;
}

double Lakeshore::from_wire(double value) const {
  return options_.units == ls::Units::Kelvin ? value : value + kZeroCelsius;
}

Result<Bytes> Lakeshore::ask(const codec::Command& command) {
  return transport_.exchange(command.tx, *command.reply);
}

Result<void> Lakeshore::connect() {
  return observe(transact(transport_, [&]() -> Result<void> {
    if (auto sent = transport_.write(ls::clear_status().tx); !sent) return fail(std::move(sent).error());
    auto reply = ask(ls::identify());
    if (!reply) return fail(std::move(reply).error());
    auto id = ls::decode_identity(*reply);
    if (!id) return fail(std::move(id).error());
    if (id->manufacturer != "LSCI" || id->model.find(options_.model) == std::string::npos) {
      return fail(ErrorKind::Config, "lakeshore: \"" + id->manufacturer + "," + id->model +
                                         "\" answered, not a Lake Shore model " + options_.model);
    }
    return {};
  }));
}

Result<double> Lakeshore::read_temperature(std::string_view input) {
  auto value = [&]() -> Result<double> {
    if (std::find(options_.inputs.begin(), options_.inputs.end(), input) == options_.inputs.end())
      return fail(ErrorKind::Config, "input \"" + std::string(input) + "\" is not configured");
    auto cmd = ls::read_input(input[0], options_.units);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = ask(*cmd);
    if (!reply) return fail(std::move(reply).error());
    auto v = ls::decode_number(*reply);
    if (!v) return fail(std::move(v).error());
    const double kelvin = from_wire(*v);
    if (kelvin < 0) return codec::protocol_error("lakeshore: below absolute zero", *reply);
    return kelvin;
  }();
  return observe(std::move(value));
}

Result<std::optional<int>> Lakeshore::range_for(int output, double kelvin) const {
  bool any = false;
  const LakeshoreBand* top = nullptr;
  for (const auto& b : options_.bands) {
    if (b.output != output) continue;
    any = true;
    if (kelvin >= b.min_k && kelvin < b.max_k) return std::optional<int>(b.range);
    if (top == nullptr || b.max_k > top->max_k) top = &b;
  }
  if (!any) return std::optional<int>{};
  if (top != nullptr && kelvin == top->max_k) return std::optional<int>(top->range);  // the highest band is closed
  return fail(ErrorKind::Config, "setpoint " + std::to_string(kelvin) + " K is in no heater range band for output " +
                                     std::to_string(output));
}

Result<void> Lakeshore::set_setpoint(int output, double kelvin) {
  auto done = [&]() -> Result<void> {
    if (auto ok = check_output(output); !ok) return ok;
    auto range = range_for(output, kelvin);
    if (!range) return fail(std::move(range).error());
    auto setp = ls::set_setpoint(output, to_wire(kelvin));
    if (!setp) return fail(std::move(setp).error());
    auto query = ls::query_setpoint(output);
    if (!query) return fail(std::move(query).error());
    return transact(transport_, [&]() -> Result<void> {
      if (*range) {
        auto cmd = ls::set_range(output, **range);
        if (!cmd) return fail(std::move(cmd).error());
        if (auto sent = transport_.write(cmd->tx); !sent) return fail(std::move(sent).error());
      }
      double last = 0.0;
      for (int attempt = 0; attempt <= options_.verify_retries; ++attempt) {
        if (auto sent = transport_.write(setp->tx); !sent) return fail(std::move(sent).error());
        auto reply = ask(*query);
        if (!reply) return fail(std::move(reply).error());
        auto read = ls::decode_number(*reply);
        if (!read) return fail(std::move(read).error());
        last = from_wire(*read);
        if (std::fabs(last - kelvin) <= options_.setpoint_tolerance) return {};
      }
      return fail(ErrorKind::Protocol, "lakeshore: output " + std::to_string(output) + " setpoint read back " +
                                           std::to_string(last) + " K, not " + std::to_string(kelvin) + " K");
    });
  }();
  return observe(std::move(done));
}

Result<double> Lakeshore::setpoint(int output) {
  auto value = [&]() -> Result<double> {
    if (auto ok = check_output(output); !ok) return fail(std::move(ok).error());
    auto cmd = ls::query_setpoint(output);
    if (!cmd) return fail(std::move(cmd).error());
    auto reply = ask(*cmd);
    if (!reply) return fail(std::move(reply).error());
    auto v = ls::decode_number(*reply);
    if (!v) return fail(std::move(v).error());
    return from_wire(*v);
  }();
  return observe(std::move(value));
}

Result<Sample> Lakeshore::sample() {
  auto t = read_temperature(options_.inputs.front());
  if (!t) return fail(std::move(t).error());
  return Sample{name(), clock_->now(), *t};
}

// --- simulator ----------------------------------------------------------------------

LakeshoreSim::LakeshoreSim(const Clock& clock, std::string model, double start_k, double base_k, Duration tau)
    : clock_(clock), model_(std::move(model)), base_k_(base_k), tau_(tau), last_(clock.now()) {
  for (char c : std::string("ABCD")) temps_[c] = start_k;
}

void LakeshoreSim::advance_locked() {
  const auto now = clock_.now();
  const double dt = std::chrono::duration<double>(now - last_).count();
  last_ = now;
  if (dt <= 0) return;
  const double k = 1.0 - std::exp(-dt / std::chrono::duration<double>(tau_).count());
  for (auto& [input, t] : temps_) {
    const int output = input - 'A' + 1;
    const auto sp = setpoints_.find(output);
    const auto r = ranges_.find(output);
    const bool heating = sp != setpoints_.end() && r != ranges_.end() && r->second > 0;
    const double target = heating ? sp->second : base_k_;
    t += (target - t) * k;
  }
}

Bytes LakeshoreSim::respond(const Bytes& tx) {
  auto request = ls::decode_request(tx);
  if (!request) return {};
  std::lock_guard lock(mutex_);
  advance_locked();
  const auto& h = request->header;
  const auto& arg = request->argument;
  if (h == "*IDN?") return ls::encode_line("LSCI," + model_ + ",SIM0001/SIM0001,1.0");
  if (h == "*CLS") return {};
  if ((h == "KRDG?" || h == "CRDG?") && arg.size() == 1 && temps_.contains(arg[0])) {
    const double k = temps_[arg[0]];
    return ls::encode_number(h == "KRDG?" ? k : k - 273.15);
  }
  const auto comma = arg.find(',');
  auto output = [&]() -> int { return arg.empty() ? 0 : arg[0] - '0'; };
  if (h == "SETP" && comma != std::string::npos) {
    if (auto v = codec::parse_decimal(arg.substr(comma + 1))) setpoints_[output()] = *v + setpoint_error_;
    return {};
  }
  if (h == "SETP?") {
    auto it = setpoints_.find(output());
    return ls::encode_number(it == setpoints_.end() ? 0.0 : it->second);
  }
  if (h == "RANGE" && comma != std::string::npos) {
    ranges_[output()] = arg[comma + 1] - '0';
    return {};
  }
  if (h == "RANGE?") {
    auto it = ranges_.find(output());
    return ls::encode_line(std::to_string(it == ranges_.end() ? 0 : it->second));
  }
  return {};
}

SimTransport::Hook LakeshoreSim::hook() {
  return [this](const Bytes& tx) { return respond(tx); };
}

double LakeshoreSim::temperature(char input) const {
  std::lock_guard lock(mutex_);
  auto it = temps_.find(input);
  return it == temps_.end() ? 0.0 : it->second;
}

void LakeshoreSim::set_temperature(char input, double kelvin) {
  std::lock_guard lock(mutex_);
  temps_[input] = kelvin;
}

std::optional<double> LakeshoreSim::setpoint(int output) const {
  std::lock_guard lock(mutex_);
  auto it = setpoints_.find(output);
  return it == setpoints_.end() ? std::nullopt : std::optional<double>(it->second);
}

int LakeshoreSim::range(int output) const {
  std::lock_guard lock(mutex_);
  auto it = ranges_.find(output);
  return it == ranges_.end() ? 0 : it->second;
}

void LakeshoreSim::set_setpoint_error(double kelvin) {
  std::lock_guard lock(mutex_);
  setpoint_error_ = kelvin;
}

}  // namespace pychron

REGISTER_DRIVER("lakeshore", pychron::Lakeshore);
