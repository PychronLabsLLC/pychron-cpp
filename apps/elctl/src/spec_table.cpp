#include "spec_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <system_error>
#include <utility>

#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/config_loader.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"
#include "pychron/systems/spectrometer/field_table_check.hpp"
#include "pychron/systems/spectrometer/field_table_store.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"

namespace elctl {

namespace {

namespace fs = std::filesystem;
namespace spec = pychron::spectrometer;
namespace cfg = pychron::spectrometer::cfg;

constexpr const char* kSpecUsage =
    "usage: elctl spec table <command> [--spectrometer <spectrometer.toml>]\n"
    "  list                          tables, their current version, (active) and (HV)\n"
    "  show [<name>] [--version V]   a version as a grid; default the active table, current version\n"
    "  versions <name>               versions, newest first\n"
    "  diff <name> <v1> [<v2>]       what changed from v1 to v2 (default current), by isotope\n"
    "  check [<name>]                diagnostics of the current version; exit 1 on an error\n";

std::string fixed5(double v) {
  std::array<char, 64> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%0.5f", v);
  return n > 0 ? std::string(buf.data(), static_cast<std::size_t>(std::min<int>(n, buf.size() - 1))) : "?";
}

std::string signed5(double v) {
  std::array<char, 64> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%+0.5f", v);
  return n > 0 ? std::string(buf.data(), static_cast<std::size_t>(std::min<int>(n, buf.size() - 1))) : "?";
}

std::string cell(const std::optional<double>& v) { return v ? fixed5(*v) : "-"; }

// What every subcommand reads: the spectrometer config and weights, and the
// data root its tables live under.
struct SpecData {
  fs::path root;
  cfg::SpectrometerConfig config;
  spec::MolecularWeights weights;

  fs::path tables() const { return root / "tables"; }
};

pychron::Result<SpecData> load_data(const fs::path& file) {
  // The schema only, not the assembler rules: those read the tables, and a
  // broken table is what `check` is for.
  auto parsed = cfg::parse_config(file);
  if (!parsed.config) {
    std::string what = "cannot read " + file.string();
    for (const auto& d : parsed.diagnostics) what += "\n  " + d.message;
    return pychron::fail(pychron::ErrorKind::Config, std::move(what));
  }
  SpecData out{file.parent_path(), std::move(*parsed.config), {}};
  cfg::MolecularWeights weights = cfg::default_molecular_weights();
  const auto weights_file = cfg::molecular_weights_path(out.root);
  std::error_code ec;
  if (fs::exists(weights_file, ec)) {
    std::ifstream in(weights_file, std::ios::binary);
    if (!in.is_open()) return pychron::fail(pychron::ErrorKind::Io, "cannot read " + weights_file.string());
    std::ostringstream text;
    text << in.rdbuf();
    auto w = cfg::load_molecular_weights_from_string(text.str(), weights_file.generic_string());
    if (!w.weights) {
      std::string what = "cannot read " + weights_file.string();
      for (const auto& d : w.diagnostics) what += "\n  " + d.message;
      return pychron::fail(pychron::ErrorKind::Config, std::move(what));
    }
    weights = std::move(*w.weights);
  }
  out.weights = spec::MolecularWeights(std::move(weights));
  return out;
}

// Directories under tables/ holding a `current` file, sorted by name.
std::vector<std::string> table_names(const SpecData& d) {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(d.tables(), ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    if (fs::is_regular_file(it->path() / "current", ec)) out.push_back(it->path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

class SpecTable {
 public:
  SpecTable(SpecData data, const Io& io) : d_(std::move(data)), io_(io) {}

  int list() {
    for (const auto& name : table_names(d_)) {
      const spec::FieldTableStore store(d_.tables(), name);
      const auto current = store.current_version();
      io_.out << name << "  " << (current ? *current : "? (" + current.error().what + ")");
      if (name == d_.config.magnet.field_table) io_.out << "  (active)";
      if (!d_.config.magnet.hv_table.empty() && name == d_.config.magnet.hv_table) io_.out << "  (HV)";
      io_.out << '\n';
    }
    return kOk;
  }

  int show(const std::string& name, const std::optional<std::string>& version) {
    auto store = open(name);
    if (!store) return kFailed;
    auto v = version ? pychron::Result<std::string>(*version) : store->current_version();
    if (!v) return failed(v.error().what);
    auto table = store->load(*v);
    if (!table) return failed(name + " " + *v + ": " + table.error().what);

    io_.out << name << ' ' << *v << " (axis " << spec::to_string(table->axis()) << ")\n";
    const auto dets = columns(*table);
    // Rows by mass; points of equal mass keep their order in the file.
    const auto& points = table->points();
    std::vector<std::size_t> rows(points.size());
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = i;
    std::stable_sort(rows.begin(), rows.end(),
                     [&points](std::size_t a, std::size_t b) { return points[a].mass < points[b].mass; });

    std::vector<std::vector<std::string>> grid;
    grid.push_back({"Isotope", "Mass"});
    grid.front().insert(grid.front().end(), dets.begin(), dets.end());
    for (const std::size_t r : rows) {
      const auto& p = points[r];
      std::vector<std::string> line{p.isotope, fixed5(p.mass)};
      for (const auto& det : dets) {
        const auto it = p.values.find(det);
        line.push_back(it == p.values.end() ? "-" : fixed5(it->second));
      }
      grid.push_back(std::move(line));
    }
    print_grid(grid);

    io_.out << "fit: " << spec::to_string(table->default_fit()) << '\n';
    for (const auto& [det, kind] : table->fit_overrides()) io_.out << "fit " << det << ": " << spec::to_string(kind) << '\n';
    return kOk;
  }

  int versions(const std::string& name) {
    auto store = open(name);
    if (!store) return kFailed;
    auto all = store->versions();
    if (!all) return failed(all.error().what);
    const auto current = store->current_version();
    for (const auto& v : std::views::reverse(*all)) {
      io_.out << v;
      if (current && *current == v) io_.out << "  (current)";
      io_.out << '\n';
    }
    return kOk;
  }

  int diff(const std::string& name, const std::string& v1, const std::optional<std::string>& v2) {
    auto store = open(name);
    if (!store) return kFailed;
    auto to = v2 ? pychron::Result<std::string>(*v2) : store->current_version();
    if (!to) return failed(to.error().what);
    auto a = store->load(v1);
    if (!a) return failed(name + " " + v1 + ": " + a.error().what);
    auto b = store->load(*to);
    if (!b) return failed(name + " " + *to + ": " + b.error().what);

    io_.out << name << ' ' << v1 << " -> " << *to << '\n';
    const auto d = spec::diff_field_tables(*a, *b);
    if (d.empty()) {
      io_.out << "no differences\n";
      return kOk;
    }
    for (const auto& f : d.fit_changes) io_.out << f << '\n';
    for (const auto& r : d.rows) io_.out << (r.change == spec::RowChange::Added ? "+ " : "- ") << r.isotope << '\n';
    for (const auto& m : d.masses)
      io_.out << m.isotope << " mass " << fixed5(m.old_mass) << " -> " << fixed5(m.new_mass) << " (\xCE\x94 "
              << signed5(m.new_mass - m.old_mass) << ")\n";
    for (const auto& c : d.cells) {
      io_.out << c.isotope << ' ' << c.detector << ' ' << cell(c.old_value) << " -> " << cell(c.new_value);
      if (c.old_value && c.new_value) io_.out << " (\xCE\x94 " << signed5(*c.new_value - *c.old_value) << ')';
      io_.out << '\n';
    }
    return kOk;
  }

  int check(const std::string& name) {
    auto store = open(name);
    if (!store) return kFailed;
    auto v = store->current_version();
    if (!v) return failed(v.error().what);
    auto table = store->load(*v);
    if (!table) {
      io_.out << name << ' ' << *v << '\n' << "error: " << table.error().what << '\n';
      return kFailed;
    }
    const auto found = spec::check_field_table(*table, d_.config, d_.weights, name);
    io_.out << name << ' ' << *v << ": " << (found.empty() ? "no problems" : std::to_string(found.size()) + " found")
            << '\n';
    for (const auto& diagnostic : found) io_.out << spec::to_string(diagnostic) << '\n';
    return spec::has_error(found) ? kFailed : kOk;
  }

  const std::string& active() const { return d_.config.magnet.field_table; }

 private:
  // The store of an existing table; says so and returns nullopt when there is none.
  std::optional<spec::FieldTableStore> open(const std::string& name) {
    std::error_code ec;
    if (!fs::is_regular_file(d_.tables() / name / "current", ec)) {
      io_.err << "elctl: no field table '" << name << "' under " << d_.tables().string() << '\n';
      return std::nullopt;
    }
    return spec::FieldTableStore(d_.tables(), name);
  }

  int failed(const std::string& what) {
    io_.err << "elctl: " << what << '\n';
    return kFailed;
  }

  // Config detectors in config order, then any only in the table.
  std::vector<std::string> columns(const spec::FieldTable& table) const {
    std::vector<std::string> out;
    out.reserve(d_.config.detectors.size());
    for (const auto& det : d_.config.detectors) out.push_back(det.name);
    for (auto& det : table.detectors())
      if (std::find(out.begin(), out.end(), det) == out.end()) out.push_back(std::move(det));
    return out;
  }

  void print_grid(const std::vector<std::vector<std::string>>& grid) {
    std::vector<std::size_t> width;
    for (const auto& row : grid) {
      if (width.size() < row.size()) width.resize(row.size(), 0);
      for (std::size_t i = 0; i < row.size(); ++i) width[i] = std::max(width[i], row[i].size());
    }
    for (const auto& row : grid) {
      std::string line;
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i > 0) line += "  ";
        // Isotope left-aligned, numbers right-aligned.
        const std::string pad(width[i] - row[i].size(), ' ');
        line += i == 0 ? row[i] + pad : pad + row[i];
      }
      while (!line.empty() && line.back() == ' ') line.pop_back();
      io_.out << line << '\n';
    }
  }

  SpecData d_;
  Io io_;
};

int usage(Io io, const std::string& message) {
  io.err << "elctl: spec: " << message << '\n' << kSpecUsage;
  return kUsage;
}

}  // namespace

int spec_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io) {
  if (args.empty() || args.front() != "table") return usage(io, args.empty() ? "needs a command" : "unknown command '" + args.front() + "'");

  fs::path file = globals.spectrometer;
  std::optional<std::string> version;
  std::vector<std::string> pos;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--spectrometer" || a == "--version") {
      if (i + 1 >= args.size()) return usage(io, a + " needs a value");
      if (a == "--spectrometer") file = args[++i];
      else version = args[++i];
    } else if (a.starts_with("--")) {
      return usage(io, "unknown option '" + a + "'");
    } else {
      pos.push_back(a);
    }
  }
  if (pos.empty()) return usage(io, "table needs a command");
  const std::string sub = pos.front();
  pos.erase(pos.begin());
  if (version && sub != "show") return usage(io, "--version is for show");

  const auto arity = [&](std::size_t lo, std::size_t hi) { return pos.size() >= lo && pos.size() <= hi; };
  if (sub == "list" && !arity(0, 0)) return usage(io, "table list takes no arguments");
  if (sub == "show" && !arity(0, 1)) return usage(io, "table show [<name>] [--version V]");
  if (sub == "versions" && !arity(1, 1)) return usage(io, "table versions <name>");
  if (sub == "diff" && !arity(2, 3)) return usage(io, "table diff <name> <v1> [<v2>]");
  if (sub == "check" && !arity(0, 1)) return usage(io, "table check [<name>]");
  if (sub != "list" && sub != "show" && sub != "versions" && sub != "diff" && sub != "check")
    return usage(io, "unknown table command '" + sub + "'");

  if (file.empty()) return usage(io, "no spectrometer: give --spectrometer <spectrometer.toml> or --install <name>");
  auto data = load_data(file);
  if (!data) {
    io.err << "elctl: " << data.error().what << '\n';
    return kFailed;
  }
  SpecTable t(std::move(*data), io);
  if (sub == "list") return t.list();
  if (sub == "show") return t.show(pos.empty() ? t.active() : pos[0], version);
  if (sub == "versions") return t.versions(pos[0]);
  if (sub == "diff") return t.diff(pos[0], pos[1], pos.size() > 2 ? std::optional<std::string>(pos[2]) : std::nullopt);
  return t.check(pos.empty() ? t.active() : pos[0]);
}

}  // namespace elctl
