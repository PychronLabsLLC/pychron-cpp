// elctl import add: register a source. Everything that can be wrong with it
// is found before anything is stored: the zone, the repository (through
// GitReader::open: not a repository, no such branch, empty, shallow, git too
// old) or the dump directory (no MANIFEST.json), the author map.

#include <ostream>
#include <system_error>

#include "import_impl.hpp"
#include "pychron/dvc/git_reader.hpp"
#include "pychron/ingest/ids.hpp"
#include "pychron/ingest/tz.hpp"

namespace elctl::import_detail {

namespace dvc = pychron::dvc;
namespace ingest = pychron::ingest;

int import_add(Context& ctx, const Flags& flags) {
  const auto kind_text = flags.get("--kind");
  const auto source = flags.get("--source");
  const auto tz = flags.get("--tz");
  if (!kind_text) return fatal(ctx.io, "add needs --kind legacy_db|meta_repo|project_repo");
  const auto kind = P::parse_import_source_kind(*kind_text);
  if (!kind) return fatal(ctx.io, "--kind is legacy_db, meta_repo or project_repo; got '" + *kind_text + "'");
  if (!source || source->empty()) return fatal(ctx.io, "add needs --source <path|url>");
  if (!tz) return fatal(ctx.io, "add needs --tz <IANA zone>, the lab's time zone (for example America/Denver)");
  if (!ingest::known_zone(*tz)) return fatal(ctx.io, "--tz: no time zone '" + *tz + "'");

  const bool project = *kind == P::ImportSourceKind::ProjectRepo;
  const bool catalog = *kind == P::ImportSourceKind::LegacyDb;
  for (const char* only_project : {"--catalog-from-repos", "--reference-runs"})
    if (flags.has(only_project) && !project)
      return fatal(ctx.io, std::string(only_project) + " applies to a project_repo only");
  if (catalog && flags.get("--branch")) return fatal(ctx.io, "--branch does not apply to a legacy_db");

  SourceSettings settings;
  settings.kind = *kind;
  settings.url = *source;
  settings.tz = *tz;
  settings.catalog_from_repos = flags.has("--catalog-from-repos");
  settings.reference_runs = flags.has("--reference-runs");
  if (const auto map = flags.get("--author-map")) {
    auto authors = read_author_map(*map);
    if (!authors) return fatal(ctx.io, authors.error());
    settings.author_map = std::move(*authors);
  }

  if (catalog || !is_url(*source)) {
    std::error_code code;
    settings.path = fs::absolute(fs::path(*source), code).lexically_normal();
    if (code) return fatal(ctx.io, "cannot resolve " + *source + ": " + code.message());
    if (catalog) settings.url = settings.path.string();
  } else {
    auto mirrored = dvc::GitReader::mirror(*source, ctx.mirrors());
    if (!mirrored) return fatal(ctx.io, mirrored.error());
    settings.path = *mirrored;
    settings.mirror = true;
  }
  if (!catalog) {
    if (const auto branch = flags.get("--branch")) {
      settings.branch = *branch;
    } else {
      // The branch HEAD names; opening at HEAD also finds what makes the
      // repository unreadable before any question of branches.
      dvc::GitConfig git;
      git.repo = settings.path;
      git.branch = "HEAD";
      git.scratch = ctx.scratch();
      auto reader = dvc::GitReader::open(std::move(git));
      if (!reader) return fatal(ctx.io, reader.error());
      auto branch_name = reader->default_branch();
      if (!branch_name) return fatal(ctx.io, branch_name.error());
      settings.branch = *branch_name;
    }
  }

  // A project repository's analyses become members of the repository of this name.
  settings.name = last_segment(ingest::normalize_source_url(catalog ? settings.path.string() : settings.url));
  if (settings.name.ends_with(".git")) settings.name.resize(settings.name.size() - 4);
  if (settings.name.empty()) return fatal(ctx.io, "cannot name the source " + *source);

  auto all = registered_sources(ctx);
  if (!all) return fatal(ctx.io, all.error());
  auto opened = open_adapter(ctx, settings, *all, std::nullopt, false);
  if (!opened) return fatal(ctx.io, opened.error());
  auto described = opened->adapter->describe();
  if (!described) return fatal(ctx.io, described.error());
  const std::string url = ingest::normalize_source_url(described->url);
  settings.uuid = ingest::source_id(described->kind, url, described->branch);
  for (const auto& line : opened->warnings) ctx.io.err << "warning: " << line << '\n';

  // Registered before: its ids were derived under the settings it has.
  for (const auto& other : *all) {
    if (other.info.spec.uuid != settings.uuid) continue;
    const bool same = other.info.spec.lab_time_zone == settings.tz &&
                      (!other.settings || other.settings->catalog_from_repos == settings.catalog_from_repos);
    if (!same)
      return fatal(ctx.io, url + " is already registered with other settings (--tz " + other.info.spec.lab_time_zone +
                               (other.settings && other.settings->catalog_from_repos ? ", --catalog-from-repos" : "") +
                               "); they decide what was imported and cannot change");
    if (auto saved = save_settings(ctx.cache, settings); !saved) return fatal(ctx.io, saved.error());
    ctx.io.out << settings.uuid.str() << '\n';
    return kOk;
  }

  auto client = importer_client(*ctx.store);
  if (!client) return fatal(ctx.io, client.error());
  if (auto saved = save_settings(ctx.cache, settings); !saved) return fatal(ctx.io, saved.error());
  ingest::BatchWriter writer(*ctx.store, *client, writer_config(settings));
  auto registered = writer.open(*opened->adapter);
  if (!registered) {
    std::error_code code;
    fs::remove(settings_file(ctx.cache, settings.uuid), code);
    return fatal(ctx.io, registered.error());
  }
  ctx.io.out << registered->spec.uuid.str() << '\n';
  return kOk;
}

}  // namespace elctl::import_detail
