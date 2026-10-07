// elctl flux show, history and monitors: what a level's saved flux holds, how
// it came to hold it, and the lab's monitor sets (flux fitting design,
// sections 4 and 7).

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "flux.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/flux_view.hpp"
#include "pychron/processing/flux_store.hpp"
#include "pychron/processing/store_source.hpp"

namespace elctl {

namespace pp = pychron::processing;
namespace ps = pychron::persistence;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

namespace {

constexpr const char* kShortUsage =
    "usage: elctl flux show <irradiation> <level> --db <url>\n"
    "       elctl flux history <irradiation> <level> [<hole>] --db <url>\n"
    "       elctl flux monitors [list | show NAME | set FILE | default NAME] --db <url> [--user NAME]\n";

int usage(Io io, const std::string& message) { return flux_usage(io, message, kShortUsage); }
int fatal(Io io, const std::string& message) { return flux_error(io, message); }
int failed(Io io, const std::string& message) { return flux_error(io, message, kFailed); }

struct Args : FluxStoreArgs {
  std::vector<std::string> positional;
};

Result<Args> parse(const std::vector<std::string>& args) {
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    if (flag.rfind("--", 0) != 0) {
      a.positional.push_back(flag);
      continue;
    }
    auto taken = flux_store_flag(args, i, a);
    if (!taken) return fail(taken.error());
    if (!*taken) return fail(ErrorKind::Config, "unknown flag '" + flag + "'");
  }
  if (a.db.empty()) return fail(ErrorKind::Config, "--db <url> is required");
  return a;
}

using Row = std::vector<std::string>;

// "NM-300 A" for a level; the holes of a position.
std::string join(const std::vector<std::string>& items, const char* separator) {
  std::string out;
  for (const auto& item : items) out += (out.empty() ? "" : separator) + item;
  return out;
}

std::string model_of(const std::optional<pp::FluxOptions>& options) {
  return options ? std::string(pp::legacy_model_name(options->fit.kind)) : "-";
}

std::string or_dash(const std::string& s) { return s.empty() ? "-" : s; }

// ---- show -------------------------------------------------------------------

int show(const Args& a, Io io) {
  if (a.positional.size() != 2) return usage(io, "show needs an irradiation and a level");
  auto store = open_flux_store(a.db);
  if (!store) return fatal(io, store.error().what);
  // What is saved, from the store alone: no holder needed, no analysis reduced.
  auto level = pp::load_saved_flux(**store, a.positional[0], a.positional[1]);
  if (!level) return failed(io, a.positional[0] + " " + a.positional[1] + ": " + level.error().what);

  std::vector<Row> rows;
  for (const auto& p : *level) {
    Row row{std::to_string(p.hole), or_dash(p.identifier), or_dash(p.sample)};
    if (p.saved) {
      const auto& s = *p.saved;
      row.insert(row.end(), {pychron::processing::flux_j_text(s.j), pychron::processing::flux_j_text(s.j_err), pychron::processing::flux_percent_of(s.j_err, s.j), model_of(s.options),
                             or_dash(s.saved_by), or_dash(s.saved_utc)});
    } else {
      row.insert(row.end(), 6, "-");
    }
    rows.push_back(std::move(row));
  }
  io.out << flux_table({"hole", "identifier", "sample", "J", "+/-", "%", "model", "saved by", "saved (UTC)"}, rows);
  return kOk;
}

// ---- history ----------------------------------------------------------------

struct Saved {
  ps::RevisionInfo revision;
  std::optional<ps::FluxValue> value;
};

int history(const Args& a, Io io) {
  if (a.positional.size() != 2 && a.positional.size() != 3) return usage(io, "history needs an irradiation, a level and perhaps a hole");
  const std::string& irradiation = a.positional[0];
  const std::string& level = a.positional[1];
  const std::string where = irradiation + " " + level;
  std::optional<int> only;
  if (a.positional.size() == 3) {
    int hole = 0;
    char rest = 0;
    if (std::sscanf(a.positional[2].c_str(), "%d%c", &hole, &rest) != 1) return usage(io, "'" + a.positional[2] + "' is not a hole number");
    only = hole;
  }
  auto store = open_flux_store(a.db);
  if (!store) return fatal(io, store.error().what);
  ps::IStore& s = **store;

  auto irradiations = s.irradiations();
  if (!irradiations) return fatal(io, irradiations.error().what);
  const auto irradiation_row = std::find_if(irradiations->begin(), irradiations->end(),
                                            [&](const ps::IrradiationRow& r) { return r.name == irradiation; });
  if (irradiation_row == irradiations->end()) return failed(io, "no irradiation '" + irradiation + "'");
  auto levels = s.levels(irradiation_row->uuid);
  if (!levels) return fatal(io, levels.error().what);
  const auto level_row = std::find_if(levels->begin(), levels->end(), [&](const ps::LevelRow& r) { return r.name == level; });
  if (level_row == levels->end()) return failed(io, "no level " + level + " of " + irradiation);
  auto sheet = s.level_sheet(level_row->uuid);
  if (!sheet) return fatal(io, sheet.error().what);
  if (!*sheet) return failed(io, "no " + where);
  auto objects = s.ref_objects(ps::RefType::FluxPosition, irradiation_row->uuid);
  if (!objects) return fatal(io, objects.error().what);
  std::map<std::string, const ps::RefObjectRow*> by_key;
  for (const auto& object : *objects) by_key.emplace(object.key, &object);

  // The revisions of each position's flux object, newest first.
  std::map<int, std::vector<Saved>> by_hole;
  for (const auto& p : (*sheet)->positions) {
    if (only && p.position != *only) continue;
    const auto object = by_key.find(irradiation + "/" + level + "/" + std::to_string(p.position));
    if (object == by_key.end()) continue;
    auto revisions = s.history(object->second->uuid, ps::Kind::RefValue);
    if (!revisions) return fatal(io, revisions.error().what);
    auto& list = by_hole[p.position];
    for (auto& revision : *revisions) {
      Saved saved{std::move(revision), std::nullopt};
      if (only) {
        auto payload = s.load_payload(saved.revision.uuid);
        if (!payload) return fatal(io, payload.error().what);
        const auto* ref = *payload ? std::get_if<ps::RefPayload>(&**payload) : nullptr;
        if (const auto* flux = ref ? std::get_if<ps::FluxValue>(ref) : nullptr) saved.value = *flux;
      }
      list.push_back(std::move(saved));
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const Saved& x, const Saved& y) { return x.revision.change_seq > y.revision.change_seq; });
  }

  if (only) {
    const auto found = by_hole.find(*only);
    if (found == by_hole.end() || found->second.empty()) return failed(io, "hole " + std::to_string(*only) + " of " + where + " has no saved flux");
    std::vector<Row> rows;
    for (const auto& saved : found->second) {
      const auto& r = saved.revision;
      const ps::FluxValue empty;
      const ps::FluxValue& v = saved.value ? *saved.value : empty;
      std::optional<pp::FluxOptions> options;
      if (v.options_json) options = pp::parse_flux_options(*v.options_json).options;
      rows.push_back({r.changeset.created.iso(), or_dash(r.author_name), or_dash(r.changeset.message), pychron::processing::flux_j_text(v.j),
                      pychron::processing::flux_j_text(v.j_err), pychron::processing::flux_percent_of(v.j_err, v.j), model_of(options)});
    }
    io.out << flux_table({"saved (UTC)", "by", "message", "J", "+/-", "%", "model"}, rows);
    return kOk;
  }

  // One line per changeset: the holes it touched.
  struct Group {
    ps::RevisionInfo first;
    ps::ChangeSeq seq = 0;
    std::set<int> holes;
  };
  std::map<std::string, Group> groups;
  for (const auto& [hole, list] : by_hole)
    for (const auto& saved : list) {
      auto [it, inserted] = groups.try_emplace(saved.revision.changeset.uuid.str(), Group{saved.revision, 0, {}});
      it->second.seq = std::max(it->second.seq, saved.revision.change_seq);
      it->second.holes.insert(hole);
    }
  if (groups.empty()) return failed(io, where + " has no saved flux");
  std::vector<const Group*> ordered;
  for (const auto& [uuid, group] : groups) ordered.push_back(&group);
  std::stable_sort(ordered.begin(), ordered.end(), [](const Group* x, const Group* y) { return x->seq > y->seq; });
  std::vector<Row> rows;
  for (const Group* g : ordered) {
    std::vector<std::string> holes;
    for (const int hole : g->holes) holes.push_back(std::to_string(hole));
    const auto& r = g->first;
    rows.push_back({r.changeset.created.iso(), or_dash(r.author_name), or_dash(r.changeset.message), join(holes, ", ")});
  }
  io.out << flux_table({"saved (UTC)", "by", "message", "positions"}, rows);
  return kOk;
}

// ---- monitors ---------------------------------------------------------------

std::string names_of(const pp::MonitorSets& sets) {
  std::vector<std::string> names;
  for (const auto& s : sets.sets) names.push_back(s.name);
  return join(names, ", ");
}

int list_sets(const pp::MonitorSets& sets, Io io) {
  std::vector<Row> rows;
  for (const auto& m : sets.sets) {
    char age[32], err[32], lambda[32];
    std::snprintf(age, sizeof age, "%g", m.age_ma);
    std::snprintf(err, sizeof err, "%g", m.age_err_ma);
    std::snprintf(lambda, sizeof lambda, "%.4e", m.lambda_k().value);
    rows.push_back({m.name == sets.default_name ? "*" : " ", m.name, m.sample, m.material, age, err, lambda});
  }
  io.out << flux_table({" ", "name", "sample", "material", "age (Ma)", "+/-", "lambda_k"}, rows);
  return kOk;
}

int show_set(const pp::MonitorSets& sets, const std::string& name, Io io) {
  const pp::MonitorSet* found = nullptr;
  for (const auto& m : sets.sets)
    if (m.name == name) found = &m;
  if (!found) return failed(io, "no monitor set '" + name + "' (available: " + names_of(sets) + ")");
  const auto doc = nlohmann::json::parse(pp::to_json(sets));
  for (const auto& e : doc.at("monitors"))
    if (e.at("name") == name) {
      io.out << e.dump(2) << '\n';
      return kOk;
    }
  return failed(io, "no monitor set '" + name + "'");
}

int monitors(const Args& a, Io io) {
  const auto& p = a.positional;
  const std::string action = p.empty() ? "list" : p[0];
  const std::size_t wanted = action == "list" ? 1 : 2;
  if (action != "list" && action != "show" && action != "set" && action != "default")
    return usage(io, "unknown monitors action '" + action + "'");
  if (p.size() > wanted || (action != "list" && p.size() < wanted))
    return usage(io, action == "list" ? "list takes no argument" : action + (action == "set" ? " needs a file" : " needs a name"));

  // set reads its file before the store: a file that is not there is no business of the store's.
  std::optional<pp::MonitorSets> given;
  if (action == "set") {
    std::ifstream in(p[1], std::ios::binary);
    if (!in) return fatal(io, "could not read " + p[1]);
    std::stringstream text;
    text << in.rdbuf();
    auto parsed = pp::parse_monitor_sets(text.str());
    if (!parsed) return fatal(io, p[1] + ": " + parsed.error().what);
    given = std::move(*parsed);
  }

  auto store = open_flux_store(a.db);
  if (!store) return fatal(io, store.error().what);
  auto loaded = pp::load_monitor_sets(**store);
  if (!loaded) return fatal(io, loaded.error().what);

  if (action == "list") return list_sets(loaded->sets, io);
  if (action == "show") return show_set(loaded->sets, p[1], io);
  if (action == "set")
    return flux_save_monitor_sets(**store, a.user, *given, *loaded, "saved " + std::to_string(given->sets.size()) + " monitor sets", io);
  // default
  pp::MonitorSets sets = loaded->sets;
  if (!sets.find(p[1]) || p[1].empty()) return failed(io, "no monitor set '" + p[1] + "' (available: " + names_of(sets) + ")");
  if (sets.default_name == p[1]) {
    io.out << p[1] << " is already the default\n";
    return kOk;
  }
  sets.default_name = p[1];
  return flux_save_monitor_sets(**store, a.user, sets, *loaded, "the default monitor set is now " + p[1], io);
}

}  // namespace

int flux_save_monitor_sets(ps::IStore& store, const std::string& user, const pp::MonitorSets& sets,
                           const pp::LoadedMonitorSets& loaded, const std::string& done, Io io) {
  auto actor = flux_actor(store, user);
  if (!actor) return fatal(io, actor.error().what);
  auto outcome = pp::save_monitor_sets(store, *actor, sets, loaded);
  if (!outcome) return fatal(io, outcome.error().what);
  if (std::holds_alternative<std::vector<ps::Conflict>>(*outcome))
    return failed(io, "not saved: someone else saved the monitor sets since they were read; run the command again");
  io.out << done << '\n';
  return kOk;
}

int flux_admin_command(const std::string& subcommand, const std::vector<std::string>& rest, Io io) {
  auto parsed = parse(rest);
  if (!parsed) return usage(io, parsed.error().what);
  if (subcommand == "show") return show(*parsed, io);
  if (subcommand == "history") return history(*parsed, io);
  return monitors(*parsed, io);
}

}  // namespace elctl
