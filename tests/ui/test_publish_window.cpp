// The Publish window (data publishing spec, section 6.3) on a seeded store:
// the releases it lists, and what it does to them. Dialogs are hooks.

#include <memory>
#include <string>

#include <QAction>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QMainWindow>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "entry_bridge.hpp"
#include "publish_actions.hpp"
#include "publish_window.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/embargo.hpp"
#include "pychron/publish/release.hpp"
#include "release_seed.hpp"

namespace ps = pychron::persistence;
namespace pp = pychron::processing;
namespace pb = pychron::publish;
using pychron::ui::EntryBridge;
using pychron::ui::PublishActions;
using pychron::ui::PublishWindow;

namespace {

constexpr int kWaitMs = 20000;

QString cell(const PublishWindow& w, int row, int column) {
  return w.releases()->model()->index(row, column).data().toString();
}

}  // namespace

class TestPublishWindow : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir dir_;
  std::string url_;
  std::unique_ptr<ps::IStore> store_;  // the seeding client's
  pb::testing::ReleaseSeed seed_;

  std::vector<std::string> analyses() const {
    return {seed_.analysis1.str(), seed_.analysis2.str(), seed_.analysis3.str()};
  }

  pb::ReleaseSpec spec(const std::string& name) const {
    pb::ReleaseSpec s;
    s.name = name;
    s.title = "Release " + name;
    s.authors = {{"J. Ross", std::nullopt, std::nullopt}};
    s.analyses = analyses();
    s.options_json = pb::release_options_json(pb::ReleaseOptions{});
    s.reduction_version = "arar-1";
    s.software_version = "pychron-cpp/test";
    return s;
  }

  void create(const std::string& name) {
    auto created = pb::create_release(*store_, seed_.reducer(), spec(name));
    QVERIFY2(created.has_value(), created ? "" : pychron::to_string(created.error()).c_str());
  }

  // A window on its own bridge and source, as PublishActions makes it.
  struct Open {
    std::unique_ptr<pp::StoreSource> source;
    std::unique_ptr<EntryBridge> bridge;
    std::unique_ptr<PublishWindow> window;
  };
  Open open(std::function<QStringList()> selection = {}) {
    Open o;
    auto source = pp::StoreSource::open(ps::StoreConfig{url_, false}, {});
    if (!source) qFatal("source: %s", pychron::to_string(source.error()).c_str());
    o.source = std::move(*source);
    auto bridge = EntryBridge::open({url_, "tester", "test-host", "publisher", "pychron-ui/test"});
    if (!bridge) qFatal("bridge: %s", pychron::to_string(bridge.error()).c_str());
    o.bridge = std::move(*bridge);
    o.window = std::make_unique<PublishWindow>(*o.bridge, *o.source, url_, std::move(selection));
    return o;
  }

  // A release made and selected in an open window.
  QString made(Open& o) {
    create("r");
    o.window->refresh();
    if (!QTest::qWaitFor([&] { return !o.window->busy(); }, kWaitMs)) return {};
    auto row = pb::find_release(*store_, "r");
    if (!row) return {};
    const QString uuid = QString::fromStdString(row->uuid.str());
    o.window->select_release(uuid);
    return uuid;
  }

  // A release made, selected and built (no figures) in an open window; its directory.
  QString built(Open& o, const QString& name) {
    if (made(o).isEmpty()) return {};
    const QString out = dir_.filePath(name);
    o.window->set_ask_build([out](const QString&) -> std::optional<PublishWindow::BuildRequest> {
      return PublishWindow::BuildRequest{out.toStdString(), false, false};
    });
    o.window->build();
    if (!QTest::qWaitFor([&] { return !o.window->busy(); }, kWaitMs)) return {};
    o.window->set_ask_directory([](const QString& suggested) { return suggested; });
    return out + QStringLiteral("/r-v1");
  }

  QString state(const PublishWindow& w) const { return cell(w, 0, PublishWindow::State); }

 private Q_SLOTS:
  void init() {
    QVERIFY(dir_.isValid());
    static int n = 0;
    url_ = "sqlite:" + dir_.filePath(QStringLiteral("store-%1.db").arg(++n)).toStdString();
    auto s = ps::open_store(ps::StoreConfig{url_, true});
    QVERIFY2(s.has_value(), s ? "" : pychron::to_string(s.error()).c_str());
    store_ = std::move(*s);
    auto seeded = pb::testing::seed_release_store(*store_);
    QVERIFY2(seeded.has_value(), seeded ? "" : pychron::to_string(seeded.error()).c_str());
    seed_ = *seeded;
  }

  void cleanup() { store_.reset(); }

  void listsTheStoresReleases() {
    create("r");
    Open o = open();
    o.window->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QCOMPARE(o.window->releases()->model()->rowCount(), 1);
    QCOMPARE(cell(*o.window, 0, PublishWindow::Name), QStringLiteral("r"));
    QCOMPARE(cell(*o.window, 0, PublishWindow::Version), QStringLiteral("1"));
    QCOMPARE(cell(*o.window, 0, PublishWindow::State), QStringLiteral("draft"));
    QCOMPARE(cell(*o.window, 0, PublishWindow::Visibility), QStringLiteral("private"));
    QCOMPARE(cell(*o.window, 0, PublishWindow::Members), QStringLiteral("3"));
  }

  void theViewMenuOpensIt() {
    create("r");
    QMainWindow owner;
    auto* actions = new PublishActions(&owner, url_);
    QCOMPARE(actions->action()->text(), QStringLiteral("Publish…"));
    actions->action()->trigger();
    PublishWindow* w = actions->window();
    QVERIFY(w != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(w->isVisible(), kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!w->busy(), kWaitMs);
    QCOMPARE(w->releases()->model()->rowCount(), 1);
  }

  void aStoreThatCannotBeOpenedIsSaid() {
    QMainWindow owner;
    const std::string missing = "sqlite:" + dir_.filePath(QStringLiteral("no/such/dir/store.db")).toStdString();
    auto* actions = new PublishActions(&owner, missing);
    QStringList said;
    actions->set_report_error([&said](const QString& text) { said << text; });
    QVERIFY(actions->window() == nullptr);
    QCOMPARE(said.size(), 1);
    QVERIFY2(said[0].contains(QString::fromStdString(missing)), qPrintable(said[0]));
  }
  void newReleaseFromTheSelection() {
    QStringList chosen = {QString::fromStdString(seed_.analysis1.str()), QString::fromStdString(seed_.analysis2.str()),
                                QString::fromStdString(seed_.analysis3.str())};
    Open o = open([chosen] { return chosen; });
    QStringList asked;
    std::size_t embargoed = 99;
    o.window->set_ask_release([&](const QStringList& analyses, std::size_t n) -> std::optional<pb::ReleaseSpec> {
      asked = analyses;
      embargoed = n;
      return spec("r");
    });
    o.window->new_release();
    QTRY_VERIFY_WITH_TIMEOUT(o.window->releases()->model()->rowCount() == 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QCOMPARE(asked, chosen);
    QCOMPARE(embargoed, std::size_t{0});
    QVERIFY(!o.window->selected_release().isEmpty());
    QVERIFY2(o.window->status()->text().contains(QStringLiteral("r v1")), qPrintable(o.window->status()->text()));
    auto row = pb::find_release(*store_, "r");
    QVERIFY(row.has_value());
    QCOMPARE(o.window->selected_release(), QString::fromStdString(row->uuid.str()));
    QCOMPARE(row->reduction_version, std::string("arar-1"));
  }

  void nothingSelectedIsSaid() {
    Open o = open([] { return QStringList(); });
    bool asked = false;
    o.window->set_ask_release([&](const QStringList&, std::size_t) -> std::optional<pb::ReleaseSpec> {
      asked = true;
      return std::nullopt;
    });
    o.window->new_release();
    QVERIFY(!asked);
    QVERIFY2(o.window->status()->text().contains(QStringLiteral("Select analyses in the Data browser first")),
             qPrintable(o.window->status()->text()));
  }

  void embargoedCountIsShown() {
    QVERIFY(pb::set_embargo(*store_, seed_.reducer(), {ps::EmbargoScopeType::Sample, "FC-1", std::nullopt, std::nullopt}));
    QStringList chosen = {QString::fromStdString(seed_.analysis1.str()), QString::fromStdString(seed_.analysis2.str())};
    Open o = open([chosen] { return chosen; });
    std::size_t embargoed = 99;
    bool asked = false;
    o.window->set_ask_release([&](const QStringList&, std::size_t n) -> std::optional<pb::ReleaseSpec> {
      embargoed = n;
      asked = true;
      return std::nullopt;
    });
    o.window->new_release();
    QTRY_VERIFY_WITH_TIMEOUT(asked, kWaitMs);
    QCOMPARE(embargoed, std::size_t{1});
  }

  void aRefusedReleaseIsSaid() {
    QStringList chosen = {QString::fromStdString(seed_.analysis1.str())};
    Open o = open([chosen] { return chosen; });
    o.window->set_ask_release([&](const QStringList&, std::size_t) -> std::optional<pb::ReleaseSpec> {
      auto s = spec("r");
      s.title.clear();
      return s;
    });
    o.window->new_release();
    QTRY_VERIFY_WITH_TIMEOUT(o.window->status()->text().contains(QStringLiteral("needs a title")), kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QCOMPARE(o.window->releases()->model()->rowCount(), 0);
  }
  void buildWritesTheDirectoryAndSaysWhere() {
    Open o = open();
    const QString uuid = made(o);
    QVERIFY(!uuid.isEmpty());
    const QString out = dir_.filePath(QStringLiteral("out-build"));
    o.window->set_ask_build([&](const QString& name) -> std::optional<PublishWindow::BuildRequest> {
      if (name != QStringLiteral("r")) return std::nullopt;
      return PublishWindow::BuildRequest{out.toStdString(), true, false};
    });
    o.window->build();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    const QString built = out + QStringLiteral("/r-v1");
    QVERIFY2(QFile::exists(built + QStringLiteral("/manifest.json")), qPrintable(o.window->status()->text()));
    QVERIFY(QFile::exists(built + QStringLiteral("/figures/FC-1/ideogram.svg")));
    QVERIFY2(o.window->status()->text().contains(built), qPrintable(o.window->status()->text()));
    QCOMPARE(o.window->progress()->value(), o.window->progress()->maximum());
    QCOMPARE(o.window->last_build(uuid), built);
  }

  void buildingWithNoReleaseSelectedIsSaid() {
    Open o = open();
    bool asked = false;
    o.window->set_ask_build([&](const QString&) -> std::optional<PublishWindow::BuildRequest> {
      asked = true;
      return std::nullopt;
    });
    o.window->build();
    QVERIFY(!asked);
    QVERIFY(o.window->status()->text().contains(QStringLiteral("Select a release first")));
  }

  void closingDuringABuildIsSafe() {
    Open o = open();
    QVERIFY(!made(o).isEmpty());
    const QString out = dir_.filePath(QStringLiteral("out-closed"));
    o.window->set_ask_build([&](const QString&) -> std::optional<PublishWindow::BuildRequest> {
      return PublishWindow::BuildRequest{out.toStdString(), true, false};
    });
    o.window->build();
    o.window.reset();  // closed while the worker builds
    QVERIFY((o.bridge->run_sync<bool>([](ps::IStore&, const ps::Actor&) -> pychron::Result<bool> { return true; })));
    QCoreApplication::processEvents();  // the queued stages and the result find no window
    QVERIFY(QFile::exists(out + QStringLiteral("/r-v1/manifest.json")));
  }
  void verifyAgreesWithTheStore() {
    Open o = open();
    const QString dir = built(o, QStringLiteral("out-verify"));
    QVERIFY(!dir.isEmpty());
    QString reported;
    o.window->set_report([&](const QString& text) { reported = text; });
    o.window->verify();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QVERIFY2(o.window->status()->text().startsWith(QStringLiteral("verified")), qPrintable(o.window->status()->text()));
    QVERIFY(reported.isEmpty());
  }

  void verifyNamesAChangedFile() {
    Open o = open();
    const QString dir = built(o, QStringLiteral("out-changed"));
    QVERIFY(!dir.isEmpty());
    {
      QFile readme(dir + QStringLiteral("/README.md"));
      QVERIFY(readme.open(QIODevice::Append));
      readme.write("edited\n");
    }
    QString reported;
    o.window->set_report([&](const QString& text) { reported = text; });
    o.window->verify();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QVERIFY2(reported.contains(QStringLiteral("changed README.md")), qPrintable(reported));
  }

  void publishMakesItPublished() {
    Open o = open();
    QVERIFY(!built(o, QStringLiteral("out-publish")).isEmpty());
    o.window->set_ask_publish([](const QString&) -> std::optional<bool> { return false; });
    o.window->publish();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QTRY_COMPARE_WITH_TIMEOUT(state(*o.window), QStringLiteral("published"), kWaitMs);
    QCOMPARE(cell(*o.window, 0, PublishWindow::Visibility), QStringLiteral("private"));
  }

  void publishRefusesAChangedDirectory() {
    Open o = open();
    const QString dir = built(o, QStringLiteral("out-refused"));
    QVERIFY(!dir.isEmpty());
    {
      QFile readme(dir + QStringLiteral("/README.md"));
      QVERIFY(readme.open(QIODevice::Append));
      readme.write("edited\n");
    }
    o.window->set_ask_publish([](const QString&) -> std::optional<bool> { return false; });
    o.window->publish();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QVERIFY2(o.window->status()->text().contains(QStringLiteral("does not match its manifest")),
             qPrintable(o.window->status()->text()));
    QCOMPARE(state(*o.window), QStringLiteral("draft"));
  }

  void withdrawNeedsAReason() {
    Open o = open();
    QVERIFY(!built(o, QStringLiteral("out-withdraw")).isEmpty());
    o.window->set_ask_publish([](const QString&) -> std::optional<bool> { return false; });
    o.window->publish();
    QTRY_COMPARE_WITH_TIMEOUT(state(*o.window), QStringLiteral("published"), kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    o.window->set_ask_text([](const QString&) -> std::optional<QString> { return QStringLiteral("  "); });
    o.window->withdraw();
    QVERIFY(o.window->status()->text().contains(QStringLiteral("needs a reason")));
    QCOMPARE(state(*o.window), QStringLiteral("published"));
    o.window->set_ask_text([](const QString&) -> std::optional<QString> { return QStringLiteral("error in J"); });
    o.window->withdraw();
    QTRY_COMPARE_WITH_TIMEOUT(state(*o.window), QStringLiteral("withdrawn"), kWaitMs);
  }

  void makePublicChangesVisibility() {
    Open o = open();
    QVERIFY(!built(o, QStringLiteral("out-public")).isEmpty());
    o.window->set_ask_publish([](const QString&) -> std::optional<bool> { return false; });
    o.window->publish();
    QTRY_COMPARE_WITH_TIMEOUT(state(*o.window), QStringLiteral("published"), kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    o.window->set_confirm([](const QString&) { return true; });
    o.window->make_public();
    QTRY_COMPARE_WITH_TIMEOUT(cell(*o.window, 0, PublishWindow::Visibility), QStringLiteral("public"), kWaitMs);
  }

  void buttonsFollowTheState() {
    Open o = open();
    QVERIFY(!built(o, QStringLiteral("out-buttons")).isEmpty());
    QVERIFY(o.window->publish_button()->isEnabled());
    QVERIFY(!o.window->withdraw_button()->isEnabled());
    QVERIFY(!o.window->public_button()->isEnabled());
    o.window->set_ask_publish([](const QString&) -> std::optional<bool> { return false; });
    o.window->publish();
    QTRY_COMPARE_WITH_TIMEOUT(state(*o.window), QStringLiteral("published"), kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QVERIFY(!o.window->publish_button()->isEnabled());
    QVERIFY(o.window->withdraw_button()->isEnabled());
    QVERIFY(o.window->public_button()->isEnabled());
  }
  void setAndLiftAnEmbargo() {
    Open o = open();
    o.window->set_ask_embargo([]() -> std::optional<PublishWindow::EmbargoForm> {
      return PublishWindow::EmbargoForm{ps::EmbargoScopeType::Sample, QStringLiteral("FC-1"), QDate(2030, 1, 1),
                                        QStringLiteral("paper")};
    });
    o.window->set_embargo();
    QTRY_COMPARE_WITH_TIMEOUT(o.window->embargoes()->model()->rowCount(), 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    const auto at = [&](int c) { return o.window->embargoes()->model()->index(0, c).data().toString(); };
    QVERIFY2(at(PublishWindow::EmbargoScope).contains(QStringLiteral("FC-1")), qPrintable(at(PublishWindow::EmbargoScope)));
    QCOMPARE(at(PublishWindow::EmbargoUntil), QStringLiteral("2030-01-01"));
    QCOMPARE(at(PublishWindow::EmbargoReason), QStringLiteral("paper"));
    o.window->embargoes()->setCurrentIndex(o.window->embargoes()->model()->index(0, 0));
    o.window->lift_embargo();
    QTRY_COMPARE_WITH_TIMEOUT(o.window->embargoes()->model()->rowCount(), 0, kWaitMs);
  }

  void anUnknownScopeIsSaid() {
    Open o = open();
    o.window->set_ask_embargo([]() -> std::optional<PublishWindow::EmbargoForm> {
      return PublishWindow::EmbargoForm{ps::EmbargoScopeType::Sample, QStringLiteral("NOPE"), std::nullopt, QString()};
    });
    o.window->set_embargo();
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    QVERIFY2(o.window->status()->text().contains(QStringLiteral("NOPE")), qPrintable(o.window->status()->text()));
    QCOMPARE(o.window->embargoes()->model()->rowCount(), 0);
  }

  void anEmbargoSetInTheDockIsCountedByNewRelease() {
    QStringList chosen = {QString::fromStdString(seed_.analysis1.str()), QString::fromStdString(seed_.analysis2.str())};
    Open o = open([chosen] { return chosen; });
    o.window->set_ask_embargo([]() -> std::optional<PublishWindow::EmbargoForm> {
      return PublishWindow::EmbargoForm{ps::EmbargoScopeType::Project, QStringLiteral("Alpha"), std::nullopt, QString()};
    });
    o.window->set_embargo();
    QTRY_COMPARE_WITH_TIMEOUT(o.window->embargoes()->model()->rowCount(), 1, kWaitMs);
    QTRY_VERIFY_WITH_TIMEOUT(!o.window->busy(), kWaitMs);
    std::size_t embargoed = 99;
    bool asked = false;
    o.window->set_ask_release([&](const QStringList&, std::size_t n) -> std::optional<pb::ReleaseSpec> {
      embargoed = n;
      asked = true;
      return std::nullopt;
    });
    o.window->new_release();
    QTRY_VERIFY_WITH_TIMEOUT(asked, kWaitMs);
    QCOMPARE(embargoed, std::size_t{2});
  }
};

QTEST_MAIN(TestPublishWindow)
#include "test_publish_window.moc"
