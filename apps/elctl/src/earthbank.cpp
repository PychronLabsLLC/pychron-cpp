// elctl earthbank export-xlsx: select analyses from the store as `export`
// does, build one submission per sample (pychron::earthbank mapping.hpp) and
// write them into the service's import workbooks (xlsx.hpp).

#include "earthbank.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/mapping.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/earthbank/xlsx.hpp"
#include "pychron/processing/model.hpp"
#include "selection.hpp"

namespace elctl {

namespace fs = std::filesystem;
namespace eb = pychron::earthbank;
namespace pp = pychron::processing;
using pychron::Error;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

namespace {

constexpr const char* kShortUsage =
    "usage: elctl earthbank export-xlsx --db <url> --out <dir> [--sample S]... [--identifier I]...\n"
    "                    [--project P]... [--irradiation R]... [--type T]... [--uuid U]...\n"
    "                    [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--include-invalid] [--limit N]\n"
    "                    [--per-sample] [--no-samples] [--check] [--templates DIR]\n"
    "                    [--overrides FILE.toml] [--constants default|legacy|legacy_preferences]\n"
    "                    [--decay-error] [--plateau fleck|mahon] [--plateau-steps N] [--plateau-gas PCT]\n";

constexpr const char* kUsageText =
    "usage: elctl earthbank export-xlsx --db <url> --out <dir> [selection] [options]\n"
    "\n"
    "Writes the 40Ar/39Ar results of the selected analyses, one data point per\n"
    "sample, into EarthBank's own import workbooks (AuScope's AusGeochem data\n"
    "platform): an Ar-Ar workbook (data points, measurements, aliquots, the age\n"
    "reported and how it was calculated) and a Sample workbook. The workbooks are\n"
    "imported by hand on the service's website; this command contacts nothing.\n"
    "\n"
    "Selection (repeat a flag for several values; every flag narrows), as export:\n"
    "  --sample <name>            --identifier <labnumber>\n"
    "  --project <name>           --irradiation <name>\n"
    "  --type <analysis type>     default: unknown\n"
    "  --uuid <analysis uuid>     explicit analyses; the other filters are then ignored\n"
    "  --from <YYYY-MM-DD>        --to <YYYY-MM-DD>     UTC, inclusive\n"
    "  --include-invalid          keep analyses tagged invalid\n"
    "  --limit <n>                at most n analyses (default 5000)\n"
    "\n"
    "Ages, as export (the ages written are those export reports):\n"
    "  --constants default|legacy|legacy_preferences\n"
    "  --decay-error              propagate the decay-constant uncertainty into the ages\n"
    "  --plateau fleck|mahon      plateau criterion (default fleck)\n"
    "  --plateau-steps <n>        minimum plateau steps (default 3)\n"
    "  --plateau-gas <percent>    minimum 39ArK in the plateau (default 50)\n"
    "\n"
    "Options:\n"
    "  --per-sample               one Ar-Ar workbook per sample, not one for all\n"
    "  --no-samples               no Sample workbook\n"
    "  --check                    say what the export would say and write nothing\n"
    "                             (--out is then not needed)\n"
    "  --templates <dir>          the service's two template workbooks (default: the\n"
    "                             installed ones, or $PYCHRON_EARTHBANK_TEMPLATES)\n"
    "  --overrides <file.toml>    what the analyses cannot say. Every key is optional;\n"
    "                             an unknown key, or a sample the selection does not\n"
    "                             hold, is an error:\n"
    "\n"
    "      setup = \"...\"                 # the lab's analytical set-up, by its name on EarthBank\n"
    "      decay_constant = \"...\"        # the service's name for it, or a citation\n"
    "      air_ratio = \"...\"             # the same, for the atmospheric 40Ar/36Ar\n"
    "      age_includes_j_error = true   # false: the reported age's uncertainty is analytical\n"
    "\n"
    "      [samples.\"<sample name>\"]\n"
    "      interpretation = \"...\"        # the service's name for what the age dates\n"
    "      preferred_age = \"plateau\"     # plateau | weighted_mean | isochron | integrated\n"
    "      flux_monitor = \"...\"          # replaces the monitor the analyses name\n"
    "      datapoint_name = \"...\"        # default: \"<sample> <material>\"\n"
    "      irradiation = \"...\"           # the irradiation batch, by its name on EarthBank\n"
    "\n"
    "Prints one line per sample (its analyses and the age reported) with its notes\n"
    "under it, then the files written, then the notes of the export. A sample that\n"
    "cannot be published stops the export: nothing is written for any sample.\n"
    "\n"
    "Exit codes: 0 wrote the files (--check: there was something to write);\n"
    "1 no analysis matched; 2 usage or fatal error.\n";

// What is published: the store, the analyses, how their ages are made and
// what the lab adds. Every subcommand that builds submissions has these.
struct Source {
  std::string db, overrides;
  Selection selection;
  AgeFlags ages;
};

struct Args {
  Source source;
  std::string out, templates;
  bool per_sample = false;
  bool samples = true;
  bool check = false;
};

int usage(Io io, const std::string& message) {
  io.err << "elctl earthbank: " << message << '\n' << kShortUsage;
  return kUsage;
}

// The library says `earthbank: ...`; under this command's name that is said once.
int fatal(Io io, std::string_view message) {
  constexpr std::string_view kLibrary = "earthbank: ";
  if (message.starts_with(kLibrary)) message.remove_prefix(kLibrary.size());
  io.err << "elctl earthbank: " << message << '\n';
  return kUsage;
}

int fatal(Io io, const Error& error) { return fatal(io, error.what); }

Result<Args> parse(const std::vector<std::string>& args) {
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    // The flags shared with `export` (selection.hpp) first.
    auto shared = take_selection_flag(a.source.selection, args, i);
    if (shared && !*shared) shared = take_age_flag(a.source.ages, args, i);
    if (!shared) return fail(shared.error());
    if (*shared) continue;
    if (flag == "--per-sample") {
      a.per_sample = true;
      continue;
    }
    if (flag == "--no-samples") {
      a.samples = false;
      continue;
    }
    if (flag == "--check") {
      a.check = true;
      continue;
    }
    if (flag.rfind("--", 0) != 0) return fail(ErrorKind::Config, "unexpected argument '" + flag + "'");
    if (i + 1 >= args.size()) return fail(ErrorKind::Config, flag + " needs a value");
    const std::string& value = args[++i];
    if (flag == "--db") {
      a.source.db = value;
    } else if (flag == "--out") {
      a.out = value;
    } else if (flag == "--templates") {
      a.templates = value;
    } else if (flag == "--overrides") {
      a.source.overrides = value;
    } else {
      return fail(ErrorKind::Config, "unknown flag '" + flag + "'");
    }
  }
  if (a.source.db.empty()) return fail(ErrorKind::Config, "--db <url> is required");
  if (a.out.empty() && !a.check) return fail(ErrorKind::Config, "--out <dir> is required");
  if (auto ok = check_selection(a.source.selection); !ok) return fail(ok.error());
  return a;
}

// ---------------------------------------------------------------- overrides

std::optional<eb::PreferredAge> parse_preferred_age(std::string_view word) {
  if (word == "plateau") return eb::AgeKind::Plateau;
  if (word == "weighted_mean") return eb::AgeKind::WeightedMean;
  if (word == "isochron") return eb::AgeKind::Isochron;
  if (word == "integrated") return eb::AgeKind::Integrated;
  return std::nullopt;
}

// The overrides file into `options`: the keys earthbank.hpp lists, and no
// other. The first thing wrong is the error, with the file and the key's
// whole path.
Result<void> read_overrides(const std::string& file, eb::PublishOptions& options) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, file + ": cannot be read");
  std::ostringstream content;
  content << in.rdbuf();
  auto parsed = toml::parse(content.str(), file);
  if (!parsed) {
    std::ostringstream where;
    where << parsed.error().source().begin;
    return fail(ErrorKind::Config, file + ": " + std::string(parsed.error().description()) + " (" + where.str() + ")");
  }

  std::optional<std::string> bad;
  const auto problem = [&](const std::string& what) {
    if (!bad) bad = file + ": " + what;
  };
  const auto text = [&](const toml::node& node, const std::string& key, std::string& into) {
    const auto value = node.value<std::string>();
    if (!node.is_string() || !value) return problem("'" + key + "' takes text");
    into = *value;
  };

  for (auto&& [key, node] : parsed.table()) {
    const std::string k(key.str());
    if (k == "setup") {
      text(node, k, options.setup);
    } else if (k == "decay_constant") {
      text(node, k, options.decay_constant);
    } else if (k == "air_ratio") {
      text(node, k, options.air_ratio);
    } else if (k == "age_includes_j_error") {
      const auto value = node.value<bool>();
      if (node.is_boolean() && value) {
        options.age_includes_j_error = *value;
      } else {
        problem("'" + k + "' takes true or false");
      }
    } else if (k == "samples") {
      const auto* samples = node.as_table();
      if (samples == nullptr) {
        problem("'samples' takes a table of samples, [samples.\"<sample name>\"]");
        continue;
      }
      for (auto&& [name, entry] : *samples) {
        const std::string sample(name.str());
        const std::string where = "samples.\"" + sample + "\"";
        const auto* settings = entry.as_table();
        if (settings == nullptr) {
          problem("'" + where + "' takes a table of that sample's settings");
          continue;
        }
        eb::SampleOptions& so = options.samples[sample];
        for (auto&& [setting, value] : *settings) {
          const std::string s(setting.str());
          const std::string path = where + "." + s;
          if (s == "interpretation") {
            text(value, path, so.interpretation);
          } else if (s == "flux_monitor") {
            text(value, path, so.flux_monitor);
          } else if (s == "datapoint_name") {
            text(value, path, so.datapoint_name);
          } else if (s == "irradiation") {
            text(value, path, so.irradiation);
          } else if (s == "preferred_age") {
            std::string word;
            text(value, path, word);
            so.preferred_age = parse_preferred_age(word);
            if (!so.preferred_age)
              problem("'" + path + "' is plateau, weighted_mean, isochron or integrated; got '" + word + "'");
          } else {
            problem("unknown key '" + path + "'");
          }
        }
      }
    } else {
      problem("unknown key '" + k + "'");
    }
  }
  if (bad) return fail(ErrorKind::Config, *bad);
  return {};
}

// A sample the overrides file names and the selection does not hold: a
// mistyped name would otherwise change nothing, in silence.
Result<void> check_override_samples(const std::string& file, const eb::PublishOptions& options,
                                    const std::vector<pp::AnalysisPtr>& analyses) {
  std::set<std::string> selected;
  for (const auto& a : analyses) selected.insert(a->sample);
  for (const auto& [sample, unused] : options.samples) {
    if (selected.contains(sample)) continue;
    std::string names;
    for (const auto& s : selected) names += (names.empty() ? "" : ", ") + s;
    return fail(ErrorKind::Config,
                file + ": sample \"" + sample + "\" is not among the selected analyses (" + names + ")");
  }
  return {};
}

// ---------------------------------------------------------------- report

std::string number(double value, int digits) {
  char text[32];
  std::snprintf(text, sizeof text, "%.*g", digits, value);
  return text;
}

// "<sample>: 2 analyses, Plateau Age 28.1234 ± 0.0412 Ma (1 sigma)", and the
// submission's notes under it.
void print_sample(std::ostream& out, const eb::Submission& s) {
  const std::size_t n = s.measurements.size();
  out << s.sample.name.value_or(s.datapoint_name) << ": " << n << (n == 1 ? " analysis, " : " analyses, ");
  const auto& age = s.age_summary;
  if (age.age) {
    out << age.ageCalcTypeName.value_or("age") << ' ' << number(*age.age, 6);
    if (age.ageUncertainty) out << " ± " << number(*age.ageUncertainty, 3);
    out << " Ma (1 sigma)";
  } else {
    out << "no age";
  }
  out << '\n';
  for (const auto& note : s.notes) out << "  " << note << '\n';
}

// The submissions of what `source` selects, one per sample; or, when there
// are none to have, the exit code, the reason having been said on io.err:
// kFailed when no analysis matched, kUsage for anything wrong (the overrides
// file, the store, a sample the mapping refuses). One sample that cannot be
// published stops them all. The start of every subcommand that publishes.
struct Built {
  int code = kOk;
  std::vector<eb::Submission> submissions;
};

Built build(const Source& source, Io io) {
  eb::PublishOptions options;
  options.reduction = source.ages.reduction();
  options.ages.plateau = source.ages.criteria();
#ifdef PYCHRON_ELCTL_VERSION
  options.software = std::string("pychron-cpp ") + PYCHRON_ELCTL_VERSION;
#endif
  if (!source.overrides.empty()) {
    if (auto read = read_overrides(source.overrides, options); !read) return {fatal(io, read.error()), {}};
  }

  std::vector<std::string> warnings;
  const auto analyses = load_selection(source.selection, source.db, warnings);
  if (!analyses) return {fatal(io, analyses.error()), {}};
  for (const auto& w : warnings) io.err << "warning: " << w << '\n';
  if (analyses->empty()) {
    io.err << "elctl earthbank: no analysis matched\n";
    return {kFailed, {}};
  }
  if (auto named = check_override_samples(source.overrides, options, *analyses); !named)
    return {fatal(io, named.error()), {}};

  auto submissions = eb::build_submissions(*analyses, options);
  if (!submissions) return {fatal(io, submissions.error()), {}};
  return {kOk, std::move(*submissions)};
}

int export_xlsx(const Args& a, Io io) {
  const Built built = build(a.source, io);
  if (built.code != kOk) return built.code;

  eb::XlsxOptions xlsx;
  xlsx.template_dir = a.templates;
  xlsx.per_sample = a.per_sample;
  xlsx.samples = a.samples;
  eb::XlsxResult written;
  if (a.check) {
    auto notes = eb::check_against_template(built.submissions, xlsx);
    if (!notes) return fatal(io, notes.error());
    written.notes = std::move(*notes);
  } else {
    auto result = eb::export_xlsx(built.submissions, fs::path(a.out), xlsx);
    if (!result) return fatal(io, result.error());
    written = std::move(*result);
  }

  for (const auto& s : built.submissions) print_sample(io.out, s);
  for (const auto& file : written.files) io.out << "wrote " << file.string() << '\n';
  if (a.check) io.out << "nothing written (--check)\n";
  for (const auto& note : written.notes) io.out << "note: " << note << '\n';
  return kOk;
}

bool is_help(const std::string& word) { return word == "help" || word == "--help"; }

}  // namespace

int earthbank_command(const std::vector<std::string>& args, Io io) {
  if (args.empty()) return usage(io, "a subcommand is required");
  if (is_help(args[0]) || (args.size() > 1 && is_help(args[1]))) {
    io.out << kUsageText;
    return kOk;
  }
  if (args[0] != "export-xlsx") return usage(io, "unknown subcommand '" + args[0] + "'");
  auto parsed = parse({args.begin() + 1, args.end()});
  if (!parsed) return usage(io, parsed.error().what);
  // No exception leaves a library, but std::filesystem can throw: a fatal
  // error like any other.
  try {
    return export_xlsx(*parsed, io);
  } catch (const std::exception& e) {
    return fatal(io, e.what());
  }
}

}  // namespace elctl
