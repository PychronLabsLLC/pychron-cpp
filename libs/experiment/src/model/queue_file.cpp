#include "pychron/experiment/model/queue_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pychron/experiment/model/positions.hpp"
#include "pychron/experiment/model/queue_toml.hpp"

namespace pychron::experiment {
namespace {

// Canonical scalar text: basic (double-quoted) strings and the shortest float
// that parses back to the same double, so dumps are stable and diff cleanly.
std::string fmt(const std::string& s) {
  std::string out = "\"";
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04X", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out + "\"";
}

std::string fmt(std::int64_t v) { return std::to_string(v); }
std::string fmt(bool v) { return v ? "true" : "false"; }

std::string fmt(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  // Exponent form only for very large or small magnitudes ("30.0", not "3e+01").
  const double mag = std::fabs(v);
  const bool plain = mag == 0 || (mag >= 1e-5 && mag < 1e16);
  char buf[32];
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    if (std::strtod(buf, nullptr) == v && !(plain && std::strchr(buf, 'e'))) break;
  }
  std::string s = buf;
  std::replace(s.begin(), s.end(), ',', '.');  // decimal-comma locales
  if (s.find_first_of(".e") == std::string::npos) s += ".0";  // keep it a TOML float
  return s;
}

std::string key(std::string_view k) {
  bool bare = !k.empty();
  for (char c : k) bare &= std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  return bare ? std::string(k) : fmt(std::string(k));
}

class Emitter {
 public:
  void header(std::string_view h) {
    if (!out_.empty()) out_ += '\n';
    out_ += h;
    out_ += '\n';
  }
  void raw(std::string_view k, const std::string& v) { out_ += key(k) + " = " + v + "\n"; }
  void str(std::string_view k, const std::string& v) {
    if (!v.empty()) raw(k, fmt(v));
  }
  void opt_str(std::string_view k, const std::optional<std::string>& v) {
    if (v) raw(k, fmt(*v));
  }
  void num(std::string_view k, double v) {
    if (v != 0) raw(k, fmt(v));
  }
  void opt_num(std::string_view k, const std::optional<double>& v) {
    if (v) raw(k, fmt(*v));
  }
  void dur(std::string_view k, Duration d) { num(k, d.count()); }
  void flag(std::string_view k, bool v) {
    if (v) raw(k, "true");
  }
  void opt_int(std::string_view k, const std::optional<int>& v) {
    if (v) raw(k, fmt(static_cast<std::int64_t>(*v)));
  }
  std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

std::string param(const ParamValue& v) {
  return std::visit([](const auto& x) { return fmt(x); }, v);
}

std::string conditionals(const std::vector<ConditionalRef>& refs) {
  std::string s = "[";
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (i) s += ", ";
    if (refs[i].kind == "action") s += fmt(refs[i].name);
    else s += "{ name = " + fmt(refs[i].name) + ", kind = " + fmt(refs[i].kind) + " }";
  }
  return s + "]";
}

bool has_extraction(const ExtractionSpec& e, const std::string& queue_device) {
  ExtractionSpec inherited;
  inherited.device = queue_device;
  return e != inherited;
}

void emit_run(Emitter& o, const RunSpec& r, const QueueSpec& q) {
  o.header("[[runs]]");
  o.raw("identifier", fmt(r.id.identifier));
  o.opt_int("aliquot", r.id.aliquot);
  o.str("step", r.id.step);
  o.flag("skip", r.skip);
  o.flag("end_after", r.end_after);
  o.str("comment", r.comment);
  o.opt_num("weight", r.weight);
  o.opt_str("post_equilibration", r.post_equilibration);
  o.opt_str("post_measurement", r.post_measurement);
  o.dur("overlap", r.overlap.duration);
  o.dur("overlap_min", r.overlap.min_delay);
  o.dur("delay_after", r.delay_after);
  if (!r.conditionals.empty()) o.raw("conditionals", conditionals(r.conditionals));

  const auto& e = r.extraction;
  if (has_extraction(e, q.extract_device)) {
    o.header("[runs.extraction]");
    if (e.device != q.extract_device) o.raw("device", fmt(e.device));
    if (e.position) o.raw("position", fmt(format_position(*e.position)));
    o.num("value", e.value);
    if (e.units != Unit::Watts) o.raw("units", fmt(std::string(to_string(e.units))));
    o.dur("duration", e.duration);
    o.dur("cleanup", e.cleanup);
    o.dur("pre_cleanup", e.pre_cleanup);
    o.dur("post_cleanup", e.post_cleanup);
    o.opt_str("pattern", e.pattern);
    o.opt_num("beam_diameter", e.beam_diameter);
    o.opt_num("ramp_rate", e.ramp_rate);
    o.dur("ramp", e.ramp);
    o.opt_num("cryo_temp", e.cryo_temp);
    o.str("script", e.script);
    o.str("options", e.options);
  }

  const auto& m = r.measurement;
  if (m != MeasurementRef{}) {
    o.header("[runs.measurement]");
    o.str("plan", m.plan);
    o.opt_str("hook", m.hook);
    if (!m.overrides.empty()) {
      o.header("[runs.measurement.overrides]");
      for (const auto& [k, v] : m.overrides) o.raw(k, param(v));
    }
  }

  const auto& s = r.sample;
  if (s != SampleInfo{}) {
    o.header("[runs.sample]");
    o.str("sample", s.sample);
    o.str("material", s.material);
    o.str("project", s.project);
    o.str("irradiation", s.irradiation);
    o.str("level", s.level);
    o.opt_int("irradiation_position", s.irradiation_position);
  }
}

}  // namespace

std::string dump_queue(const QueueSpec& q) {
  Emitter o;
  o.header("[queue]");
  o.raw("schema_version", fmt(std::int64_t{kQueueSchemaVersion}));
  o.str("name", q.name);
  o.str("mass_spectrometer", q.mass_spectrometer);
  o.str("extract_device", q.extract_device);
  o.str("tray", q.tray);
  o.str("load", q.load);
  o.str("username", q.username);
  o.str("email", q.email);
  o.str("queue_conditionals", q.queue_conditionals);
  o.str("repository", q.repository);

  // Delays are always written: their defaults are not zero.
  o.header("[queue.delays]");
  o.raw("before_analyses", fmt(q.delays.before_analyses.count()));
  o.raw("between_analyses", fmt(q.delays.between_analyses.count()));
  o.raw("after_blank", fmt(q.delays.after_blank.count()));
  o.raw("extract_delay", fmt(q.delays.extract_delay.count()));

  for (const auto& r : q.runs) emit_run(o, r, q);
  return o.take();
}

Result<QueueSpec> load_queue_file(const std::string& path, const IdentifierRules& ids) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open queue file '" + path + "'");
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return fail(ErrorKind::Io, "cannot read queue file '" + path + "'");
  return parse_queue(ss.str(), ids, path);
}

Result<void> save_queue_file(const std::string& path, const QueueSpec& q) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail(ErrorKind::Io, "cannot write queue file '" + tmp + "'");
    out << dump_queue(q);
    out.flush();
    if (!out) return fail(ErrorKind::Io, "cannot write queue file '" + tmp + "'");
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return fail(ErrorKind::Io, "cannot replace queue file '" + path + "'");
  }
  return {};
}

}  // namespace pychron::experiment
