#include "pychron/setup/installer.hpp"

#include <cctype>
#include <cstdio>
#include <system_error>

#include "pychron/core/env.hpp"
#include "pychron/core/process.hpp"

namespace pychron::setup {

namespace fs = std::filesystem;

namespace {

bool has_profiles(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_directory(dir / "profiles", ec) && fs::is_directory(dir / "examples", ec);
}

}  // namespace

Resources find_resources() { return find_resources(executable_dir()); }

Resources find_resources(const fs::path& exe) {
  Resources r{fs::path(PYCHRON_SETUP_SOURCE_DIR) / "profiles", fs::path(PYCHRON_SETUP_SOURCE_DIR) / "configs" / "examples"};
  if (!exe.empty()) {
    for (const fs::path& base : {exe / ".." / "share" / "pychron", exe / ".." / "Resources"}) {
      if (!has_profiles(base)) continue;
      std::error_code ec;
      const fs::path canonical = fs::weakly_canonical(base, ec);
      const fs::path& b = ec ? base : canonical;
      r = {b / "profiles", b / "examples"};
      break;
    }
  }
  if (auto env = env_var("PYCHRON_PROFILES_DIR"); env && !env->empty()) r.profiles = *env;
  if (auto env = env_var("PYCHRON_EXAMPLES_DIR"); env && !env->empty()) r.examples = *env;
  return r;
}

fs::path lab_state_dir(const fs::path& lab, const fs::path& examples, const fs::path& user_dir) {
  if (examples.empty()) return lab;
  std::error_code ec;
  fs::path at = fs::weakly_canonical(lab, ec);
  if (ec) at = fs::absolute(lab, ec).lexically_normal();
  // Up from the lab to the examples, by what the folders are rather than how
  // they are spelled: a link, or other letter case on a disk that ignores it,
  // is still the examples.
  fs::path rel;
  while (true) {
    std::error_code same;
    if (fs::equivalent(at, examples, same)) break;
    if (at == at.parent_path() || at.empty()) return lab;
    if (!at.filename().empty()) rel = rel.empty() ? at.filename() : at.filename() / rel;
    at = at.parent_path();
  }
  fs::path base = user_dir;
  if (base.empty()) base = fs::absolute(fs::temp_directory_path(ec) / "pychron" / "examples", ec);
  return rel.empty() ? base : base / rel;
}

Result<fs::path> field_table_root(const fs::path& root, const fs::path& examples, const fs::path& user_dir) {
  const fs::path where = root.empty() ? fs::path(".") : root;
  fs::path state = lab_state_dir(where, examples, user_dir);
  if (state == where) return state;
  std::error_code ec;
  if (!fs::is_directory(where / "tables", ec)) return state;
  std::error_code listing;
  for (const auto& table : fs::directory_iterator(where / "tables", listing)) {
    if (!table.is_directory(ec)) continue;
    const fs::path name = table.path().filename();
    if (!fs::exists(table.path() / "current", ec) || fs::exists(state / "tables" / name / "current", ec)) continue;
    fs::create_directories(state / "tables" / name, ec);
    if (!ec)
      fs::copy(table.path(), state / "tables" / name, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
               ec);
    if (ec) {
      return fail(ErrorKind::Io, "cannot copy field table " + table.path().string() + " to " +
                                     (state / "tables" / name).string() + ": " + ec.message());
    }
  }
  if (listing) return fail(ErrorKind::Io, "cannot read " + (where / "tables").string() + ": " + listing.message());
  return state;
}

fs::path home_dir() {
#ifdef _WIN32
  if (auto h = env_var("USERPROFILE"); h && !h->empty()) return *h;
#else
  if (auto h = env_var("HOME"); h && !h->empty()) return *h;
#endif
  return ".";
}

std::string default_install_name(const Profile& profile) {
  return profile.kind == ProfileKind::DataReduction ? std::string("data-reduction") : profile.name;
}

fs::path default_root(const Profile& profile, const std::string& name) {
  if (profile.kind == ProfileKind::DataReduction) return home_dir() / "Documents" / "Pychron";
  return home_dir() / "Pychron" / name;
}

std::string_view version() noexcept { return PYCHRON_VERSION; }

Answers builtin_answers(const std::string& name, const fs::path& root) {
  return {{"install_name", Value{name}}, {"root", Value{root.generic_string()}}};
}

bool is_asked(const Question& q, const Answers& so_far) {
  if (q.when.empty()) return true;
  auto c = evaluate(q.when, so_far);
  return c && *c;
}

bool keep_unanswered_secrets(const ResolvedProfile& profile, Answers& given) {
  bool any = false;
  for (const auto& q : profile.questions) {
    if (q.type == QuestionType::Secret && !given.contains(q.id)) {
      given[q.id] = Value{std::string{}};
      any = true;
    }
  }
  return any;
}

std::string percent_encode(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

std::string database_url_for(const Answers& answers, const fs::path& root, bool with_password) {
  auto text = [&](const char* id) {
    auto it = answers.find(id);
    return it == answers.end() ? std::string{} : to_text(it->second);
  };
  if (text("data_source") != "server") {
    std::string given = text("db_file");
    const fs::path file = given.empty() ? fs::path("data/pychron.db") : fs::path(std::move(given));
    return "sqlite:" + (file.is_absolute() ? file : root / file).generic_string();
  }
  std::string user = percent_encode(text("db_user"));
  if (const std::string pw = text("db_password"); with_password && !pw.empty()) user += ":" + percent_encode(pw);
  return "postgresql://" + user + "@" + text("db_host") + ":" + text("db_port") + "/" + text("db_name");
}

Result<void> register_install(const SiteInstall& install, const fs::path& site_path) {
  auto site = load_site(site_path);
  if (!site) return fail(std::move(site).error());
  site->upsert(install);
  if (site->default_install.empty()) site->default_install = install.name;
  return save_site(*site, site_path);
}

}  // namespace pychron::setup
