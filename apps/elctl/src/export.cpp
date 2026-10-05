// elctl export: select analyses from the store, reduce and group them with
// the built-in processing units, and write the Schaen et al. (2021) report.

#include "export.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string_view>
#include <system_error>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/report.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/processing/units.hpp"

namespace elctl {

namespace fs = std::filesystem;
namespace pp = pychron::processing;
namespace ps = pychron::persistence;
namespace r = pychron::reduction;
using pychron::Error;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

namespace {

constexpr const char* kShortUsage =
    "usage: elctl export --db <url> --out <file.csv|file.json> [--sample S]... [--identifier I]...\n"
    "                    [--project P]... [--irradiation R]... [--type T]... [--uuid U]...\n"
    "                    [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--include-invalid]\n"
    "                    [--group-by auto|aliquot|identifier|sample|none] [--sigma 1|2]\n"
    "                    [--constants default|legacy|legacy_preferences] [--decay-error]\n"
    "                    [--plateau fleck|mahon] [--plateau-steps N] [--plateau-gas PCT]\n"
    "                    [--lab NAME] [--note TEXT]... [--limit N]\n";

constexpr const char* kUsageText =
    "usage: elctl export --db <url> --out <file.csv|file.json> [selection] [options]\n"
    "\n"
    "Writes a 40Ar/39Ar data report after Schaen et al. (2021), GSA Bulletin 133,\n"
    "461-487: sample and irradiation metadata, the constants used, one row per\n"
    "analysis (corrected intensities, blanks, 40Ar*/39ArK, %40Ar*, 39ArK, K/Ca,\n"
    "isochron coordinates, ages with analytical and with-J uncertainties) and the\n"
    "summary ages of every group (integrated, plateau, weighted mean, inverse\n"
    "isochron). CSV, or JSON when the file name ends in .json.\n"
    "\n"
    "Selection (repeat a flag for several values; every flag narrows):\n"
    "  --sample <name>            --identifier <labnumber>\n"
    "  --project <name>           --irradiation <name>\n"
    "  --type <analysis type>     default: unknown\n"
    "  --uuid <analysis uuid>     explicit analyses; the other filters are then ignored\n"
    "  --from <YYYY-MM-DD>        --to <YYYY-MM-DD>     UTC, inclusive\n"
    "  --include-invalid          keep analyses tagged invalid\n"
    "\n"
    "Options:\n"
    "  --group-by auto|aliquot|identifier|sample|none\n"
    "                             one summary row per group (default auto: aliquot when\n"
    "                             any analysis is a heating step, else identifier)\n"
    "  --sigma 1|2                level of every ± column (default 2)\n"
    "  --constants default|legacy|legacy_preferences\n"
    "  --decay-error              propagate the decay-constant uncertainty into the ages\n"
    "  --plateau fleck|mahon      plateau criterion (default fleck)\n"
    "  --plateau-steps <n>        minimum plateau steps (default 3)\n"
    "  --plateau-gas <percent>    minimum 39ArK in the plateau (default 50)\n"
    "  --lab <name>               the laboratory, in the metadata\n"
    "  --note <text>              a metadata row (repeatable)\n"
    "  --limit <n>                at most n analyses (default 5000)\n"
    "\n"
    "Exit codes: 0 wrote the file; 1 no analysis matched; 2 usage or fatal error.\n";

struct Args {
  std::string db, out;
  std::vector<std::string> samples, identifiers, projects, irradiations, types, uuids, notes;
  std::optional<double> from, to;
  bool include_invalid = false;
  std::string group_by = "auto";
  int sigma = 2;
  std::string constants = "default";
  bool decay_error = false;
  std::string plateau = "fleck";
  int plateau_steps = 3;
  double plateau_gas = 50.0;
  std::string lab;
  int limit = 5000;
};

int usage(Io io, const std::string& message) {
  io.err << "elctl export: " << message << '\n' << kShortUsage;
  return kUsage;
}

int fatal(Io io, const std::string& message) {
  io.err << "elctl export: " << message << '\n';
  return kUsage;
}

int fatal(Io io, const Error& error) { return fatal(io, error.what); }

// "YYYY-MM-DD" as UTC epoch seconds of its midnight.
std::optional<double> parse_date(const std::string& text) {
  int y = 0, m = 0, d = 0;
  char rest = 0;
  if (std::sscanf(text.c_str(), "%d-%d-%d%c", &y, &m, &d, &rest) != 3) return std::nullopt;
  using namespace std::chrono;
  const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)}, day{static_cast<unsigned>(d)}};
  if (!ymd.ok()) return std::nullopt;
  return static_cast<double>(sys_days{ymd}.time_since_epoch().count()) * 86400.0;
}

std::optional<int> parse_int(const std::string& text) {
  int value = 0;
  char rest = 0;
  if (std::sscanf(text.c_str(), "%d%c", &value, &rest) != 1) return std::nullopt;
  return value;
}

std::optional<double> parse_double(const std::string& text) {
  double value = 0;
  char rest = 0;
  if (std::sscanf(text.c_str(), "%lf%c", &value, &rest) != 1) return std::nullopt;
  return value;
}

Result<Args> parse(const std::vector<std::string>& args) {
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    if (flag == "--include-invalid") {
      a.include_invalid = true;
      continue;
    }
    if (flag == "--decay-error") {
      a.decay_error = true;
      continue;
    }
    if (flag.rfind("--", 0) != 0) return fail(ErrorKind::Config, "unexpected argument '" + flag + "'");
    if (i + 1 >= args.size()) return fail(ErrorKind::Config, flag + " needs a value");
    const std::string& value = args[++i];
    if (flag == "--db") {
      a.db = value;
    } else if (flag == "--out") {
      a.out = value;
    } else if (flag == "--sample") {
      a.samples.push_back(value);
    } else if (flag == "--identifier") {
      a.identifiers.push_back(value);
    } else if (flag == "--project") {
      a.projects.push_back(value);
    } else if (flag == "--irradiation") {
      a.irradiations.push_back(value);
    } else if (flag == "--type") {
      a.types.push_back(value);
    } else if (flag == "--uuid") {
      a.uuids.push_back(value);
    } else if (flag == "--note") {
      a.notes.push_back(value);
    } else if (flag == "--lab") {
      a.lab = value;
    } else if (flag == "--from" || flag == "--to") {
      const auto t = parse_date(value);
      if (!t) return fail(ErrorKind::Config, flag + " takes a UTC date YYYY-MM-DD; got '" + value + "'");
      (flag == "--from" ? a.from : a.to) = flag == "--from" ? *t : *t + 86399.0;  // --to: the whole day
    } else if (flag == "--group-by") {
      for (const char* k : {"auto", "aliquot", "identifier", "sample", "none"})
        if (value == k) a.group_by = value;
      if (a.group_by != value) return fail(ErrorKind::Config, "--group-by is auto, aliquot, identifier, sample or none");
    } else if (flag == "--sigma") {
      const auto n = parse_int(value);
      if (!n || (*n != 1 && *n != 2)) return fail(ErrorKind::Config, "--sigma is 1 or 2; got '" + value + "'");
      a.sigma = *n;
    } else if (flag == "--constants") {
      bool known = false;
      for (const auto p : {r::ConstantsPreset::Default, r::ConstantsPreset::Legacy, r::ConstantsPreset::LegacyPreferences})
        known = known || r::to_string(p) == value;
      if (!known) return fail(ErrorKind::Config, "--constants is default, legacy or legacy_preferences; got '" + value + "'");
      a.constants = value;
    } else if (flag == "--plateau") {
      if (value != "fleck" && value != "mahon") return fail(ErrorKind::Config, "--plateau is fleck or mahon");
      a.plateau = value;
    } else if (flag == "--plateau-steps") {
      const auto n = parse_int(value);
      if (!n || *n < 2) return fail(ErrorKind::Config, "--plateau-steps takes a whole number of 2 or more");
      a.plateau_steps = *n;
    } else if (flag == "--plateau-gas") {
      const auto pct = parse_double(value);
      if (!pct || !(*pct >= 0 && *pct <= 100)) return fail(ErrorKind::Config, "--plateau-gas takes a percent, 0 to 100");
      a.plateau_gas = *pct;
    } else if (flag == "--limit") {
      const auto n = parse_int(value);
      if (!n || *n < 1) return fail(ErrorKind::Config, "--limit takes a whole positive number");
      a.limit = *n;
    } else {
      return fail(ErrorKind::Config, "unknown flag '" + flag + "'");
    }
  }
  if (a.db.empty()) return fail(ErrorKind::Config, "--db <url> is required");
  if (a.out.empty()) return fail(ErrorKind::Config, "--out <file> is required");
  if (a.from && a.to && *a.to < *a.from) return fail(ErrorKind::Config, "--to is before --from");
  return a;
}

bool any_step(const pp::Dataset& d) {
  for (const auto& it : d.items())
    if (it.analysis->analysis->increment >= 0) return true;
  return false;
}

int run(const Args& a, Io io) {
  // A report reads: a mistyped SQLite path is an error, not a new empty store.
  constexpr std::string_view kSqlite = "sqlite:";
  if (a.db.starts_with(kSqlite) && a.db != "sqlite::memory:") {
    std::error_code code;
    const fs::path file(a.db.substr(kSqlite.size()));
    if (!fs::is_regular_file(file, code)) return fatal(io, "no database at " + file.string());
    if (fs::file_size(file, code) == 0 && !code) return fatal(io, file.string() + " is empty: not a pychron store");
  }
  auto source = pp::StoreSource::open(ps::StoreConfig{a.db, false}, pp::StoreSourceOptions{1, "", ""});
  if (!source) return fatal(io, source.error());

  const auto& reg = pp::UnitRegistry::builtin();
  pp::Pipeline pipeline;
  pipeline.name = "export";
  auto& select = pipeline.add(reg, "select", "select");
  auto set = [&](pp::NodeSpec& node, std::string_view key, pp::OptionValue value) -> std::optional<Error> {
    auto ok = node.options.set(key, std::move(value));
    if (!ok) return ok.error();
    return std::nullopt;
  };
  std::optional<Error> bad;
  auto apply = [&](std::optional<Error> e) {
    if (e && !bad) bad = e;
  };
  apply(set(select, "uuids", a.uuids));
  apply(set(select, "samples", a.samples));
  apply(set(select, "identifiers", a.identifiers));
  apply(set(select, "projects", a.projects));
  apply(set(select, "irradiations", a.irradiations));
  apply(set(select, "analysis_types", a.types.empty() ? std::vector<std::string>{"unknown"} : a.types));
  if (a.from) apply(set(select, "from", *a.from));
  if (a.to) apply(set(select, "to", *a.to));
  apply(set(select, "limit", static_cast<std::int64_t>(a.limit)));
  apply(set(select, "remove_tags", a.include_invalid ? std::vector<std::string>{} : std::vector<std::string>{"invalid"}));
  auto& reduce = pipeline.add(reg, "reduce", "reduce", {"select"});
  apply(set(reduce, "constants", a.constants));
  apply(set(reduce, "include_decay_error", a.decay_error));
  if (bad) return fatal(io, *bad);

  pp::Runner runner(reg, source->get());
  auto reduced = runner.run(pipeline, "reduce");
  if (!reduced) return fatal(io, reduced.error());
  const auto& before_grouping = std::get<pp::DatasetPtr>(reduced->at(0));
  if (before_grouping->empty()) {
    io.err << "elctl export: no analysis matched\n";
    return kFailed;
  }
  std::string key = a.group_by;
  if (key == "auto") key = any_step(*before_grouping) ? "aliquot" : "identifier";
  auto& group = pipeline.add(reg, "group", "group", {"reduce"});
  if (auto e = set(group, "key", key)) return fatal(io, *e);
  auto grouped = runner.run(pipeline, "group");
  if (!grouped) return fatal(io, grouped.error());
  const auto& dataset = std::get<pp::DatasetPtr>(grouped->at(0));
  for (const auto& d : runner.diagnostics()) io.err << "warning: " << d << '\n';

  pp::ReportOptions options;
  options.nsigma = a.sigma;
  options.laboratory = a.lab;
  options.notes = a.notes;
#ifdef PYCHRON_ELCTL_VERSION
  options.software = std::string("pychron-cpp ") + PYCHRON_ELCTL_VERSION;
#endif
  options.plateau.method = a.plateau == "mahon" ? r::PlateauMethod::Mahon : r::PlateauMethod::Fleck;
  options.plateau.nsteps = a.plateau_steps;
  options.plateau.gas_fraction = a.plateau_gas;
  const pp::Report report = pp::make_report(*dataset, options);
  for (const auto& w : report.warnings) io.err << "warning: " << w << '\n';
  if (auto saved = pp::save_report(report, fs::path(a.out)); !saved) return fatal(io, saved.error());
  io.out << "wrote " << a.out << ": " << report.analyses.rows.size() << " analyses, " << report.summary.rows.size()
         << (report.summary.rows.size() == 1 ? " group" : " groups") << " (grouped by " << key << ")\n";
  return kOk;
}

}  // namespace

int export_command(const std::vector<std::string>& args, Io io) {
  if (!args.empty() && (args[0] == "help" || args[0] == "--help")) {
    io.out << kUsageText;
    return kOk;
  }
  auto parsed = parse(args);
  if (!parsed) return usage(io, parsed.error().what);
  // No exception leaves a library, but std::filesystem can throw: a fatal
  // error like any other.
  try {
    return run(*parsed, io);
  } catch (const std::exception& e) {
    return fatal(io, e.what());
  }
}

}  // namespace elctl
