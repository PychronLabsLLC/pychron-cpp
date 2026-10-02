#include "pychron/codecs/thermo_qtegra.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace pychron::codec::qtegra {

namespace {

// Preferred (pychron Python) entries first; aliases after. Sources:
// spectrometer/thermo/spectrometer/base.py and helix.py hardware_names,
// source/base.py and source/helix.py read_* / _set_*.
constexpr std::array<ParamName, 25> kNames{{
    {"hv", "HV", ""},
    {"trap_current", "Trap Current Set", "Trap Current Readback"},
    {"trap_voltage", "Trap Voltage Set", "Trap Voltage Readback"},
    // Set name not seen in pychron Python; readback from ThermoSource.read_emission.
    {"emission", "Electron Emission Set", "Source Current Readback"},
    {"electron_energy", "Electron Energy Set", ""},
    {"ion_repeller", "Ion Repeller Set", ""},
    {"extraction_lens", "Extraction Lens Set", ""},
    {"extraction_focus", "Extraction Focus Set", ""},        // Helix hardware_names
    {"extraction_symmetry", "Extraction Symmetry Set", ""},  // Helix hardware_names
    {"y_symmetry", "Y-Symmetry Set", ""},
    {"z_symmetry", "Z-Symmetry Set", ""},
    {"z_focus", "Z-Focus Set", ""},
    {"horizontal_symmetry", "Horizontal Symmetry Set", ""},  // Helix hardware_names
    {"flatapole", "DAC_1_0_(Flata-Pole)", ""},               // Helix
    {"rotation_quad", "RotationQuad", ""},                   // Helix hardware_names, read
    {"pole_n", "DAC_0_0_(Pole-N)", ""},                      // Helix
    {"pole_s", "DAC_0_4_(Pole-S)", ""},                      // Helix
    {"esa_plus", "ESA+ Set", ""},                            // not seen in pychron Python
    {"esa_minus", "ESA- Set", ""},                           // not seen in pychron Python
    // Aliases.
    {"rotation_quad", "Rotation Quad", ""},         // HelixSource._set_rotation_quad
    {"horizontal_symmetry", "H-Symmetry Set", ""},  // not seen in pychron Python
    {"flatapole", "Flatapole Set", ""},             // not seen in pychron Python
    {"rotation_quad", "Rotation Quad Set", ""},     // not seen in pychron Python
    {"pole_n", "Pole N Set", ""},                   // not seen in pychron Python
    {"pole_s", "Pole S Set", ""},                   // not seen in pychron Python
}};

Unexpected<Error> config_error(std::string what) { return fail(ErrorKind::Config, std::move(what)); }

Result<Command> simple(std::string_view text, Terminator term) {
  return Command::ascii(std::string(text) + std::string(terminator_text(term)), reply_spec());
}

Result<Command> with_number(std::string_view verb, double v, Terminator term) {
  if (!std::isfinite(v)) return config_error(std::string(verb) + ": value is not finite");
  return simple(std::string(verb) + " " + format_number(v), term);
}

Result<void> check_name(std::string_view name) {
  if (name.empty()) return config_error("empty name");
  for (char c : name) {
    if (c == ',' || static_cast<unsigned char>(c) < 0x20) return config_error("invalid character in name");
  }
  return {};
}

Result<Command> with_name(std::string_view verb, std::string_view name, Terminator term) {
  if (auto ok = check_name(name); !ok) return fail(ok.error());
  return simple(std::string(verb) + " " + std::string(name), term);
}

Result<Command> with_name_number(std::string_view verb, std::string_view name, double v, Terminator term) {
  if (auto ok = check_name(name); !ok) return fail(ok.error());
  if (!std::isfinite(v)) return config_error(std::string(verb) + ": value is not finite");
  return simple(std::string(verb) + " " + std::string(name) + "," + format_number(v), term);
}

Result<Command> with_name_list(std::string_view verb, std::span<const std::string> names, Terminator term) {
  if (names.empty()) return config_error(std::string(verb) + ": empty name list");
  std::string text = std::string(verb) + " ";
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (auto ok = check_name(names[i]); !ok) return fail(ok.error());
    if (i) text += ',';
    text += names[i];
  }
  return simple(text, term);
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// Trimmed reply body (pychron strips replies); "ERROR..." is a Protocol error.
Result<std::string> body(const Bytes& reply) {
  const std::string raw = to_string(reply);
  std::string text(trim(raw));
  if (text.rfind("ERROR", 0) == 0) return protocol_error("device error: " + text, reply);
  return text;
}

// GetData body: pychron read_intensities treats any reply containing "ERROR"
// as an error.
Result<std::string> data_body(const Bytes& reply) {
  const std::string raw = to_string(reply);
  std::string text(trim(raw));
  if (text.find("ERROR") != std::string::npos) return protocol_error("device error: " + text, reply);
  return text;
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  for (;;) {
    auto comma = s.find(',', start);
    fields.push_back(s.substr(start, comma == std::string::npos ? comma : comma - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return fields;
}

// Python float() tolerates whitespace around each field.
std::optional<double> field_number(const std::string& field) { return parse_decimal(trim(field)); }

template <class Names>
Result<Pairs> pair_values(const std::string& text, const Names& names, const Bytes& reply) {
  auto fields = split_csv(text);
  if (fields.size() != names.size()) return protocol_error("value count does not match request", reply);
  Pairs out;
  out.reserve(fields.size());
  for (std::size_t i = 0; i < fields.size(); ++i) {
    auto v = field_number(fields[i]);
    if (!v) return protocol_error("not a number", reply);
    out.emplace_back(std::string(names[i]), *v);
  }
  return out;
}

}  // namespace

std::string_view terminator_text(Terminator t) noexcept {
  switch (t) {
    case Terminator::CR:
      return "\r";
    case Terminator::LF:
      return "\n";
    case Terminator::CRLF:
      return "\r\n";
  }
  return "\r";
}

const ReadSpec& reply_spec() noexcept {
  static const ReadSpec spec = ReadSpec::until_any("\r\n");
  return spec;
}

double snap_integration_time(double seconds) noexcept {
  if (!std::isfinite(seconds)) return kIntegrationTimes.front();
  double best = kIntegrationTimes.front();
  double best_err = std::fabs(seconds - best);
  for (double legal : kIntegrationTimes) {
    const double err = std::fabs(seconds - legal);
    if (err < best_err) {
      best_err = err;
      best = legal;
    }
  }
  return best;
}

std::string format_number(double v) {
  // Shortest round-trip digits in scientific form give the decimal exponent;
  // Python repr switches to scientific outside [-4, 16).
  std::array<char, 64> buf{};
  auto sci = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::scientific);
  std::string s(buf.data(), sci.ptr);
  const auto e = s.find('e');
  const int exponent = e == std::string::npos ? 0 : std::atoi(s.c_str() + e + 1);
  if (exponent >= -4 && exponent < 16) {
    auto fixed = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed);
    return std::string(buf.data(), fixed.ptr);
  }
  return s;
}

std::span<const ParamName> param_names() noexcept { return kNames; }

std::optional<std::string_view> hardware_name(std::string_view canonical) noexcept {
  for (const auto& n : kNames)
    if (n.canonical == canonical) return n.hardware;
  return std::nullopt;
}

std::optional<std::string_view> readback_name(std::string_view canonical) noexcept {
  for (const auto& n : kNames)
    if (n.canonical == canonical && !n.readback.empty()) return n.readback;
  return std::nullopt;
}

std::optional<std::string_view> canonical_name(std::string_view hardware) noexcept {
  for (const auto& n : kNames)
    if (n.hardware == hardware || (!n.readback.empty() && n.readback == hardware)) return n.canonical;
  return std::nullopt;
}

Result<Command> set_magnet_dac(double dac, Terminator t) { return with_number("SetMagnetDAC", dac, t); }
Result<Command> get_magnet_dac(Terminator t) { return simple("GetMagnetDAC", t); }
Result<Command> get_magnet_moving(Terminator t) { return simple("GetMagnetMoving", t); }
Result<Command> blank_beam(bool blank, Terminator t) {
  return simple(blank ? "BlankBeam True" : "BlankBeam False", t);
}
Result<Command> protect_detector(std::string_view detector, bool protect, Terminator t) {
  if (auto ok = check_name(detector); !ok) return fail(ok.error());
  return simple("ProtectDetector " + std::string(detector) + (protect ? ",On" : ",Off"), t);
}
Result<Command> protect_detector_parameter(std::string_view detector, bool protect, Terminator t) {
  if (auto ok = check_name(detector); !ok) return fail(ok.error());
  return simple("SetParameter ProtectDetector," + std::string(detector) + (protect ? ",On" : ",Off"), t);
}
Result<Command> set_deflection(std::string_view d, double v, Terminator t) {
  return with_name_number("SetDeflection", d, v, t);
}
Result<Command> get_deflection(std::string_view d, Terminator t) { return with_name("GetDeflection", d, t); }
Result<Command> get_deflections(std::span<const std::string> detectors, Terminator t) {
  return with_name_list("GetDeflections", detectors, t);
}
Result<Command> set_gain(std::string_view d, double g, Terminator t) { return with_name_number("SetGain", d, g, t); }
Result<Command> get_gain(std::string_view d, Terminator t) { return with_name("GetGain", d, t); }
Result<Command> set_ion_counter_voltage(double v, Terminator t) { return with_number("SetIonCounterVoltage", v, t); }
Result<Command> set_integration_time(double seconds, Terminator t) {
  if (!std::isfinite(seconds)) return config_error("integration time is not finite");
  return with_number("SetIntegrationTime", snap_integration_time(seconds), t);
}
Result<Command> get_integration_time(Terminator t) { return simple("GetIntegrationTime", t); }
Result<Command> get_data(Terminator t) { return simple("GetData", t); }
Result<Command> set_hv(double volts, Terminator t) { return with_number("SetHV", volts, t); }
Result<Command> get_high_voltage(Terminator t) { return simple("GetHighVoltage", t); }
Result<Command> set_parameter(std::string_view n, double v, Terminator t) {
  return with_name_number("SetParameter", n, v, t);
}
Result<Command> get_parameter(std::string_view n, Terminator t) { return with_name("GetParameter", n, t); }
Result<Command> get_parameters(std::span<const std::string> names, Terminator t) {
  return with_name_list("GetParameters", names, t);
}
Result<Command> set_y_symmetry(double v, Terminator t) { return with_number("SetYSymmetry", v, t); }
Result<Command> set_z_symmetry(double v, Terminator t) { return with_number("SetZSymmetry", v, t); }
Result<Command> set_extraction_lens(double v, Terminator t) { return with_number("SetExtractionLens", v, t); }
Result<Command> get_extraction_symmetry(Terminator t) { return simple("GetExtractionSymmetry", t); }
Result<Command> set_sub_cup_configuration(std::string_view name, Terminator t) {
  return with_name("SetSubCupConfiguration", name, t);
}
Result<Command> reset(Terminator t) { return simple("Reset", t); }

Result<void> decode_ok(const Bytes& reply) {
  auto b = body(reply);
  if (!b) return fail(b.error());
  if (lower(*b) != "ok") return protocol_error("expected OK", reply);
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
  // pychron core/helpers/strtools.py to_bool.
  static constexpr std::array<std::string_view, 7> kTrue{"true", "t", "yes", "y", "1", "ok", "open"};
  static constexpr std::array<std::string_view, 6> kFalse{"false", "f", "no", "n", "0", "closed"};
  const auto s = lower(*b);
  for (auto k : kTrue)
    if (s == k) return true;
  for (auto k : kFalse)
    if (s == k) return false;
  return protocol_error("expected a boolean", reply);
}

Result<Pairs> decode_named_values(const Bytes& reply, std::span<const std::string> names) {
  auto b = body(reply);
  if (!b) return fail(b.error());
  if (b->empty()) return protocol_error("empty reply", reply);
  return pair_values(*b, names, reply);
}

Result<Pairs> decode_data(const Bytes& reply) {
  auto b = data_body(reply);
  if (!b) return fail(b.error());
  Pairs out;
  if (b->empty()) return out;
  auto fields = split_csv(*b);
  if (fields.size() % 2 != 0) return protocol_error("unpaired tag", reply);
  for (std::size_t i = 0; i < fields.size(); i += 2) {
    if (fields[i].empty()) return protocol_error("empty tag", reply);
    auto v = field_number(fields[i + 1]);
    if (!v) return protocol_error("not a number", reply);
    out.emplace_back(std::move(fields[i]), *v);
  }
  return out;
}

Result<Pairs> decode_data(const Bytes& reply, std::span<const std::string_view> order) {
  auto b = data_body(reply);
  if (!b) return fail(b.error());
  if (b->empty()) return Pairs{};
  return pair_values(*b, order, reply);
}

Result<Request> decode_request(const Bytes& tx) {
  const std::string raw = to_string(tx);
  const std::string_view line = trim(raw);
  if (line.empty()) return protocol_error("empty command", tx);
  Request request;
  const auto space = line.find(' ');
  request.verb = std::string(line.substr(0, space));
  if (space == std::string_view::npos) return request;
  for (auto& field : split_csv(std::string(line.substr(space + 1)))) request.args.emplace_back(trim(field));
  return request;
}

Bytes encode_ok() { return to_bytes("OK\r\n"); }
Bytes encode_number(double v) { return to_bytes(format_number(v) + "\r\n"); }
Bytes encode_bool(bool v) { return to_bytes(v ? "True\r\n" : "False\r\n"); }
Bytes encode_error(std::string_view message) { return to_bytes("ERROR: " + std::string(message) + "\r\n"); }

}  // namespace pychron::codec::qtegra
