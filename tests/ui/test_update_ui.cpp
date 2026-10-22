// The update check's face in a window: the status-bar indicator, the update
// dialog and Help > Check for Updates… (update check spec 2.3). GitHub is a
// FakeHttp (tests/update) behind a real UpdateChecker; the time is fixed.

#include <chrono>
#include <memory>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QMainWindow>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QUrl>
#include <QtTest/QtTest>

#include "fake_http.hpp"
#include "menu_hub.hpp"
#include "pychron/update/update.hpp"
#include "settings_guard.hpp"
#include "update_checker.hpp"
#include "update_ui.hpp"

using pychron::ErrorKind;
using pychron::ui::MenuHub;
using pychron::ui::UpdateChecker;
using pychron::ui::UpdateDialog;
using pychron::ui::UpdateIndicator;
using pychron::update::testing::FakeHttp;
using pychron::update::testing::release_json;
namespace upd = pychron::update;
using std::chrono::system_clock;

namespace {

constexpr int kWaitMs = 5000;  // real time; a check against a fake takes none

const system_clock::time_point kNow{std::chrono::seconds(1'792'000'000)};
constexpr upd::Version kRunning{0, 4, 0};

// The update dialog on screen, or null.
UpdateDialog* shown_dialog() {
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (auto* d = qobject_cast<UpdateDialog*>(w); d != nullptr && d->isVisible()) return d;
  }
  return nullptr;
}

}  // namespace

// Takes the URLs QDesktopServices::openUrl is given, in place of a browser.
class UrlCatcher : public QObject {
  Q_OBJECT

 public:
  QList<QUrl> opened;

 public slots:
  void open(const QUrl& url) { opened << url; }
};

class TestUpdateUi : public QObject {
  Q_OBJECT

 private:
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  QTemporaryDir dir_;
  std::unique_ptr<QSettings> settings_;
  std::unique_ptr<FakeHttp> fake_;
  std::unique_ptr<QMainWindow> window_;
  std::unique_ptr<UpdateChecker> checker_;  // after the window: goes first

  UpdateIndicator* indicator() const { return window_->findChild<UpdateIndicator*>(); }

  QAction* help_action() const {
    for (const MenuHub::Command& c : MenuHub::instance().commands()) {
      if (c.menu == MenuHub::Menu::Help && c.action->text() == QStringLiteral("Check for Updates…")) return c.action;
    }
    return nullptr;
  }

  // An automatic check that finds v999.0.0: the indicator shows it.
  void find_newer_automatically() {
    fake_->answer(200, release_json("v999.0.0"));
    checker_->check_on_startup();
    QTRY_VERIFY_WITH_TIMEOUT(indicator()->isVisible(), kWaitMs);
  }

 private slots:
  void init() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    settings_ = std::make_unique<QSettings>(dir_.filePath(QStringLiteral("update-ui.ini")), QSettings::IniFormat);
    settings_->clear();
    fake_ = std::make_unique<FakeHttp>();
    window_ = std::make_unique<QMainWindow>();
    checker_ = std::make_unique<UpdateChecker>(*settings_, kRunning, [] { return kNow; },
                                               [this] { return fake_->borrow(); });
    pychron::ui::install_update_check(window_.get(), *checker_);
    window_->show();
  }

  void cleanup() {
    for (QWidget* w : QApplication::topLevelWidgets()) {
      if (qobject_cast<UpdateDialog*>(w) != nullptr) w->close();
    }
    checker_.reset();
    window_.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }

  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }

  void indicatorHiddenAtStart() {
    QVERIFY(indicator() != nullptr);
    QVERIFY(indicator()->isHidden());
  }

  // Never a dialog that opens by itself: the indicator only.
  void automaticAvailableShowsIndicatorOnly() {
    find_newer_automatically();
    QCOMPARE(indicator()->text(), QStringLiteral("Update available: v999.0.0"));
    QCoreApplication::processEvents();
    QVERIFY(shown_dialog() == nullptr);
  }

  void indicatorOpensTheReleaseDialog() {
    find_newer_automatically();
    indicator()->click();
    UpdateDialog* d = shown_dialog();
    QVERIFY(d != nullptr);
    QCOMPARE(d->windowTitle(), QStringLiteral("pychron 999.0.0 is available"));
    QVERIFY(d->notes() != nullptr);
    QVERIFY(d->notes()->isReadOnly());
    QVERIFY(!d->notes()->toPlainText().isEmpty());
    QVERIFY(d->download_button() != nullptr);
    QVERIFY(d->skip_button() != nullptr);
  }

  void skipHidesIndicator() {
    find_newer_automatically();
    indicator()->click();
    UpdateDialog* d = shown_dialog();
    QVERIFY(d != nullptr);
    d->skip_button()->click();
    QVERIFY(indicator()->isHidden());
    QCOMPARE(settings_->value(QStringLiteral("updates/skipped_version")).toString(), QStringLiteral("999.0.0"));
    QTRY_VERIFY_WITH_TIMEOUT(shown_dialog() == nullptr, kWaitMs);
  }

  // Download opens the release's page in the browser, and that is all.
  void downloadOpensTheReleasePage() {
    UrlCatcher catcher;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "open");
    find_newer_automatically();
    indicator()->click();
    UpdateDialog* d = shown_dialog();
    QVERIFY(d != nullptr);
    d->download_button()->click();
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    QCOMPARE(catcher.opened.size(), 1);
    QCOMPARE(catcher.opened.front().host(), QStringLiteral("github.com"));
    QVERIFY(indicator()->isVisible());
    QVERIFY(!settings_->contains(QStringLiteral("updates/skipped_version")));
  }

  void helpActionPresent() {
    QAction* action = help_action();
    QVERIFY(action != nullptr);
    QVERIFY(action->isEnabled());
  }

  void manualAvailableShowsDialog() {
    fake_->answer(200, release_json("v999.0.0"));
    help_action()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(shown_dialog() != nullptr, kWaitMs);
    QCOMPARE(shown_dialog()->windowTitle(), QStringLiteral("pychron 999.0.0 is available"));
  }

  void manualUpToDateShowsDialog() {
    fake_->answer(200, release_json("v0.4.0"));
    help_action()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(shown_dialog() != nullptr, kWaitMs);
    QCOMPARE(shown_dialog()->message(), QStringLiteral("pychron 0.4.0 is up to date"));
    QVERIFY(shown_dialog()->download_button() == nullptr);
  }

  void manualNoReleaseShowsDialog() {
    fake_->answer(404, "{}");
    help_action()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(shown_dialog() != nullptr, kWaitMs);
    QCOMPARE(shown_dialog()->message(), QStringLiteral("No release has been published yet"));
  }

  void manualFailureShowsReason() {
    fake_->answer(503, "");
    help_action()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(shown_dialog() != nullptr, kWaitMs);
    QCOMPARE(shown_dialog()->message(), QStringLiteral("Could not check for updates: GitHub answered 503"));
  }

  void manualNetworkFailureSaysGitHubCannotBeReached() {
    fake_->fail(ErrorKind::Io);
    help_action()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(shown_dialog() != nullptr, kWaitMs);
    QVERIFY2(shown_dialog()->message().startsWith(QStringLiteral("Could not check for updates: Could not reach GitHub")),
             qPrintable(shown_dialog()->message()));
  }

  // An automatic failure is the log's, never a dialog.
  void automaticFailureShowsNothing() {
    fake_->fail(ErrorKind::Timeout);
    QSignalSpy failed(checker_.get(), &UpdateChecker::failed);
    checker_->check_on_startup();
    QVERIFY(failed.wait(kWaitMs));
    QCoreApplication::processEvents();
    QVERIFY(shown_dialog() == nullptr);
    QVERIFY(indicator()->isHidden());
  }

  void helpActionDisabledWhileChecking() {
    fake_->hold();
    fake_->answer(200, release_json("v0.4.0"));
    help_action()->trigger();
    QVERIFY(!help_action()->isEnabled());
    fake_->release();
    QTRY_VERIFY_WITH_TIMEOUT(help_action()->isEnabled(), kWaitMs);
  }
};

QTEST_MAIN(TestUpdateUi)
#include "test_update_ui.moc"
