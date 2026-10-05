#include "pychron/laser/calibration_store.hpp"

#include <cmath>
#include <fstream>
#include <locale>
#include <numbers>
#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "pychron/core/sha256.hpp"

namespace pychron::laser {

namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kSchemaVersion = 1;

bool safe(std::string_view name) { return safe_file_part(name); }

Result<void> check_names(std::string_view device, std::string_view tray) {
  if (!safe(device)) {
    return fail(ErrorKind::Config, "'" + std::string(device) + "' cannot name a device in a calibration file");
  }
  if (!safe(tray)) {
    return fail(ErrorKind::Config, "'" + std::string(tray) + "' cannot name a tray in a calibration file");
  }
  return {};
}

std::string where(std::string_view device, std::string_view tray) {
  return "tray " + std::string(tray) + " on " + std::string(device);
}

}  // namespace

bool safe_file_part(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == '\0') return false;
  }
  return true;
}

std::string fingerprint(std::span<const CalibrationPoint> points) {
  // Exact: the bits of each coordinate, not a rounded decimal.
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << std::hexfloat;
  for (const auto& p : points) text << p.hole.size() << ':' << p.hole << '\t' << p.x << '\t' << p.y << '\n';
  return to_hex(sha256(std::string_view(text.str())));
}

std::string_view to_string(CalibrationState state) noexcept {
  switch (state) {
    case CalibrationState::Missing: return "not calibrated";
    case CalibrationState::Stale: return "stale";
    case CalibrationState::Unsolvable: return "unsolvable";
    case CalibrationState::Ok: return "ok";
  }
  return "unsolvable";
}

CalibrationStore::CalibrationStore(fs::path dir) : dir_(std::move(dir)) {}

fs::path CalibrationStore::file(std::string_view device, std::string_view tray) const {
  return dir_ / (std::string(device) + "." + std::string(tray) + ".toml");
}

Result<std::optional<StoredCalibration>> CalibrationStore::load(std::string_view device, std::string_view tray) const {
  if (auto ok = check_names(device, tray); !ok) return fail(ok.error());
  const fs::path path = file(device, tray);
  std::error_code ec;
  if (!fs::exists(path, ec)) return std::optional<StoredCalibration>{};

  const auto bad = [&path](const std::string& what) {
    return fail(ErrorKind::Config, path.string() + ": " + what);
  };
  std::ifstream in(path, std::ios::binary);
  if (!in) return bad("cannot be read");
  std::ostringstream text;
  text << in.rdbuf();
  const toml::parse_result parsed = toml::parse(text.str());
  if (!parsed) return bad(std::string(parsed.error().description()));
  const toml::table& table = parsed.table();

  const auto version = table["schema_version"].value<std::int64_t>();
  if (!version || *version != kSchemaVersion) {
    return bad("schema_version must be " + std::to_string(kSchemaVersion));
  }
  StoredCalibration out;
  const auto text_key = [&table](const char* key) { return table[key].value<std::string>(); };
  const auto file_device = text_key("device");
  const auto file_tray = text_key("tray");
  const auto sha = text_key("tray_sha256");
  if (!file_device) return bad("device is missing");
  if (!file_tray) return bad("tray is missing");
  if (!sha) return bad("tray_sha256 is missing");
  if (*file_device != device) return bad("is for device '" + *file_device + "', not '" + std::string(device) + "'");
  if (*file_tray != tray) return bad("is for tray '" + *file_tray + "', not '" + std::string(tray) + "'");
  out.device = *file_device;
  out.tray = *file_tray;
  out.tray_sha256 = *sha;

  if (const auto node = table["points"]; node) {
    const toml::array* points = node.as_array();
    if (points == nullptr) return bad("points must be an array of tables");
    for (const auto& item : *points) {
      const toml::table* p = item.as_table();
      if (p == nullptr) return bad("points must be an array of tables");
      const auto hole = (*p)["hole"].value<std::string>();
      const auto x = (*p)["x"].value<double>();  // an integer converts
      const auto y = (*p)["y"].value<double>();
      if (!hole || !x || !y) return bad("each of points needs hole (text), x and y (numbers)");
      out.points.push_back({*hole, *x, *y});
    }
  }
  return std::optional<StoredCalibration>{std::move(out)};
}

Result<void> CalibrationStore::save(const TrayMap& map, std::string_view device,
                                    std::span<const CalibrationPoint> points) const {
  if (auto ok = check_names(device, map.name()); !ok) return ok;
  const auto solved = solve(map, points);
  if (!solved) return fail(solved.error());

  toml::table table;
  table.insert("schema_version", kSchemaVersion);
  table.insert("device", std::string(device));
  table.insert("tray", map.name());
  table.insert("tray_sha256", map.sha256());
  // Written for the reader; load() takes only the points.
  const Transform& t = solved->transform;
  table.insert("center", toml::array{t.cx, t.cy});
  table.insert("rotation_deg", t.rotation * 180.0 / std::numbers::pi);
  table.insert("scale", t.scale);
  table.insert("rms_mm", solved->rms_mm);
  toml::array list;
  for (const auto& p : points) {
    toml::table point;
    point.insert("hole", p.hole);
    point.insert("x", p.x);
    point.insert("y", p.y);
    list.push_back(std::move(point));
  }
  table.insert("points", std::move(list));

  const fs::path path = file(device, map.name());
  const auto cannot = [&path](const std::string& why) {
    return fail(ErrorKind::Io, "cannot write " + path.string() + (why.empty() ? "" : ": " + why));
  };
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) return cannot(ec.message());
  // Beside the target, then renamed: a reader never sees a torn file.
  const fs::path tmp = fs::path(path).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (out) out << table << '\n';
    out.flush();
    if (!out) {
      fs::remove(tmp, ec);
      return cannot("");
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(tmp, ec);
    return cannot(why);
  }
  return {};
}

Result<void> CalibrationStore::clear(std::string_view device, std::string_view tray) const {
  if (auto ok = check_names(device, tray); !ok) return ok;
  std::error_code ec;
  fs::remove(file(device, tray), ec);
  if (ec) return fail(ErrorKind::Io, "cannot delete " + file(device, tray).string() + ": " + ec.message());
  return {};
}

CalibrationStatus CalibrationStore::status(const TrayMap& map, std::string_view device) const {
  CalibrationStatus out;
  const auto loaded = load(device, map.name());
  if (!loaded) {
    out.state = CalibrationState::Unsolvable;
    out.why = where(device, map.name()) + ": " + loaded.error().what;
    return out;
  }
  if (!loaded->has_value()) {
    out.state = CalibrationState::Missing;
    out.why = where(device, map.name()) + " is not calibrated";
    return out;
  }
  if ((*loaded)->tray_sha256 != map.sha256()) {
    out.state = CalibrationState::Stale;
    out.why = where(device, map.name()) + ": the calibration is stale (the tray map has changed since it was made)";
    return out;
  }
  auto solved = solve(map, (*loaded)->points);
  if (!solved) {
    out.state = CalibrationState::Unsolvable;
    out.why = where(device, map.name()) + ": " + solved.error().what;
    return out;
  }
  out.state = CalibrationState::Ok;
  out.solution = std::move(*solved);
  out.fingerprint = fingerprint((*loaded)->points);
  return out;
}

}  // namespace pychron::laser
