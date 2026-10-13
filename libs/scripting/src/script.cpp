#include "pychron/scripting/script.hpp"

#include <cctype>
#include <charconv>
#include <string>

namespace pychron::scripting {
namespace {

constexpr std::string_view kHeaderPrefix = "#! pychron:";

bool is_separator(char c) { return c == ',' || std::isspace(static_cast<unsigned char>(c)); }

// Parses "k=v, k2=\"a b\"" into `header`.
Result<void> parse_pairs(std::string_view body, int line_no, ScriptHeader& header) {
  auto bad = [&](std::string why) {
    return fail(ErrorKind::Config,
                "script header line " + std::to_string(line_no) + ": " + std::move(why));
  };
  std::size_t i = 0;
  while (true) {
    while (i < body.size() && is_separator(body[i])) ++i;
    if (i >= body.size()) return {};
    std::size_t key_start = i;
    while (i < body.size() && body[i] != '=' && !is_separator(body[i])) ++i;
    std::string key(body.substr(key_start, i - key_start));
    if (i >= body.size() || body[i] != '=') return bad("expected key=value near '" + key + "'");
    if (key.empty()) return bad("empty key");
    ++i;
    std::string value;
    if (i < body.size() && (body[i] == '"' || body[i] == '\'')) {
      char quote = body[i++];
      std::size_t end = body.find(quote, i);
      if (end == std::string_view::npos) return bad("unterminated quote for '" + key + "'");
      value = std::string(body.substr(i, end - i));
      i = end + 1;
    } else {
      std::size_t value_start = i;
      while (i < body.size() && !is_separator(body[i])) ++i;
      value = std::string(body.substr(value_start, i - value_start));
    }
    header.set(std::move(key), std::move(value));
  }
}

}  // namespace

std::string_view to_string(ScriptKind kind) noexcept {
  switch (kind) {
    case ScriptKind::Extraction: return "extraction";
    case ScriptKind::PostEquilibration: return "post_equilibration";
    case ScriptKind::PostMeasurement: return "post_measurement";
    case ScriptKind::MeasurementHook: return "measurement_hooks";
  }
  return "unknown";
}

std::optional<std::string> ScriptHeader::get(std::string_view key) const {
  auto it = values_.find(key);
  if (it == values_.end()) return std::nullopt;
  return it->second;
}

Result<std::optional<double>> ScriptHeader::number(std::string_view key) const {
  auto v = get(key);
  if (!v) return std::optional<double>{};
  try {
    std::size_t used = 0;
    double d = std::stod(*v, &used);
    if (used == v->size()) return std::optional<double>{d};
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): falls through to the error below
  }
  return fail(ErrorKind::Config, "script header '" + std::string(key) + "' is not a number: " + *v);
}

Result<ScriptHeader> parse_header(std::string_view text) {
  ScriptHeader header;
  int line_no = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    ++line_no;
    std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos && line.substr(first).starts_with(kHeaderPrefix)) {
      auto parsed = parse_pairs(line.substr(first + kHeaderPrefix.size()), line_no, header);
      if (!parsed) return fail(parsed.error());
    }
    pos = end + 1;
  }
  return header;
}

ValueMap default_context() {
  using namespace std::string_literals;
  return ValueMap{
      {"analysis_type", "unknown"s},
      {"run_identifier", ""s},
      {"position", ""s},
      {"extract_value", 0.0},
      {"extract_units", "percent"s},
      {"duration", 0.0},
      {"cleanup", 0.0},
      {"pre_cleanup", 0.0},
      {"post_cleanup", 0.0},
      {"tray", ""s},
      {"pattern", ""s},
      {"beam_diameter", std::monostate{}},
      {"light_value", 0.0},
      {"ramp_rate", 0.0},
      {"ramp_duration", 0.0},
      {"cryo_temperature", 0.0},
      {"load_identifier", "default_load"s},
      {"extract_device", ""s},
      {"disable_between_positions", false},
  };
}

ScriptContext make_context(ValueMap overrides, ValueMap options) {
  ScriptContext ctx{default_context(), std::move(options)};
  for (auto& [k, v] : overrides) ctx.globals[k] = std::move(v);
  return ctx;
}

}  // namespace pychron::scripting
