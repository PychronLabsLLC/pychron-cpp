#pragma once

// What the files of `elctl import` share (import.cpp dispatches; one file per
// larger subcommand). Not included outside them.

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cli.hpp"
#include "pychron/core/error.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/ingest/verify.hpp"
#include "pychron/ingest/writer.hpp"
#include "pychron/persistence/store.hpp"

namespace elctl::import_detail {

namespace fs = std::filesystem;
namespace P = pychron::persistence;
using pychron::Error;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

// ---------------------------------------------------------------- arguments

struct FlagSpec {
  std::vector<std::string_view> with_value, switches;
};

struct Flags {
  std::map<std::string, std::string, std::less<>> values;
  std::set<std::string, std::less<>> on;

  std::optional<std::string> get(std::string_view name) const;
  bool has(std::string_view name) const { return on.contains(name); }
};

// Every argument must be a flag `spec` names; anything else is an error.
Result<Flags> parse_flags(const std::vector<std::string>& args, const FlagSpec& spec);
// A whole positive number.
Result<int> positive_int(std::string_view flag, const std::string& text);

// ---------------------------------------------------------------- one source's settings

// What `add` was told about a source and the store does not keep, in
// <cache>/<source uuid>.toml:
//
//   kind = "project_repo"            # legacy_db | meta_repo | project_repo
//   name = "IR1010"                  # shown; a project repo's repository name
//   url = "https://github.com/NMGRLData/IR1010"   # the --source given to add
//   branch = "master"                # repositories only
//   path = "/home/me/.cache/pychron/import/mirrors/IR1010-1a2b3c4d"
//                                    # what is read: the repository, the
//                                    # mirror of a url, or the catalog directory
//   mirror = true                    # `path` is a mirror; `run` fetches it first
//   tz = "America/Denver"
//   catalog_from_repos = false       # project repos
//   reference_runs = false           # project repos: imported before the others
//
//   [author_map]                     # git email -> user name
//   "ann@example.org" = "ann"
struct SourceSettings {
  P::Uuid uuid;
  P::ImportSourceKind kind = P::ImportSourceKind::ProjectRepo;
  std::string name, url, branch;
  fs::path path;
  bool mirror = false;
  std::string tz;
  bool catalog_from_repos = false, reference_runs = false;
  std::map<std::string, std::string> author_map;
};

fs::path settings_file(const fs::path& cache, P::Uuid uuid);
Result<void> save_settings(const fs::path& cache, const SourceSettings& settings);
// nullopt: the cache has no file for this source.
Result<std::optional<SourceSettings>> load_settings(const fs::path& cache, P::Uuid uuid);
// `--author-map`: a TOML file whose top-level keys are git emails and whose
// values are user names.
Result<std::map<std::string, std::string>> read_author_map(const fs::path& file);

// <user cache dir>/pychron/import: %LOCALAPPDATA%, ~/Library/Caches,
// $XDG_CACHE_HOME or ~/.cache; the system temp directory when none is known.
fs::path default_cache_dir();
// True for "scheme://..." and scp-like "user@host:path"; a local path otherwise.
bool is_url(std::string_view source);
// The last component of a normalized source url.
std::string last_segment(std::string_view normalized_url);

// ---------------------------------------------------------------- the command's context

struct Context {
  Io io;
  fs::path cache;
  std::unique_ptr<P::IStore> store;

  fs::path mirrors() const { return cache / "mirrors"; }
  fs::path scratch() const { return cache / "scratch"; }
};

// A registered source: the store's row and, when the cache has them, its settings.
struct Source {
  P::ImportSourceInfo info;
  std::optional<SourceSettings> settings;
  std::string name;  // settings->name, else the last component of the url
};

// Every registered source in the order `run --all` imports them: the catalog,
// meta repositories, project repositories of reference runs, then the other
// project repositories; by url within each.
Result<std::vector<Source>> registered_sources(Context& ctx);
// `which`: a uuid, a name or a url; nullopt: all of them.
Result<std::vector<Source>> select_sources(Context& ctx, const std::optional<std::string>& which);

// The client every import command writes as: {hostname, "importer"}.
Result<P::Uuid> importer_client(P::IStore& store);
pychron::ingest::WriterConfig writer_config(const SourceSettings& settings);

struct OpenedAdapter {
  std::unique_ptr<pychron::ingest::ISourceAdapter> adapter;
  std::vector<std::string> warnings;  // for the operator; the import goes on
};

// The adapter of a source as its import configured it. `all` supplies the
// tag lookup of a project repository: the dump of every registered legacy_db
// source. `batch`: commits (rows, for a catalog) per batch; nullopt: the
// adapter's default. `fetch`: update the mirror of a remote source first.
Result<OpenedAdapter> open_adapter(Context& ctx, const SourceSettings& settings, const std::vector<Source>& all,
                                   std::optional<int> batch, bool fetch);

// A pending conflict that only annotates a row that was imported (spec
// 10.26): it does not fail verify, and does not make `run` exit 1.
bool is_warning(const P::ImportConflictRow& row);
// "1 blocking (unparseable 1), 2 warnings (identity_clash 2)".
struct PendingConflicts {
  std::map<std::string, int> blocking, warnings;  // kind -> rows
  int blocking_total = 0, warnings_total = 0;
};
Result<PendingConflicts> pending_conflicts(P::IStore& store, P::Uuid source);
std::string describe(const PendingConflicts& pending, bool with_kinds);

// The text of an error on one line.
std::string one_line(std::string text);
// "elctl import: <message>", exit 2.
int fatal(Io io, const std::string& message);
int fatal(Io io, const Error& error);

// ---------------------------------------------------------------- subcommands

int import_add(Context& ctx, const Flags& flags);
int import_run(Context& ctx, const Flags& flags);
int import_verify(Context& ctx, const Flags& flags);

// The age of an analysis as of an interpreted age of this project repository
// (spec 10.30 and 10.32), for ingest::verify. `store` outlives the function.
Result<pychron::ingest::AgeFn> make_age_fn(P::IStore& store, const SourceSettings& settings, const fs::path& scratch);

}  // namespace elctl::import_detail
