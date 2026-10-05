#include "pychron/laser/correction_store.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "pychron/laser/calibration_store.hpp"

namespace pychron::laser {

namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kSchemaVersion = 1;

Result<void> check_names(std::string_view device, std::string_view tray) {
  if (!safe_file_part(device)) {
    return fail(ErrorKind::Config, "'" + std::string(device) + "' cannot name a device in a corrections file");
  }
  if (!safe_file_part(tray)) {
    return fail(ErrorKind::Config, "'" + std::string(tray) + "' cannot name a tray in a corrections file");
  }
  return {};
}

std::optional<double> finite_number(const toml::node_view<const toml::node>& node) {
  double value = 0;
  if (const auto* f = node.as_floating_point()) value = f->get();
  else if (const auto* i = node.as_integer()) value = static_cast<double>(i->get());
  else return std::nullopt;
  if (!std::isfinite(value)) return std::nullopt;
  return value;
}

}  // namespace

CorrectionStore::CorrectionStore(fs::path dir) : dir_(std::move(dir)) {}

fs::path CorrectionStore::file(std::string_view device, std::string_view tray) const {
  return dir_ / (std::string(device) + "." + std::string(tray) + ".toml");
}

Result<HoleCorrections> CorrectionStore::load(const TrayMap& map, std::string_view device,
                                              std::string_view calibration) const {
  if (auto ok = check_names(device, map.name()); !ok) return fail(ok.error());
  const fs::path path = file(device, map.name());
  std::error_code ec;
  if (!fs::exists(path, ec)) return HoleCorrections{};

  const auto bad = [&path](const std::string& what) { return fail(ErrorKind::Config, path.string() + ": " + what); };
  std::ifstream in(path, std::ios::binary);
  if (!in) return bad("cannot be read");
  std::ostringstream text;
  text << in.rdbuf();
  const toml::parse_result parsed = toml::parse(text.str());
  if (!parsed) return bad(std::string(parsed.error().description()));
  const toml::table& table = parsed.table();

  const auto version = table["schema_version"].value<std::int64_t>();
  if (!version || *version != kSchemaVersion) return bad("schema_version must be " + std::to_string(kSchemaVersion));
  const auto sha = table["tray_sha256"].value<std::string>();
  const auto made_with = table["calibration"].value<std::string>();
  // For another version of the map, or another calibration: nothing applies.
  if (!sha || !made_with || *sha != map.sha256() || *made_with != calibration) return HoleCorrections{};

  HoleCorrections out;
  if (const toml::table* holes = table["holes"].as_table()) {
    for (const auto& [k, node] : *holes) {
      const std::string hole(k.str());
      const toml::table* entry = node.as_table();
      if (entry == nullptr || map.find(hole) == nullptr) continue;
      const toml::table& e = *entry;
      const auto x = finite_number(e["x"]);
      const auto y = finite_number(e["y"]);
      if (!x || !y) continue;
      HoleCorrection c;
      c.x = *x;
      c.y = *y;
      c.residual_mm = finite_number(e["residual_mm"]).value_or(0);
      c.found = e["found"].value<std::string>().value_or(std::string{});
      out.insert_or_assign(hole, std::move(c));
    }
  }
  return out;
}

Result<void> CorrectionStore::write(const TrayMap& map, std::string_view device, std::string_view calibration,
                                    const HoleCorrections& holes) const {
  toml::table table;
  table.insert("schema_version", kSchemaVersion);
  table.insert("device", std::string(device));
  table.insert("tray", map.name());
  table.insert("tray_sha256", map.sha256());
  table.insert("calibration", std::string(calibration));
  toml::table entries;
  for (const auto& [hole, c] : holes) {
    toml::table entry;
    entry.insert("x", c.x);
    entry.insert("y", c.y);
    entry.insert("residual_mm", c.residual_mm);
    entry.insert("found", c.found);
    entries.insert(hole, std::move(entry));
  }
  table.insert("holes", std::move(entries));

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

Result<void> CorrectionStore::put(const TrayMap& map, std::string_view device, std::string_view calibration,
                                  std::string_view hole, const HoleCorrection& correction) const {
  if (map.find(hole) == nullptr) {
    return fail(ErrorKind::Config, "no hole " + std::string(hole) + " on tray " + map.name());
  }
  if (!std::isfinite(correction.x) || !std::isfinite(correction.y) || !std::isfinite(correction.residual_mm)) {
    return fail(ErrorKind::Config, "the correction of hole " + std::string(hole) + " is not a position");
  }
  // What still applies is kept; a file for another map or calibration is
  // started again. One that cannot be understood is not written over.
  auto holes = load(map, device, calibration);
  if (!holes) return fail(holes.error());
  holes->insert_or_assign(std::string(hole), correction);
  return write(map, device, calibration, *holes);
}

Result<void> CorrectionStore::clear(std::string_view device, std::string_view tray) const {
  if (auto ok = check_names(device, tray); !ok) return ok;
  std::error_code ec;
  fs::remove(file(device, tray), ec);
  if (ec) return fail(ErrorKind::Io, "cannot delete " + file(device, tray).string() + ": " + ec.message());
  return {};
}

Result<void> CorrectionStore::clear_hole(const TrayMap& map, std::string_view device, std::string_view calibration,
                                         std::string_view hole) const {
  auto holes = load(map, device, calibration);
  if (!holes) return fail(holes.error());
  const auto it = holes->find(hole);
  if (it == holes->end()) return {};
  holes->erase(it);
  return write(map, device, calibration, *holes);
}

}  // namespace pychron::laser
