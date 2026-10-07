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
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/user_file.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/flux_store.hpp"
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
    "                      [--monitors NAME] [--sample NAME] [--all-positions]\n"
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
    "  --fit-error sem|msem|sd    error of the mean models' prediction (not sd for a surface)\n"
    "  --neighbors N              nearest\n"
    "  --interpolation weighted|average|linear   bracketing\n"
    "  --axis x|y                 the 1D models\n"
    "  --degree 1..4              ls1d\n"
    "Monitors:\n"
    "  --monitors NAME            the monitor set (default: the saved fit's, else the store's)\n"
    "  --sample NAME              the monitor sample, when it is not the set's\n"
    "  --all-positions            every position that has analyses is a monitor\n"
    "Edits (a level only):\n"
    "  --omit RECORD_ID  --include RECORD_ID   leave an analysis out of, or back in, its mean\n"
    "  --exclude-position HOLE    a monitor position stays out of the fit\n"
    "  --no-save-position HOLE    do not save that position\n"
    "  --reset-omits              ignore the omissions and exclusions of the saved fit\n"
    "Output:\n"
    "  --csv FILE                 every position, one row each\n"
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

struct Args {
  std::string db, irradiation, level, csv, user;
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

int usage(Io io, const std::string& message) {
  io.err << "elctl flux: " << message << '\n' << kShortUsage;
  return kUsage;
}

int fatal(Io io, const std::string& message) {
  io.err << "elctl flux: " << message << '\n';
  return kUsage;
}

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

std::string_view model_cli_name(r::ModelKind kind) {
  switch (kind) {
    case r::ModelKind::Plane: return "plane";
    case r::ModelKind::Bowl: return "bowl";
    case r::ModelKind::WeightedMean: return "weighted-mean";
    case r::ModelKind::Matching: return "matching";
    case r::ModelKind::NearestNeighbors: return "nearest";
    case r::ModelKind::Bracketing: return "bracketing";
    case r::ModelKind::LeastSquares1D: return "ls1d";
    case r::ModelKind::WeightedMean1D: return "mean1d";
    case r::ModelKind::Bracketing1D: return "bracketing1d";
  }
  return "plane";
}

std::string_view interpolation_name(r::Interpolation i) {
  switch (i) {
    case r::Interpolation::WeightedMean: return "weighted";
    case r::Interpolation::Average: return "average";
    case r::Interpolation::Linear: return "linear";
  }
  return "weighted";
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
    if (flag == "--all-positions") {
      a.selection.all_positions = true;
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
    if (i + 1 >= args.size()) return fail(ErrorKind::Config, flag + " needs a value");
    const std::string& value = args[++i];
    if (value.rfind("--", 0) == 0) return fail(ErrorKind::Config, flag + " needs a value; got the flag '" + value + "'");
    auto bad = [&](const std::string& what) { return fail(ErrorKind::Config, flag + " is " + what + "; got '" + value + "'"); };
    if (flag == "--db") {
      a.db = value;
    } else if (flag == "--model") {
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
    } else if (flag == "--user") {
      a.user = value;
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

std::string fixed(double v, const char* format) {
  if (!std::isfinite(v)) return "-";
  char buf[64];
  std::snprintf(buf, sizeof buf, format, v);
  return buf;
}

std::string j_text(double v) { return fixed(v, "%.4e"); }
std::string j_text(const std::optional<double>& v) { return v ? j_text(*v) : "-"; }
std::string pct_text(double v) { return fixed(v, "%.2f"); }
std::string pct_text(const std::optional<double>& v) { return v ? pct_text(*v) : "-"; }
std::string percent_of(double err, double value) { return value != 0.0 ? pct_text(err / value * 100.0) : "-"; }
std::string percent_of(const std::optional<double>& err, const std::optional<double>& value) {
  return err && value ? percent_of(*err, *value) : "-";
}

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

std::string model_line(const pp::FluxOptions& o) {
  std::ostringstream s;
  s << "model " << model_cli_name(o.fit.kind);
  switch (o.fit.kind) {
    case r::ModelKind::Plane:
    case r::ModelKind::Bowl: s << (o.fit.weighted ? ", weighted" : ", unweighted"); break;
    case r::ModelKind::NearestNeighbors: s << ", " << o.fit.n_neighbors << " neighbors"; break;
    case r::ModelKind::Bracketing: s << ", " << interpolation_name(o.fit.interpolation); break;
    case r::ModelKind::LeastSquares1D:
      s << ", " << (o.fit.weighted ? "weighted" : "unweighted") << ", degree " << o.fit.degree << ", axis "
        << (o.fit.axis == r::Axis::X ? 'x' : 'y');
      break;
    case r::ModelKind::WeightedMean1D: s << ", axis " << (o.fit.axis == r::Axis::X ? 'x' : 'y'); break;
    case r::ModelKind::Bracketing1D:
      s << ", " << interpolation_name(o.fit.interpolation) << ", axis " << (o.fit.axis == r::Axis::X ? 'x' : 'y');
      break;
    case r::ModelKind::WeightedMean:
    case r::ModelKind::Matching: break;
  }
  s << "; mean " << r::to_string(o.mean) << " (" << r::to_string(o.mean_error) << "); fit error "
    << r::to_string(o.fit.error);
  return s.str();
}

std::vector<std::string> warnings_of(const pp::LevelFit& fit) {
  std::vector<std::string> out;
  const auto hole = [](const pp::FittedPosition& p) { return "hole " + std::to_string(p.hole); };
  for (const auto& p : fit.positions) {
    for (const auto& id : p.rejected) out.push_back("analysis " + id + " of " + hole(p) + " was rejected: it gives no J");
    for (const auto note : p.notes) {
      switch (note) {
        case pp::PositionNote::AnalysisNotReduced:
          out.push_back("an analysis of " + hole(p) + " could not be reduced and was left out");
          break;
        case pp::PositionNote::NoUsableAnalysis: out.push_back(hole(p) + " has no usable analysis"); break;
        case pp::PositionNote::LeftOutOfFit: out.push_back(hole(p) + " left out of the fit"); break;
        case pp::PositionNote::MeanMswdOutsideLimits:
          out.push_back(hole(p) + ": mean MSWD " + pct_text(p.mean_j_mswd) + " is outside its limits");
          break;
        case pp::PositionNote::Extrapolated: out.push_back(hole(p) + " is extrapolated (outside the monitors)"); break;
        case pp::PositionNote::AnalysisRejected: break;  // named above, by record id
      }
    }
  }
  if (fit.mswd_outside_limits) out.push_back("fit MSWD " + pct_text(fit.mswd) + " is outside its limits");
  return out;
}

std::string notes_text(const pp::FittedPosition& p) {
  std::string text;
  const auto add = [&](const std::string& s) { text += (text.empty() ? "" : "; ") + s; };
  for (const auto& id : p.rejected) add("rejected " + id);
  for (const auto note : p.notes) {
    switch (note) {
      case pp::PositionNote::Extrapolated: add("extrapolated"); break;
      case pp::PositionNote::MeanMswdOutsideLimits: add("mean MSWD outside limits"); break;
      case pp::PositionNote::NoUsableAnalysis: add("no usable analysis"); break;
      case pp::PositionNote::LeftOutOfFit: add("left out of fit"); break;
      case pp::PositionNote::AnalysisNotReduced: add("analysis not reduced"); break;
      case pp::PositionNote::AnalysisRejected: break;
    }
  }
  return text;
}

// Seventeen significant digits: a double reads back as it was.
std::string number17(double v) {
  if (!std::isfinite(v)) return "";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}
std::string number17(const std::optional<double>& v) { return v ? number17(*v) : ""; }

}  // namespace

std::string flux_j_text(const std::optional<double>& v) { return j_text(v); }
std::string flux_percent_of(const std::optional<double>& err, const std::optional<double>& value) {
  return percent_of(err, value);
}
std::string flux_table(const std::vector<std::string>& head, const std::vector<std::vector<std::string>>& rows) {
  std::ostringstream out;
  print_table(out, head, rows);
  return out.str();
}

std::string format_flux_fit(const pp::LevelFit& fit, const std::vector<std::string>& extra_warnings) {
  std::ostringstream out;
  const auto& m = fit.monitor_set;
  char age[96];
  std::snprintf(age, sizeof age, "%g +/- %g Ma, lambda_k %.3e", m.age_ma, m.age_err_ma, m.lambda_k().value);
  out << fit.irradiation << ' ' << fit.level << "   holder " << (fit.holder.empty() ? "-" : fit.holder)
      << "   monitors " << m.name << ": " << age << '\n';
  out << model_line(fit.options) << "\n\n";

  std::vector<Row> monitors, unknowns;
  for (const auto& p : fit.positions) {
    const std::string hole = std::to_string(p.hole);
    if (p.monitor) {
      monitors.push_back({hole, p.identifier, p.sample, std::to_string(p.n), j_text(p.saved_j), j_text(p.saved_j_err),
                          j_text(p.mean_j), j_text(p.mean_j_err), percent_of(p.mean_j_err, p.mean_j),
                          pct_text(p.mean_j_mswd), j_text(p.j), j_text(p.j_err), percent_of(p.j_err, p.j),
                          pct_text(p.dev_percent), p.used_in_fit ? "yes" : "no"});
    } else {
      unknowns.push_back({hole, p.identifier, p.sample, j_text(p.saved_j), j_text(p.saved_j_err), j_text(p.j),
                          j_text(p.j_err), percent_of(p.j_err, p.j), pct_text(p.dev_percent)});
    }
  }
  out << "Monitors\n";
  print_table(out, {"hole", "identifier", "sample", "n", "saved J", "+/-", "mean J", "+/-", "%", "MSWD", "pred J", "+/-",
                    "%", "dev %", "fit"},
              monitors);
  out << "\nUnknowns\n";
  print_table(out, {"hole", "identifier", "sample", "saved J", "+/-", "pred J", "+/-", "%", "dev %"}, unknowns);
  out << "\nfit MSWD " << pct_text(fit.mswd) << " (" << fit.dof << " dof)   J min " << j_text(fit.min_j) << "  max "
      << j_text(fit.max_j) << "  delta " << pct_text(fit.delta_j_percent) << " %\n";
  for (const auto& w : warnings_of(fit)) out << "warning: " << w << '\n';
  for (const auto& w : extra_warnings) out << "warning: " << w << '\n';
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

std::string csv_field(std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(text);
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + '"';
}

std::string flux_csv_header() {
  return "kind,irradiation,level,hole,identifier,sample,x,y,n,saved_j,saved_j_err,mean_j,mean_j_err,mean_j_mswd,j,j_err,"
         "dev_percent,used_in_fit,notes\r\n";
}

std::string flux_csv_rows(const pp::LevelFit& fit) {
  std::string out;
  for (const auto& p : fit.positions) {
    const std::vector<std::string> cells = {p.monitor ? "monitor" : "unknown",
                                            fit.irradiation,
                                            fit.level,
                                            std::to_string(p.hole),
                                            p.identifier,
                                            p.sample,
                                            number17(p.x),
                                            number17(p.y),
                                            p.monitor ? std::to_string(p.n) : "",
                                            number17(p.saved_j),
                                            number17(p.saved_j_err),
                                            number17(p.mean_j),
                                            number17(p.mean_j_err),
                                            number17(p.mean_j_mswd),
                                            number17(p.j),
                                            number17(p.j_err),
                                            number17(p.dev_percent),
                                            p.monitor ? (p.used_in_fit ? "yes" : "no") : "",
                                            notes_text(p)};
    for (std::size_t i = 0; i < cells.size(); ++i) out += (i ? "," : "") + csv_field(cells[i]);
    out += "\r\n";
  }
  return out;
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

// The author of the revision that moved a head, from its history.
std::string author_of(ps::IStore& store, const ps::Conflict& conflict) {
  if (!conflict.actual) return {};
  auto history = store.history(conflict.subject, conflict.kind);
  if (!history) return {};
  for (const auto& revision : *history)
    if (revision.uuid == *conflict.actual) return revision.author_name;
  return {};
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
    std::vector<std::string> warnings;
    if (loaded->saved_options && loaded->saved_sd_replaced && !a.fit_error && r::is_least_squares(options.fit.kind))
      warnings.push_back("saved fit used SD, which a fitted surface does not have: using msem");
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
    any_fitted = true;
    io.out << format_flux_fit(*fitted, warnings);
    csv_rows += flux_csv_rows(*fitted);
    if (!a.save) return;
    auto saved = pp::save_level(store, *actor, *fitted, pp::SaveSelection{a.no_save}, software());
    if (!saved) return fail_level(saved.error());
    io.out << format_flux_save(*saved, saved->conflict ? author_of(store, *saved->conflict) : std::string());
    if (saved->conflict) code = kFailed;
  }
};

}  // namespace

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

int run(const Args& a, Io io) {
  auto store = open_flux_store(a.db);
  if (!store) return fatal(io, store.error().what);
  auto source = pp::StoreSource::open(ps::StoreConfig{a.db, false}, pp::StoreSourceOptions{1, "", ""});
  if (!source) return fatal(io, source.error().what);

  // The CSV's destination is opened before anything is saved: an unwritable path writes nothing.
  std::ofstream csv;
  if (!a.csv.empty()) {
    csv.open(a.csv, std::ios::binary | std::ios::trunc);
    if (!csv) return fatal(io, "could not write " + a.csv);
  }

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

  if (!a.csv.empty()) {
    csv << flux_csv_header() << s.csv_rows;
    csv.close();
    if (!csv) return fatal(io, "could not write " + a.csv);
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
