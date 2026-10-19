// The Publish window (data publishing spec, section 6.3) on a seeded store:
// the releases it lists, and what it does to them. Dialogs are hooks.

#include <memory>
#include <string>

#include <QAction>
#include <QLabel>
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
};

QTEST_MAIN(TestPublishWindow)
#include "test_publish_window.moc"
