// The setup wizard over the shipped profiles: data reduction (local and
// server, Test connection), instruments (simulation skips the connection
// page, validation on the page), an existing install filled in again, File >
// Installations, and the data-reduction main window.

#include <QtTest>

#include <QAbstractButton>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QTextBrowser>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "data_main_window.hpp"
#include "installations_dialog.hpp"
#include "pychron/processing/record_source.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/site.hpp"
#include "setup_support.hpp"
#include "setup_wizard.hpp"

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

// Walks forward to `id` (or until the wizard stops moving).
bool walk_to(SetupWizard& w, int id) {
  for (int guard = 0; guard < 20 && w.currentId() != id; ++guard) {
    const int before = w.currentId();
    w.next();
    if (w.currentId() == before) return false;
  }
  return w.currentId() == id;
}

std::vector<int> path_of(SetupWizard& w) {
  std::vector<int> ids{w.currentId()};
  for (int guard = 0; guard < 20 && w.currentId() != SetupWizard::kReady; ++guard) {
    w.next();
    if (w.currentId() == ids.back()) break;
    ids.push_back(w.currentId());
  }
  return ids;
}

QRadioButton* radio(QWidget* editor, const char* name) { return editor->findChild<QRadioButton*>(QString::fromLatin1(name)); }

}  // namespace

class TestSetupWizard : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;
  setup::ProfileLibrary library_ = shipped();

  fs::path dir(const char* name) const { return fs::path(tmp_.path().toStdString()) / name; }

 private slots:
  void welcomeOffersDataReductionFirst() {
    SetupWizard w(library_, {dir("site-a.toml"), {}, {}, {}});
    w.restart();
    QCOMPARE(w.chosen(), QStringLiteral("data-reduction"));
    for (const char* p : {"argus", "helix", "ngx"}) {
      w.choose(QString::fromLatin1(p));
      QCOMPARE(w.chosen(), QString::fromLatin1(p));
    }
    // Fragments are never offered.
    w.choose(QStringLiteral("lab-common"));
    QCOMPARE(w.chosen(), QStringLiteral("ngx"));
  }

  void localDataReductionInstallsCreatesItsDatabaseAndIsRecorded() {
    const fs::path site = dir("site-b.toml");
    SetupWizard w(library_, {site, database_opener(), {}, {}});
    w.restart();
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kLocation));
    QCOMPARE(w.name_edit()->text(), QStringLiteral("data-reduction"));
    QVERIFY(w.root_edit()->text().endsWith(QStringLiteral("Pychron")));
    w.root_edit()->setText(QString::fromStdString(dir("dr").string()));
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kFirstGroup));  // Data
    QVERIFY(w.is_shown(QStringLiteral("data_source")));
    QVERIFY(!w.is_shown(QStringLiteral("db_host")));
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kReady));
    QVERIFY2(w.ready_error()->isHidden(), qPrintable(w.ready_error()->text()));
    QVERIFY(w.summary()->toPlainText().contains(QStringLiteral("pychron.db")));
    w.next();  // Install
    QCOMPARE(w.currentId(), int(SetupWizard::kDone));
    QVERIFY(w.installed());
    QCOMPARE(QString::fromStdString(w.installed()->kind), QStringLiteral("data_reduction"));
    QVERIFY(fs::exists(dir("dr") / "README.md"));
    QVERIFY(!fs::exists(dir("dr") / ".pychron" / "credentials.toml"));
    if (database_opener()) QVERIFY(fs::exists(dir("dr") / "data" / "pychron.db"));
    QVERIFY(!setup::any_fail(w.checks()));
    QVERIFY(w.open_now());
    auto loaded = setup::load_site(site);
    QVERIFY(loaded);
    QCOMPARE(QString::fromStdString(loaded->default_install), QStringLiteral("data-reduction"));
    QVERIFY(w.done_report()->toPlainText().contains(QStringLiteral("data-reduction")));
  }

  void serverAsksForTheConnectionAndTestsItWithThePassword() {
    std::vector<std::pair<std::string, bool>> opened;
    bool succeed = true;
    SetupWizard::OpenDatabase fake = [&](const std::string& url, bool create) -> Result<std::string> {
      opened.emplace_back(url, create);
      if (!succeed) return fail(ErrorKind::NotConnected, "could not connect to server: Connection refused\ndetail");
      return std::string("schema version 7");
    };
    SetupWizard w(library_, {dir("site-c.toml"), fake, {}, {}});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("dr-server").string()));
    w.next();
    QVERIFY(w.test_button() != nullptr);
    QVERIFY(w.test_button()->isHidden() || !w.test_button()->parentWidget()->isVisibleTo(w.currentPage()));
    radio(w.editor(QStringLiteral("data_source")), "data_source-server")->click();
    QVERIFY(w.is_shown(QStringLiteral("db_host")));
    QVERIFY(w.test_button()->parentWidget()->isVisibleTo(w.currentPage()));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("db_host")))->setText(QStringLiteral("db.lab.edu"));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("db_password")))->setText(QStringLiteral("p@ss word"));
    w.test_button()->click();
    QCOMPARE(opened.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(opened[0].first),
             QStringLiteral("postgresql://pychron:p%40ss%20word@db.lab.edu:5432/pychron"));
    QVERIFY(!opened[0].second);  // never migrated from a test
    QVERIFY(w.test_result()->text().contains(QStringLiteral("schema version 7")));
    succeed = false;
    w.test_button()->click();
    QCOMPARE(w.test_result()->text(), QStringLiteral("could not connect to server: Connection refused"));

    succeed = true;
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kReady));
    QVERIFY(!w.summary()->toPlainText().contains(QStringLiteral("p@ss")));  // the password is never shown
    QVERIFY(w.summary()->toPlainText().contains(QStringLiteral("postgresql://pychron@db.lab.edu:5432/pychron")));
    opened.clear();
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kDone));
    // Nothing is created on a server; the doctor opens it with the password.
    QCOMPARE(opened.size(), std::size_t{1});
    QVERIFY(opened[0].first.find("p%40ss%20word") != std::string::npos);
    QVERIFY(!opened[0].second);
    QVERIFY(fs::exists(dir("dr-server") / ".pychron" / "credentials.toml"));
    QCOMPARE(QString::fromStdString(w.installed()->database), QStringLiteral("postgresql://pychron@db.lab.edu:5432/pychron"));
  }

  // Install defaults: an instrument's database is made, then given the seed.
  void anInstrumentInstallMakesAndSeedsItsDatabase() {
    std::vector<std::string> calls;
    SetupWizard::OpenDatabase open = [&](const std::string& url, bool create) -> Result<std::string> {
      calls.push_back(std::string(create ? "create " : "open ") + url);
      return std::string("schema version 7");
    };
    bool succeed = true;
    SetupWizard::SeedDatabase seed = [&](const std::string& url, const fs::path& file, bool migrate) -> Result<std::string> {
      calls.push_back(std::string(migrate ? "seed+migrate " : "seed ") + url + " " + file.filename().string());
      if (!succeed) return fail(ErrorKind::Config, "seed.toml: syntax error");
      return std::string("seeded 1 project");
    };
    const fs::path root = dir("argus-seeded");
    const std::string db = "sqlite:" + (root / "data" / "pychron.db").generic_string();
    {
      SetupWizard w(library_, {dir("site-s.toml"), open, QStringLiteral("argus"), {}, seed});
      w.restart();
      w.next();
      w.root_edit()->setText(QString::fromStdString(root.string()));
      QVERIFY(walk_to(w, SetupWizard::kReady));
      w.next();
      QCOMPARE(w.currentId(), int(SetupWizard::kDone));
      // The seed makes the local database; the doctor then opens it.
      QCOMPARE(calls, (std::vector<std::string>{"seed+migrate " + db + " seed.toml", "open " + db}));
      QVERIFY(fs::is_directory(root / "data"));
      QCOMPARE(QString::fromStdString(w.installed()->database), QString::fromStdString(db));
      QVERIFY(!setup::any_fail(w.checks()));
      bool said = false;
      for (const auto& c : w.checks()) said |= c.name == "seed" && c.status == setup::Check::Status::Ok && c.detail == "seeded 1 project";
      QVERIFY(said);
    }
    // A seed that cannot run is a warning on the last page, not a failed install.
    succeed = false;
    calls.clear();
    const fs::path other = dir("helix-seed-fails");
    SetupWizard w(library_, {dir("site-s2.toml"), open, QStringLiteral("helix"), {}, seed});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(other.string()));
    QVERIFY(walk_to(w, SetupWizard::kReady));
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kDone));
    QVERIFY(w.installed().has_value());
    QVERIFY(!setup::any_fail(w.checks()));
    bool warned = false;
    for (const auto& c : w.checks()) {
      if (c.name != "seed") continue;
      warned = c.status == setup::Check::Status::Warn && c.detail.find("seed.toml: syntax error") != std::string::npos &&
               c.hint.find("elctl entry seed ") != std::string::npos;
    }
    QVERIFY(warned);
  }

  void aDataReductionInstallIsNotSeededAndNoSeederIsNoSeed() {
    int seeded = 0;
    SetupWizard::SeedDatabase seed = [&](const std::string&, const fs::path&, bool) -> Result<std::string> {
      ++seeded;
      return std::string("seeded");
    };
    SetupWizard::OpenDatabase open = [](const std::string&, bool) -> Result<std::string> { return std::string("schema version 7"); };
    {
      SetupWizard w(library_, {dir("site-t.toml"), open, QStringLiteral("data-reduction"), {}, seed});
      w.restart();
      w.next();
      w.root_edit()->setText(QString::fromStdString(dir("dr-not-seeded").string()));
      QVERIFY(walk_to(w, SetupWizard::kDone));
      QCOMPARE(seeded, 0);
    }
    SetupWizard w(library_, {dir("site-t2.toml"), open, QStringLiteral("argus"), {}, {}});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("argus-no-seeder").string()));
    QVERIFY(walk_to(w, SetupWizard::kDone));
    for (const auto& c : w.checks()) QVERIFY(c.name != "seed");
  }

  void simulationSkipsTheConnectionPage() {
    SetupWizard w(library_, {dir("site-d.toml"), {}, QStringLiteral("argus"), {}});
    w.restart();
    QCOMPARE(w.chosen(), QStringLiteral("argus"));
    w.next();
    QCOMPARE(w.name_edit()->text(), QStringLiteral("argus"));
    w.root_edit()->setText(QString::fromStdString(dir("argus").string()));
    const auto ids = path_of(w);
    for (int id : ids) QVERIFY(id < SetupWizard::kFirstGroup || id >= SetupWizard::kReady || w.page(id)->title() != QStringLiteral("Instrument connection"));
    QCOMPARE(ids.back(), int(SetupWizard::kReady));
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kDone));
    QVERIFY(w.installed()->simulation);
    QVERIFY(fs::exists(dir("argus") / "spectrometer.toml"));
    QVERIFY(!setup::any_fail(w.checks()));
  }

  void aRealInstrumentAsksForItsConnectionAndChecksIt() {
    SetupWizard w(library_, {dir("site-e.toml"), {}, QStringLiteral("ngx"), {}});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("ngx").string()));
    w.next();
    QCOMPARE(w.currentPage()->title(), QStringLiteral("Simulation"));
    qobject_cast<QCheckBox*>(w.editor(QStringLiteral("simulation")))->setChecked(false);
    QVERIFY(w.is_shown(QStringLiteral("ngx_host")));
    QVERIFY(!w.is_shown(QStringLiteral("ngx_password")));  // only with a user
    w.next();  // the connection comes before the detectors
    QCOMPARE(w.currentPage()->title(), QStringLiteral("Instrument connection"));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("ngx_host")))->setText(QStringLiteral("not a host!"));
    const int here = w.currentId();
    w.next();
    QCOMPARE(w.currentId(), here);  // refused
    QVERIFY(w.error_for(QStringLiteral("ngx_host")).contains(QStringLiteral("not a host")));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("ngx_host")))->setText(QStringLiteral("10.0.0.20"));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("ngx_user")))->setText(QStringLiteral("lab"));
    QVERIFY(w.is_shown(QStringLiteral("ngx_password")));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("ngx_password")))->setText(QStringLiteral("secret"));
    QVERIFY(walk_to(w, SetupWizard::kReady));
    QVERIFY(!w.summary()->toPlainText().contains(QStringLiteral("secret")));
    w.next();
    QCOMPARE(w.currentId(), int(SetupWizard::kDone));
    QVERIFY(!w.installed()->simulation);
    std::ifstream in(dir("ngx") / "spectrometer.toml");
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    QVERIFY(text.find("10.0.0.20") != std::string::npos);
    QVERIFY(text.find("secret") == std::string::npos);  // in spectrometer.local.toml only
  }

  void theInstrumentConnectionPageCanTestTheConnection() {
    std::string tried_host;
    auto fake = [&](const setup::ProfileLibrary&, const setup::ResolvedProfile& profile,
                    const setup::Answers& a) -> Result<std::string> {
      tried_host = setup::to_text(a.at("ngx_host"));
      if (tried_host == "10.9.9.9") return fail(ErrorKind::NotConnected, "connection refused\nmore");
      return "connected: spec: isotopx_ngx (" + profile.top.name + ")";
    };
    SetupWizard w(library_, {dir("site-h.toml"), {}, QStringLiteral("ngx"), fake});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("ngx-test").string()));
    w.next();
    qobject_cast<QCheckBox*>(w.editor(QStringLiteral("simulation")))->setChecked(false);
    w.next();
    QCOMPARE(w.currentPage()->title(), QStringLiteral("Instrument connection"));
    QVERIFY(w.instrument_test_button() != nullptr);
    QVERIFY(w.instrument_test_button()->isVisibleTo(w.currentPage()));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("ngx_host")))->setText(QStringLiteral("10.0.0.20"));
    w.instrument_test_button()->click();
    QCOMPARE(QString::fromStdString(tried_host), QStringLiteral("10.0.0.20"));
    QCOMPARE(w.instrument_test_result()->text(), QStringLiteral("connected: spec: isotopx_ngx (ngx)"));
    qobject_cast<QLineEdit*>(w.editor(QStringLiteral("ngx_host")))->setText(QStringLiteral("10.9.9.9"));
    w.instrument_test_button()->click();
    QCOMPARE(w.instrument_test_result()->text(), QStringLiteral("connection refused"));
    // Data reduction has no instrument to test.
    SetupWizard dr(library_, {dir("site-i.toml"), {}, {}, fake});
    dr.restart();
    dr.next();
    dr.next();
    QVERIFY(dr.instrument_test_button() == nullptr);
  }

  void theExtractionLineIsTheStarterOrTheLabsOwnFiles() {
    const fs::path examples = setup::find_resources().examples;
    SetupWizard w(library_, {dir("site-j.toml"), {}, QStringLiteral("helix"), {}});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("helix-own").string()));
    w.next();
    while (w.currentPage()->title() != QStringLiteral("Extraction line")) QVERIFY(walk_to(w, w.nextId()));
    QVERIFY(radio(w.editor(QStringLiteral("line_source")), "line_source-starter")->isChecked());
    QVERIFY(!w.is_shown(QStringLiteral("line_file")));
    radio(w.editor(QStringLiteral("line_source")), "line_source-import")->click();
    QVERIFY(w.is_shown(QStringLiteral("line_file")));
    QVERIFY(w.is_shown(QStringLiteral("canvas_file")));
    // Required once asked.
    const int here = w.currentId();
    w.next();
    QCOMPARE(w.currentId(), here);
    QVERIFY(!w.error_for(QStringLiteral("line_file")).isEmpty());
    auto set_path = [&](const char* id, const fs::path& p) {
      w.editor(QString::fromLatin1(id))->findChild<QLineEdit*>()->setText(QString::fromStdString(p.string()));
    };
    set_path("line_file", examples / "extraction_line.toml");
    set_path("canvas_file", dir("nowhere.toml"));
    QVERIFY(walk_to(w, SetupWizard::kReady));
    QVERIFY(w.ready_error()->text().contains(QStringLiteral("canvas.toml: cannot read")));
    QVERIFY(!w.button(QWizard::CommitButton)->isEnabled());
    w.back();
    set_path("canvas_file", examples / "canvas.toml");
    QVERIFY(walk_to(w, SetupWizard::kReady));
    QVERIFY2(w.ready_error()->isHidden(), qPrintable(w.ready_error()->text()));
    QVERIFY(walk_to(w, SetupWizard::kDone));
    QVERIFY(fs::exists(dir("helix-own") / "extraction_line.toml"));
    QVERIFY(!setup::any_fail(w.checks()));
  }

  void aLegacySetupFolderIsConvertedAndItsNotesShownBeforeWriting() {
    // A synthetic legacy setupfiles folder: two valves on a Qtegra actuator.
    const fs::path legacy = dir("setupfiles");
    fs::create_directories(legacy / "extractionline");
    fs::create_directories(legacy / "devices");
    std::ofstream(legacy / "extractionline" / "valves.yaml")
        << "- name: A\n  address: Valve 1_1 Set\n  query_state: false\n- name: B\n  address: Valve 1_2 Set\n";
    std::ofstream(legacy / "devices" / "switch_controller.cfg")
        << "[General]\ntype = QtegraGPActuator\n[Communications]\nhost = localhost\nport = 1069\n";

    SetupWizard w(library_, {dir("site-k.toml"), {}, QStringLiteral("helix"), {}});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("helix-legacy").string()));
    w.next();
    while (w.currentPage()->title() != QStringLiteral("Extraction line")) QVERIFY(walk_to(w, w.nextId()));
    QVERIFY(!w.is_shown(QStringLiteral("legacy_folder")));
    radio(w.editor(QStringLiteral("line_source")), "line_source-legacy")->click();
    QVERIFY(w.is_shown(QStringLiteral("legacy_folder")));
    QVERIFY(!w.is_shown(QStringLiteral("line_file")));
    const int here = w.currentId();
    w.next();
    QCOMPARE(w.currentId(), here);
    QVERIFY(w.error_for(QStringLiteral("legacy_folder")).contains(QStringLiteral("folder")));
    w.editor(QStringLiteral("legacy_folder"))->findChild<QLineEdit*>()->setText(QString::fromStdString(legacy.string()));
    QVERIFY(walk_to(w, SetupWizard::kReady));
    QVERIFY2(w.ready_error()->isHidden(), qPrintable(w.ready_error()->text()));
    const QString summary = w.summary()->toPlainText();
    QVERIFY2(summary.contains(QStringLiteral("query_state not carried over")), qPrintable(summary));
    // The actuator now converts (qtegra_valves); its notes still show first.
    QVERIFY2(summary.contains(QStringLiteral("Qtegra takes one client")), qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("written as udp")), qPrintable(summary));
    QVERIFY(walk_to(w, SetupWizard::kDone));
    std::ifstream in(dir("helix-legacy") / "extraction_line.toml");
    const std::string line((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    QVERIFY(line.find("address = \"Valve 1_2 Set\"") != std::string::npos);
    QVERIFY(line.find("kind = \"qtegra_valves\"") != std::string::npos);
    QVERIFY(fs::exists(dir("helix-legacy") / "canvas.toml"));
    QVERIFY(!setup::any_fail(w.checks()));
  }

  void anExistingInstallIsFilledInAndAnotherProfileIsRefused() {
    const fs::path site = dir("site-f.toml");
    {
      SetupWizard w(library_, {site, {}, QStringLiteral("helix"), {}});
      w.restart();
      w.next();
      w.root_edit()->setText(QString::fromStdString(dir("helix").string()));
      w.next();
      while (w.currentPage()->title() != QStringLiteral("Detectors")) QVERIFY(walk_to(w, w.nextId()));
      qobject_cast<QLineEdit*>(w.editor(QStringLiteral("peak_window")))->setText(QStringLiteral("0.08"));
      QVERIFY(walk_to(w, SetupWizard::kDone));
    }
    {
      SetupWizard w(library_, {site, {}, QStringLiteral("argus"), {}});
      w.restart();
      w.next();
      w.root_edit()->setText(QString::fromStdString(dir("helix").string()));
      w.next();
      QCOMPARE(w.currentId(), int(SetupWizard::kLocation));
      QVERIFY(w.location_note()->text().contains(QStringLiteral("helix")));
    }
    SetupWizard w(library_, {site, {}, QStringLiteral("helix"), {}});
    w.restart();
    w.next();
    w.root_edit()->setText(QString::fromStdString(dir("helix").string()));
    w.next();
    QVERIFY(w.location_note()->text().contains(QStringLiteral("already holds")));
    QCOMPARE(qobject_cast<QLineEdit*>(w.editor(QStringLiteral("peak_window")))->text(), QStringLiteral("0.08"));
    QVERIFY(walk_to(w, SetupWizard::kReady));
    QVERIFY(w.summary()->toPlainText().contains(QStringLiteral("0 file(s) to write")));
    QVERIFY(walk_to(w, SetupWizard::kDone));
  }

  void installationsListsSwitchesDefaultsAndForgets() {
    const fs::path site = dir("site-g.toml");
    setup::SiteConfig config;
    config.upsert({"one", "instrument", "argus", dir("one"), "extraction_line.toml", "canvas.toml", "spectrometer.toml", "data", "", true});
    config.upsert({"two", "data_reduction", "data-reduction", dir("two"), "", "", "", "data", "sqlite:/x.db", false});
    config.default_install = "one";
    QVERIFY(setup::save_site(config, site));
    fs::create_directories(dir("two"));

    InstallationsDialog d(site, &library_, {}, "one");
    QCOMPARE(d.list()->count(), 2);
    QVERIFY(d.list()->item(0)->text().contains(QStringLiteral("opens by default")));
    QVERIFY(d.list()->item(0)->text().contains(QStringLiteral("open now")));
    QVERIFY(!d.open_button()->isEnabled());  // already open
    QVERIFY(!d.remove_button()->isEnabled());
    d.list()->setCurrentRow(1);
    QVERIFY(d.open_button()->isEnabled());
    d.default_button()->click();
    QCOMPARE(QString::fromStdString(setup::load_site(site)->default_install), QStringLiteral("two"));
    d.list()->setCurrentRow(1);
    d.remove_button()->click();
    QCOMPARE(d.list()->count(), 1);
    QVERIFY(fs::exists(dir("two")));  // files are never removed
    QVERIFY(setup::load_site(site)->default_install.empty());

    bool ran = false;
    d.run_wizard = [&](InstallationsDialog&) -> std::optional<setup::SiteInstall> {
      ran = true;
      return std::nullopt;  // cancelled
    };
    d.new_button()->click();
    QVERIFY(ran);
    QVERIFY(!d.to_open());
    d.list()->setCurrentRow(0);
    QVERIFY(!d.open_button()->isEnabled());
  }

  void theSelfTestPassesFromTheSourceTree() {
    std::ostringstream out;
    QCOMPARE(self_test(out), 0);
    const QString text = QString::fromStdString(out.str());
    QVERIFY2(!text.contains(QStringLiteral("FAIL")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("OK    profile data-reduction")));
    QVERIFY(text.contains(QStringLiteral("setup wizard builds")));
  }

  void theInstallerIconsAreRenderedAtEverySize() {
    const fs::path out = dir("icons");
    std::ostringstream log;
    QCOMPARE(write_icons(out, log), 0);
    for (const int size : {16, 24, 32, 48, 64, 128, 256, 512, 1024}) {
      const QImage img(QString::fromStdString((out / ("pychron-" + std::to_string(size) + ".png")).string()));
      QCOMPARE(img.width(), size);
      QCOMPARE(img.height(), size);
      QVERIFY(img.hasAlphaChannel());
      QCOMPARE(qAlpha(img.pixel(0, 0)), 0);                 // rounded corner
      QVERIFY(qAlpha(img.pixel(size / 2, size / 2)) > 0);  // the tile
    }
  }

  void aDataReductionWindowIsTheBrowserAlone() {
    fs::create_directories(dir("records"));
    processing::RecordDirectorySource records(dir("records"));
    processing::PresetStore presets(dir("presets"), dir("lab-presets"));
    DataMainWindow w(records, presets, QStringLiteral("data-reduction"));
    QVERIFY(w.browser() != nullptr);
    QCOMPARE(w.centralWidget(), static_cast<QWidget*>(w.browser()));
    QVERIFY(!w.browser()->isWindow());
    QVERIFY(w.windowTitle().contains(QStringLiteral("data-reduction")));
    QVERIFY(!w.installations_action()->isVisible());
    bool asked = false;
    w.set_installations_handler([&] { asked = true; });
    QVERIFY(w.installations_action()->isVisible());
    w.installations_action()->trigger();
    QVERIFY(asked);
  }
};

QTEST_MAIN(TestSetupWizard)
#include "test_setup_wizard.moc"
