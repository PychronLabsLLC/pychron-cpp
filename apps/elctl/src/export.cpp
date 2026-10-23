// elctl export: select analyses from the store, reduce and group them with
// the built-in processing units, and write the Schaen et al. (2021) report.

#include "export.hpp"

#include <filesystem>
#include <optional>
#include <ostream>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/processing/report.hpp"
#include "pychron/processing/units.hpp"
#include "selection.hpp"

namespace elctl {

namespace fs = std::filesystem;
namespace pp = pychron::processing;
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
  Selection selection;
  AgeFlags ages;
  std::vector<std::string> notes;
  std::string group_by = "auto";
  int sigma = 2;
  std::string lab;
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

Result<Args> parse(const std::vector<std::string>& args) {
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    // The flags shared with `earthbank` (selection.hpp) first.
    auto shared = take_selection_flag(a.selection, args, i);
    if (shared && !*shared) shared = take_age_flag(a.ages, args, i);
    if (!shared) return fail(shared.error());
    if (*shared) continue;
    if (!flag.starts_with("--")) return fail(ErrorKind::Config, "unexpected argument '" + flag + "'");
    if (i + 1 >= args.size()) return fail(ErrorKind::Config, flag + " needs a value");
    const std::string& value = args[++i];
    if (flag == "--db") {
      a.db = value;
    } else if (flag == "--out") {
      a.out = value;
    } else if (flag == "--note") {
      a.notes.push_back(value);
    } else if (flag == "--lab") {
      a.lab = value;
    } else if (flag == "--group-by") {
      for (const char* k : {"auto", "aliquot", "identifier", "sample", "none"})
        if (value == k) a.group_by = value;
      if (a.group_by != value) return fail(ErrorKind::Config, "--group-by is auto, aliquot, identifier, sample or none");
    } else if (flag == "--sigma") {
      const auto n = parse_int(value);
      if (!n || (*n != 1 && *n != 2)) return fail(ErrorKind::Config, "--sigma is 1 or 2; got '" + value + "'");
      a.sigma = *n;
    } else {
      return fail(ErrorKind::Config, "unknown flag '" + flag + "'");
    }
  }
  if (a.db.empty()) return fail(ErrorKind::Config, "--db <url> is required");
  if (a.out.empty()) return fail(ErrorKind::Config, "--out <file> is required");
  if (auto ok = check_selection(a.selection); !ok) return fail(ok.error());
  return a;
}

bool any_step(const pp::Dataset& d) {
  for (const auto& it : d.items())
    if (it.analysis->analysis->increment >= 0) return true;
  return false;
}

int run(const Args& a, const pp::Domain& domain, Io io) {
  auto source = open_source(a.db);
  if (!source) return fatal(io, source.error());

  const auto& reg = domain.units();
  pp::Pipeline pipeline;
  pipeline.name = "export";
  if (auto selected = add_select(pipeline, reg, a.selection); !selected) return fatal(io, selected.error());
  auto set = [&](pp::NodeSpec& node, std::string_view key, pp::OptionValue value) -> std::optional<Error> {
    auto ok = node.options.set(key, std::move(value));
    if (!ok) return ok.error();
    return std::nullopt;
  };
  auto& reduce = pipeline.add(reg, "reduce", "reduce", {"select"});
  if (auto e = set(reduce, "constants", a.ages.constants)) return fatal(io, *e);
  if (auto e = set(reduce, "include_decay_error", a.ages.decay_error)) return fatal(io, *e);

  pp::Runner runner(reg, source->get(), &domain);
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
  options.plateau = a.ages.criteria();
  const pp::Report report = pp::make_report(*dataset, options);
  for (const auto& w : report.warnings) io.err << "warning: " << w << '\n';
  if (auto saved = pp::save_report(report, fs::path(a.out)); !saved) return fatal(io, saved.error());
  io.out << "wrote " << a.out << ": " << report.analyses.rows.size() << " analyses, " << report.summary.rows.size()
         << (report.summary.rows.size() == 1 ? " group" : " groups") << " (grouped by " << key << ")\n";
  return kOk;
}

}  // namespace

int export_command(const std::vector<std::string>& args, const pp::Domain& domain, Io io) {
  if (!args.empty() && (args[0] == "help" || args[0] == "--help")) {
    io.out << kUsageText;
    return kOk;
  }
  auto parsed = parse(args);
  if (!parsed) return usage(io, parsed.error().what);
  // No exception leaves a library, but std::filesystem can throw: a fatal
  // error like any other.
  try {
    return run(*parsed, domain, io);
  } catch (const std::exception& e) {
    return fatal(io, e.what());
  }
}

}  // namespace elctl
