#include "setup_support.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <QApplication>
#include <QCoreApplication>
#include <QProcess>
#include <QStringList>

#ifdef PYCHRON_UI_HAS_STORE
#pragma push_macro("signals")
#undef signals
#include "pychron/entry/seed.hpp"
#include "pychron/persistence/store.hpp"
#pragma pop_macro("signals")
#endif

#include "brand.hpp"
#include "pychron/core/env.hpp"
#include "pychron/setup/installer.hpp"

namespace pychron::ui {

SetupWizard::OpenDatabase database_opener() {
#ifdef PYCHRON_UI_HAS_STORE
  return [](const std::string& url, bool create) -> Result<std::string> {
    auto store = persistence::open_store(persistence::StoreConfig{url, create});
    if (!store) return fail(std::move(store).error());
    auto status = (*store)->schema_status();
    if (!status) return fail(std::move(status).error());
    return status->empty() ? std::string("empty schema") : "schema version " + std::to_string(status->back().version);
  };
#else
  return {};
#endif
}

SetupWizard::SeedDatabase database_seeder() {
#ifdef PYCHRON_UI_HAS_STORE
  return [](const std::string& url, const std::filesystem::path& seed_file, bool migrate) -> Result<std::string> {
    std::ifstream in(seed_file, std::ios::binary);
    if (!in) return fail(ErrorKind::Io, "cannot open " + seed_file.string());
    std::ostringstream text;
    text << in.rdbuf();
    auto seed = entry::parse_seed(text.str(), seed_file.string());
    if (!seed) return fail(std::move(seed).error());
    auto store = persistence::open_store(persistence::StoreConfig{url, migrate});
    if (!store) {
      Error e = std::move(store).error();
      e.what = "the database could not be opened: " + e.what;
      return fail(std::move(e));
    }
    auto client = (*store)->register_client(
        {env_var("HOSTNAME").value_or("localhost"), "reduction", std::nullopt, "pychron-ui"});
    if (!client) return fail(std::move(client).error());
    auto user = (*store)->ensure_user(*client, env_var("USER").value_or("pychron"));
    if (!user) return fail(std::move(user).error());
    auto report = entry::apply_seed(**store, *seed, persistence::Actor{*user, *client});
    if (!report) return fail(std::move(report).error());
    return entry::describe(*report);
  };
#else
  return {};
#endif
}

bool start_install(const std::string& name) {
  return QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--install"), QString::fromStdString(name)});
}

int self_test(std::ostream& out) {
  int failed = 0;
  auto check = [&](bool ok, const std::string& what) {
    out << (ok ? "OK    " : "FAIL  ") << what << "\n";
    if (!ok) ++failed;
  };
  out << "pychron-ui " << setup::version() << "\n";
  check(QApplication::instance() != nullptr && !QGuiApplication::platformName().isEmpty(),
        "Qt platform: " + QGuiApplication::platformName().toStdString());
  const setup::Resources r = setup::find_resources();
  auto library = setup::ProfileLibrary::load(r.profiles, r.examples);
  check(library.has_value(), "profiles: " + r.profiles.string() + (library ? "" : ": " + library.error().what));
  if (library) {
    int resolved = 0;
    for (const auto* p : library->list()) {
      if (p->kind == setup::ProfileKind::Fragment) continue;
      auto rp = library->resolve(p->name);
      check(rp.has_value(), "profile " + p->name + (rp ? "" : ": " + rp.error().what));
      if (rp) ++resolved;
    }
    check(resolved > 0, "installable profiles: " + std::to_string(resolved));
    SetupWizard wizard(*library, {std::filesystem::path("/nonexistent/site.toml"), {}, {}, {}});
    check(wizard.pageIds().size() >= 4, "setup wizard builds");
  }
  if (auto open = database_opener()) {
    auto db = open("sqlite::memory:", true);
    check(db.has_value(), "database (Qt SQLite plugin): " + (db ? *db : db.error().what));
  } else {
    out << "skip  database: built without the DVC store\n";
  }
  if (auto seed = database_seeder()) {
    const auto file = r.profiles / "instrument-common" / "seed.toml";
    auto done = seed("sqlite::memory:", file, true);
    check(done.has_value(), "seed (" + file.string() + "): " + (done ? *done : done.error().what));
  } else {
    out << "skip  seed: built without the DVC store\n";
  }
  return failed == 0 ? 0 : 1;
}

int write_icons(const std::filesystem::path& dir, std::ostream& out) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  int failed = 0;
  for (const int size : {16, 24, 32, 48, 64, 128, 256, 512, 1024}) {
    const std::filesystem::path file = dir / ("pychron-" + std::to_string(size) + ".png");
    const bool ok = brand::icon_pixmap(size).save(QString::fromStdString(file.string()), "PNG");
    out << (ok ? "wrote " : "FAILED ") << file.string() << "\n";
    if (!ok) ++failed;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace pychron::ui
