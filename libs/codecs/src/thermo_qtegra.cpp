#include "pychron/codecs/thermo_qtegra.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace pychron::codec::qtegra {

namespace {

const ReadSpec& line_reply() {
  static const ReadSpec spec = ReadSpec::until(kTerminator);
  return spec;
}

constexpr std::array<ParamName, 19> kNames{{
    {"hv", "HV"},
    {"trap_current", "Trap Current Set"},
    {"trap_voltage", "Trap Voltage Set"},
    {"emission", "Electron Emission Set"},
    {"electron_energy", "Electron Energy Set"},
    {"ion_repeller", "Ion Repeller Set"},
    {"extraction_lens", "Extraction Lens Set"},
    {"extraction_focus", "Extraction Focus Set"},
    {"extraction_symmetry", "Extraction Symmetry Set"},
    {"y_symmetry", "Y-Symmetry Set"},
    {"z_symmetry", "Z-Symmetry Set"},
    {"z_focus", "Z-Focus Set"},
    {"horizontal_symmetry", "H-Symmetry Set"},
    {"flatapole", "Flatapole Set"},
    {"rotation_quad", "Rotation Quad Set"},
    {"pole_n", "Pole N Set"},
    {"pole_s", "Pole S Set"},
    {"esa_plus", "ESA+ Set"},
    {"esa_minus", "ESA- Set"},
}};

std::string number(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

Unexpected<Error> config_error(std::string what) { return fail(ErrorKind::Config, std::move(what)); }

Result<Command> simple(std::string_view text) {
  return Command::ascii(std::string(text) + std::string(kTerminator), line_reply());
}

Result<Command> with_number(std::string_view verb, double v) {
  if (!std::isfinite(v)) return config_error(std::string(verb) + ": value is not finite");
  return simple(std::string(verb) + " " + number(v));
}

Result<void> check_name(std::string_view name) {
  if (name.empty()) return config_error("empty name");
  for (char c : name) {
    if (c == ',' || static_cast<unsigned char>(c) < 0x20) return config_error("invalid character in name");
  }
  return {};
}

Result<Command> with_name(std::string_view verb, std::string_view name) {
  if (auto ok = check_name(name); !ok) return fail(ok.error());
  return simple(std::string(verb) + " " + std::string(name));
}

Result<Command> with_name_number(std::string_view verb, std::string_view name, double v) {
  if (auto ok = check_name(name); !ok) return fail(ok.error());
  if (!std::isfinite(v)) return config_error(std::string(verb) + ": value is not finite");
  return simple(std::string(verb) + " " + std::string(name) + "," + number(v));
}

// Reply body without terminator; "ERROR..." replies are Protocol errors.
Result<std::string> body(const Bytes& reply) {
  auto text = strip_terminator(reply, kTerminator);
  if (!text) return fail(text.error());
  // Tolerate "\r\n".
  if (!text->empty() && text->back() == '\r') text->pop_back();
  if (text->rfind("ERROR", 0) == 0) return protocol_error("device error: " + *text, reply);
  return text;
}

}  // namespace

double snap_integration_time(double seconds) noexcept {
  if (!std::isfinite(seconds) || seconds <= kBaseIntegration) return kBaseIntegration;
  int best = 0;
  double best_err = 1e300;
  for (int n = 0; n <= kMaxIntegrationExponent; ++n) {
    double legal = kBaseIntegration * static_cast<double>(1 << n);
    double err = std::fabs(std::log(seconds / legal));
    if (err < best_err) {
      best_err = err;
      best = n;
    }
  }
  return kBaseIntegration * static_cast<double>(1 << best);
}

std::span<const ParamName> param_names() noexcept { return kNames; }

std::optional<std::string_view> hardware_name(std::string_view canonical) noexcept {
  for (const auto& n : kNames)
    if (n.canonical == canonical) return n.hardware;
  return std::nullopt;
}

std::optional<std::string_view> canonical_name(std::string_view hardware) noexcept {
  for (const auto& n : kNames)
    if (n.hardware == hardware) return n.canonical;
  return std::nullopt;
}

Result<Command> set_magnet_dac(double dac) { return with_number("SetMagnetDAC", dac); }
Result<Command> get_magnet_dac() { return simple("GetMagnetDAC"); }
Result<Command> get_magnet_moving() { return simple("GetMagnetMoving"); }
Result<Command> blank_beam(bool blank) { return simple(blank ? "BlankBeam True" : "BlankBeam False"); }
Result<Command> protect_detector(std::string_view detector, bool protect) {
  if (auto ok = check_name(detector); !ok) return fail(ok.error());
  return simple("ProtectDetector " + std::string(detector) + (protect ? ",On" : ",Off"));
}
Result<Command> set_deflection(std::string_view d, double v) { return with_name_number("SetDeflection", d, v); }
Result<Command> get_deflection(std::string_view d) { return with_name("GetDeflection", d); }
Result<Command> get_deflections() { return simple("GetDeflections"); }
Result<Command> set_gain(std::string_view d, double g) { return with_name_number("SetGain", d, g); }
Result<Command> get_gain(std::string_view d) { return with_name("GetGain", d); }
Result<Command> set_ion_counter_voltage(double v) { return with_number("SetIonCounterVoltage", v); }
Result<Command> set_integration_time(double seconds) {
  if (!std::isfinite(seconds)) return config_error("integration time is not finite");
  return with_number("SetIntegrationTime", snap_integration_time(seconds));
}
Result<Command> get_integration_time() { return simple("GetIntegrationTime"); }
Result<Command> get_data() { return simple("GetData"); }
Result<Command> set_hv(double kv) { return with_number("SetHV", kv); }
Result<Command> get_high_voltage() { return simple("GetHighVoltage"); }
Result<Command> set_parameter(std::string_view n, double v) { return with_name_number("SetParameter", n, v); }
Result<Command> get_parameter(std::string_view n) { return with_name("GetParameter", n); }
Result<Command> get_parameters() { return simple("GetParameters"); }
Result<Command> reset() { return simple("Reset"); }

Result<void> decode_ok(const Bytes& reply) {
  auto b = body(reply);
  if (!b) return fail(b.error());
  if (*b != "OK") return protocol_error("expected OK", reply);
  return {};
}

Result<double> decode_number(const Bytes& reply) {
  auto b = body(reply);
  if (!b) return fail(b.error());
  auto v = parse_decimal(*b);
  if (!v) return protocol_error("not a number", reply);
  return *v;
}

Result<bool> decode_bool(const Bytes& reply) {
  auto b = body(reply);
  if (!b) return fail(b.error());
  if (*b == "True") return true;
  if (*b == "False") return false;
  return protocol_error("expected True/False", reply);
}

Result<Pairs> decode_pairs(const Bytes& reply) {
  auto b = body(reply);
  if (!b) return fail(b.error());
  Pairs out;
  if (b->empty()) return out;
  std::vector<std::string> fields;
  std::size_t start = 0;
  for (;;) {
    auto comma = b->find(',', start);
    fields.push_back(b->substr(start, comma == std::string::npos ? comma : comma - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (fields.size() % 2 != 0) return protocol_error("unpaired tag", reply);
  for (std::size_t i = 0; i < fields.size(); i += 2) {
    if (fields[i].empty()) return protocol_error("empty tag", reply);
    auto v = parse_decimal(fields[i + 1]);
    if (!v) return protocol_error("not a number", reply);
    out.emplace_back(std::move(fields[i]), *v);
  }
  return out;
}

}  // namespace pychron::codec::qtegra
