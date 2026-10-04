// elctl entry: sample and package entry over the DVC store (entry spec,
// section 10). Argument handling and one function per subcommand; the work
// is libs/entry's.

#include "entry.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "pychron/core/env.hpp"
#include "pychron/dvc/meta_files.hpp"
#include "pychron/entry/csv.hpp"
#include "pychron/entry/export.hpp"
#include "pychron/entry/holder_import.hpp"
#include "pychron/entry/identifier_plan.hpp"
#include "pychron/entry/level_sheet.hpp"
#include "pychron/entry/names.hpp"
#include "pychron/entry/package_edit.hpp"
#include "pychron/entry/positions_import.hpp"
#include "pychron/entry/sample_import.hpp"
#include "pychron/entry/settings.hpp"
#include "pychron/persistence/store.hpp"

namespace elctl {

namespace {

namespace ps = pychron::persistence;
namespace en = pychron::entry;
using pychron::Error;
using pychron::Result;

constexpr const char* kShortUsage =
    "usage: elctl entry <samples|package|positions|identifiers|holders|settings> <action> [args] --db <url>\n"
    "run 'elctl entry help' for the options\n";

constexpr const char* kHelp =
    "usage: elctl entry <what> <action> [args] --db <url> [--user <name>]\n"
    "\n"
    "Sample and package entry in the store at --db (sqlite:/path/to/file.db or\n"
    "postgresql://user:password@host/db). Writing commands print one line per row\n"
    "they write; with --dry-run they print it and write nothing.\n"
    "\n"
    "  samples import <file.csv> [--update-existing] [--errors <out.csv>] [--dry-run]\n"
    "        Add the samples of a CSV (or tab-separated) file, with their PIs, projects\n"
    "        and materials. Rows already stored are left alone; with --update-existing\n"
    "        their differing fields are updated. Any row in error: nothing is written.\n"
    "  samples template <out.csv>     write a CSV with every column the import reads\n"
    "  samples list [--pi <name>] [--project <name>] [--material <name>] [--text <t>]\n"
    "\n"
    "  package add <name> [--kind irradiation|package] [--levels A-C | A,B,D]\n"
    "              [--holder <name>] [--z <z>] [--reactor <name>] [--chronology <file> --tz <zone>]\n"
    "        A package and its levels in one change. Kind irradiation (the default)\n"
    "        needs a reactor (its production comes from reactors.json) and a\n"
    "        chronology file of \"power,start,end\" lines in local time.\n"
    "  package show <name> [--level <L>] [--csv]\n"
    "  package set-kind <name> irradiation|package\n"
    "\n"
    "  positions import <package> <file.csv> [--dry-run]\n"
    "        Columns level, position, sample, and optionally project,\n"
    "        principal_investigator, material, grainsize, weight, packet, note.\n"
    "  identifiers generate <package> [--overwrite] [--dry-run]\n"
    "        Number every position with a sample and no identifier, continuing the\n"
    "        store's sequence. --overwrite renumbers identifiers nothing has used.\n"
    "  holders import <file.txt> [--name <name>]  a legacy irradiation holder file\n"
    "  settings show | settings set <key> <value>\n"
    "\n"
    "Exit codes: 0 ok; 1 nothing was written (a stale, refused or invalid row);\n"
    "            2 usage or fatal error.\n";

// Positional arguments and --flags; a flag takes a value unless it is one of `switches`.
struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> values;
  std::set<std::string> switches;
  std::string error;

  std::optional<std::string> get(const std::string& k) const {
    auto it = values.find(k);
    if (it == values.end()) return std::nullopt;
    return it->second;
  }
  bool has(const std::string& k) const { return switches.count(k) > 0; }
};

Args parse(const std::vector<std::string>& args, const std::set<std::string>& switch_names) {
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& s = args[i];
    if (s.rfind("--", 0) != 0) {
      a.positional.push_back(s);
      continue;
    }
    if (switch_names.count(s)) {
      a.switches.insert(s);
      continue;
    }
    if (i + 1 >= args.size()) {
      a.error = s + " needs a value";
      return a;
    }
    a.values[s] = args[++i];
  }
  return a;
}

int usage(Io io, const std::string& message) {
  io.err << "elctl entry: " << message << '\n' << kShortUsage;
  return kUsage;
}

int fatal(Io io, const Error& e) {
  io.err << "elctl entry: " << pychron::to_string(e) << '\n';
  return kUsage;
}

int fatal(Io io, const std::string& what) {
  io.err << "elctl entry: " << what << '\n';
  return kUsage;
}

Result<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return pychron::fail(pychron::ErrorKind::Io, "cannot read " + path);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

Result<void> write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return pychron::fail(pychron::ErrorKind::Io, "cannot write " + path);
  out << text;
  if (!out) return pychron::fail(pychron::ErrorKind::Io, "cannot write " + path);
  return {};
}

std::string text_of(const ps::CatalogValue& v) {
  struct {
    std::string operator()(std::monostate) const { return "-"; }
    std::string operator()(const std::string& s) const { return s; }
    std::string operator()(double d) const {
      std::ostringstream o;
      o << d;
      return o.str();
    }
    std::string operator()(std::int64_t i) const { return std::to_string(i); }
    std::string operator()(bool b) const { return b ? "true" : "false"; }
    std::string operator()(const ps::Uuid& u) const { return u.str(); }
  } visitor;
  return std::visit(visitor, v);
}

// The store, the actor writes are made as, and the flags every subcommand shares.
struct Context {
  Io io;
  std::unique_ptr<ps::IStore> store;
  ps::Actor actor;
  bool dry_run = false;
};

Result<Context> open(Io io, const Args& a, bool writes) {
  const auto db = a.get("--db");
  if (!db) return pychron::fail(pychron::ErrorKind::Config, "--db <url> is required");
  // Never migrate from here: entry needs a store `elctl import` or an install made.
  auto store = ps::open_store(ps::StoreConfig{*db, false});
  if (!store) return pychron::fail(store.error());
  Context ctx{io, std::move(*store), {}, a.has("--dry-run")};
  if (writes && !ctx.dry_run) {
    const std::string host = pychron::env_var("HOSTNAME").value_or("localhost");
    auto client = ctx.store->register_client({host, "reduction", std::nullopt, "elctl"});
    if (!client) return pychron::fail(client.error());
    const std::string user = a.get("--user").value_or(pychron::env_var("USER").value_or("pychron"));
    auto u = ctx.store->ensure_user(*client, user);
    if (!u) return pychron::fail(u.error());
    ctx.actor = ps::Actor{*u, *client};
  }
  return ctx;
}

// Prints an outcome that wrote nothing; returns the exit code.
int report(Io io, const ps::CatalogOutcome& outcome) {
  if (const auto* stale = std::get_if<std::vector<ps::StaleRow>>(&outcome)) {
    for (const auto& s : *stale) {
      io.out << "stale " << ps::table_name(s.table) << ' ' << s.uuid.str();
      for (const auto& [k, v] : s.actual) io.out << ' ' << k << '=' << text_of(v);
      io.out << '\n';
    }
    io.err << "elctl entry: another client changed these rows; nothing was written\n";
    return kFailed;
  }
  if (const auto* refused = std::get_if<std::vector<ps::Refusal>>(&outcome)) {
    for (const auto& r : *refused) io.out << "refused " << r.rule << ": " << r.what << '\n';
    io.err << "elctl entry: nothing was written\n";
    return kFailed;
  }
  if (const auto* lost = std::get_if<std::vector<ps::RefConflict>>(&outcome)) {
    for (const auto& c : *lost) io.out << "conflict reference " << c.subject.str() << '\n';
    io.err << "elctl entry: a reference changed meanwhile; nothing was written\n";
    return kFailed;
  }
  return kOk;
}

void print_edits(Io io, const ps::CatalogEditBatch& batch) {
  for (const auto& e : batch.edits) {
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          const char* op = std::is_same_v<T, ps::CatalogInsert> ? "insert"
                           : std::is_same_v<T, ps::CatalogUpdate> ? "update"
                                                                  : "delete";
          io.out << op << ' ' << ps::table_name(x.table);
          if constexpr (!std::is_same_v<T, ps::CatalogDelete>)
            for (const auto& [k, v] : x.values)
              if (k.find("uuid") == std::string::npos) io.out << ' ' << k << '=' << text_of(v);
          io.out << '\n';
        },
        e);
  }
}

Result<std::optional<ps::IrradiationRow>> find_package(ps::IStore& store, const std::string& name) {
  auto rows = store.irradiations();
  if (!rows) return pychron::fail(rows.error());
  for (const auto& r : *rows)
    if (r.name == name) return std::optional<ps::IrradiationRow>{r};
  return std::optional<ps::IrradiationRow>{};
}

// ---------------------------------------------------------------- samples

int samples_import(Context& ctx, const Args& a) {
  if (a.positional.size() != 3) return usage(ctx.io, "samples import takes one file");
  auto text = read_file(a.positional[2]);
  if (!text) return fatal(ctx.io, text.error());
  auto table = en::read_csv(*text);
  if (!table) return fatal(ctx.io, table.error());
  auto catalog = en::read_snapshot(*ctx.store);
  if (!catalog) return fatal(ctx.io, catalog.error());
  auto settings = en::load_settings(*ctx.store);
  if (!settings) return fatal(ctx.io, settings.error());
  en::ImportOptions options;
  options.update_existing = a.has("--update-existing");
  options.pi_names_allowed = settings->settings.pi_names_allowed;
  const auto plan = en::plan_sample_import(*table, en::default_mapping(table->header), *catalog, options);
  for (const auto& r : plan.rows) {
    ctx.io.out << "line " << r.line << ' ' << en::to_string(r.state) << ' ' << r.sample;
    for (const auto& m : r.messages) ctx.io.out << " | " << m;
    if (r.state == en::RowState::Update)
      for (const auto& c : r.changed) ctx.io.out << " ~" << c;
    ctx.io.out << '\n';
  }
  for (const auto& n : plan.new_principal_investigators) ctx.io.out << "new principal investigator " << n << '\n';
  for (const auto& n : plan.new_projects) ctx.io.out << "new project " << n << '\n';
  for (const auto& n : plan.new_materials) ctx.io.out << "new material " << n << '\n';
  ctx.io.out << plan.creates << " to create, " << plan.updates << " differing, " << plan.exists << " stored, "
             << plan.errors << " in error\n";
  if (const auto errors = a.get("--errors"); errors && plan.errors > 0)
    if (auto r = write_file(*errors, en::errors_csv(plan)); !r) return fatal(ctx.io, r.error());
  if (plan.errors > 0) {
    ctx.io.err << "elctl entry: rows in error; nothing was written\n";
    return kFailed;
  }
  if (ctx.dry_run) return kOk;
  const auto batch = en::to_batch(plan, *catalog, options);
  if (batch.edits.empty()) return kOk;
  auto out = ctx.store->apply_catalog_edits(ctx.actor.client, batch);
  if (!out) return fatal(ctx.io, out.error());
  return report(ctx.io, *out);
}

int samples_list(Context& ctx, const Args& a) {
  auto catalog = en::read_snapshot(*ctx.store);
  if (!catalog) return fatal(ctx.io, catalog.error());
  std::string text = a.get("--text").value_or("");
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto& s : catalog->samples) {
    if (const auto pi = a.get("--pi"); pi && s.principal_investigator_name != *pi) continue;
    if (const auto p = a.get("--project"); p && s.project_name != *p) continue;
    if (const auto m = a.get("--material"); m && s.material_name != *m) continue;
    std::string lower = s.name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!text.empty() && lower.find(text) == std::string::npos) continue;
    ctx.io.out << s.name << '\t' << s.project_name << '\t' << s.principal_investigator_name << '\t' << s.material_name
               << '\t' << s.grainsize << '\t' << s.n_positions << " positions\t" << s.n_analyses << " analyses\n";
  }
  return kOk;
}

// ---------------------------------------------------------------- packages

Result<std::vector<std::string>> level_names(const std::string& spec) {
  std::vector<std::string> out;
  if (spec.size() == 3 && spec[1] == '-' && std::isupper(static_cast<unsigned char>(spec[0])) &&
      std::isupper(static_cast<unsigned char>(spec[2])) && spec[0] <= spec[2]) {
    for (char c = spec[0]; c <= spec[2]; ++c) out.emplace_back(1, c);
    return out;
  }
  std::stringstream s(spec);
  std::string part;
  while (std::getline(s, part, ','))
    if (!en::trim(part).empty()) out.push_back(en::trim(part));
  if (out.empty()) return pychron::fail(pychron::ErrorKind::Config, "--levels: write A-C or A,B,D");
  return out;
}

int package_add(Context& ctx, const Args& a) {
  if (a.positional.size() != 3) return usage(ctx.io, "package add takes a name");
  en::NewPackage p;
  p.name = a.positional[2];
  p.kind = a.get("--kind").value_or("irradiation");
  std::optional<ps::Uuid> holder;
  if (const auto h = a.get("--holder")) {
    auto found = ctx.store->find_catalog_row(ps::CatalogTable::RefObject, {std::string("irradiation_holder"), *h});
    if (!found) return fatal(ctx.io, found.error());
    if (!*found) return fatal(ctx.io, "no irradiation holder '" + *h + "' (elctl entry holders import)");
    holder = **found;
  }
  std::optional<double> z;
  if (const auto zt = a.get("--z")) {
    double v = 0;
    auto [end, ec] = std::from_chars(zt->data(), zt->data() + zt->size(), v);
    if (ec != std::errc() || end != zt->data() + zt->size()) return usage(ctx.io, "--z: not a number");
    z = v;
  }
  if (const auto levels = a.get("--levels")) {
    auto names = level_names(*levels);
    if (!names) return usage(ctx.io, names.error().what);
    for (const auto& n : *names) p.levels.push_back(en::NewLevel{n, holder, z, std::nullopt});
  }
  if (p.kind == "irradiation") {
    if (const auto file = a.get("--chronology")) {
      const auto tz = a.get("--tz");
      if (!tz) return usage(ctx.io, "--chronology needs --tz <IANA zone>");
      auto text = read_file(*file);
      if (!text) return fatal(ctx.io, text.error());
      auto chronology = pychron::dvc::parse_chronology_text(*text, *tz);
      if (!chronology) return fatal(ctx.io, chronology.error());
      p.doses = chronology->doses;
    }
    p.reactor = a.get("--reactor");
    if (p.reactor) {
      auto reactors = en::load_reactors(*ctx.store);
      if (!reactors) return fatal(ctx.io, reactors.error());
      if (auto it = reactors->find(*p.reactor); it != reactors->end()) {
        p.production = it->second;
      } else {
        ctx.io.err << "elctl entry: no reactor '" << *p.reactor << "' in reactors.json; its production is empty\n";
      }
    }
  }
  auto existing = ctx.store->irradiations();
  if (!existing) return fatal(ctx.io, existing.error());
  std::vector<std::string> names;
  for (const auto& r : *existing) names.push_back(r.name);
  if (const auto problems = en::validate(p, names); !problems.empty()) {
    for (const auto& m : problems) ctx.io.out << "invalid " << m << '\n';
    return kFailed;
  }
  ctx.io.out << "package " << p.name << " (" << p.kind << "), " << p.levels.size() << " levels, " << p.doses.size()
             << " doses";
  if (!p.doses.empty()) ctx.io.out << ", " << en::dose_hours(p.doses) << " h";
  ctx.io.out << '\n';
  if (ctx.dry_run) return kOk;
  auto made = en::create_package(*ctx.store, ctx.actor, p);
  if (!made) return fatal(ctx.io, made.error());
  ctx.io.out << "created " << made->package.str() << '\n';
  return kOk;
}

int package_show(Context& ctx, const Args& a) {
  if (a.positional.size() != 3) return usage(ctx.io, "package show takes a name");
  auto pkg = find_package(*ctx.store, a.positional[2]);
  if (!pkg) return fatal(ctx.io, pkg.error());
  if (!*pkg) return fatal(ctx.io, "no package '" + a.positional[2] + "'");
  auto sheets = en::package_sheets(*ctx.store, (*pkg)->uuid);
  if (!sheets) return fatal(ctx.io, sheets.error());
  if (const auto level = a.get("--level"))
    std::erase_if(*sheets, [&](const ps::LevelSheet& s) { return s.level.name != *level; });
  if (a.has("--csv")) {
    ctx.io.out << en::export_package_csv(*sheets);
    return kOk;
  }
  const auto& p = **pkg;
  ctx.io.out << p.name << " (" << p.kind << ") " << p.n_levels << " levels, " << p.n_positions << " positions, "
             << p.n_analyzed << " analyzed" << (p.has_chronology ? ", chronology" : "") << '\n';
  for (const auto& s : *sheets) {
    ctx.io.out << "level " << s.level.name << " holder " << s.level.holder_name.value_or("-") << " z "
               << (s.z && s.z->z ? std::to_string(*s.z->z) : std::string("-")) << '\n';
    for (const auto& r : s.positions)
      ctx.io.out << "  " << r.position << '\t' << r.identifier.value_or("-") << '\t'
                 << (r.sample ? r.sample_name : std::string("-")) << '\t' << r.project << '\t'
                 << r.principal_investigator << '\t' << r.material << '\t' << r.packet.value_or("") << '\n';
  }
  return kOk;
}

int package_set_kind(Context& ctx, const Args& a) {
  if (a.positional.size() != 4) return usage(ctx.io, "package set-kind takes a name and a kind");
  const std::string& kind = a.positional[3];
  if (kind != "irradiation" && kind != "package") return usage(ctx.io, "the kind is irradiation or package");
  auto pkg = find_package(*ctx.store, a.positional[2]);
  if (!pkg) return fatal(ctx.io, pkg.error());
  if (!*pkg) return fatal(ctx.io, "no package '" + a.positional[2] + "'");
  ps::CatalogEditBatch batch;
  batch.edits = {ps::CatalogUpdate{ps::CatalogTable::Irradiation, (*pkg)->uuid, {{"kind", (*pkg)->kind}}, {{"kind", kind}}}};
  print_edits(ctx.io, batch);
  if (ctx.dry_run) return kOk;
  auto out = ctx.store->apply_catalog_edits(ctx.actor.client, batch);
  if (!out) return fatal(ctx.io, out.error());
  return report(ctx.io, *out);
}

// ---------------------------------------------------------------- positions, identifiers

Result<std::map<std::string, en::LevelSheetEdit>> package_edits(ps::IStore& store, ps::Uuid package) {
  auto sheets = en::package_sheets(store, package);
  if (!sheets) return pychron::fail(sheets.error());
  std::map<std::string, en::LevelSheetEdit> out;
  for (auto& s : *sheets) {
    std::optional<ps::HolderValue> holder;
    if (s.level.holder) {
      auto h = en::load_holder(store, *s.level.holder);
      if (!h) return pychron::fail(h.error());
      holder = *h;
    }
    const std::string name = s.level.name;
    out.emplace(name, en::LevelSheetEdit(std::move(s), std::move(holder)));
  }
  return out;
}

int positions_import(Context& ctx, const Args& a) {
  if (a.positional.size() != 4) return usage(ctx.io, "positions import takes a package and a file");
  auto pkg = find_package(*ctx.store, a.positional[2]);
  if (!pkg) return fatal(ctx.io, pkg.error());
  if (!*pkg) return fatal(ctx.io, "no package '" + a.positional[2] + "'");
  auto text = read_file(a.positional[3]);
  if (!text) return fatal(ctx.io, text.error());
  auto table = en::read_csv(*text);
  if (!table) return fatal(ctx.io, table.error());
  auto catalog = en::read_snapshot(*ctx.store);
  if (!catalog) return fatal(ctx.io, catalog.error());
  auto edits = package_edits(*ctx.store, (*pkg)->uuid);
  if (!edits) return fatal(ctx.io, edits.error());
  auto settings = en::load_settings(*ctx.store);
  if (!settings) return fatal(ctx.io, settings.error());
  const auto result = en::apply_position_import(*table, *catalog, *edits, settings->settings.pi_names_allowed);
  if (!result.errors.empty()) {
    for (const auto& e : result.errors) ctx.io.out << e << '\n';
    ctx.io.err << "elctl entry: rows in error; nothing was written\n";
    return kFailed;
  }
  ps::CatalogEditBatch batch;
  batch.message = "positions import " + (*pkg)->name;
  for (auto& [name, edit] : *edits) {
    if (const auto problems = edit.validate(settings->settings); !problems.empty()) {
      for (const auto& m : problems) ctx.io.out << "level " << name << ": " << m << '\n';
      return kFailed;
    }
    batch.allow_analyzed_sample_change = false;
    for (auto& e : edit.to_batch().edits) batch.edits.push_back(std::move(e));
  }
  print_edits(ctx.io, batch);
  if (ctx.dry_run || batch.edits.empty()) return kOk;
  auto out = ctx.store->apply_catalog_edits(ctx.actor.client, batch);
  if (!out) return fatal(ctx.io, out.error());
  return report(ctx.io, *out);
}

int identifiers_generate(Context& ctx, const Args& a) {
  if (a.positional.size() != 3) return usage(ctx.io, "identifiers generate takes a package");
  auto pkg = find_package(*ctx.store, a.positional[2]);
  if (!pkg) return fatal(ctx.io, pkg.error());
  if (!*pkg) return fatal(ctx.io, "no package '" + a.positional[2] + "'");
  auto sheets = en::package_sheets(*ctx.store, (*pkg)->uuid);
  if (!sheets) return fatal(ctx.io, sheets.error());
  auto settings = en::load_settings(*ctx.store);
  if (!settings) return fatal(ctx.io, settings.error());
  if ((*pkg)->kind == "irradiation")
    for (const auto& w : en::human_error_checks(*sheets, settings->settings, (*pkg)->name))
      ctx.io.out << "warning " << w << '\n';
  auto last = en::current_last(*ctx.store);
  if (!last) return fatal(ctx.io, last.error());
  const auto plan = en::plan_identifiers(*sheets, *last, a.has("--overwrite"));
  for (const auto& p : plan.assignments)
    ctx.io.out << p.level << p.position_number << ' ' << p.sample << ' ' << p.current.value_or("-") << " -> "
               << p.number << '\n';
  ctx.io.out << plan.assignments.size() << " identifiers, " << plan.expected_last + 1 << " to " << plan.last << '\n';
  if (ctx.dry_run || plan.assignments.empty()) return kOk;
  auto out = ctx.store->allocate_identifiers(ctx.actor.client, plan.allocation());
  if (!out) return fatal(ctx.io, out.error());
  if (const auto* stale = std::get_if<ps::AllocationStale>(&*out)) {
    ctx.io.err << "elctl entry: identifiers were allocated meanwhile (last is now " << stale->actual_last
               << "); nothing was written, run again\n";
    return kFailed;
  }
  if (const auto* refused = std::get_if<std::vector<ps::Refusal>>(&*out)) {
    for (const auto& r : *refused) ctx.io.out << "refused " << r.rule << ": " << r.what << '\n';
    return kFailed;
  }
  return kOk;
}

// ---------------------------------------------------------------- holders, settings

int holders_import(Context& ctx, const Args& a) {
  if (a.positional.size() != 3) return usage(ctx.io, "holders import takes a file");
  const std::string path = a.positional[2];
  auto text = read_file(path);
  if (!text) return fatal(ctx.io, text.error());
  auto holder = en::read_holder(*text);
  if (!holder) return fatal(ctx.io, holder.error());
  std::string name = a.get("--name").value_or("");
  if (name.empty()) {
    const auto slash = path.find_last_of("/\\");
    name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (const auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) name.resize(dot);
  }
  ctx.io.out << "holder " << name << ": " << holder->holes.size() << " holes\n";
  if (ctx.dry_run) return kOk;
  auto id = en::save_holder(*ctx.store, ctx.actor, name, *holder);
  if (!id) return fatal(ctx.io, id.error());
  return kOk;
}

int settings_command(Context& ctx, const Args& a) {
  auto loaded = en::load_settings(*ctx.store);
  if (!loaded) return fatal(ctx.io, loaded.error());
  if (a.positional[1] == "show") {
    ctx.io.out << en::to_json(loaded->settings) << '\n';
    return kOk;
  }
  if (a.positional.size() != 4) return usage(ctx.io, "settings set takes a key and a value");
  auto doc = nlohmann::json::parse(en::to_json(loaded->settings));
  auto value = nlohmann::json::parse(a.positional[3], nullptr, false);
  doc[a.positional[2]] = value.is_discarded() ? nlohmann::json(a.positional[3]) : value;
  auto parsed = en::parse_settings(doc.dump());
  if (!parsed) return fatal(ctx.io, parsed.error());
  if (ctx.dry_run) {
    ctx.io.out << en::to_json(*parsed) << '\n';
    return kOk;
  }
  auto out = en::save_settings(*ctx.store, ctx.actor, *parsed, *loaded);
  if (!out) return fatal(ctx.io, out.error());
  if (!std::holds_alternative<ps::Committed>(*out)) {
    ctx.io.err << "elctl entry: the settings changed meanwhile; nothing was written\n";
    return kFailed;
  }
  return kOk;
}

}  // namespace

int entry_command(const std::vector<std::string>& args, Io io) {
  const Args a = parse(args, {"--dry-run", "--update-existing", "--overwrite", "--csv"});
  if (!a.error.empty()) return usage(io, a.error);
  if (a.positional.empty() || a.positional[0] == "help") {
    io.out << kHelp;
    return a.positional.empty() ? kUsage : kOk;
  }
  if (a.positional.size() < 2) return usage(io, "what to do with " + a.positional[0] + "?");
  const std::string what = a.positional[0], action = a.positional[1];
  if (what == "samples" && action == "template") {
    if (a.positional.size() != 3) return usage(io, "samples template takes an output file");
    if (auto r = write_file(a.positional[2], en::template_csv()); !r) return fatal(io, r.error());
    return kOk;
  }
  struct Command {
    const char* what;
    const char* action;
    bool writes;
    int (*run)(Context&, const Args&);
  };
  static const Command commands[] = {
      {"samples", "import", true, samples_import},
      {"samples", "list", false, samples_list},
      {"package", "add", true, package_add},
      {"package", "show", false, package_show},
      {"package", "set-kind", true, package_set_kind},
      {"positions", "import", true, positions_import},
      {"identifiers", "generate", true, identifiers_generate},
      {"holders", "import", true, holders_import},
      {"settings", "show", false, settings_command},
      {"settings", "set", true, settings_command},
  };
  for (const auto& c : commands) {
    if (what != c.what || action != c.action) continue;
    auto ctx = open(io, a, c.writes);
    if (!ctx) return fatal(io, ctx.error());
    return c.run(*ctx, a);
  }
  return usage(io, "unknown command '" + what + " " + action + "'");
}

}  // namespace elctl
