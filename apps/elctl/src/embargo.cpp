// elctl embargo: hide a project, a sample or an analysis from every public
// release (data publishing design, sections 4.2 and 6.2), over
// libs/publish's embargo.hpp.

#include "embargo.hpp"

#include <optional>
#include <ostream>
#include <utility>

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/host_name.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/publish/embargo.hpp"

namespace elctl {

namespace ps = pychron::persistence;
namespace pb = pychron::publish;
using pychron::Error;
using pychron::ErrorKind;
using pychron::fail;
using pychron::Result;

namespace {

constexpr const char* kUsageText =
    "usage: elctl embargo <command> [options]\n"
    "\n"
    "An embargo hides a project, a sample or an analysis from every public release\n"
    "and hosted view until a date, or until it is lifted.\n"
    "\n"
    "  set  --db <url> (--project P | --sample S | --analysis A)... [--until YYYY-MM-DD] [--reason <text>]\n"
    "  lift --db <url> (--id <uuid> | --project P | --sample S | --analysis A)...\n"
    "                              by scope: every live embargo on it\n"
    "  list --db <url> [--live]\n"
    "\n"
    "A project is its name or uuid; a sample \"name\", \"name@project\" or a uuid; an\n"
    "analysis a run id (70001-01, 70001-01A) or a uuid. --until is a date, inclusive:\n"
    "the embargo ends at midnight UTC after it; without it, the embargo lasts until\n"
    "lifted. An ambiguous name is refused naming the candidates.\n"
    "\n"
    "Exit codes: 0 done; 2 usage or fatal error.\n";

int usage(Io io, const std::string& message) {
  io.err << "elctl embargo: " << message << "\nelctl embargo help lists the commands\n";
  return kUsage;
}

int fatal(Io io, const std::string& message) {
  io.err << "elctl embargo: " << message << '\n';
  return kUsage;
}

int fatal(Io io, const Error& error) { return fatal(io, error.what); }

struct Scope {
  ps::EmbargoScopeType type;
  std::string name;
};

struct Args {
  std::string db, until, reason, id;
  std::vector<Scope> scopes;
  bool live = false;
};

Result<Args> parse(const std::vector<std::string>& args) {
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    if (flag == "--live") {
      a.live = true;
      continue;
    }
    if (!flag.starts_with("--")) return fail(ErrorKind::Config, "unexpected argument '" + flag + "'");
    if (i + 1 >= args.size()) return fail(ErrorKind::Config, flag + " needs a value");
    const std::string& value = args[++i];
    if (flag == "--db") a.db = value;
    else if (flag == "--until") a.until = value;
    else if (flag == "--reason") a.reason = value;
    else if (flag == "--id") a.id = value;
    else if (flag == "--project") a.scopes.push_back({ps::EmbargoScopeType::Project, value});
    else if (flag == "--sample") a.scopes.push_back({ps::EmbargoScopeType::Sample, value});
    else if (flag == "--analysis") a.scopes.push_back({ps::EmbargoScopeType::Analysis, value});
    else return fail(ErrorKind::Config, "unknown flag '" + flag + "'");
  }
  if (a.db.empty()) return fail(ErrorKind::Config, "--db <url> is required");
  return a;
}

struct Opened {
  std::unique_ptr<ps::IStore> store;
  ps::Actor actor;
};

Result<Opened> open_as_publisher(const std::string& url) {
  auto store = ps::open_store(ps::StoreConfig{url, false});
  if (!store) return fail(store.error());
  auto client = (*store)->register_client({pychron::host_name(), "publisher", std::nullopt, "elctl"});
  if (!client) return fail(client.error());
  const std::string user = pychron::env_var("USER").value_or(pychron::env_var("USERNAME").value_or("pychron"));
  auto user_id = (*store)->ensure_user(*client, user.empty() ? "pychron" : user);
  if (!user_id) return fail(user_id.error());
  return Opened{std::move(*store), ps::Actor{*user_id, *client}};
}

// A date "YYYY-MM-DD", inclusive: midnight UTC of the day after it.
Result<ps::UtcTime> day_after(const std::string& date) {
  auto t = ps::UtcTime::parse(date + "T00:00:00Z");
  if (!t) return fail(ErrorKind::Config, "--until '" + date + "' is not a date (YYYY-MM-DD)");
  return ps::UtcTime{t->micros + 86'400LL * 1'000'000LL};
}

std::string date_before(const ps::UtcTime& until) {
  // The inclusive date an until-midnight time was made from.
  return ps::UtcTime{until.micros - 1}.iso().substr(0, 10);
}

int set(const Args& a, Io io) {
  if (a.scopes.empty()) return usage(io, "set needs --project, --sample or --analysis");
  std::optional<ps::UtcTime> until;
  if (!a.until.empty()) {
    auto t = day_after(a.until);
    if (!t) return usage(io, t.error().what);
    until = *t;
  }
  auto opened = open_as_publisher(a.db);
  if (!opened) return fatal(io, opened.error());
  for (const auto& s : a.scopes) {
    pb::EmbargoRequest request;
    request.type = s.type;
    request.name_or_uuid = s.name;
    request.until = until;
    if (!a.reason.empty()) request.reason = a.reason;
    auto scope = pb::resolve_scope(*opened->store, s.type, s.name);
    if (!scope) return fatal(io, scope.error());
    auto id = pb::set_embargo(*opened->store, opened->actor, request);
    if (!id) return fatal(io, id.error());
    io.out << id->str() << "  " << ps::to_string(s.type) << " " << s.name << "  "
           << (until ? "until " + date_before(*until) : std::string("indefinite")) << '\n';
  }
  return kOk;
}

int lift(const Args& a, Io io) {
  if (a.id.empty() && a.scopes.empty()) return usage(io, "lift needs --id, --project, --sample or --analysis");
  auto opened = open_as_publisher(a.db);
  if (!opened) return fatal(io, opened.error());
  std::size_t lifted = 0;
  if (!a.id.empty()) {
    if (auto r = pb::lift_embargo(*opened->store, opened->actor, a.id); !r) return fatal(io, r.error());
    ++lifted;
  }
  for (const auto& s : a.scopes) {
    auto n = pb::lift_embargoes_on(*opened->store, opened->actor, s.type, s.name);
    if (!n) return fatal(io, n.error());
    lifted += *n;
  }
  io.out << "lifted " << lifted << '\n';
  return kOk;
}

int list(const Args& a, Io io) {
  auto store = ps::open_store(ps::StoreConfig{a.db, false});
  if (!store) return fatal(io, store.error());
  auto rows = pb::list_embargoes(**store, a.live, ps::UtcTime::now());
  if (!rows) return fatal(io, rows.error());
  for (const auto& e : *rows) {
    io.out << e.row.uuid.str() << "  " << e.scope_name << "  "
           << (e.row.until ? "until " + date_before(*e.row.until) : std::string("indefinite"));
    if (e.row.reason) io.out << "  " << *e.row.reason;
    if (e.row.lifted) io.out << "  lifted " << e.row.lifted->iso().substr(0, 10);
    io.out << '\n';
  }
  if (rows->empty()) io.out << (a.live ? "no live embargoes\n" : "no embargoes\n");
  return kOk;
}

}  // namespace

int embargo_command(const std::vector<std::string>& args, Io io) {
  if (args.empty()) return usage(io, "a command is required");
  const std::string& cmd = args[0];
  if (cmd == "help" || cmd == "--help") {
    io.out << kUsageText;
    return kOk;
  }
  if (cmd != "set" && cmd != "lift" && cmd != "list") return usage(io, "unknown command '" + cmd + "'");
  auto parsed = parse(std::vector<std::string>(args.begin() + 1, args.end()));
  if (!parsed) return usage(io, parsed.error().what);
  try {
    if (cmd == "set") return set(*parsed, io);
    if (cmd == "lift") return lift(*parsed, io);
    return list(*parsed, io);
  } catch (const std::exception& e) {
    return fatal(io, e.what());
  }
}

}  // namespace elctl
