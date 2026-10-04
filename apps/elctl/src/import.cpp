// elctl import: argument handling, dispatch, and the two listings (status,
// conflicts). add, run and verify have a file each.

#include "import.hpp"

#include <charconv>
#include <ostream>

#include <nlohmann/json.hpp>

#include "import_impl.hpp"

namespace elctl {

namespace import_detail {

namespace {

using Json = nlohmann::json;

constexpr const char* kShortUsage =
    "usage: elctl import <add|run|status|conflicts|verify> --db <url> [--cache <dir>] [options]\n"
    "run 'elctl import help' for the options\n";

constexpr const char* kHelp =
    "usage: elctl import <add|run|status|conflicts|verify> --db <url> [--cache <dir>] [options]\n"
    "\n"
    "Brings legacy pychron data into the store at --db (sqlite:/path/to/file.db or\n"
    "postgresql://user:password@host/db).\n"
    "\n"
    "  add --kind legacy_db|meta_repo|project_repo --source <path|url> --tz <IANA zone>\n"
    "      [--branch <b>] [--author-map <file.toml>] [--catalog-from-repos] [--reference-runs]\n"
    "        Register a source; prints its id. Registering it again changes nothing.\n"
    "        --source            legacy_db: the directory tools/legacy_dump_to_jsonl.py wrote;\n"
    "                            a repository: a local path, read in place and never\n"
    "                            modified, or a url, mirrored into the cache\n"
    "        --tz                the lab's time zone; legacy times are naive local times\n"
    "        --branch            default: the branch the repository's HEAD names\n"
    "        --author-map        TOML, one line per author: \"git email\" = \"user name\"\n"
    "        --catalog-from-repos\n"
    "                            project_repo, when there is no database dump: make\n"
    "                            identifiers, positions and spectrometers from the records\n"
    "        --reference-runs    project_repo: this repository holds blanks, airs and\n"
    "                            cocktails (a per-spectrometer repository). 'run' imports\n"
    "                            such repositories before the others: a blank or IC-factor\n"
    "                            reference to an analysis that is not in the store yet is\n"
    "                            stored unlinked and stays unlinked\n"
    "\n"
    "  run [--source <id|name> | --all] [--batch <n>] [--limit <n>] [--replay] [--dry-run]\n"
    "        Import, resuming where the last run stopped. Without --source: every source,\n"
    "        the database dump first, then meta repositories, repositories of reference\n"
    "        runs, and the other project repositories by url. Ctrl-C finishes the batch\n"
    "        being written and stops; the next run goes on from there.\n"
    "        --batch             commits (rows of a dump) per batch; default 500 (2000)\n"
    "        --limit             stop after this many batches\n"
    "        --replay            walk the source again from its start: what is imported\n"
    "                            is left alone, analyses refused earlier (unknown_analysis)\n"
    "                            are imported now that the catalog has their identifier\n"
    "        --dry-run           write nothing; print what a run would write\n"
    "\n"
    "  status\n"
    "        One line per source: id kind name status done/total head\n"
    "\n"
    "  conflicts [--source <id|name>] [--kind <kind>] [--all] [--json]\n"
    "        What the import could not import or left a note about, one line each:\n"
    "        kind path entity detail. Pending ones only; --all adds those dealt with.\n"
    "        kinds: unparseable unknown_analysis identity_clash value_mismatch hand_edit\n"
    "               provisional_renumber\n"
    "\n"
    "  verify [--source <id|name>] [--tolerance <relative, default 1e-9>] [--json]\n"
    "        Checks each import: the source is imported to its end, every file of it is\n"
    "        accounted for, a second run would write nothing, the ages stored with each\n"
    "        interpreted age are reproduced from the imported data, and no conflict that\n"
    "        means missing or disagreeing data is pending.\n"
    "\n"
    "  --cache <dir>   settings of each source (<id>.toml) and mirrors of remote\n"
    "                  repositories; default: the user's cache directory, pychron/import\n"
    "\n"
    "Exit codes: 0 ok, also for a paused run and one that left only warnings;\n"
    "            1 verify is not ok, or a run finished with blocking conflicts pending;\n"
    "            2 usage or fatal error.\n";

int usage(Io io, const std::string& message) {
  io.err << "elctl import: " << message << '\n' << kShortUsage;
  return ::elctl::kUsage;
}

int status(Context& ctx) {
  auto sources = registered_sources(ctx);
  if (!sources) return fatal(ctx.io, sources.error());
  for (const auto& s : *sources)
    ctx.io.out << s.info.spec.uuid.str() << ' ' << P::to_string(s.info.spec.kind) << ' ' << s.name << ' '
               << s.info.status << ' ' << s.info.done << '/' << s.info.total << ' ' << s.info.head_sha.value_or("-")
               << '\n';
  return kOk;
}

int conflicts(Context& ctx, const Flags& flags) {
  P::ConflictFilter filter;
  if (const auto kind = flags.get("--kind")) {
    filter.kind = P::parse_conflict_kind(*kind);
    if (!filter.kind) return fatal(ctx.io, "--kind: no conflict kind '" + *kind + "'");
  }
  if (!flags.has("--all")) filter.resolution = "pending";
  if (const auto which = flags.get("--source")) {
    auto sources = select_sources(ctx, which);
    if (!sources) return fatal(ctx.io, sources.error());
    filter.source = sources->front().info.spec.uuid;
  }
  auto rows = ctx.store->import_conflicts(filter);
  if (!rows) return fatal(ctx.io, rows.error());

  Json listed = Json::array();
  for (const auto& row : *rows) {
    const Json detail = Json::parse(row.detail_json, nullptr, false);
    const std::string detail_text =
        detail.is_discarded() ? one_line(row.detail_json) : detail.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (flags.has("--json")) {
      listed.push_back({{"uuid", row.uuid.str()},
                        {"kind", std::string(P::to_string(row.kind))},
                        {"path", row.path},
                        {"entity", row.entity ? Json(row.entity->str()) : Json(nullptr)},
                        {"resolution", row.resolution},
                        {"blocking", row.resolution == "pending" && !is_warning(row)},
                        {"detail", detail.is_discarded() ? Json(row.detail_json) : detail}});
      continue;
    }
    ctx.io.out << P::to_string(row.kind) << ' ' << row.path << ' ' << (row.entity ? row.entity->str() : "-") << ' '
               << detail_text;
    if (row.resolution != "pending") ctx.io.out << " (" << row.resolution << ')';
    ctx.io.out << '\n';
  }
  if (flags.has("--json")) ctx.io.out << listed.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
  return kOk;
}

}  // namespace

std::optional<std::string> Flags::get(std::string_view name) const {
  const auto it = values.find(name);
  if (it == values.end()) return std::nullopt;
  return it->second;
}

Result<Flags> parse_flags(const std::vector<std::string>& args, const FlagSpec& spec) {
  const auto among = [](const std::vector<std::string_view>& names, const std::string& arg) {
    for (const auto name : names)
      if (name == arg) return true;
    return false;
  };
  Flags flags;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (among(spec.switches, arg)) {
      flags.on.insert(arg);
    } else if (among(spec.with_value, arg)) {
      if (i + 1 >= args.size()) return fail(ErrorKind::Config, arg + " needs a value");
      flags.values.insert_or_assign(arg, args[++i]);
    } else if (arg.starts_with("-")) {
      return fail(ErrorKind::Config, "unknown option '" + arg + "'");
    } else {
      return fail(ErrorKind::Config, "unexpected argument '" + arg + "'");
    }
  }
  return flags;
}

Result<int> positive_int(std::string_view flag, const std::string& text) {
  int value = 0;
  const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (code != std::errc{} || end != text.data() + text.size() || value < 1)
    return fail(ErrorKind::Config, std::string(flag) + " takes a whole number, 1 or more; got '" + text + "'");
  return value;
}

std::string one_line(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  for (char& c : text)
    if (c == '\n' || c == '\r') c = ' ';
  return text;
}

int fatal(Io io, const std::string& message) {
  io.err << "elctl import: " << one_line(message) << '\n';
  return ::elctl::kUsage;
}

int fatal(Io io, const Error& error) { return fatal(io, error.what); }

}  // namespace import_detail

int import_command(const std::vector<std::string>& args, Io io) {
  using namespace import_detail;
  if (args.empty()) {
    io.err << kShortUsage;
    return ::elctl::kUsage;
  }
  const std::string& verb = args.front();
  if (verb == "help" || verb == "-h" || verb == "--help") {
    io.out << kHelp;
    return kOk;
  }

  FlagSpec spec{{"--db", "--cache"}, {}};
  if (verb == "add") {
    spec.with_value.insert(spec.with_value.end(), {"--kind", "--source", "--branch", "--tz", "--author-map"});
    spec.switches = {"--catalog-from-repos", "--reference-runs"};
  } else if (verb == "run") {
    spec.with_value.insert(spec.with_value.end(), {"--source", "--batch", "--limit"});
    spec.switches = {"--all", "--replay", "--dry-run"};
  } else if (verb == "conflicts") {
    spec.with_value.insert(spec.with_value.end(), {"--source", "--kind"});
    spec.switches = {"--all", "--json"};
  } else if (verb == "verify") {
    spec.with_value.insert(spec.with_value.end(), {"--source", "--tolerance"});
    spec.switches = {"--json"};
  } else if (verb != "status") {
    return usage(io, "unknown subcommand '" + verb + "'");
  }
  auto flags = parse_flags({args.begin() + 1, args.end()}, spec);
  if (!flags) return usage(io, verb + ": " + flags.error().what);
  const auto db = flags->get("--db");
  if (!db) return usage(io, verb + " needs --db <url>");

  Context ctx{io, {}, nullptr};
  const auto cache = flags->get("--cache");
  ctx.cache = cache ? fs::path(*cache) : default_cache_dir();
  auto store = P::open_store(P::StoreConfig{*db, true});
  if (!store) return fatal(io, store.error());
  ctx.store = std::move(*store);

  if (verb == "add") return import_add(ctx, *flags);
  if (verb == "run") return import_run(ctx, *flags);
  if (verb == "status") return status(ctx);
  if (verb == "conflicts") return conflicts(ctx, *flags);
  return import_verify(ctx, *flags);
}

}  // namespace elctl
