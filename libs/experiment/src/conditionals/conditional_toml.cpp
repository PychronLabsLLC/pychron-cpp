// to_toml: the canonical text of a conditionals file (editor spec 4.1).

#include <cstdio>
#include <string>
#include <vector>

#include "number.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"

namespace pychron::experiment {
namespace {

// A TOML basic string.
std::string quoted(std::string_view s) {
  std::string out = "\"";
  for (const char ch : s) {
    const auto u = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (u < 0x20 || u == 0x7f) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04X", u);
          out += b;
        } else {
          out += ch;
        }
    }
  }
  return out + "\"";
}

std::string quoted_list(const std::vector<std::string>& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + quoted(items[i]);
  return out + "]";
}

// A TOML float: never a token that reads as an integer.
std::string toml_float(double v) {
  std::string s = detail::shortest(v);
  if (s.find_first_of(".en") == std::string::npos) s += ".0";
  return s;
}

void write_item(std::string& out, const Conditional& c) {
  const KindFields& f = fields_of(c.kind);
  out += "[[" + std::string(table_name(c.kind)) + "]]\n";
  if (!c.name.empty() && c.name != default_name(c.kind, c.check)) out += "name = " + quoted(c.name) + "\n";
  out += "check = " + quoted(c.check) + "\n";
  if (c.start != 0) out += "start = " + std::to_string(c.start) + "\n";
  if (c.frequency != 1) out += "frequency = " + std::to_string(c.frequency) + "\n";
  if (c.ntrips != 1) out += "ntrips = " + std::to_string(c.ntrips) + "\n";
  if (c.window) out += "window = " + std::to_string(*c.window) + "\n";
  if (!c.mapper.empty()) out += "mapper = " + quoted(c.mapper) + "\n";
  if (!c.analysis_types.empty()) out += "analysis_types = " + quoted_list(c.analysis_types) + "\n";
  if (c.abbreviated_count_ratio != 1.0)
    out += "abbreviated_count_ratio = " + toml_float(c.abbreviated_count_ratio) + "\n";
  if (!f.actions.empty() && c.action.type != ActionSpec::Type::None &&
      c.action != ActionSpec{.type = f.default_action})
    out += "action = " + quoted(to_string(c.action)) + "\n";
  if (c.resume) out += "resume = true\n";
  if (c.truncate) out += "truncate = true\n";
  if (c.terminate) out += "terminate = true\n";
}

}  // namespace

std::string to_toml(const ConditionalSet& set) {
  std::string out;
  if (!set.disable.empty()) out += "disable = " + quoted_list(set.disable) + "\n";
  for (const ConditionalKind kind : kFileOrder) {
    for (const auto& c : set.items) {
      if (c.kind != kind) continue;
      if (!out.empty()) out += '\n';
      write_item(out, c);
    }
  }
  return out;
}

}  // namespace pychron::experiment
