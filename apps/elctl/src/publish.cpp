// elctl publish: releases of a lab's reduced data (data publishing design,
// section 6.2), over libs/publish.

#include "publish.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <string_view>

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/host_name.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/manifest.hpp"
#include "pychron/publish/release.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "selection.hpp"

namespace elctl {

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
namespace pb = pychron::publish;
using pychron::Error;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

namespace {

constexpr const char* kUsageText =
    "usage: elctl publish <command> [options]\n"
    "\n"
    "A release is an immutable, citable directory of a selection's reduced data:\n"
    "GeoParquet tables, an OGC API - Records catalogue, the Schaen et al. (2021)\n"
    "report, a manifest of every file's hash and of the revisions pinned. Nothing\n"
    "is public by default.\n"
    "\n"
    "  create  --db <url> [selection] [--tag <name>]... [--ia <name>|--no-ia] --name <slug>\n"
    "          --title <text> --author \"<name>[|orcid|affiliation]\"... [--licence <spdx>]\n"
    "          [--description <text>] [--publisher <name>] [--embargo-until YYYY-MM-DD]\n"
    "          [--supersedes <name|name@vN|uuid>] [reduction and report options]\n"
    "                              pin the selection's heads and references as a new draft\n"
    "  build   --db <url> --release <r> --out <dir> [--private] [--with-figures] [--with-viewer]\n"
    "                              write <dir>/<name>-v<N>; --private keeps embargoed rows\n"
    "  verify  <dir> [--db <url>]  check every file against the manifest; with --db, against a rebuild\n"
    "  publish --db <url> --release <r> --manifest <dir>/manifest.json [--public]\n"
    "                              draft -> published, recording that build; private unless --public\n"
    "  withdraw --db <url> --release <r> --reason <text>\n"
    "  list    --db <url> [--state draft|published|withdrawn]\n"
    "  show    --db <url> --release <r>\n"
    "  delete  --db <url> --release <r>   a draft only\n"
    "  doi | igsn | upload         not yet (phase 7)\n"
    "\n"
    "<r> is a release name (its newest version), name@vN, or a uuid.\n"
    "Selection (repeat a flag for several values; every flag narrows):\n"
    "  --sample <name>  --identifier <labnumber>  --project <name>  --irradiation <name>\n"
    "  --type <analysis type> (default unknown)  --uuid <analysis uuid>  --from/--to <YYYY-MM-DD>\n"
    "  --include-invalid  --limit <n>\n"
    "Reduction and report options, as elctl export: --constants, --decay-error, --plateau,\n"
    "  --plateau-steps, --plateau-gas, --sigma 1|2, --group-by, --lab, --note.\n"
    "\n"
    "Exit codes: 0 done; 1 nothing matched, or verify found a difference; 2 usage or fatal error.\n";

int usage(Io io, const std::string& message) {
  io.err << "elctl publish: " << message << "\nelctl publish help lists the commands\n";
  return kUsage;
}

int fatal(Io io, const std::string& message) {
  io.err << "elctl publish: " << message << '\n';
  return kUsage;
}

int fatal(Io io, const Error& error) { return fatal(io, error.what); }

// The flags every subcommand with a store shares.
struct Common {
  std::string db, release;
};

// Takes `--flag value` at args[i]; true when taken.
bool take(const std::vector<std::string>& args, std::size_t& i, const char* flag, std::string& into, std::optional<Error>& bad) {
  if (args[i] != flag) return false;
  if (i + 1 >= args.size()) {
    bad = Error{.kind = ErrorKind::Config, .what = std::string(flag) + " needs a value", .device = {}, .code = {}};
    return true;
  }
  into = args[++i];
  return true;
}

std::string software_version() {
#ifdef PYCHRON_ELCTL_VERSION
  return std::string("pychron-cpp/") + PYCHRON_ELCTL_VERSION;
#else
  return "pychron-cpp";
#endif
}

struct Opened {
  std::unique_ptr<ps::IStore> store;
  ps::Actor actor;
};

// The store, and this program as a publisher client with the user at the keyboard.
Result<Opened> open_store_as_publisher(const std::string& url) {
  auto store = ps::open_store(ps::StoreConfig{url, false});
  if (!store) return fail(store.error());
  auto client = (*store)->register_client({pychron::host_name(), "publisher", std::nullopt, software_version()});
  if (!client) return fail(client.error());
  const std::string user = pychron::env_var("USER").value_or(pychron::env_var("USERNAME").value_or("pychron"));
  auto user_id = (*store)->ensure_user(*client, user.empty() ? "pychron" : user);
  if (!user_id) return fail(user_id.error());
  return Opened{std::move(*store), ps::Actor{*user_id, *client}};
}

Result<pb::Author> parse_author(const std::string& text) {
  pb::Author a;
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == '|') {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  if (parts.empty() || parts[0].empty()) return fail(ErrorKind::Config, "--author needs a name");
  a.name = parts[0];
  if (parts.size() > 1 && !parts[1].empty()) a.orcid = parts[1];
  if (parts.size() > 2 && !parts[2].empty()) a.affiliation = parts[2];
  return a;
}

// A date "YYYY-MM-DD" as midnight UTC of the day after it (the embargo is
// inclusive of the date).
Result<ps::UtcTime> day_after(const std::string& date) {
  auto t = ps::UtcTime::parse(date + "T00:00:00Z");
  if (!t) return fail(ErrorKind::Config, "'" + date + "' is not a date (YYYY-MM-DD)");
  return ps::UtcTime{t->micros + 86'400LL * 1'000'000LL};
}

constexpr std::array<std::string_view, 3> kShippedLicences{"CC-BY-4.0", "CC-BY-SA-4.0", "CC0-1.0"};

// ---------------------------------------------------------------- create

int create(const std::vector<std::string>& args, Io io) {
  Selection selection;
  AgeFlags ages;
  pb::ReleaseSpec spec;
  pb::ReleaseOptions options;
  std::string db, embargo_until, sigma, group_by;
  bool no_ia = false;
  std::vector<std::string> ias;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    auto shared = take_selection_flag(selection, args, i);
    if (shared && !*shared) shared = take_age_flag(ages, args, i);
    if (!shared) return usage(io, shared.error().what);
    if (*shared) continue;
    const std::string& flag = args[i];
    std::string value;
    if (flag == "--no-ia") {
      no_ia = true;
      continue;
    }
    if (take(args, i, "--db", db, bad)) continue;
    if (take(args, i, "--name", spec.name, bad)) continue;
    if (take(args, i, "--title", spec.title, bad)) continue;
    if (take(args, i, "--licence", spec.licence, bad)) continue;
    if (take(args, i, "--description", value, bad)) {
      spec.description = value;
      continue;
    }
    if (take(args, i, "--publisher", value, bad)) {
      spec.publisher = value;
      continue;
    }
    if (take(args, i, "--embargo-until", embargo_until, bad)) continue;
    if (take(args, i, "--supersedes", value, bad)) {
      spec.supersedes = value;
      continue;
    }
    if (take(args, i, "--author", value, bad)) {
      auto a = parse_author(value);
      if (!a) return usage(io, a.error().what);
      spec.authors.push_back(*a);
      continue;
    }
    if (take(args, i, "--tag", value, bad)) {
      spec.tags.push_back(value);
      continue;
    }
    if (take(args, i, "--ia", value, bad)) {
      ias.push_back(value);
      continue;
    }
    if (take(args, i, "--sigma", sigma, bad)) continue;
    if (take(args, i, "--group-by", group_by, bad)) continue;
    if (take(args, i, "--lab", options.laboratory, bad)) continue;
    if (take(args, i, "--note", value, bad)) {
      options.notes.push_back(value);
      continue;
    }
    return usage(io, "unknown flag '" + flag + "'");
  }
  if (bad) return usage(io, bad->what);
  if (db.empty()) return usage(io, "--db <url> is required");
  if (spec.name.empty()) return usage(io, "--name <slug> is required");
  if (spec.title.empty()) return usage(io, "--title <text> is required");
  if (auto ok = check_selection(selection); !ok) return usage(io, ok.error().what);
  if (!sigma.empty()) {
    const auto n = parse_int(sigma);
    if (!n || (*n != 1 && *n != 2)) return usage(io, "--sigma is 1 or 2");
    options.sigma = *n;
  }
  if (!group_by.empty()) options.group_by = group_by;
  options.constants = ages.constants;
  options.decay_error = ages.decay_error;
  options.plateau = ages.plateau;
  options.plateau_steps = ages.plateau_steps;
  options.plateau_gas = ages.plateau_gas;
  spec.options_json = pb::release_options_json(options);
  if (auto parsed = pb::parse_release_options(spec.options_json); !parsed) return usage(io, parsed.error().what);
  if (!embargo_until.empty()) {
    auto until = day_after(embargo_until);
    if (!until) return usage(io, until.error().what);
    spec.embargo_until = *until;
  }
  if (no_ia) spec.interpreted_ages = std::vector<std::string>{};
  else if (!ias.empty()) spec.interpreted_ages = ias;
  spec.reduction_version = std::string(pychron::reduction::kReductionVersion);
  spec.software_version = software_version();
  if (std::find(kShippedLicences.begin(), kShippedLicences.end(), spec.licence) == kShippedLicences.end())
    io.err << "warning: no text is shipped for licence " << spec.licence << "; LICENSE.txt will name it only\n";

  std::vector<std::string> warnings;
  auto loaded = load_selection(selection, db, warnings);
  if (!loaded) return fatal(io, loaded.error());
  for (const auto& w : warnings) io.err << "warning: " << w << '\n';
  if (loaded->empty()) {
    io.err << "elctl publish: no analysis matched\n";
    return kFailed;
  }
  for (const auto& a : *loaded) spec.analyses.push_back(a->uuid);

  auto opened = open_store_as_publisher(db);
  if (!opened) return fatal(io, opened.error());
  auto created = pb::create_release(*opened->store, opened->actor, spec);
  if (!created) return fatal(io, created.error());
  io.out << "release " << created->release.str() << ": " << spec.name << " version " << created->version << "; members "
         << created->members << ", samples " << created->samples << ", interpreted ages " << created->interpreted_ages
         << "; embargoed samples " << created->embargoed_samples << ", analyses " << created->embargoed_analyses
         << "\n";
  return kOk;
}

// ---------------------------------------------------------------- build

class Progress final : public pb::IProgress {
 public:
  explicit Progress(const Io& io) : io_(io) {}
  void stage(std::string_view name) override { io_.err << "  " << name << '\n'; }

 private:
  Io io_;
};

int build(const std::vector<std::string>& args, Io io) {
  Common c;
  pb::BuildOptions options;
  std::string out;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", c.db, bad)) continue;
    if (take(args, i, "--release", c.release, bad)) continue;
    if (take(args, i, "--out", out, bad)) continue;
    if (args[i] == "--private") {
      options.private_build = true;
      continue;
    }
    if (args[i] == "--with-figures") {
      options.with_figures = true;
      continue;
    }
    if (args[i] == "--with-viewer") {
      options.with_viewer = true;
      continue;
    }
    return usage(io, "unknown flag '" + args[i] + "'");
  }
  if (bad) return usage(io, bad->what);
  if (c.db.empty()) return usage(io, "--db <url> is required");
  if (c.release.empty()) return usage(io, "--release <name|uuid> is required");
  if (out.empty()) return usage(io, "--out <dir> is required");
  options.out = out;
  options.store_url = c.db;
  auto opened = open_store_as_publisher(c.db);
  if (!opened) return fatal(io, opened.error());
  auto row = pb::find_release(*opened->store, c.release);
  if (!row) return fatal(io, row.error());
  auto source = open_source(c.db);
  if (!source) return fatal(io, source.error());
  Progress progress(io);
  auto built = pb::build_release(*opened->store, **source, row->uuid, options, &progress);
  if (!built) return fatal(io, built.error());
  for (const auto& w : built->warnings) io.err << "warning: " << w << '\n';
  io.out << "built " << built->dir.string() << ": " << built->manifest.pins.analyses.size() << " analyses, "
         << built->manifest.pins.samples.size() << " samples, " << built->manifest.files.size() << " files"
         << (built->parquet_written ? "" : " (no Parquet tables)") << '\n';
  return kOk;
}

// ---------------------------------------------------------------- verify

int verify(const std::vector<std::string>& args, Io io) {
  std::string dir, db;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", db, bad)) continue;
    if (args[i].starts_with("--")) return usage(io, "unknown flag '" + args[i] + "'");
    if (!dir.empty()) return usage(io, "one directory");
    dir = args[i];
  }
  if (bad) return usage(io, bad->what);
  if (dir.empty()) return usage(io, "verify <dir> [--db <url>]");
  auto files = pb::verify_files(dir);
  if (!files) return fatal(io, files.error());
  bool ok = true;
  const auto report = [&](const pb::VerifyReport& r, const char* against) {
    for (const auto& p : r.missing) io.err << against << ": missing " << p << '\n';
    for (const auto& p : r.changed) io.err << against << ": changed " << p << '\n';
    for (const auto& p : r.extra) io.err << against << ": not in the manifest " << p << '\n';
    if (r.reduction_version_differs) io.err << against << ": " << *r.reduction_version_differs << '\n';
    if (!r.ok()) ok = false;
  };
  report(*files, "files");
  if (!db.empty()) {
    auto manifest = pb::read_manifest(dir);
    if (!manifest) return fatal(io, manifest.error());
    auto opened = open_store_as_publisher(db);
    if (!opened) return fatal(io, opened.error());
    auto source = open_source(db);
    if (!source) return fatal(io, source.error());
    const fs::path scratch =
        fs::temp_directory_path() / ("pychron_verify_" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    pb::BuildOptions options;
    options.out = scratch;
    options.private_build = manifest->build.private_build;
    options.store_url = db;
    auto rebuilt = pb::build_release(*opened->store, **source, manifest->release.uuid, options, nullptr);
    if (!rebuilt) {
      fs::remove_all(scratch, ec);
      return fatal(io, rebuilt.error());
    }
    auto against = pb::verify_rebuild(dir, rebuilt->manifest);
    fs::remove_all(scratch, ec);
    if (!against) return fatal(io, against.error());
    report(*against, "rebuild");
  }
  if (!ok) return kFailed;
  io.out << "ok: " << files->missing.size() + files->changed.size() + files->extra.size() << " differences in " << dir
         << (db.empty() ? "" : ", and the rebuild agrees") << '\n';
  return kOk;
}

// ---------------------------------------------------------------- publish, withdraw, delete

int publish(const std::vector<std::string>& args, Io io) {
  Common c;
  std::string manifest_path;
  bool make_public = false;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", c.db, bad)) continue;
    if (take(args, i, "--release", c.release, bad)) continue;
    if (take(args, i, "--manifest", manifest_path, bad)) continue;
    if (args[i] == "--public") {
      make_public = true;
      continue;
    }
    return usage(io, "unknown flag '" + args[i] + "'");
  }
  if (bad) return usage(io, bad->what);
  if (c.db.empty()) return usage(io, "--db <url> is required");
  if (c.release.empty()) return usage(io, "--release <name|uuid> is required");
  if (manifest_path.empty()) return usage(io, "--manifest <dir>/manifest.json is required");
  std::ifstream in(manifest_path, std::ios::binary);
  if (!in) return fatal(io, "cannot read " + manifest_path);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string bytes = ss.str();
  auto manifest = pb::parse_manifest(bytes);
  if (!manifest) return fatal(io, manifest.error());
  auto opened = open_store_as_publisher(c.db);
  if (!opened) return fatal(io, opened.error());
  auto row = pb::find_release(*opened->store, c.release);
  if (!row) return fatal(io, row.error());
  if (manifest->release.uuid != row->uuid || manifest->pins.bookmark != row->bookmark)
    return fatal(io, "the manifest's pins are not this release's (it was built from " + manifest->release.name + " v" +
                         std::to_string(manifest->release.version) + ")");
  auto pins = opened->store->bookmark_heads(row->bookmark);
  if (!pins) return fatal(io, pins.error());
  for (const auto& a : manifest->pins.analyses)
    for (const auto& [kind, revision] : a.heads) {
      const bool pinned = std::any_of(pins->begin(), pins->end(), [&](const ps::HeadInfo& h) {
        return h.subject == a.uuid && std::string(ps::to_string(h.kind)) == kind && h.revision.str() == revision;
      });
      if (!pinned) return fatal(io, "the manifest's pins differ from the release's bookmark (" + a.runid + " " + kind + ")");
    }
  auto published = pb::publish_release(*opened->store, opened->actor, row->uuid, pychron::sha256(std::string_view(bytes)),
                                       make_public);
  if (!published) return fatal(io, published.error());
  io.out << "published " << row->name << " v" << row->version << (make_public ? " (public)" : " (private)") << '\n';
  return kOk;
}

int withdraw(const std::vector<std::string>& args, Io io) {
  Common c;
  std::string reason;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", c.db, bad)) continue;
    if (take(args, i, "--release", c.release, bad)) continue;
    if (take(args, i, "--reason", reason, bad)) continue;
    return usage(io, "unknown flag '" + args[i] + "'");
  }
  if (bad) return usage(io, bad->what);
  if (c.db.empty()) return usage(io, "--db <url> is required");
  if (c.release.empty()) return usage(io, "--release <name|uuid> is required");
  if (reason.empty()) return usage(io, "--reason <text> is required");
  auto opened = open_store_as_publisher(c.db);
  if (!opened) return fatal(io, opened.error());
  auto row = pb::find_release(*opened->store, c.release);
  if (!row) return fatal(io, row.error());
  if (auto r = pb::withdraw_release(*opened->store, opened->actor, row->uuid, reason); !r) return fatal(io, r.error());
  io.out << "withdrawn " << row->name << " v" << row->version << '\n';
  return kOk;
}

int remove(const std::vector<std::string>& args, Io io) {
  Common c;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", c.db, bad)) continue;
    if (take(args, i, "--release", c.release, bad)) continue;
    return usage(io, "unknown flag '" + args[i] + "'");
  }
  if (bad) return usage(io, bad->what);
  if (c.db.empty()) return usage(io, "--db <url> is required");
  if (c.release.empty()) return usage(io, "--release <name|uuid> is required");
  auto opened = open_store_as_publisher(c.db);
  if (!opened) return fatal(io, opened.error());
  auto row = pb::find_release(*opened->store, c.release);
  if (!row) return fatal(io, row.error());
  if (auto r = pb::delete_release(*opened->store, opened->actor, row->uuid); !r) return fatal(io, r.error());
  io.out << "deleted draft " << row->name << " v" << row->version << '\n';
  return kOk;
}

// ---------------------------------------------------------------- list, show

int list(const std::vector<std::string>& args, Io io) {
  std::string db, state;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", db, bad)) continue;
    if (take(args, i, "--state", state, bad)) continue;
    return usage(io, "unknown flag '" + args[i] + "'");
  }
  if (bad) return usage(io, bad->what);
  if (db.empty()) return usage(io, "--db <url> is required");
  ps::ReleaseQuery query;
  if (!state.empty()) {
    query.state = ps::parse_release_state(state);
    if (!query.state) return usage(io, "--state is draft, published or withdrawn");
  }
  auto store = ps::open_store(ps::StoreConfig{db, false});
  if (!store) return fatal(io, store.error());
  auto rows = (*store)->releases(query);
  if (!rows) return fatal(io, rows.error());
  for (const auto& r : *rows) {
    io.out << r.name << " v" << r.version << "  " << ps::to_string(r.state) << "  " << ps::to_string(r.visibility)
           << "  " << r.created.iso().substr(0, 10) << "  " << r.title;
    if (r.doi) io.out << "  doi " << *r.doi;
    io.out << '\n';
  }
  if (rows->empty()) io.out << "no releases\n";
  return kOk;
}

int show(const std::vector<std::string>& args, Io io) {
  Common c;
  std::optional<Error> bad;
  for (std::size_t i = 0; i < args.size() && !bad; ++i) {
    if (take(args, i, "--db", c.db, bad)) continue;
    if (take(args, i, "--release", c.release, bad)) continue;
    return usage(io, "unknown flag '" + args[i] + "'");
  }
  if (bad) return usage(io, bad->what);
  if (c.db.empty()) return usage(io, "--db <url> is required");
  if (c.release.empty()) return usage(io, "--release <name|uuid> is required");
  auto store = ps::open_store(ps::StoreConfig{c.db, false});
  if (!store) return fatal(io, store.error());
  auto row = pb::find_release(**store, c.release);
  if (!row) return fatal(io, row.error());
  auto pins = (*store)->bookmark_heads(row->bookmark);
  if (!pins) return fatal(io, pins.error());
  std::size_t members = 0, refs = 0, ias = 0;
  std::vector<ps::Uuid> seen;
  for (const auto& p : *pins) {
    if (p.kind == ps::Kind::RefValue) ++refs;
    else if (p.kind == ps::Kind::InterpretedAge) ++ias;
    else if (std::find(seen.begin(), seen.end(), p.subject) == seen.end()) {
      seen.push_back(p.subject);
      ++members;
    }
  }
  auto samples = (*store)->release_samples(row->uuid);
  if (!samples) return fatal(io, samples.error());
  io.out << "release " << row->uuid.str() << '\n'
         << "name " << row->name << " version " << row->version << '\n'
         << "title " << row->title << '\n'
         << "state " << ps::to_string(row->state) << ", " << ps::to_string(row->visibility) << '\n'
         << "licence " << row->licence << '\n'
         << "authors " << row->authors_json << '\n'
         << "members " << members << ", samples " << samples->size() << ", pinned references " << refs
         << ", interpreted ages " << ias << '\n'
         << "bookmark " << row->bookmark.str() << '\n'
         << "options " << row->options_json << '\n'
         << "software " << row->software_version << ", reduction " << row->reduction_version << '\n'
         << "created " << row->created.iso() << '\n';
  if (row->published) io.out << "published " << row->published->iso() << '\n';
  if (row->withdrawn) io.out << "withdrawn " << row->withdrawn->iso() << ": " << row->withdrawn_reason.value_or("") << '\n';
  if (row->doi) io.out << "doi " << *row->doi << " (" << row->doi_state.value_or("") << ")\n";
  if (row->supersedes) io.out << "supersedes " << row->supersedes->str() << '\n';
  if (row->embargo_until) io.out << "embargoed until " << row->embargo_until->iso() << '\n';
  return kOk;
}

}  // namespace

int publish_command(const std::vector<std::string>& args, Io io) {
  if (args.empty()) return usage(io, "a command is required");
  const std::string& cmd = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (cmd == "help" || cmd == "--help") {
    io.out << kUsageText;
    return kOk;
  }
  try {
    if (cmd == "create") return create(rest, io);
    if (cmd == "build") return build(rest, io);
    if (cmd == "verify") return verify(rest, io);
    if (cmd == "publish") return publish(rest, io);
    if (cmd == "withdraw") return withdraw(rest, io);
    if (cmd == "delete") return remove(rest, io);
    if (cmd == "list") return list(rest, io);
    if (cmd == "show") return show(rest, io);
    if (cmd == "doi" || cmd == "igsn" || cmd == "upload") return fatal(io, cmd + " is not built yet (phase 7)");
  } catch (const std::exception& e) {
    return fatal(io, e.what());
  }
  return usage(io, "unknown command '" + cmd + "'");
}

}  // namespace elctl
