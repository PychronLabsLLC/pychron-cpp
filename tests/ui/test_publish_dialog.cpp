// The Publish dialog (publication workflow design, section 6.1) on a seeded
// store: the release section, the destinations and their tabs, and the
// refusals that keep Run off. Running a plan is not here yet.

#include <memory>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "entry_bridge.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/store_source.hpp"
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
  Open open() {
    Open o;
    auto source = pp::StoreSource::open(ps::StoreConfig{url_, false}, {});
    if (!source) qFatal("source: %s", pychron::to_string(source.error()).c_str());
    o.source = std::move(*source);
    auto bridge = EntryBridge::open({url_, "tester", "test-host", "publisher", "pychron-ui/test"});
    if (!bridge) qFatal("bridge: %s", pychron::to_string(bridge.error()).c_str());
    o.bridge = std::move(*bridge);
    o.dialog = std::make_unique<PublishDialog>(*o.bridge, *o.source, url_, earthbank(), settings());
    return o;
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
    QVERIFY(!o.dialog->run_button()->isEnabled());  // nothing ticked
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
};

QTEST_MAIN(TestPublishDialog)
#include "test_publish_dialog.moc"
