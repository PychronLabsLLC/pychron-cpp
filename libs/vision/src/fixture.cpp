#include "pychron/vision/fixture.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>

#include "pychron/vision/pgm.hpp"

namespace pychron::vision {
namespace {

// Shortest text that parses back to the same double, always a TOML float.
std::string number(double v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  std::string s(buf, r.ptr);
  if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
  return s;
}

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (c == '\n') {
      o += "\\n";
    } else {
      o += c;
    }
  }
  return o + "\"";
}

const char* to_name(Provenance p) {
  switch (p) {
    case Provenance::Synthetic: return "synthetic";
    case Provenance::ScreenRecording: return "screen_recording";
    case Provenance::Raw: return "raw";
  }
  return "synthetic";
}

}  // namespace

Result<FixtureCase> load_case(const std::filesystem::path& dir) {
  const auto file = dir / "case.toml";
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + file.string());
  std::stringstream ss;
  ss << in.rdbuf();

  auto parsed = toml::parse(ss.str());
  if (!parsed) return fail(ErrorKind::Config, file.string() + ": " + std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();

  FixtureCase c;
  c.dir = dir;

  const auto prov = root["provenance"].value<std::string>();
  if (!prov) return fail(ErrorKind::Config, file.string() + ": provenance missing");
  if (*prov == "synthetic") c.provenance = Provenance::Synthetic;
  else if (*prov == "screen_recording") c.provenance = Provenance::ScreenRecording;
  else if (*prov == "raw") c.provenance = Provenance::Raw;
  else return fail(ErrorKind::Config, file.string() + ": unknown provenance '" + *prov + "'");

  const auto mode = root["mode"].value<std::string>();
  if (!mode) return fail(ErrorKind::Config, file.string() + ": mode missing");
  if (*mode == "hole") c.mode = FinderMode::Hole;
  else if (*mode == "glow") c.mode = FinderMode::Glow;
  else return fail(ErrorKind::Config, file.string() + ": unknown mode '" + *mode + "'");

  const auto radius = root["expected_radius_px"].value<double>();
  if (!radius || !std::isfinite(*radius) || *radius <= 0)
    return fail(ErrorKind::Config, file.string() + ": expected_radius_px must be a positive number");
  c.expected_radius_px = *radius;
  const auto tol = root["tolerance_px"].value<double>();
  if (!tol || !std::isfinite(*tol) || *tol <= 0)
    return fail(ErrorKind::Config, file.string() + ": tolerance_px must be a positive number");
  c.tolerance_px = *tol;

  if (const auto* n = root.get("channel")) {
    const auto s = n->value<std::string>();
    if (!s) return fail(ErrorKind::Config, file.string() + ": channel must be a string");
    c.channel = *s;
  }
  if (const auto* n = root.get("note")) {
    const auto s = n->value<std::string>();
    if (!s) return fail(ErrorKind::Config, file.string() + ": note must be a string");
    c.note = *s;
  }

  const auto* frames = root["frames"].as_array();
  if (!frames) return fail(ErrorKind::Config, file.string() + ": [[frames]] missing");
  for (const auto& node : *frames) {
    const auto* t = node.as_table();
    if (!t) return fail(ErrorKind::Config, file.string() + ": frames entries must be tables");
    FixtureFrame f;
    const auto name = (*t)["file"].value<std::string>();
    if (!name || name->empty()) return fail(ErrorKind::Config, file.string() + ": frame without file");
    f.file = *name;
    if (const auto* cp = t->get("center_px")) {
      const auto* a = cp->as_array();
      if (!a || a->size() != 2 || !a->get(0)->is_number() || !a->get(1)->is_number())
        return fail(ErrorKind::Config, file.string() + ": center_px must be [x, y]");
      f.center_px = Vec2{a->get(0)->value<double>().value(), a->get(1)->value<double>().value()};
    }
    if (const auto* sk = t->get("skip")) {
      const auto b = sk->value<bool>();
      if (!b) return fail(ErrorKind::Config, file.string() + ": skip must be a boolean");
      f.skip = *b;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dir / f.file, ec))
      return fail(ErrorKind::Io, "frame file missing: " + (dir / f.file).string());
    c.frames.push_back(std::move(f));
  }
  return c;
}

Result<void> save_case(const FixtureCase& c) {
  std::string s;
  s += std::string("provenance = ") + quote(to_name(c.provenance)) + "\n";
  s += std::string("mode = ") + quote(c.mode == FinderMode::Hole ? "hole" : "glow") + "\n";
  s += "expected_radius_px = " + number(c.expected_radius_px) + "\n";
  s += "tolerance_px = " + number(c.tolerance_px) + "\n";
  s += "channel = " + quote(c.channel) + "\n";
  if (!c.note.empty()) s += "note = " + quote(c.note) + "\n";
  for (const auto& f : c.frames) {
    s += "\n[[frames]]\nfile = " + quote(f.file) + "\n";
    if (f.center_px) s += "center_px = [" + number(f.center_px->x) + ", " + number(f.center_px->y) + "]\n";
    if (f.skip) s += "skip = true\n";
  }
  const auto file = c.dir / "case.toml";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return fail(ErrorKind::Io, "cannot open " + file.string() + " for writing");
  out << s;
  out.close();
  if (!out) return fail(ErrorKind::Io, "write failed for " + file.string());
  return {};
}

FrameRecorder::FrameRecorder(std::filesystem::path dir, Provenance p, FinderMode m, double expected_radius_px) {
  case_.dir = std::move(dir);
  case_.provenance = p;
  case_.mode = m;
  case_.expected_radius_px = expected_radius_px;
}

Result<void> FrameRecorder::add(const FrameView& v, std::optional<Vec2> center_px) {
  std::error_code ec;
  std::filesystem::create_directories(case_.dir, ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + case_.dir.string() + ": " + ec.message());
  char name[32];
  std::snprintf(name, sizeof name, "%04zu.pgm", case_.frames.size() + 1);
  if (auto r = write_pgm(case_.dir / name, v); !r) return fail(r.error());
  case_.frames.push_back(FixtureFrame{name, center_px, false});
  return {};
}

Result<void> FrameRecorder::finish() {
  std::error_code ec;
  std::filesystem::create_directories(case_.dir, ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + case_.dir.string() + ": " + ec.message());
  return save_case(case_);
}

}  // namespace pychron::vision
