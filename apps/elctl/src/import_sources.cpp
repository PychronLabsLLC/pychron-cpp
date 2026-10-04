// elctl import: the settings kept per source in the cache directory, the
// list of registered sources, and the adapter of each.

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <tuple>

#include <toml++/toml.hpp>

#include "import_impl.hpp"
#include "pychron/core/env.hpp"
#include "pychron/dvc/catalog_adapter.hpp"
#include "pychron/dvc/git_reader.hpp"
#include "pychron/dvc/meta_adapter.hpp"
#include "pychron/dvc/project_adapter.hpp"
#include "pychron/ingest/ids.hpp"

#ifndef PYCHRON_ELCTL_VERSION
#define PYCHRON_ELCTL_VERSION "0"
#endif

namespace elctl::import_detail {

namespace dvc = pychron::dvc;
namespace ingest = pychron::ingest;

namespace {

Result<std::string> read_text(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + utf8(file));
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

Result<toml::table> read_toml(const fs::path& file) {
  auto text = read_text(file);
  if (!text) return fail(text.error());
  auto parsed = toml::parse(*text);
  if (!parsed) return fail(ErrorKind::Config, utf8(file) + ": " + std::string(parsed.error().description()));
  return std::move(parsed).table();
}

// legacy_db, then meta_repo, then project_repo.
int kind_rank(P::ImportSourceKind kind) {
  switch (kind) {
    case P::ImportSourceKind::LegacyDb:
      return 0;
    case P::ImportSourceKind::MetaRepo:
      return 1;
    case P::ImportSourceKind::ProjectRepo:
      return 2;
  }
  return 3;
}

}  // namespace

std::string utf8(const fs::path& path) noexcept {
  try {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
  } catch (...) {
    return "?";
  }
}

fs::path path_from_utf8(std::string_view text) noexcept {
  try {
    return fs::path(std::u8string(text.begin(), text.end()));
  } catch (...) {
    return {};
  }
}

fs::path settings_file(const fs::path& cache, P::Uuid uuid) { return cache / (uuid.str() + ".toml"); }

std::string missing_settings(const Context& ctx, const Source& source) {
  return "no settings for " + source.name + " in " + utf8(ctx.cache) + " (" + source.info.spec.uuid.str() +
         ".toml); give the --cache it was added with, or run elctl import add for it again";
}

Result<void> save_settings(const fs::path& cache, const SourceSettings& s) {
  std::error_code code;
  fs::create_directories(cache, code);
  if (code) return fail(ErrorKind::Io, "cannot create " + utf8(cache) + ": " + code.message());

  toml::table root;
  root.insert("kind", std::string(P::to_string(s.kind)));
  root.insert("name", s.name);
  root.insert("url", s.url);
  if (!s.branch.empty()) root.insert("branch", s.branch);
  root.insert("path", utf8(s.path));
  root.insert("mirror", s.mirror);
  root.insert("tz", s.tz);
  root.insert("catalog_from_repos", s.catalog_from_repos);
  root.insert("reference_runs", s.reference_runs);
  toml::table authors;
  for (const auto& [email, name] : s.author_map) authors.insert(email, name);
  root.insert("author_map", std::move(authors));

  const fs::path file = settings_file(cache, s.uuid);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << "# elctl import: the settings of one source. Written by `elctl import add`.\n" << root << '\n';
  out.flush();
  if (!out) return fail(ErrorKind::Io, "cannot write " + utf8(file));
  return {};
}

Result<std::optional<SourceSettings>> load_settings(const fs::path& cache, P::Uuid uuid) {
  const fs::path file = settings_file(cache, uuid);
  std::error_code code;
  if (!fs::exists(file, code)) return std::optional<SourceSettings>{};
  auto root = read_toml(file);
  if (!root) return fail(root.error());

  const auto text = [&](const char* key) { return (*root)[key].value<std::string>().value_or(""); };
  SourceSettings s;
  s.uuid = uuid;
  const auto kind = P::parse_import_source_kind(text("kind"));
  if (!kind) return fail(ErrorKind::Config, utf8(file) + ": no source kind '" + text("kind") + "'");
  s.kind = *kind;
  s.name = text("name");
  s.url = text("url");
  s.branch = text("branch");
  s.path = path_from_utf8(text("path"));
  s.mirror = (*root)["mirror"].value<bool>().value_or(false);
  s.tz = text("tz");
  s.catalog_from_repos = (*root)["catalog_from_repos"].value<bool>().value_or(false);
  s.reference_runs = (*root)["reference_runs"].value<bool>().value_or(false);
  if (s.url.empty() || s.path.empty() || s.tz.empty())
    return fail(ErrorKind::Config, utf8(file) + ": url, path and tz are required");
  if (const auto* authors = (*root)["author_map"].as_table())
    for (const auto& [email, name] : *authors) {
      const auto value = name.value<std::string>();
      if (!value) return fail(ErrorKind::Config, utf8(file) + ": author_map: " + std::string(email.str()) + " is not a name");
      s.author_map.emplace(std::string(email.str()), *value);
    }
  return std::optional<SourceSettings>{std::move(s)};
}

Result<std::map<std::string, std::string>> read_author_map(const fs::path& file) {
  auto root = read_toml(file);
  if (!root) return fail(root.error());
  std::map<std::string, std::string> out;
  for (const auto& [email, name] : *root) {
    const auto value = name.value<std::string>();
    if (!value)
      return fail(ErrorKind::Config,
                  utf8(file) + ": " + std::string(email.str()) + " must be a user name in quotes");
    out.emplace(std::string(email.str()), *value);
  }
  return out;
}

fs::path default_cache_dir() {
  using pychron::env_var;
  const auto set = [](const char* name) -> std::optional<fs::path> {
    auto value = env_var(name);
    if (!value || value->empty()) return std::nullopt;
    return fs::path(*value);
  };
  std::optional<fs::path> base;
#if defined(_WIN32)
  base = set("LOCALAPPDATA");
#elif defined(__APPLE__)
  if (const auto home = set("HOME")) base = *home / "Library" / "Caches";
#else
  base = set("XDG_CACHE_HOME");
  if (!base)
    if (const auto home = set("HOME")) base = *home / ".cache";
#endif
  if (!base) {
    std::error_code code;
    base = fs::temp_directory_path(code);
    if (code) base = fs::path(".");
  }
  return *base / "pychron" / "import";
}

bool is_url(std::string_view source) {
  if (source.find("://") != std::string_view::npos) return true;
  // scp-like "user@host:path"; a one-letter "host" is a Windows drive.
  const auto colon = source.find(':');
  const auto slash = source.find_first_of("/\\");
  return colon != std::string_view::npos && colon > 1 && (slash == std::string_view::npos || colon < slash);
}

std::string last_segment(std::string_view normalized_url) {
  while (!normalized_url.empty() && (normalized_url.back() == '/' || normalized_url.back() == '\\'))
    normalized_url.remove_suffix(1);
  const auto cut = normalized_url.find_last_of("/\\:");
  return std::string(cut == std::string_view::npos ? normalized_url : normalized_url.substr(cut + 1));
}

Result<std::vector<Source>> registered_sources(Context& ctx) {
  auto stored = ctx.store->import_sources();
  if (!stored) return fail(stored.error());
  std::vector<Source> out;
  for (auto& info : *stored) {
    Source s;
    auto settings = load_settings(ctx.cache, info.spec.uuid);
    if (!settings) return fail(settings.error());
    s.settings = std::move(*settings);
    s.name = s.settings && !s.settings->name.empty() ? s.settings->name : last_segment(info.spec.url_or_path);
    s.info = std::move(info);
    out.push_back(std::move(s));
  }
  const auto key = [](const Source& s) {
    const bool reference = s.settings && s.settings->reference_runs;
    return std::tuple(kind_rank(s.info.spec.kind), reference ? 0 : 1, s.info.spec.url_or_path,
                      s.info.spec.branch.value_or(""));
  };
  std::sort(out.begin(), out.end(), [&](const Source& a, const Source& b) { return key(a) < key(b); });
  return out;
}

Result<std::vector<Source>> select_sources(Context& ctx, const std::optional<std::string>& which) {
  auto all = registered_sources(ctx);
  if (!all || !which) return all;
  const std::string normalized = ingest::normalize_source_url(*which);
  std::vector<Source> by_id, by_name;
  for (const auto& s : *all) {
    if (s.info.spec.uuid.str() == *which || s.info.spec.url_or_path == *which || s.info.spec.url_or_path == normalized)
      by_id.push_back(s);
    else if (s.name == *which)
      by_name.push_back(s);
  }
  if (!by_id.empty()) return by_id;
  if (by_name.size() > 1)
    return fail(ErrorKind::Config, "more than one source is named '" + *which + "'; give its id (elctl import status)");
  if (by_name.empty()) return fail(ErrorKind::Config, "no source '" + *which + "' (elctl import status lists them)");
  return by_name;
}

Result<P::Uuid> importer_client(P::IStore& store) {
  using pychron::env_var;
  std::string host = env_var("HOSTNAME").value_or("");
  if (host.empty()) host = env_var("COMPUTERNAME").value_or("");
  if (host.empty()) host = "localhost";
  return store.register_client({host, "importer", std::nullopt, "elctl " PYCHRON_ELCTL_VERSION});
}

ingest::WriterConfig writer_config(const SourceSettings& settings) {
  ingest::WriterConfig config;
  config.importer_version = "elctl " PYCHRON_ELCTL_VERSION;
  config.lab_time_zone = settings.tz;
  config.author_map = settings.author_map;
  return config;
}

Result<OpenedAdapter> open_adapter(Context& ctx, const SourceSettings& settings, const std::vector<Source>& all,
                                   std::optional<int> batch, bool fetch) {
  OpenedAdapter out;
  if (settings.kind == P::ImportSourceKind::LegacyDb) {
    dvc::CatalogAdapterConfig config;
    config.dir = settings.path;
    config.lab_time_zone = settings.tz;
    if (batch) config.batch_rows = *batch;
    auto adapter = dvc::CatalogAdapter::open(std::move(config));
    if (!adapter) return fail(adapter.error());
    out.warnings = (*adapter)->warnings();
    out.adapter = std::move(*adapter);
    return out;
  }

  dvc::GitConfig git;
  git.repo = settings.path;
  git.branch = settings.branch;
  git.scratch = ctx.scratch();
  if (settings.mirror && fetch) {
    auto mirrored = dvc::GitReader::mirror(settings.url, ctx.mirrors());
    if (!mirrored) return fail(mirrored.error());
    git.repo = *mirrored;
  }
  if (settings.kind == P::ImportSourceKind::MetaRepo) {
    dvc::MetaAdapterConfig config;
    config.git = std::move(git);
    config.url = settings.url;
    config.lab_time_zone = settings.tz;
    if (batch) config.batch_commits = *batch;
    auto adapter = dvc::MetaRepoAdapter::open(std::move(config));
    if (!adapter) return fail(adapter.error());
    out.adapter = std::move(*adapter);
    return out;
  }

  dvc::ProjectAdapterConfig config;
  config.git = std::move(git);
  config.url = settings.url;
  config.repository_name = settings.name;
  config.lab_time_zone = settings.tz;
  config.catalog_from_repos = settings.catalog_from_repos;
  if (batch) config.batch_commits = *batch;
  // Tags the legacy database kept (spec 10.3): from every registered dump.
  using Lookup = std::function<std::optional<std::string>(const P::Uuid&)>;
  std::vector<Lookup> lookups;
  for (const auto& other : all) {
    if (other.info.spec.kind != P::ImportSourceKind::LegacyDb) continue;
    const fs::path dir = other.settings ? other.settings->path : path_from_utf8(other.info.spec.url_or_path);
    // A dump that was moved or deleted must not stop every project.
    const auto no_tags = [&](const std::string& why) {
      out.warnings.push_back("no tags from the database dump " + other.name + ": " + why +
                             "; analyses without a tags file get the default tag");
    };
    std::error_code code;
    if (dir.empty() || !fs::is_directory(dir, code)) {
      no_tags(utf8(dir) + " is not there any more");
      continue;
    }
    auto lookup = dvc::load_tag_lookup(dir);
    if (!lookup) {
      no_tags(one_line(lookup.error().what));
      continue;
    }
    lookups.push_back(std::move(*lookup));
  }
  if (!lookups.empty())
    config.tag_lookup = [lookups = std::move(lookups)](const P::Uuid& analysis) -> std::optional<std::string> {
      for (const auto& lookup : lookups)
        if (auto tag = lookup(analysis)) return tag;
      return std::nullopt;
    };
  auto adapter = dvc::ProjectRepoAdapter::open(std::move(config));
  if (!adapter) return fail(adapter.error());
  out.adapter = std::move(*adapter);
  return out;
}

Result<PendingConflicts> pending_conflicts(P::IStore& store, P::Uuid source) {
  auto rows = store.import_conflicts({source, std::nullopt, std::string("pending")});
  if (!rows) return fail(rows.error());
  PendingConflicts out;
  for (const auto& row : *rows) {
    const bool warning = ingest::is_warning_conflict(row);
    ++(warning ? out.warnings : out.blocking)[std::string(P::to_string(row.kind))];
    ++(warning ? out.warnings_total : out.blocking_total);
  }
  return out;
}

std::string describe(const PendingConflicts& pending, bool with_kinds) {
  const auto kinds = [&](const std::map<std::string, int>& by_kind) {
    if (!with_kinds || by_kind.empty()) return std::string();
    std::string out = " (";
    for (const auto& [kind, rows] : by_kind) {
      if (out.size() > 2) out += ", ";
      out += kind + " " + std::to_string(rows);
    }
    return out + ")";
  };
  return std::to_string(pending.blocking_total) + " blocking" + kinds(pending.blocking) + ", " +
         std::to_string(pending.warnings_total) + (pending.warnings_total == 1 ? " warning" : " warnings") +
         kinds(pending.warnings);
}

}  // namespace elctl::import_detail
