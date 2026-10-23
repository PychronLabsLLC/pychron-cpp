// The Publish dialog (publication workflow design, section 6.1) on a seeded
// store: the release section, the destinations and their tabs, the refusals
// that keep Run off, and a run of the plan with a result per destination.
// EarthBank is the in-memory fake (earthbank_fixture.hpp): nothing here
// reaches EarthBank.

#include <memory>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "earthbank_fixture.hpp"
#include "entry_bridge.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/arar/store_loader.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/embargo.hpp"
#include "pychron/publish/publication.hpp"
#include "pychron/publish/release.hpp"
#include "release_seed.hpp"
// The dialog's EarthBank tab; keyword-safe headers (Q_SIGNALS), as this
// suite is built with QT_NO_KEYWORDS.
#include "earthbank_publish_panel.hpp"
#include "publish_actions.hpp"
#include "publish_dialog.hpp"
#include "pychron/earthbank/secret_store.hpp"

namespace ps = pychron::persistence;
namespace pp = pychron::processing;
namespace pb = pychron::publish;
using pb::Destination;
using pb::StepState;
using Action = pychron::ui::EarthbankPublishPanel::Action;
using pychron::ui::EarthbankPublishPanel;
using pychron::ui::EarthbankSetup;
using pychron::ui::EntryBridge;
using pychron::ui::PublishActions;
using pychron::ui::PublishDialog;

namespace {

constexpr int kWaitMs = 20000;

bool has_tab(const PublishDialog& d, const QString& title) {
  for (int i = 0; i < d.tabs()->count(); ++i)
    if (d.tabs()->tabText(i) == title) return true;
  return false;
}

// What run_finished said, each time it said it.
struct Finished {
  int count = 0;
  std::vector<pb::StepResult> results;
  bool release_ok = false;
};
std::shared_ptr<Finished> watch(PublishDialog& d) {
  auto got = std::make_shared<Finished>();
  QObject::connect(&d, &PublishDialog::run_finished, &d, [got](const std::vector<pb::StepResult>& results, bool release_ok) {
    ++got->count;
    got->results = results;
    got->release_ok = release_ok;
  });
  return got;
}

void set_action(EarthbankPublishPanel& panel, Action action) {
  panel.action()->setCurrentIndex(panel.action()->findData(static_cast<int>(action)));
}

std::size_t record_posts(const pychron::earthbank::testing::FakeEarthBank& bank) {
  return bank.count("POST", "/api/arar/") + bank.count("POST", "/api/core/");
}

PublishDialog::AskRelease named(const std::string& name) {
  return [name](const QStringList&, std::size_t) -> std::optional<pb::ReleaseSpec> {
    pb::ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
    return s;
  };
}

// A spec create_release refuses (no licence), which check_plan does not judge.
PublishDialog::AskRelease unmakeable(const std::string& name) {
  return [name](const QStringList&, std::size_t) -> std::optional<pb::ReleaseSpec> {
    pb::ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
    s.licence.clear();
    return s;
  };
}

QString state_text(const PublishDialog& d, int row) { return d.results()->item(row, 1)->text(); }

}  // namespace

class TestPublishDialog : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir dir_;
  std::string url_;
  QString settings_file_;
  std::unique_ptr<ps::IStore> store_;  // the seeding client's
  pb::testing::ReleaseSeed seed_;

  QStringList analyses() const {
    return {QString::fromStdString(seed_.analysis1.str()), QString::fromStdString(seed_.analysis2.str()),
            QString::fromStdString(seed_.analysis3.str())};
  }

  pb::ReleaseSpec spec(const std::string& name) const {
    pb::ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
    s.authors = {{"J. Ross", std::nullopt, std::nullopt}};
    for (const QString& a : analyses()) s.analyses.push_back(a.toStdString());
    s.options_json = pb::release_options_json(pb::ReleaseOptions{});
    s.reduction_version = "arar-1";
    s.software_version = "pychron-cpp/test";
    return s;
  }

  ps::Uuid create(const std::string& name) {
    auto created = pb::create_release(*store_, seed_.reducer(), spec(name));
    if (!created) qFatal("release: %s", pychron::to_string(created.error()).c_str());
    return created->release;
  }

  // A release published straight in the store, as if from a build.
  ps::Uuid published(const std::string& name, bool make_public) {
    const ps::Uuid r = create(name);
    auto ok = pb::publish_release(*store_, seed_.reducer(), r, pychron::sha256(std::string_view{"x"}), make_public);
    if (!ok) qFatal("publish: %s", pychron::to_string(ok.error()).c_str());
    return r;
  }

  ps::ReleaseRow release_row(const std::string& name) {
    auto row = pb::find_release(*store_, name);
    if (!row) qFatal("find: %s", pychron::to_string(row.error()).c_str());
    return *row;
  }

  EarthbankSetup earthbank() const {
    EarthbankSetup setup;
    setup.secrets = [] { return std::shared_ptr<pychron::earthbank::ISecretStore>(pychron::earthbank::make_memory_secret_store()); };
    setup.profiles_file = dir_.filePath(QStringLiteral("earthbank-profiles.toml")).toStdString();
    return setup;
  }

  PublishDialog::SettingsFactory settings() const {
    return [file = settings_file_] { return std::make_unique<QSettings>(file, QSettings::IniFormat); };
  }

  // A dialog on its own bridge and source, as PublishActions makes it.
  struct Open {
    std::unique_ptr<pp::StoreSource> source;
    std::unique_ptr<EntryBridge> bridge;
    std::unique_ptr<PublishDialog> dialog;
  };
  Open open() { return open(earthbank()); }
  Open open(EarthbankSetup setup) {
    Open o;
    auto source = pp::StoreSource::open(ps::StoreConfig{url_, false}, pychron::arar::store_source_options());
    if (!source) qFatal("source: %s", pychron::to_string(source.error()).c_str());
    o.source = std::move(*source);
    auto bridge = EntryBridge::open({url_, "tester", "test-host", "publisher", "pychron-ui/test"});
    if (!bridge) qFatal("bridge: %s", pychron::to_string(bridge.error()).c_str());
    o.bridge = std::move(*bridge);
    o.dialog = std::make_unique<PublishDialog>(*o.bridge, *o.source, url_, std::move(setup), settings());
    return o;
  }

  // A published public release uploaded through the dialog, the first record
  // of it lost on the way: the run ends with that record in doubt and the
  // EarthBank tab's in-doubt buttons shown.
  ps::Uuid upload_leaving_in_doubt(pychron::ui::test::EarthbankWorld& w, Open& o) {
    const ps::Uuid r = published("r", true);
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    if (panel == nullptr) qFatal("no EarthBank panel");
    panel->ask_confirm = [](const QString&) { return true; };
    set_action(*panel, Action::Upload);
    panel->allow_unresolved()->setChecked(true);
    if (!QTest::qWaitFor([&] { return panel->table()->rowCount() == 3; }, kWaitMs)) qFatal("rows");
    if (!QTest::qWaitFor([&] { return o.dialog->run_button()->isEnabled(); }, kWaitMs)) qFatal("run off");
    w.bank.lose_next("POST", "/api/arar/ArArAliquot");  // never saved, and no answer
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    if (!QTest::qWaitFor([&] { return got->count == 1; }, kWaitMs)) qFatal("run did not finish");
    if (!QTest::qWaitFor([&] { return panel->ready(); }, kWaitMs)) qFatal("panel not ready");
    if (!panel->not_there_button()->isVisibleTo(panel) || !panel->not_there_button()->isEnabled())
      qFatal("no record in doubt: %s", qPrintable(panel->result_pane()->toPlainText()));
    return r;
  }

 private Q_SLOTS:
  void init() {
    QVERIFY(dir_.isValid());
    static int n = 0;
    ++n;
    url_ = "sqlite:" + dir_.filePath(QStringLiteral("store-%1.db").arg(n)).toStdString();
    settings_file_ = dir_.filePath(QStringLiteral("settings-%1.ini").arg(n));
    auto s = ps::open_store(ps::StoreConfig{url_, true});
    QVERIFY2(s.has_value(), s ? "" : pychron::to_string(s.error()).c_str());
    store_ = std::move(*s);
    auto seeded = pb::testing::seed_release_store(*store_);
    QVERIFY2(seeded.has_value(), seeded ? "" : pychron::to_string(seeded.error()).c_str());
    seed_ = *seeded;
  }

  void cleanup() { store_.reset(); }

  void OpenNewPrefillsSelection() {
    Open o = open();
    o.dialog->open_new(analyses());
    o.dialog->show();
    QVERIFY(o.dialog->new_radio()->isChecked());
    QVERIFY2(o.dialog->members()->text().contains(QStringLiteral("3 analyses")), qPrintable(o.dialog->members()->text()));
    // How many of them are under embargo now is read from the store.
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->members()->text().contains(QStringLiteral("0 under embargo")), kWaitMs);
    QVERIFY(!o.dialog->release_combo()->isEnabled());
  }

  void OpenExistingSelectsRelease() {
    create("a");
    const ps::Uuid r = create("r");
    Open o = open();
    o.dialog->open_existing(r);
    o.dialog->show();
    QVERIFY(o.dialog->existing_radio()->isChecked());
    QTRY_COMPARE_WITH_TIMEOUT(o.dialog->release_combo()->currentData().toString(), QString::fromStdString(r.str()),
                              kWaitMs);
    QCOMPARE(o.dialog->release_combo()->count(), 2);
    QVERIFY2(o.dialog->release_combo()->currentText().contains(QStringLiteral("r@v1")),
             qPrintable(o.dialog->release_combo()->currentText()));
    const auto plan = o.dialog->plan();
    QVERIFY(plan.has_value());
    const auto* existing = std::get_if<pb::ExistingRelease>(&plan->release);
    QVERIFY(existing != nullptr);
    QCOMPARE(existing->name_or_uuid, r.str());
  }

  void TabsFollowCheckboxes() {
    Open o = open();
    o.dialog->open_new(analyses());
    o.dialog->show();
    QCOMPARE(o.dialog->tabs()->count(), 0);  // default: none ticked
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    QVERIFY(has_tab(*o.dialog, QStringLiteral("EarthBank")));
    QVERIFY(o.dialog->earthbank_panel() != nullptr);
    o.dialog->destination(Destination::Folder)->setChecked(true);
    QCOMPARE(o.dialog->tabs()->count(), 2);
    QCOMPARE(o.dialog->tabs()->tabText(0), QStringLiteral("Release folder"));  // run order, not tick order
    o.dialog->destination(Destination::EarthBank)->setChecked(false);
    QVERIFY(!has_tab(*o.dialog, QStringLiteral("EarthBank")));
    QCOMPARE(o.dialog->tabs()->count(), 1);
  }

  void DoiAndHostedDisabled() {
    Open o = open();
    for (Destination d : {Destination::Doi, Destination::Hosted}) {
      QVERIFY(!o.dialog->destination(d)->isEnabled());
      QVERIFY(!o.dialog->destination(d)->isChecked());
      QCOMPARE(o.dialog->destination(d)->toolTip(), QStringLiteral("Not built yet"));
    }
  }

  void RefusalDisablesRun() {
    const ps::Uuid r = create("r");  // a draft
    Open o = open();
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    panel->action()->setCurrentIndex(panel->action()->findData(static_cast<int>(EarthbankPublishPanel::Action::Upload)));
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->refusals()->text().contains(QStringLiteral("published")), kWaitMs);
    QVERIFY(!o.dialog->run_button()->isEnabled());
    const auto plan = o.dialog->plan();
    QVERIFY(plan && plan->earthbank && plan->earthbank->mode == pb::EarthBankMode::Upload);
    QVERIFY(!plan->store);
  }

  void RunEnabledWhenThePlanPasses() {
    const ps::Uuid r = create("r");
    Open o = open();
    o.dialog->open_existing(r);
    o.dialog->show();
    // An existing release with nothing ticked: a run would do nothing.
    QTest::qWait(400);  // past the check's delay: it is not the pending check that keeps Run off
    QVERIFY(!o.dialog->run_button()->isEnabled());
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    QVERIFY2(o.dialog->refusals()->text().isEmpty(), qPrintable(o.dialog->refusals()->text()));
  }

  void EmptySelectionDisablesRun() {
    Open o = open();
    o.dialog->open_new({});
    o.dialog->show();
    o.dialog->destination(Destination::Folder)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->refusals()->text().contains(QStringLiteral("no analyses")), kWaitMs);
    QVERIFY(!o.dialog->run_button()->isEnabled());
    // Details… is not offered for nothing, and nothing was made.
    QVERIFY(!o.dialog->details_button()->isEnabled());
    auto releases = store_->releases({});
    QVERIFY(releases.has_value());
    QVERIFY(releases->empty());
  }

  void DetailsNamesTheNewRelease() {
    Open o = open();
    QStringList asked;
    o.dialog->set_ask_release([&](const QStringList& chosen, std::size_t) -> std::optional<pb::ReleaseSpec> {
      asked = chosen;
      pb::ReleaseSpec s;
      s.name = "fresh";
      s.title = "Fresh";
      return s;
    });
    o.dialog->open_new(analyses());
    o.dialog->show();
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->refusals()->text().contains(QStringLiteral("name")), kWaitMs);
    QVERIFY(!o.dialog->run_button()->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->details_button()->isEnabled(), kWaitMs);
    o.dialog->details_button()->click();
    QCOMPARE(asked, analyses());
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    const auto plan = o.dialog->plan();
    QVERIFY(plan.has_value());
    const auto* fresh = std::get_if<pb::NewRelease>(&plan->release);
    QVERIFY(fresh != nullptr);
    QCOMPARE(fresh->spec.name, std::string("fresh"));
    QCOMPARE(fresh->spec.analyses.size(), std::size_t{3});
    QVERIFY(!fresh->spec.options_json.empty());
  }

  // Spec 6.2: a new release and no destination ticked: the run only makes the release.
  void NewReleaseWithNoDestinationEnablesRun() {
    Open o = open();
    o.dialog->set_ask_release([](const QStringList&, std::size_t) -> std::optional<pb::ReleaseSpec> {
      pb::ReleaseSpec s;
      s.name = "fresh";
      s.title = "Fresh";
      return s;
    });
    o.dialog->open_new(analyses());
    o.dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->details_button()->isEnabled(), kWaitMs);
    o.dialog->details_button()->click();
    for (Destination d : {Destination::Folder, Destination::Store, Destination::EarthBank, Destination::Doi,
                          Destination::Hosted})
      QVERIFY(!o.dialog->destination(d)->isChecked());
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    QVERIFY2(o.dialog->refusals()->text().isEmpty(), qPrintable(o.dialog->refusals()->text()));
  }

  void NewReleaseEarthBankTabIsReadOnly() {
    Open o = open();
    o.dialog->open_new(analyses());
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    QVERIFY(o.dialog->earthbank_panel() != nullptr);
    QVERIFY(o.dialog->earthbank_panel()->read_only());
    QVERIFY(!o.dialog->earthbank_note()->isHidden());
  }

  void ExistingReleaseFillsEarthBankTab() {
    const ps::Uuid r = create("r");
    Open o = open();
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    QVERIFY(!panel->read_only());
    QVERIFY(o.dialog->earthbank_note()->isHidden());
    // Three members in three samples, surveyed on the panel's worker.
    QTRY_COMPARE_WITH_TIMEOUT(panel->table()->rowCount(), 3, kWaitMs);
  }

  void StoreDirShownOnlyWithoutFolder() {
    Open o = open();
    o.dialog->open_new(analyses());
    o.dialog->show();
    o.dialog->destination(Destination::Store)->setChecked(true);
    QVERIFY(!o.dialog->store_dir()->isHidden());
    o.dialog->destination(Destination::Folder)->setChecked(true);
    QVERIFY(o.dialog->store_dir()->isHidden());
    o.dialog->destination(Destination::Folder)->setChecked(false);
    QVERIFY(!o.dialog->store_dir()->isHidden());
    // Without Folder and without a directory the store step is refused.
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->refusals()->text().contains(QStringLiteral("built directory")), kWaitMs);
  }

  void RemembersDestinations() {
    {
      Open o = open();
      o.dialog->destination(Destination::Folder)->setChecked(true);
      o.dialog->destination(Destination::Store)->setChecked(true);
    }
    {
      QSettings s(settings_file_, QSettings::IniFormat);
      QCOMPARE(s.value(QStringLiteral("publish/destinations")).toStringList(),
               QStringList({QStringLiteral("Release folder"), QStringLiteral("Publish in store")}));
    }
    Open o = open();
    QVERIFY(o.dialog->destination(Destination::Folder)->isChecked());
    QVERIFY(o.dialog->destination(Destination::Store)->isChecked());
    QVERIFY(!o.dialog->destination(Destination::EarthBank)->isChecked());
  }

  void ActionsKeepOneDialog() {
    const ps::Uuid r = create("r");
    QMainWindow owner;
    auto* actions = new PublishActions(&owner, url_);
    actions->set_earthbank_setup(earthbank());
    actions->set_settings_factory(settings());
    actions->open_dialog_existing(r);
    PublishDialog* first = actions->dialog();
    QVERIFY(first != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(first->isVisible(), kWaitMs);
    QVERIFY(first->existing_radio()->isChecked());
    actions->open_dialog_new(analyses());
    QCOMPARE(actions->dialog(), first);
    QVERIFY(first->new_radio()->isChecked());
    QVERIFY2(first->members()->text().contains(QStringLiteral("3 analyses")), qPrintable(first->members()->text()));
  }
  // ---- Running a plan (task 6) ----

  void RunFolderAndStorePublishes() {
    Open o = open();
    o.dialog->set_ask_release(named("fresh"));
    o.dialog->open_new(analyses());
    o.dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->details_button()->isEnabled(), kWaitMs);
    o.dialog->details_button()->click();
    const QString out = dir_.filePath(QStringLiteral("run-out"));
    o.dialog->folder_dir()->setText(out);
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::Store)->setChecked(true);
    o.dialog->store_public()->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QVERIFY(o.dialog->busy());
    QVERIFY(!o.dialog->run_button()->isEnabled());
    QVERIFY(!o.dialog->destination(Destination::Folder)->isEnabled());
    QVERIFY(!o.dialog->new_radio()->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QVERIFY(!o.dialog->busy());
    QCOMPARE(got->results.size(), std::size_t{2});
    QCOMPARE(got->results[0].destination, Destination::Folder);
    QVERIFY2(got->results[0].state == StepState::Ok, got->results[0].message.c_str());
    QCOMPARE(got->results[1].destination, Destination::Store);
    QVERIFY2(got->results[1].state == StepState::Ok, got->results[1].message.c_str());
    QCOMPARE(o.dialog->results()->rowCount(), 2);
    QCOMPARE(o.dialog->results()->item(0, 0)->text(), QStringLiteral("Release folder"));
    QCOMPARE(state_text(*o.dialog, 0), QStringLiteral("Ok"));
    QCOMPARE(o.dialog->results()->item(1, 0)->text(), QStringLiteral("Publish in store"));
    QCOMPARE(state_text(*o.dialog, 1), QStringLiteral("Ok"));
    const ps::ReleaseRow row = release_row("fresh");
    QCOMPARE(row.state, ps::ReleaseState::Published);
    QCOMPARE(row.visibility, ps::Visibility::Public);
    QVERIFY(QDir(out + QStringLiteral("/fresh-v1")).exists());
    // The release section now holds the release the run made: a second run does not make another.
    QVERIFY(o.dialog->existing_radio()->isChecked());
    QTRY_COMPARE_WITH_TIMEOUT(o.dialog->release_combo()->currentData().toString(), QString::fromStdString(row.uuid.str()),
                              kWaitMs);
  }

  void NewReleaseOnlyMakesTheRelease() {
    Open o = open();
    o.dialog->set_ask_release(named("only"));
    o.dialog->open_new(analyses());
    o.dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->details_button()->isEnabled(), kWaitMs);
    o.dialog->details_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QVERIFY(got->results.empty());
    QVERIFY(got->release_ok);
    QCOMPARE(o.dialog->results()->rowCount(), 1);
    QCOMPARE(o.dialog->results()->item(0, 0)->text(), QStringLiteral("Release"));
    QCOMPARE(state_text(*o.dialog, 0), QStringLiteral("Ok"));
    QCOMPARE(release_row("only").state, ps::ReleaseState::Draft);
  }

  void FolderFailSkipsStore() {
    create("r");
    const QString out = dir_.filePath(QStringLiteral("taken"));
    QVERIFY(QDir().mkpath(out + QStringLiteral("/r-v1")));
    {
      QFile stale(out + QStringLiteral("/r-v1/stale.txt"));
      QVERIFY(stale.open(QIODevice::WriteOnly));
      stale.write("x");
    }
    Open o = open();
    o.dialog->open_existing(release_row("r").uuid);
    o.dialog->show();
    o.dialog->folder_dir()->setText(out);
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::Store)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{2});
    QVERIFY(got->results[0].state == StepState::Failed);
    QVERIFY(!got->results[0].message.empty());
    QVERIFY(got->results[1].state == StepState::Skipped);
    QCOMPARE(got->results[1].message, std::string("skipped: Release folder failed"));
    QCOMPARE(state_text(*o.dialog, 1), QStringLiteral("Skipped"));
    QCOMPARE(o.dialog->results()->item(1, 2)->text(), QStringLiteral("skipped: Release folder failed"));
    QCOMPARE(release_row("r").state, ps::ReleaseState::Draft);
  }

  void XlsxFromDraftAgainstFakeEarthBank() {
    pychron::ui::test::EarthbankWorld w;  // no profile: the workbooks need none
    const ps::Uuid r = create("r");
    Open o = open(w.setup());
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    set_action(*panel, Action::ExportXlsx);
    const QString folder = dir_.filePath(QStringLiteral("workbooks"));
    panel->folder()->setText(folder);
    QTRY_COMPARE_WITH_TIMEOUT(panel->table()->rowCount(), 3, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{1});
    QCOMPARE(got->results[0].destination, Destination::EarthBank);
    QVERIFY2(got->results[0].state == StepState::Ok, got->results[0].message.c_str());
    const QStringList files = QDir(folder).entryList({QStringLiteral("*.xlsx")}, QDir::Files);
    QVERIFY2(!files.isEmpty(), qPrintable(panel->result_pane()->toPlainText()));
    QCOMPARE(w.bank.requests().size(), std::size_t{0});
  }

  void UploadRefusedOnPrivate() {
    pychron::ui::test::EarthbankWorld w;
    w.signed_up();
    const ps::Uuid r = published("r", false);
    Open o = open(w.setup());
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    set_action(*panel, Action::Upload);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->refusals()->text().contains(QStringLiteral("private")), kWaitMs);
    QVERIFY(!o.dialog->run_button()->isEnabled());
    auto got = watch(*o.dialog);
    o.dialog->run();  // refused: Run is off
    QVERIFY(!o.dialog->busy());
    QCOMPARE(got->count, 0);
    QCOMPARE(record_posts(w.bank), std::size_t{0});
  }

  // Review focus 2: the D3 condition is judged again just before the upload.
  void UploadRecheckedAtRun() {
    pychron::ui::test::EarthbankWorld w;
    w.signed_up();
    const ps::Uuid r = published("r", true);
    Open o = open(w.setup());
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    bool asked = false;
    panel->ask_confirm = [&asked](const QString&) {
      asked = true;
      return true;
    };
    set_action(*panel, Action::Upload);
    panel->allow_unresolved()->setChecked(true);
    QTRY_COMPARE_WITH_TIMEOUT(panel->table()->rowCount(), 3, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    QVERIFY2(o.dialog->refusals()->text().isEmpty(), qPrintable(o.dialog->refusals()->text()));
    // An embargo the dialog's check never saw.
    auto embargo = pb::set_embargo(*store_, seed_.reducer(), {ps::EmbargoScopeType::Sample, "FC-1", std::nullopt, std::nullopt});
    QVERIFY2(embargo.has_value(), embargo ? "" : pychron::to_string(embargo.error()).c_str());
    const std::size_t before = w.bank.requests().size();
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{1});
    QVERIFY(got->results[0].state == StepState::Failed);
    QVERIFY2(got->results[0].message.find("embargo") != std::string::npos, got->results[0].message.c_str());
    QVERIFY(!asked);
    QCOMPARE(w.bank.requests().size(), before);
    QCOMPARE(record_posts(w.bank), std::size_t{0});
  }

  void UploadConfirmNoIsCancelled() {
    pychron::ui::test::EarthbankWorld w;
    w.signed_up();
    const ps::Uuid r = published("r", true);
    Open o = open(w.setup());
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    bool asked = false;
    panel->ask_confirm = [&asked](const QString&) {
      asked = true;
      return false;
    };
    set_action(*panel, Action::Upload);
    panel->allow_unresolved()->setChecked(true);
    QTRY_COMPARE_WITH_TIMEOUT(panel->table()->rowCount(), 3, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QVERIFY2(asked, qPrintable(panel->result_pane()->toPlainText()));
    QCOMPARE(got->results.size(), std::size_t{1});
    QVERIFY2(got->results[0].state == StepState::Cancelled, got->results[0].message.c_str());
    QCOMPARE(state_text(*o.dialog, 0), QStringLiteral("Cancelled"));
    QCOMPARE(record_posts(w.bank), std::size_t{0});
  }

  void CancelBetweenSteps() {
    const ps::Uuid r = create("r");
    const QString out = dir_.filePath(QStringLiteral("cancel-out"));
    Open o = open();
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->folder_dir()->setText(out);
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::Store)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    // The build is on the worker; its result has not come back yet.
    QCOMPARE(o.dialog->cancel_button()->text(), QStringLiteral("Cancel"));
    o.dialog->cancel_button()->click();
    QVERIFY(o.dialog->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{2});
    QVERIFY2(got->results[0].state == StepState::Ok, got->results[0].message.c_str());  // a build is not interrupted
    QVERIFY(got->results[1].state == StepState::Cancelled);
    QCOMPARE(state_text(*o.dialog, 1), QStringLiteral("Cancelled"));
    QVERIFY(QDir(out + QStringLiteral("/r-v1")).exists());  // nothing undone
    QCOMPARE(release_row("r").state, ps::ReleaseState::Draft);
    QCOMPARE(o.dialog->cancel_button()->text(), QStringLiteral("Close"));
    QVERIFY(o.dialog->isVisible());
  }

  // Review focus 3: closing during a run asks; yes cancels, and the dialog
  // closes once the step that is out has come back.
  void CloseDuringRunCancelsThenCloses() {
    const ps::Uuid r = create("r");
    Open o = open();
    int asked = 0;
    o.dialog->set_ask_close_while_running([&asked] { return ++asked > 1; });  // no, then yes
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("close-out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::Store)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    o.dialog->close();  // no: it stays, and runs on
    QCOMPARE(asked, 1);
    QVERIFY(o.dialog->isVisible());
    QVERIFY(o.dialog->busy());
    o.dialog->close();  // yes
    QCOMPARE(asked, 2);
    QVERIFY(o.dialog->isVisible());  // until the build comes back
    QTRY_VERIFY_WITH_TIMEOUT(!o.dialog->isVisible(), kWaitMs);
    QCOMPARE(got->count, 1);
    QVERIFY(got->results[0].state == StepState::Ok);
    QVERIFY(got->results[1].state == StepState::Cancelled);
    QVERIFY(!o.dialog->busy());
  }

  // A dialog destroyed with a step out: its result reaches nothing.
  void DestroyedDuringRunIsSafe() {
    const ps::Uuid r = create("r");
    Open o = open();
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("gone-out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    o.dialog->run_button()->click();
    o.dialog.reset();
    // The build runs out on the worker; its stages and result are queued and dropped.
    auto flushed = o.bridge->run_sync<int>([](ps::IStore&, const ps::Actor&) -> pychron::Result<int> { return 0; });
    QVERIFY(flushed.has_value());
    QTest::qWait(50);
  }

  void NewReleaseFillsEarthBankPanelMidRun() {
    pychron::ui::test::EarthbankWorld w;
    Open o = open(w.setup());
    o.dialog->set_ask_release(named("fresh"));
    o.dialog->open_new(analyses());
    o.dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->details_button()->isEnabled(), kWaitMs);
    o.dialog->details_button()->click();
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    QVERIFY(panel->read_only());
    set_action(*panel, Action::ExportXlsx);
    const QString folder = dir_.filePath(QStringLiteral("fresh-workbooks"));
    panel->folder()->setText(folder);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    QCOMPARE(panel->table()->rowCount(), 0);
    // Whether the table was read-only when the release's rows came in.
    int filled_read_only = -1;
    QObject::connect(panel, &EarthbankPublishPanel::controls_changed, panel, [&filled_read_only, panel] {
      if (filled_read_only < 0 && panel->table()->rowCount() > 0) filled_read_only = panel->read_only() ? 1 : 0;
    });
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{1});
    QVERIFY2(got->results[0].state == StepState::Ok, got->results[0].message.c_str());
    QCOMPARE(filled_read_only, 1);
    QCOMPARE(panel->table()->rowCount(), 3);
    QVERIFY(!QDir(folder).entryList({QStringLiteral("*.xlsx")}, QDir::Files).isEmpty());
  }
  void NewReleaseCreationFailureSkipsDestinations() {
    Open o = open();
    o.dialog->set_ask_release(unmakeable("broken"));
    o.dialog->open_new(analyses());
    o.dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->details_button()->isEnabled(), kWaitMs);
    o.dialog->details_button()->click();
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("broken-out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::Store)->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QVERIFY(!got->release_ok);
    QCOMPARE(got->results.size(), std::size_t{2});
    for (const pb::StepResult& step : got->results) {
      QVERIFY(step.state == StepState::Skipped);
      QVERIFY2(step.message.starts_with("skipped: release could not be made:"), step.message.c_str());
    }
    QVERIFY(o.dialog->new_radio()->isChecked());  // nothing was made to switch to
    auto releases = store_->releases({});
    QVERIFY(releases.has_value());
    QVERIFY(releases->empty());
  }

  // While a run is under way the EarthBank tab cannot change what runs.
  void EarthBankPageLockedDuringRun() {
    pychron::ui::test::EarthbankWorld w;
    const ps::Uuid r = create("r");
    Open o = open(w.setup());
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("locked-out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    set_action(*panel, Action::ExportXlsx);
    panel->folder()->setText(dir_.filePath(QStringLiteral("locked-workbooks")));
    QTRY_COMPARE_WITH_TIMEOUT(panel->table()->rowCount(), 3, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    QVERIFY(panel->action()->isEnabled());
    // A sample whose kind of age can be chosen.
    int usable = -1;
    for (int row = 0; row < panel->table()->rowCount() && usable < 0; ++row)
      if (panel->preferred_age(row) != nullptr && panel->preferred_age(row)->isEnabled()) usable = row;
    QVERIFY(usable >= 0);
    QVERIFY(panel->table()->editTriggers() != QAbstractItemView::NoEditTriggers);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    // The release folder is building: EarthBank waits, and its page is locked.
    QVERIFY(o.dialog->busy());
    QVERIFY(!panel->action()->isEnabled());
    QVERIFY(!panel->preferred_age(usable)->isEnabled());
    QVERIFY(panel->table()->editTriggers() == QAbstractItemView::NoEditTriggers);
    QVERIFY(!panel->folder()->isEnabled());
    QVERIFY(!panel->cancel_button()->isEnabled());
    QVERIFY(!panel->not_there_button()->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{2});
    QVERIFY2(got->results[1].state == StepState::Ok, got->results[1].message.c_str());
    QVERIFY(!panel->read_only());
    QVERIFY(panel->action()->isEnabled());
    QVERIFY(panel->preferred_age(usable)->isEnabled());
    QVERIFY(panel->table()->editTriggers() != QAbstractItemView::NoEditTriggers);
  }

  // Store makes the release public in the same run; the upload's recheck counts it.
  void StorePublicThenUploadInOneRun() {
    pychron::ui::test::EarthbankWorld w;
    w.signed_up();
    const ps::Uuid r = create("r");  // a draft
    Open o = open(w.setup());
    o.dialog->open_existing(r);
    o.dialog->show();
    o.dialog->folder_dir()->setText(dir_.filePath(QStringLiteral("upload-out")));
    o.dialog->destination(Destination::Folder)->setChecked(true);
    o.dialog->destination(Destination::Store)->setChecked(true);
    o.dialog->store_public()->setChecked(true);
    o.dialog->destination(Destination::EarthBank)->setChecked(true);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    QVERIFY(panel != nullptr);
    bool asked = false;
    panel->ask_confirm = [&asked](const QString&) {
      asked = true;
      return true;
    };
    set_action(*panel, Action::Upload);
    panel->allow_unresolved()->setChecked(true);
    QTRY_COMPARE_WITH_TIMEOUT(panel->table()->rowCount(), 3, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(o.dialog->run_button()->isEnabled(), kWaitMs);
    auto got = watch(*o.dialog);
    o.dialog->run_button()->click();
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{3});
    QVERIFY2(got->results[0].state == StepState::Ok, got->results[0].message.c_str());
    QVERIFY2(got->results[1].state == StepState::Ok, got->results[1].message.c_str());
    QVERIFY2(got->results[2].state == StepState::Ok,
             qPrintable(QString::fromStdString(got->results[2].message) + QLatin1Char('\n') +
                        panel->result_pane()->toPlainText()));
    QVERIFY(asked);
    QVERIFY(record_posts(w.bank) > 0);
    const ps::ReleaseRow row = release_row("r");
    QCOMPARE(row.state, ps::ReleaseState::Published);
    QCOMPARE(row.visibility, ps::Visibility::Public);
  }
  // Finding: the in-doubt answer resends records. It is a run of the dialog's
  // own, through the upload gate: a release no longer public sends nothing.
  void InDoubtResumeRefusedWhenReleaseNoLongerPublic() {
    pychron::ui::test::EarthbankWorld w;
    w.signed_up();
    Open o = open(w.setup());
    const ps::Uuid r = upload_leaving_in_doubt(w, o);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    auto ok = pb::withdraw_release(*store_, seed_.reducer(), r, "superseded");
    QVERIFY2(ok.has_value(), ok ? "" : pychron::to_string(ok.error()).c_str());

    const std::size_t requests = w.bank.requests().size();
    const std::size_t posts = record_posts(w.bank);
    auto got = watch(*o.dialog);
    panel->not_there_button()->click();
    QVERIFY(o.dialog->busy());
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{1});
    QVERIFY(got->results[0].destination == Destination::EarthBank);
    QVERIFY2(got->results[0].state == StepState::Failed, got->results[0].message.c_str());
    QVERIFY2(got->results[0].message.find("withdrawn") != std::string::npos, got->results[0].message.c_str());
    QCOMPARE(o.dialog->results()->rowCount(), 1);
    QCOMPARE(state_text(*o.dialog, 0), QStringLiteral("Failed"));
    QVERIFY(!o.dialog->busy());
    QTest::qWait(200);  // nothing goes out late either
    QCOMPARE(w.bank.requests().size(), requests);
    QCOMPARE(record_posts(w.bank), posts);
    QCOMPARE(got->count, 1);
  }

  void InDoubtResumeRunsThroughTheDialog() {
    pychron::ui::test::EarthbankWorld w;
    w.signed_up();
    Open o = open(w.setup());
    upload_leaving_in_doubt(w, o);
    EarthbankPublishPanel* panel = o.dialog->earthbank_panel();
    const std::size_t aliquots = w.bank.store().aliquots.size();
    auto got = watch(*o.dialog);
    panel->not_there_button()->click();
    QVERIFY(o.dialog->busy());
    QVERIFY(!o.dialog->run_button()->isEnabled());
    QVERIFY(!o.dialog->destination(Destination::EarthBank)->isEnabled());  // the destinations are locked
    QVERIFY(!o.dialog->release_combo()->isEnabled());                       // and so is the release section
    QVERIFY(panel->locked());
    QCOMPARE(o.dialog->results()->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(got->count, 1, kWaitMs);
    QCOMPARE(got->results.size(), std::size_t{1});
    QVERIFY(got->results[0].destination == Destination::EarthBank);
    QVERIFY2(got->results[0].state == StepState::Ok,
             qPrintable(QString::fromStdString(got->results[0].message) + QLatin1Char('\n') +
                        panel->result_pane()->toPlainText()));
    QVERIFY(got->release_ok);
    QVERIFY(w.bank.store().aliquots.size() > aliquots);  // the record not there was sent again
    QVERIFY(!o.dialog->busy());
  }
};

QTEST_MAIN(TestPublishDialog)
#include "test_publish_dialog.moc"
