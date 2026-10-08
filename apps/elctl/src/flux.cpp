// elctl flux: parse the command line, load a level (or every level of an
// irradiation), fit it, print the tables and optionally write a CSV and save.

#include "flux.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/user_file.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/flux_view.hpp"
#include "pychron/processing/flux_store.hpp"
#include "pychron/processing/report.hpp"
#include "pychron/processing/store_source.hpp"

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
    "usage: elctl flux fit <irradiation> [<level>] --db <url>\n"
    "                      [--model plane|bowl|weighted-mean|matching|nearest|bracketing|ls1d|mean1d|bracketing1d]\n"
    "                      [--weighted | --unweighted] [--mean arithmetic|weighted]\n"
    "                      [--mean-error sem|msem|sd] [--fit-error sem|msem|sd]\n"
    "                      [--neighbors N] [--interpolation weighted|average|linear] [--axis x|y] [--degree 1..4]\n"
    "                      [--monitors NAME] [--sample NAME] [--all-positions | --monitor-positions]\n"
    "                      [--omit RECORD_ID]... [--include RECORD_ID]... [--reset-omits]\n"
    "                      [--exclude-position HOLE]... [--no-save-position HOLE]...\n"
    "                      [--csv FILE] [--save] [--user NAME]\n"
    "       elctl flux show <irradiation> <level> --db <url>\n"
    "       elctl flux history <irradiation> <level> [<hole>] --db <url>\n"
    "       elctl flux monitors [list | show NAME | set FILE | default NAME] --db <url> [--user NAME]\n";

constexpr const char* kUsageText =
    "usage: elctl flux fit <irradiation> [<level>] --db <url> [options]\n"
    "\n"
    "Fits the J of an irradiation level from its flux monitors and prints the monitor\n"
    "and the unknown positions. Without a level, every level of the irradiation.\n"
    "Each option given replaces one field of the options of the level's saved fit\n"
    "(else of the defaults: plane, arithmetic mean with msem).\n"
    "\n"
    "Model:\n"
    "  --model plane|bowl|weighted-mean|matching|nearest|bracketing|ls1d|mean1d|bracketing1d\n"
    "  --weighted | --unweighted  least-squares models\n"
    "  --mean arithmetic|weighted --mean-error sem|msem|sd   a position's mean J\n"
    "  --fit-error sem|msem|sd    error of the predicted J: of the mean models' mean, and sem or\n"
    "                             msem of a fitted surface (plane, bowl, ls1d; not sd)\n"
    "  --neighbors N              nearest\n"
    "  --interpolation weighted|average|linear   bracketing\n"
    "  --axis x|y                 the 1D models\n"
    "  --degree 1..4              ls1d\n"
    "Monitors:\n"
    "  --monitors NAME            the monitor set (default: the saved fit's, else the store's)\n"
    "  --sample NAME              the monitor sample (default: the saved fit's under its own\n"
    "                             monitor set, else the set's); undoes a saved --all-positions\n"
    "  --all-positions            every position that has analyses is a monitor\n"
    "  --monitor-positions        the monitor sample's positions (the default, unless the\n"
    "                             saved fit used --all-positions)\n"
    "Edits (a level only):\n"
    "  --omit RECORD_ID  --include RECORD_ID   leave an analysis out of, or back in, its mean\n"
    "  --exclude-position HOLE    a monitor position stays out of the fit\n"
    "  --no-save-position HOLE    do not save that position\n"
    "  --reset-omits              ignore the omissions and exclusions of the saved fit\n"
    "Output:\n"
    "  --csv FILE                 every position, one row each; written at the end, and only\n"
    "                             when a level was fitted\n"
    "  --save [--user NAME]       save the fit; nothing is written without it\n"
    "\n"
    "elctl flux show <irradiation> <level> --db <url>\n"
    "  The saved J of every position of a level, with its model and who saved it.\n"
    "elctl flux history <irradiation> <level> [<hole>] --db <url>\n"
    "  The saves of a level, newest first, one line per changeset with the holes it\n"
    "  touched; with a hole, one line per revision of that position with its J.\n"
    "elctl flux monitors [list | show NAME | set FILE | default NAME] --db <url> [--user NAME]\n"
    "  The lab's monitor sets: list them (the default is marked *), print one as JSON,\n"
    "  replace them all from a JSON file, or choose the default.\n"
    "\n"
    "Exit codes: 0 done; 1 a level could not be fitted, a save conflicted, or the name\n"
    "asked for does not exist; 2 usage or a fatal error.\n";

struct Args : FluxStoreArgs {
  std::string irradiation, level, csv;
  std::optional<r::ModelKind> model;
  std::optional<bool> weighted;
  std::optional<r::MeanKind> mean;
  std::optional<r::MeanErrorKind> mean_error, fit_error;
  std::optional<int> neighbors, degree;
  std::optional<r::Interpolation> interpolation;
  std::optional<r::Axis> axis;
  pp::MonitorSelection selection;
  pp::Edits edits;
  std::set<int> no_save;
  bool save = false;
};

int usage(Io io, const std::string& message) { return flux_usage(io, message, kShortUsage); }
int fatal(Io io, const std::string& message) { return flux_error(io, message); }

std::optional<int> parse_int(const std::string& text) {
  int value = 0;
  const char* end = text.data() + text.size();
  const auto [stop, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || stop != end) return std::nullopt;
  return value;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

constexpr const char* kSdOfSurface = "sd is not an error kind of a fitted surface";

Result<Args> parse(const std::vector<std::string>& args) {
  Args a;
  std::vector<std::string> positional;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    if (flag.rfind("--", 0) != 0) {
      positional.push_back(flag);
      continue;
    }
    if (flag == "--weighted" || flag == "--unweighted") {
      if (a.weighted && *a.weighted != (flag == "--weighted"))
        return fail(ErrorKind::Config, "--weighted and --unweighted exclude each other");
      a.weighted = flag == "--weighted";
      continue;
    }
    if (flag == "--all-positions" || flag == "--monitor-positions") {
      if (a.selection.all_positions && *a.selection.all_positions != (flag == "--all-positions"))
        return fail(ErrorKind::Config, "--all-positions and --monitor-positions exclude each other");
      a.selection.all_positions = flag == "--all-positions";
      continue;
    }
    if (flag == "--reset-omits") {
      a.edits.reset_omits = true;
      continue;
    }
    if (flag == "--save") {
      a.save = true;
      continue;
    }
    auto store_flag = flux_store_flag(args, i, a);
    if (!store_flag) return fail(store_flag.error());
    if (*store_flag) continue;
    auto given = flux_flag_value(args, i);
    if (!given) return fail(given.error());
    const std::string& value = *given;
    auto bad = [&](const std::string& what) { return fail(ErrorKind::Config, flag + " is " + what + "; got '" + value + "'"); };
    if (flag == "--model") {
      a.model = pp::parse_model_kind(value);
      if (!a.model) return bad("plane, bowl, weighted-mean, matching, nearest, bracketing, ls1d, mean1d or bracketing1d");
    } else if (flag == "--mean") {
      a.mean = r::parse_mean_kind(value);
      if (!a.mean) return bad("arithmetic or weighted");
    } else if (flag == "--mean-error" || flag == "--fit-error") {
      const auto kind = r::parse_mean_error_kind(value);
      if (!kind) return bad("sem, msem or sd");
      (flag == "--mean-error" ? a.mean_error : a.fit_error) = kind;
    } else if (flag == "--neighbors") {
      a.neighbors = parse_int(value);
      if (!a.neighbors || *a.neighbors < 1) return bad("a whole positive number");
    } else if (flag == "--degree") {
      a.degree = parse_int(value);
      if (!a.degree || *a.degree < 1 || *a.degree > 4) return bad("1 to 4");
    } else if (flag == "--interpolation") {
      const std::string v = lower(value);
      if (v == "weighted") a.interpolation = r::Interpolation::WeightedMean;
      else if (v == "average") a.interpolation = r::Interpolation::Average;
      else if (v == "linear") a.interpolation = r::Interpolation::Linear;
      else return bad("weighted, average or linear");
    } else if (flag == "--axis") {
      const std::string v = lower(value);
      if (v == "x") a.axis = r::Axis::X;
      else if (v == "y") a.axis = r::Axis::Y;
      else return bad("x or y");
    } else if (flag == "--monitors") {
      a.selection.monitor_set = value;
    } else if (flag == "--sample") {
      a.selection.sample = value;
    } else if (flag == "--omit") {
      a.edits.omit.insert(value);
    } else if (flag == "--include") {
      a.edits.include.insert(value);
    } else if (flag == "--exclude-position" || flag == "--no-save-position") {
      const auto hole = parse_int(value);
      if (!hole) return bad("a hole number");
      (flag == "--exclude-position" ? a.edits.exclude_positions : a.no_save).insert(*hole);
    } else if (flag == "--csv") {
      a.csv = value;
    } else {
      return fail(ErrorKind::Config, "unknown flag '" + flag + "'");
    }
  }
  if (positional.empty()) return fail(ErrorKind::Config, "an irradiation is required");
  if (positional.size() > 2) return fail(ErrorKind::Config, "unexpected argument '" + positional[2] + "'");
  a.irradiation = positional[0];
  if (positional.size() > 1) a.level = positional[1];
  if (a.db.empty()) return fail(ErrorKind::Config, "--db <url> is required");
  if (a.level.empty()) {
    for (const auto& [flag, given] : {std::pair<const char*, bool>{"--omit", !a.edits.omit.empty()},
                                      {"--include", !a.edits.include.empty()},
                                      {"--exclude-position", !a.edits.exclude_positions.empty()},
                                      {"--no-save-position", !a.no_save.empty()}})
      if (given) return fail(ErrorKind::Config, std::string(flag) + " needs a level");
  }
  // An explicit surface with SD for its error: the saved fit's model is not known yet.
  if (a.fit_error == r::MeanErrorKind::Sd && a.model && r::is_least_squares(*a.model))
    return fail(ErrorKind::Config, std::string("--fit-error sd: ") + kSdOfSurface);
  return a;
}

// ---- printing ---------------------------------------------------------------

using Row = std::vector<std::string>;

// The rows, left aligned, columns two spaces apart (the head row's cells as given).
void print_table(std::ostringstream& out, const Row& head, const std::vector<Row>& rows) {
  std::vector<std::size_t> width(head.size());
  for (std::size_t c = 0; c < head.size(); ++c) width[c] = head[c].size();
  for (const auto& row : rows)
    for (std::size_t c = 0; c < row.size(); ++c) width[c] = std::max(width[c], row[c].size());
  const auto line = [&](const Row& row) {
    std::string text;
    for (std::size_t c = 0; c < row.size(); ++c) {
      text += row[c];
      if (c + 1 < row.size()) text += std::string(width[c] - row[c].size() + 2, ' ');
    }
    out << text << '\n';
  };
  line(head);
  for (const auto& row : rows) line(row);
}

}  // namespace

std::string flux_table(const std::vector<std::string>& head, const std::vector<std::vector<std::string>>& rows) {
  std::ostringstream out;
  print_table(out, head, rows);
  return out.str();
}

std::string format_flux_fit(const pp::LevelFit& fit, const std::vector<std::string>& warnings) {
  std::ostringstream out;
  const auto& m = fit.monitor_set;
  char age[96];
  std::snprintf(age, sizeof age, "%g +/- %g Ma, lambda_k %.3e", m.age_ma, m.age_err_ma, m.lambda_k().value);
  out << fit.irradiation << ' ' << fit.level << "   holder " << (fit.holder.empty() ? "-" : fit.holder)
      << "   monitors " << m.name << ": " << age << '\n';
  out << "model " << pp::flux_model_line(fit.options) << "\n\n";

  std::vector<Row> monitors, unknowns;
  for (const auto& p : fit.positions) {
    const std::string hole = std::to_string(p.hole);
    if (p.monitor) {
      monitors.push_back({hole, p.identifier, p.sample, std::to_string(p.n), pp::flux_j_text(p.saved_j), pp::flux_j_text(p.saved_j_err),
                          pp::flux_j_text(p.mean_j), pp::flux_j_text(p.mean_j_err), pp::flux_percent_of(p.mean_j_err, p.mean_j),
                          pp::flux_pct_text(p.mean_j_mswd), pp::flux_j_text(p.j), pp::flux_j_text(p.j_err), pp::flux_percent_of(p.j_err, p.j),
                          pp::flux_pct_text(p.dev_percent), p.used_in_fit ? "yes" : "no"});
    } else {
      unknowns.push_back({hole, p.identifier, p.sample, pp::flux_j_text(p.saved_j), pp::flux_j_text(p.saved_j_err), pp::flux_j_text(p.j),
                          pp::flux_j_text(p.j_err), pp::flux_percent_of(p.j_err, p.j), pp::flux_pct_text(p.dev_percent)});
    }
  }
  out << "Monitors\n";
  print_table(out, {"hole", "identifier", "sample", "n", "saved J", "+/-", "mean J", "+/-", "%", "MSWD", "pred J", "+/-",
                    "%", "dev %", "fit"},
              monitors);
  out << "\nUnknowns\n";
  print_table(out, {"hole", "identifier", "sample", "saved J", "+/-", "pred J", "+/-", "%", "dev %"}, unknowns);
  out << '\n' << pp::flux_summary(fit) << '\n';
  for (const auto& w : warnings) out << "warning: " << w << '\n';
  return out.str();
}

std::string format_flux_save(const pp::FluxSaveOutcome& outcome, std::string_view saved_by) {
  std::ostringstream out;
  if (outcome.conflict) {
    out << "not saved: " << outcome.conflict_position << " was saved by "
        << (saved_by.empty() ? "someone else" : std::string(saved_by));
    if (outcome.conflict->actual_by) out << " at " << outcome.conflict->actual_by->created.iso();
    out << " since this fit was loaded\n";
  } else if (outcome.written == 0) {
    out << "nothing to save: " << outcome.unchanged << " positions unchanged";
    if (outcome.skipped > 0) out << ", " << outcome.skipped << " not saved";
    out << '\n';
  } else {
    out << "saved " << outcome.written << " positions (" << outcome.unchanged << " unchanged)";
    if (outcome.skipped > 0) out << ", " << outcome.skipped << " not saved";
    out << '\n';
  }
  return out.str();
}

namespace {

std::string software() {
#ifdef PYCHRON_ELCTL_VERSION
  return std::string("pychron-cpp ") + PYCHRON_ELCTL_VERSION;
#else
  return "pychron-cpp";
#endif
}

// The options of a level: its saved fit's, else the defaults, each flag given replacing its field.
pp::FluxOptions resolve(const Args& a, const pp::LevelInputs& in) {
  pp::FluxOptions o = in.saved_options.value_or(pp::FluxOptions{});
  if (a.model) o.fit.kind = *a.model;
  if (a.weighted) o.fit.weighted = *a.weighted;
  if (a.mean) o.mean = *a.mean;
  if (a.mean_error) o.mean_error = *a.mean_error;
  if (a.fit_error) o.fit.error = *a.fit_error;
  if (a.neighbors) o.fit.n_neighbors = *a.neighbors;
  if (a.degree) o.fit.degree = *a.degree;
  if (a.interpolation) o.fit.interpolation = *a.interpolation;
  if (a.axis) o.fit.axis = *a.axis;
  return o;
}

// Who moved the head a save conflicted on; empty when that cannot be read.
std::string author_of(ps::IStore& store, const pp::LevelFit& fit, const pp::FluxSaveOutcome& outcome) {
  auto info = pp::flux_head_info(store, fit.irradiation, fit.level, outcome.conflict_hole);
  return info ? info->saved_by : std::string();
}

struct Session {
  Io io;
  const Args& a;
  ps::IStore& store;
  pp::StoreSource& source;
  std::optional<ps::Actor> actor;
  std::string csv_rows;
  int code = kOk;
  bool any_fitted = false;

  // One level: load, fit, print, save. A level that fails is named on stderr.
  void level(const std::string& name) {
    const std::string where = a.irradiation + " " + name;
    auto fail_level = [&](const Error& e) {
      io.err << "elctl flux: " << where << ": " << e.what << '\n';
      code = kFailed;
    };
    auto loaded = pp::load_level(source, store, a.irradiation, name, a.selection);
    if (!loaded) return fail_level(loaded.error());
    const pp::FluxOptions options = resolve(a, *loaded);
    auto fitted = pp::fit_level(*loaded, options, a.edits);
    if (!fitted) return fail_level(fitted.error());
    std::set<int> holes;
    for (const auto& p : fitted->positions) holes.insert(p.hole);
    for (const int hole : a.no_save)
      if (!holes.contains(hole)) {
        std::string list;
        for (const int h : holes) list += (list.empty() ? "" : ", ") + std::to_string(h);
        Error e;
        e.kind = ErrorKind::Config;
        e.what = "--no-save-position: hole " + std::to_string(hole) + " is not a position of " + where + " (holes: " + list + ")";
        return fail_level(e);
      }
    // The shared warnings, then the ones that depend on the edits given here.
    std::vector<std::string> warnings = pp::flux_warnings(
        *loaded, *fitted, pp::FluxWarningContext{!a.selection.monitor_set.empty(), a.fit_error.has_value()});
    // R10: any position of the level may be named; one that is no monitor has nothing to exclude.
    for (const auto& p : fitted->positions)
      if (!p.monitor && a.edits.exclude_positions.contains(p.hole))
        warnings.push_back("hole " + std::to_string(p.hole) + " is not a monitor position: excluding it changes nothing");
    any_fitted = true;
    io.out << format_flux_fit(*fitted, warnings);
    csv_rows += pp::flux_csv_rows(*fitted);
    if (!a.save) return;
    auto saved = pp::save_level(store, *actor, *fitted, pp::SaveSelection{a.no_save}, software());
    if (!saved) return fail_level(saved.error());
    io.out << format_flux_save(*saved, saved->conflict ? author_of(store, *fitted, *saved) : std::string());
    if (saved->conflict) code = kFailed;
  }
};

}  // namespace

Result<std::string> flux_flag_value(const std::vector<std::string>& args, std::size_t& i) {
  const std::string& flag = args[i];
  if (i + 1 >= args.size()) return fail(ErrorKind::Config, flag + " needs a value");
  const std::string& value = args[++i];
  if (value.rfind("--", 0) == 0) return fail(ErrorKind::Config, flag + " needs a value; got the flag '" + value + "'");
  return value;
}

int flux_usage(Io io, const std::string& message, std::string_view short_usage) {
  io.err << "elctl flux: " << message << '\n' << short_usage;
  return kUsage;
}

int flux_error(Io io, const std::string& message, int code) {
  io.err << "elctl flux: " << message << '\n';
  return code;
}

Result<bool> flux_store_flag(const std::vector<std::string>& args, std::size_t& i, FluxStoreArgs& into) {
  const std::string& flag = args[i];
  if (flag != "--db" && flag != "--user") return false;
  auto value = flux_flag_value(args, i);
  if (!value) return fail(value.error());
  (flag == "--db" ? into.db : into.user) = *value;
  return true;
}

Result<std::unique_ptr<ps::IStore>> open_flux_store(const std::string& db) {
  // A command reads: a mistyped SQLite path is an error, not a new empty store.
  constexpr std::string_view kSqlite = "sqlite:";
  if (db.starts_with(kSqlite) && db != "sqlite::memory:") {
    std::error_code code;
    const fs::path file(db.substr(kSqlite.size()));
    if (!fs::is_regular_file(file, code)) return fail(ErrorKind::Config, "no database at " + file.string());
    if (fs::file_size(file, code) == 0 && !code)
      return fail(ErrorKind::Config, file.string() + " is empty: not a pychron store");
  }
  return ps::open_store(ps::StoreConfig{db, false});
}

Result<ps::Actor> flux_actor(ps::IStore& store, const std::string& user_name) {
  const std::string host = pychron::env_var("HOSTNAME").value_or("localhost");
  auto client = store.register_client({host, "reduction", std::nullopt, "elctl"});
  if (!client) return fail(client.error());
  const std::string user = user_name.empty() ? pychron::env_var("USER").value_or("pychron") : user_name;
  auto u = store.ensure_user(*client, user);
  if (!u) return fail(u.error());
  return ps::Actor{*u, *client};
}

namespace {

// The --csv destination (R19). It is written only by commit(), when the run
// has something to write: until then it is as it was, so a run that fits
// nothing leaves an earlier file alone. open() only finds out, before
// anything is saved, that the directory can be written, by making the
// sibling temporary file that commit() fills and renames over the
// destination. The temporary is removed on every other way out.
class CsvFile {
 public:
  CsvFile() = default;
  CsvFile(const CsvFile&) = delete;
  CsvFile& operator=(const CsvFile&) = delete;
  ~CsvFile() { discard(); }

  Result<void> open(const std::string& destination) {
    destination_ = fs::path(destination);
    std::error_code code;
    fs::path directory = destination_.parent_path();
    if (directory.empty()) directory = ".";
    if (destination_.filename().empty() || fs::is_directory(destination_, code) || !fs::is_directory(directory, code))
      return could_not_write();
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, ".%08x.tmp", static_cast<unsigned>(std::random_device{}()));
    temporary_ = directory / ("." + destination_.filename().string() + suffix);
    std::ofstream probe(temporary_, std::ios::binary | std::ios::trunc);
    if (!probe) {
      temporary_.clear();
      return could_not_write();
    }
    return {};
  }

  Result<void> commit(const std::string& text) {
    std::ofstream out(temporary_, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    std::error_code code;
    if (out) fs::rename(temporary_, destination_, code);
    if (!out || code) {
      discard();
      return could_not_write();
    }
    temporary_.clear();
    return {};
  }

 private:
  pychron::Unexpected<Error> could_not_write() const { return fail(ErrorKind::Io, "could not write " + destination_.string()); }
  void discard() {
    if (temporary_.empty()) return;
    std::error_code code;
    fs::remove(temporary_, code);
    temporary_.clear();
  }

  fs::path destination_, temporary_;
};

int run(const Args& a, Io io) {
  auto store = open_flux_store(a.db);
  if (!store) return fatal(io, store.error().what);
  auto source = pp::StoreSource::open(ps::StoreConfig{a.db, false}, pp::StoreSourceOptions{1, "", ""});
  if (!source) return fatal(io, source.error().what);

  // Before anything is saved: a CSV that cannot be written saves nothing.
  CsvFile csv;
  if (!a.csv.empty())
    if (auto opened = csv.open(a.csv); !opened) return fatal(io, opened.error().what);

  Session s{io, a, **store, **source, std::nullopt, {}, kOk, false};
  if (a.save) {
    auto actor = flux_actor(**store, a.user);
    if (!actor) return fatal(io, actor.error().what);
    s.actor = *actor;
  }

  std::vector<std::string> levels;
  if (!a.level.empty()) {
    levels.push_back(a.level);
  } else {
    auto irradiations = (*store)->irradiations();
    if (!irradiations) return fatal(io, irradiations.error().what);
    std::string known;
    const ps::IrradiationRow* found = nullptr;
    for (const auto& row : *irradiations) {
      known += (known.empty() ? "" : ", ") + row.name;
      if (row.name == a.irradiation) found = &row;
    }
    if (!found) return fatal(io, "no irradiation " + a.irradiation + " (irradiations: " + known + ")");
    auto rows = (*store)->levels(found->uuid);
    if (!rows) return fatal(io, rows.error().what);
    for (const auto& row : *rows) levels.push_back(row.name);
    if (levels.empty()) return fatal(io, a.irradiation + " has no levels");
  }
  for (const auto& name : levels) {
    if (&name != &levels.front()) io.out << '\n';
    s.level(name);
  }

  // Only what was fitted is written: no level, no file (and an earlier one stays).
  if (!a.csv.empty() && s.any_fitted) {
    if (auto written = csv.commit(pp::flux_csv_header() + s.csv_rows); !written) return fatal(io, written.error().what);
    pychron::mark_as_user_file(a.csv);
    io.out << "wrote " << a.csv << '\n';
  }
  return s.code;
}

}  // namespace

int flux_command(const std::vector<std::string>& args, Io io) {
  if (args.empty() || args[0] == "help" || args[0] == "--help") {
    (args.empty() ? io.err : io.out) << kUsageText;
    return args.empty() ? kUsage : kOk;
  }
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (args[0] != "fit" && args[0] != "show" && args[0] != "history" && args[0] != "monitors")
    return usage(io, "unknown subcommand '" + args[0] + "'");
  if (!rest.empty() && (rest[0] == "help" || rest[0] == "--help")) {
    io.out << kUsageText;
    return kOk;
  }
  if (args[0] != "fit") {
    try {
      return flux_admin_command(args[0], rest, io);
    } catch (const std::exception& e) {
      return fatal(io, e.what());
    }
  }
  auto parsed = parse(rest);
  if (!parsed) return usage(io, parsed.error().what);
  // No exception leaves a library, but std::filesystem and iostreams can throw.
  try {
    return run(*parsed, io);
  } catch (const std::exception& e) {
    return fatal(io, e.what());
  }
}

}  // namespace elctl
