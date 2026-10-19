// Preferences > Data source (data source editing spec, 3.5): the page shows
// the install's database and data folder, says a kept password is kept, tries
// a source without writing, is read-only while a queue runs and notes a
// session's --db.

#include <QtTest>

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "data_source_page.hpp"
#include "preferences_dialog.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/template.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"
#include "setup_support.hpp"
#include "temp_path.hpp"

using namespace pychron;
using namespace pychron::ui;
namespace fs = std::filesystem;

namespace {

setup::ProfileLibrary shipped() {
  const auto r = setup::find_resources();
  auto lib = setup::ProfileLibrary::load(r.profiles, r.examples);
  if (!lib) qFatal("%s", lib.error().what.c_str());
  return std::move(*lib);
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every file and directory under `dir`, with a file's bytes: anything made,
// written or removed shows. A SQLite store's shared-memory index (<file>-shm)
// is named but not read: SQLite rewrites it on every open, a read included.
std::map<std::string, std::string> tree(const fs::path& dir) {
  std::map<std::string, std::string> out;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    const std::string name = fs::relative(entry.path(), dir).generic_string();
    if (name.ends_with("-shm")) {
      out[name] = "shm";
      continue;
    }
    out[name] = entry.is_regular_file() ? read_text(entry.path()) : std::string("dir");
  }
  return out;
}

QString text_of(QWidget* editor) {
  auto* e = qobject_cast<QLineEdit*>(editor);
  if (e == nullptr && editor != nullptr) e = editor->findChild<QLineEdit*>(QStringLiteral("text"));
  return e ? e->text() : QString();
}

// A data-reduction install made as the wizard makes one, in a temp folder,
// recorded in a site file of its own.
struct Lab {
  fs::path dir = pychron::testing::unique_temp_path("pychron-ui-data-source-");
  fs::path root = dir / "lab";
  fs::path site_path = dir / "site" / "site.toml";
  setup::ProfileLibrary library = shipped();
  setup::SiteInstall install;

  explicit Lab(const setup::Answers& given = {}) {
    fs::create_directories(root);
    auto resolved = library.resolve("data-reduction");
    if (!resolved) qFatal("%s", resolved.error().what.c_str());
    auto answers = setup::complete_answers(*resolved, given, setup::builtin_answers("lab", root));
    if (!answers) qFatal("%s", answers.error().what.c_str());
    auto plan = setup::plan_install(library, *resolved, *answers, root);
    if (!plan) qFatal("%s", plan.error().what.c_str());
    if (auto report = setup::apply_install(*plan); !report) qFatal("%s", report.error().what.c_str());
    install = setup::site_install(*plan, "lab");
    if (auto r = setup::register_install(install, site_path); !r) qFatal("%s", r.error().what.c_str());
  }
  ~Lab() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  Lab(const Lab&) = delete;
  Lab& operator=(const Lab&) = delete;
  Lab(Lab&&) = delete;
  Lab& operator=(Lab&&) = delete;

  [[nodiscard]] std::vector<setup::Question> questions() const { return library.find("store-common")->questions; }

  [[nodiscard]] DataSourcePageDeps deps() const {
    DataSourcePageDeps d;
    d.install = install;
    d.site_path = site_path;
    d.questions = questions();
    return d;
  }
};

setup::Answers server_answers() {
  return {{"data_source", setup::Value{std::string("server")}}, {"db_host", setup::Value{std::string("db.lab")}},
          {"db_user", setup::Value{std::string("ar")}}, {"db_password", setup::Value{std::string("s3cret")}}};
}

}  // namespace

class TestDataSourcePage : public QObject {
  Q_OBJECT

 private slots:
  void loadsTheCurrentSource() {
    const Lab lab;
    DataSourcePage page(lab.deps());
    auto* local = page.editor(QStringLiteral("data_source"))->findChild<QRadioButton*>(QStringLiteral("data_source-local"));
    QVERIFY(local != nullptr);
    QVERIFY(local->isChecked());
    QCOMPARE(text_of(page.editor(QStringLiteral("db_file"))), QStringLiteral("data/pychron.db"));
    QCOMPARE(text_of(page.editor(QStringLiteral("data_folder"))), QStringLiteral("data"));
    QVERIFY(page.editor(QStringLiteral("db_file"))->isVisibleTo(&page));
    for (const char* id : {"db_host", "db_port", "db_name", "db_user", "db_password"})
      QVERIFY2(!page.editor(QString::fromLatin1(id))->isVisibleTo(&page), id);
    QVERIFY(!page.changed());
    auto source = page.source();
    QVERIFY(source);
    QVERIFY(std::holds_alternative<setup::LocalDatabase>(source->database));

    // Choosing the server shows its fields and hides the file.
    page.editor(QStringLiteral("data_source"))->findChild<QRadioButton*>(QStringLiteral("data_source-server"))->setChecked(true);
    QVERIFY(page.editor(QStringLiteral("db_host"))->isVisibleTo(&page));
    QVERIFY(!page.editor(QStringLiteral("db_file"))->isVisibleTo(&page));
    QVERIFY(page.changed());
  }

  void aKeptPasswordIsShownAsKept() {
    const Lab lab(server_answers());
    DataSourcePage page(lab.deps());
    auto* password = qobject_cast<QLineEdit*>(page.editor(QStringLiteral("db_password")));
    QVERIFY(password != nullptr);
    QVERIFY(password->text().isEmpty());
    QCOMPARE(password->placeholderText(), QStringLiteral("kept"));
    QCOMPARE(text_of(page.editor(QStringLiteral("db_host"))), QStringLiteral("db.lab"));
    QVERIFY(page.editor(QStringLiteral("db_host"))->isVisibleTo(&page));
    auto source = page.source();
    QVERIFY(source);
    const auto* server = std::get_if<setup::ServerDatabase>(&source->database);
    QVERIFY(server != nullptr);
    QVERIFY(!server->password.has_value());  // blank: keep
    QVERIFY(!page.changed());
    password->setText(QStringLiteral("new"));
    QVERIFY(page.changed());
  }

  void testConnectionOnATempSqlite() {
#ifndef PYCHRON_UI_HAS_STORE
    QSKIP("built without the DVC store");
#else
    const Lab lab;
    // An existing, current store beside the install.
    const fs::path copy = lab.dir / "copy.db";
    auto open = database_opener();
    QVERIFY(bool(open));
    QVERIFY(open("sqlite:" + copy.generic_string(), true));

    auto deps = lab.deps();
    deps.openers.open_database = open;
    DataSourcePage page(std::move(deps));
    auto* file = page.editor(QStringLiteral("db_file"))->findChild<QLineEdit*>(QStringLiteral("text"));
    QVERIFY(file != nullptr);
    file->setText(QString::fromStdString(copy.generic_string()));
    const auto before = tree(lab.dir);
    page.test_connection();
    QVERIFY2(page.result()->text().contains(QStringLiteral("schema")), qPrintable(page.result()->text()));
    QCOMPARE(tree(lab.dir), before);
#endif
  }

  void readOnlyWhileAQueueRuns() {
    const Lab lab;
    auto deps = lab.deps();
    bool running = true;
    deps.queue_running = [&running] { return running; };
    DataSourcePage page(std::move(deps));
    page.refresh_read_only();
    for (const char* id : {"data_source", "db_file", "data_folder"})
      QVERIFY2(!page.editor(QString::fromLatin1(id))->isEnabled(), id);
    QVERIFY(!page.test_button()->isEnabled());
    QVERIFY(page.note()->text().contains(QStringLiteral("Stop the queue to change the data source")));
    // Nor written from code while it runs.
    const std::string site_before = read_text(lab.site_path);
    page.editor(QStringLiteral("data_folder"))->findChild<QLineEdit*>(QStringLiteral("text"))->setText(QStringLiteral("runs"));
    QVERIFY(page.apply().contains(QStringLiteral("Stop the queue")));
    QCOMPARE(read_text(lab.site_path), site_before);
    running = false;
    page.refresh_read_only();
    QVERIFY(page.editor(QStringLiteral("data_folder"))->isEnabled());
    QVERIFY(!page.note()->text().contains(QStringLiteral("Stop the queue")));
  }

  void aSessionDatabaseIsNoted() {
    const Lab lab;
    auto deps = lab.deps();
    deps.session_database = "postgresql://ar:s3cret@db.lab:5432/pychron";
    DataSourcePage page(std::move(deps));
    const QString note = page.note()->text();
    QVERIFY2(note.contains(QStringLiteral("--db")), qPrintable(note));
    QVERIFY2(note.contains(QStringLiteral("postgresql://ar@db.lab:5432/pychron")), qPrintable(note));
    QVERIFY2(!note.contains(QStringLiteral("s3cret")), qPrintable(note));
    QVERIFY(page.editor(QStringLiteral("data_folder"))->isEnabled());  // editing still allowed
  }

  // ---- in Preferences: OK/Apply, Save without checking, reopen ----------------

  void applyWritesSiteAndRecordAndOffersReopen() {
    const Lab lab;
    int reopened = 0;
    int asked = 0;
    PreferencesDialog dialog({Preferences{}, std::nullopt, std::nullopt}, {});
    dialog.add_data_source_page(
        lab.deps(), [&reopened] { ++reopened; },
        [&asked] {
          ++asked;
          return true;
        });
    DataSourcePage* page = dialog.data_source_page();
    QVERIFY(page != nullptr);
    QCOMPARE(dialog.pages()->item(dialog.pages()->count() - 1)->text(), QStringLiteral("Data source"));

    // Unchanged: Apply writes nothing and asks nothing.
    const std::string site_before = read_text(lab.site_path);
    dialog.buttons()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(asked, 0);
    QCOMPARE(read_text(lab.site_path), site_before);

    page->editor(QStringLiteral("data_folder"))->findChild<QLineEdit*>(QStringLiteral("text"))->setText(QStringLiteral("runs"));
    QVERIFY(page->changed());
    dialog.buttons()->button(QDialogButtonBox::Apply)->click();
    QVERIFY2(dialog.problem()->isHidden(), qPrintable(dialog.problem()->text()));
    auto site = setup::load_site(lab.site_path);
    QVERIFY(site);
    QCOMPARE(site->find("lab")->data, std::string("runs"));
    auto record = setup::read_install_record(lab.root);
    QVERIFY(record);
    QCOMPARE(setup::to_text(record->answers.at("data_folder")), std::string("runs"));
    QVERIFY(fs::is_directory(lab.root / "runs"));
    QCOMPARE(asked, 1);
    QCOMPARE(reopened, 1);

    // Written: the page holds what the install now uses.
    QVERIFY(!page->changed());
    dialog.buttons()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(asked, 1);
  }

  void laterDoesNotReopen() {
    const Lab lab;
    int reopened = 0;
    PreferencesDialog dialog({Preferences{}, std::nullopt, std::nullopt}, {});
    dialog.add_data_source_page(lab.deps(), [&reopened] { ++reopened; }, [] { return false; });
    dialog.data_source_page()->editor(QStringLiteral("data_folder"))->findChild<QLineEdit*>(QStringLiteral("text"))->setText(
        QStringLiteral("runs"));
    dialog.buttons()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(setup::load_site(lab.site_path)->find("lab")->data, std::string("runs"));
    QCOMPARE(reopened, 0);
  }

  void aFailedCheckOffersSaveWithoutChecking() {
    const Lab lab;
    auto deps = lab.deps();
    int opened = 0;
    deps.openers.open_database = [&opened](const std::string&, bool) -> Result<std::string> {
      ++opened;
      return fail(ErrorKind::Io, "the server went away");
    };
    int reopened = 0;
    PreferencesDialog dialog({Preferences{}, std::nullopt, std::nullopt}, {});
    dialog.add_data_source_page(std::move(deps), [&reopened] { ++reopened; }, [] { return true; });
    QVERIFY(dialog.save_without_checking()->isHidden());
    const std::string site_before = read_text(lab.site_path);
    dialog.data_source_page()->editor(QStringLiteral("data_folder"))->findChild<QLineEdit*>(QStringLiteral("text"))->setText(
        QStringLiteral("runs"));
    dialog.buttons()->button(QDialogButtonBox::Apply)->click();
    QVERIFY(opened > 0);
    QVERIFY(!dialog.problem()->isHidden());
    QVERIFY2(dialog.problem()->text().contains(QStringLiteral("the server went away")), qPrintable(dialog.problem()->text()));
    QCOMPARE(read_text(lab.site_path), site_before);
    QCOMPARE(reopened, 0);
    QVERIFY(!dialog.save_without_checking()->isHidden());

    const int opened_before = opened;
    dialog.save_without_checking()->click();
    QCOMPARE(opened, opened_before);  // not checked
    QCOMPARE(setup::load_site(lab.site_path)->find("lab")->data, std::string("runs"));
    QVERIFY(dialog.problem()->isHidden());
    QVERIFY(dialog.save_without_checking()->isHidden());
    QCOMPARE(reopened, 1);
  }
};

QTEST_MAIN(TestDataSourcePage)
#include "test_data_source_page.moc"
