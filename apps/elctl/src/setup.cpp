#include "setup.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include "pychron/core/env.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"

#ifdef PYCHRON_ELCTL_HAS_STORE
#include "pychron/persistence/store.hpp"
#endif

namespace elctl {

namespace fs = std::filesystem;
using namespace pychron;
using namespace pychron::setup;

namespace {

constexpr const char* kInitUsage =
    "usage: elctl init --list\n"
    "       elctl init <profile> [--root DIR] [--name NAME] [--answers FILE] [--set id=value]... [--yes]\n"
    "       elctl init --reconfigure [--install NAME] [--set id=value]... [--yes]\n"
    "options: --profiles DIR   where the profiles are (default: the ones shipped with elctl)\n";

Resources resources(const std::optional<fs::path>& profiles) {
  Resources r = find_resources();
  if (profiles) r.profiles = *profiles;
  return r;
}

bool yes(std::istream& in, std::ostream& out, const std::string& question) {
  out << question << " [Y/n] " << std::flush;
  std::string line;
  if (!std::getline(in, line)) return false;
  return line.empty() || line == "y" || line == "Y" || line == "yes";
}

std::string shown(const Question& q, const Value& v) {
  if (q.type == QuestionType::Secret) return "****";
  if (const auto* rows = std::get_if<std::vector<Row>>(&v)) {
    std::string out;
    for (const auto& r : *rows) {
      std::string row;
      for (const auto& c : q.columns) row += (row.empty() ? "" : " ") + (r.count(c) && !r.at(c).empty() ? r.at(c) : "-");
      out += "\n      " + row;
    }
    return out;
  }
  return to_text(v);
}

// Asks every question still without an answer whose `when` holds, group by
// group in the profile's group order.
Result<void> ask(const ResolvedProfile& profile, Answers& given, const Answers& fixed, Io io) {
  Answers so_far = fixed;
  for (const auto& [k, v] : given) so_far[k] = v;
  std::vector<const Question*> ordered;
  for (const auto& g : profile.groups)
    for (const auto& q : profile.questions)
      if (q.group == g) ordered.push_back(&q);
  std::string group;
  for (const Question* qp : ordered) {
    const Question& q = *qp;
    if (!is_asked(q, so_far)) continue;
    if (given.count(q.id)) continue;
    if (q.group != group) {
      group = q.group;
      io.out << "\n" << group << "\n";
    }
    if (q.type == QuestionType::Table) {
      // Tables are edited in an answers file; at a prompt the default is shown.
      if (q.default_value) {
        io.out << "  " << q.prompt << ":" << shown(q, *q.default_value) << "\n"
               << "  (to change them, put " << q.id << " in an answers file and pass --answers)\n";
        so_far[q.id] = *q.default_value;
      }
      continue;
    }
    for (;;) {
      io.out << "  " << q.prompt;
      if (q.type == QuestionType::Choice) {
        std::string all;
        for (const auto& c : q.choices) all += (all.empty() ? "" : "/") + c;
        io.out << " (" << all << ")";
      }
      if (q.default_value) io.out << " [" << shown(q, *q.default_value) << "]";
      io.out << ": " << std::flush;
      std::string line;
      if (!std::getline(io.in, line)) return fail(ErrorKind::Cancelled, "no answer for " + q.id + " (input ended)");
      if (line.empty() && q.default_value) {
        so_far[q.id] = *q.default_value;
        break;
      }
      auto v = parse_answer(q, line);
      if (!v) {
        io.out << "  " << v.error().what << "\n";
        continue;
      }
      given[q.id] = *v;
      so_far[q.id] = *v;
      break;
    }
  }
  return {};
}

void print_plan(const InstallPlan& plan, Io io) {
  int writes = 0;
  for (const auto& f : plan.files) {
    const bool interesting = f.action != PlannedFile::Action::Same;
    if (f.action == PlannedFile::Action::Write || f.action == PlannedFile::Action::Update) ++writes;
    if (interesting) io.out << "  " << to_string(f.action) << "  " << f.to.generic_string() << "\n";
  }
  io.out << writes << " file(s) to write under " << plan.root.string() << "\n";
}

#ifdef PYCHRON_ELCTL_HAS_STORE
Result<std::string> open_store(const std::string& url, bool migrate) {
  auto store = persistence::open_store(persistence::StoreConfig{url, migrate});
  if (!store) return fail(std::move(store).error());
  auto status = (*store)->schema_status();
  if (!status) return fail(std::move(status).error());
  return "schema " + (status->empty() ? std::string("empty") : "version " + std::to_string(status->back().version));
}
#endif

int report_doctor(const SiteInstall& install, const ProfileLibrary* library, bool strict, bool probe, Io io) {
  DoctorOptions options;
  options.library = library;
  options.probe = probe;
#ifdef PYCHRON_ELCTL_HAS_STORE
  options.open_database = [](const std::string& url) { return open_store(url, false); };
#endif
  const auto checks = doctor(install, options);
  for (const auto& c : checks) {
    (c.status == Check::Status::Ok ? io.out : io.err) << "[" << to_string(c.status) << "] " << c.name
                                                      << (c.detail.empty() ? "" : ": " + c.detail) << "\n";
    if (!c.hint.empty()) (c.status == Check::Status::Ok ? io.out : io.err) << "       " << c.hint << "\n";
  }
  if (any_fail(checks) || (strict && any_warn(checks))) return kFailed;
  return kOk;
}

}  // namespace

Result<SiteInstall> resolve_install(const std::optional<std::string>& name) {
  const auto path = default_site_path();
  auto site = load_site(path);
  if (!site) return fail(std::move(site).error());
  if (const auto* i = site->pick(name)) return *i;
  if (name) return fail(ErrorKind::Config, "no install named '" + *name + "' in " + path.string());
  if (site->installs.empty()) return fail(ErrorKind::Config, "nothing is installed yet (elctl init --list)");
  return fail(ErrorKind::Config, "several installs and no default in " + path.string() + "; pass --install NAME");
}

int init_command(const std::vector<std::string>& args, Io io) {
  std::optional<fs::path> profiles, root, answers_file;
  std::optional<std::string> profile_name, name, install;
  std::vector<std::string> sets;
  bool list = false, reconfigure = false, assume_yes = false;
  auto usage = [&](const std::string& m) {
    io.err << "elctl init: " << m << "\n" << kInitUsage;
    return static_cast<int>(kUsage);
  };
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&]() -> std::optional<std::string> { return i + 1 < args.size() ? std::optional(args[++i]) : std::nullopt; };
    if (a == "--list") list = true;
    else if (a == "--reconfigure") reconfigure = true;
    else if (a == "--yes" || a == "-y") assume_yes = true;
    else if (a == "--profiles" || a == "--root" || a == "--name" || a == "--answers" || a == "--set" || a == "--install") {
      auto v = value();
      if (!v) return usage(a + " needs a value");
      if (a == "--profiles") profiles = *v;
      else if (a == "--root") root = *v;
      else if (a == "--name") name = *v;
      else if (a == "--answers") answers_file = *v;
      else if (a == "--install") install = *v;
      else sets.push_back(*v);
    } else if (!a.starts_with("-") && !profile_name) {
      profile_name = a;
    } else {
      return usage("unexpected '" + a + "'");
    }
  }

  const Resources where = resources(profiles);
  auto library = ProfileLibrary::load(where.profiles, where.examples);
  if (!library) {
    io.err << "error: " << library.error().what << "\n";
    return kFailed;
  }
  if (list) {
    for (const auto* p : library->list()) {
      if (p->kind == ProfileKind::Fragment) continue;
      io.out << p->name << "  (" << to_string(p->kind) << ")  " << p->title << "\n    " << p->summary << "\n";
    }
    return kOk;
  }

  Answers given;
  std::optional<SiteInstall> existing;
  if (reconfigure) {
    if (profile_name) return usage("--reconfigure takes no profile (it uses the install's)");
    auto i = resolve_install(install);
    if (!i) {
      io.err << "error: " << i.error().what << "\n";
      return kFailed;
    }
    existing = *i;
    auto record = read_install_record(i->root);
    if (!record) {
      io.err << "error: " << record.error().what << "\n";
      return kFailed;
    }
    profile_name = record->profile;
    given = record->answers;
    root = i->root;
    name = i->name;
  } else if (!profile_name) {
    return usage("name a profile (elctl init --list)");
  }

  auto resolved = library->resolve(*profile_name);
  if (!resolved) {
    io.err << "error: " << resolved.error().what << "\n";
    return kFailed;
  }
  if (answers_file) {
    std::ifstream in(*answers_file, std::ios::binary);
    if (!in) {
      io.err << "error: cannot read " << answers_file->string() << "\n";
      return kFailed;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto file = answers_from_toml(ss.str(), answers_file->string());
    if (!file) {
      io.err << "error: " << file.error().what << "\n";
      return kFailed;
    }
    for (auto& [k, v] : *file) given[k] = std::move(v);
  }
  for (const auto& s : sets) {
    auto kv = parse_assignment(s);
    if (!kv) return usage(kv.error().what);
    given[kv->first] = Value{kv->second};  // typed by complete_answers
  }
  if (!name) name = default_install_name(resolved->top);
  if (!root) root = default_root(resolved->top, *name);
  const Answers builtins = builtin_answers(*name, *root);

  // A reconfigure without the secrets again keeps the files that hold them.
  const bool keep_secrets = reconfigure && keep_unanswered_secrets(*resolved, given);
  if (!assume_yes && !answers_file && !reconfigure) {
    io.out << resolved->top.title << ": " << resolved->top.summary << "\n"
           << "Press Enter to accept the value in [brackets].\n";
    if (auto asked = ask(*resolved, given, builtins, io); !asked) {
      io.err << "\n" << asked.error().what << "\n";
      return kFailed;
    }
  }
  auto answers = complete_answers(*resolved, given, builtins);
  if (!answers) {
    io.err << "error: " << answers.error().what << "\n";
    return kFailed;
  }
  auto plan = plan_install(*library, *resolved, *answers, *root, PlanOptions{reconfigure, keep_secrets});
  if (!plan) {
    io.err << "error: " << plan.error().what << "\n";
    return kFailed;
  }
  io.out << "\n";
  print_plan(*plan, io);
  if (!assume_yes && !yes(io.in, io.out, "Write them?")) {
    io.out << "nothing written\n";
    return kFailed;
  }
  auto report = apply_install(*plan);
  if (!report) {
    io.err << "error: " << report.error().what << "\n";
    return kFailed;
  }
  io.out << report->written.size() << " written, " << report->updated.size() << " updated, " << report->kept.size()
         << " kept as they were";
  if (!report->conflicts.empty()) io.out << ", " << report->conflicts.size() << " edited file(s) got a .new beside them";
  io.out << "\n";

  SiteInstall entry = site_install(*plan, *name);
#ifdef PYCHRON_ELCTL_HAS_STORE
  if (entry.kind == "data_reduction" && entry.database.starts_with("sqlite:")) {
    // A local database: create it and bring its schema up to date.
    auto made = open_store(entry.database, true);
    if (!made) {
      io.err << "error: creating " << entry.database << ": " << made.error().what << "\n";
      return kFailed;
    }
    io.out << "database " << entry.database << " ready (" << *made << ")\n";
  }
#endif
  const auto site_path = default_site_path();
  if (auto saved = register_install(entry, site_path); !saved) {
    io.err << "error: " << saved.error().what << "\n";
    return kFailed;
  }
  io.out << "install '" << entry.name << "' recorded in " << site_path.string() << "\n\n";
  if (!plan->placeholders.empty()) {
    io.out << "Placeholders to replace before measuring (see CALIBRATE.md):\n";
    for (const auto& p : plan->placeholders) io.out << "  " << p.generic_string() << "\n";
    io.out << "\n";
  }
  const int doctor_rc = report_doctor(entry, &*library, false, false, io);
  return doctor_rc;
}

int doctor_command(const std::vector<std::string>& args, std::optional<std::string> install, Io io) {
  bool strict = false, probe = false;
  std::optional<fs::path> profiles;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--strict") strict = true;
    else if (args[i] == "--probe") probe = true;
    else if (args[i] == "--install" && i + 1 < args.size()) install = args[++i];
    else if (args[i] == "--profiles" && i + 1 < args.size()) profiles = args[++i];
    else {
      io.err << "elctl doctor: unexpected '" << args[i] << "'\nusage: elctl doctor [--install NAME] [--strict] [--probe]\n";
      return kUsage;
    }
  }
  auto i = resolve_install(install);
  if (!i) {
    io.err << "error: " << i.error().what << "\n";
    return kFailed;
  }
  const Resources where = resources(profiles);
  auto library = ProfileLibrary::load(where.profiles, where.examples);
  return report_doctor(*i, library ? &*library : nullptr, strict, probe, io);
}

}  // namespace elctl
