// UpdateChecker: one check for a newer release on a worker thread, its answer
// back on the GUI thread, and the three QSettings keys it keeps. GitHub is a
// FakeHttp (tests/update); the time is a fixed `now`.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "fake_http.hpp"
#include "pychron/update/update.hpp"
#include "settings_guard.hpp"
#include "update_checker.hpp"

using pychron::ErrorKind;
using pychron::ui::UpdateChecker;
using pychron::update::testing::FakeHttp;
using pychron::update::testing::release_json;
namespace upd = pychron::update;
using std::chrono::system_clock;

namespace {

constexpr int kWaitMs = 5000;  // real time; a check against a fake takes none

// Whole seconds: last_check is kept to the second (Qt::ISODate).
const system_clock::time_point kNow{std::chrono::seconds(1'792'000'000)};
constexpr upd::Version kRunning{0, 4, 0};

}  // namespace

class TestUpdateChecker : public QObject {
  Q_OBJECT

 private:
  pychron::ui::test::ApplicationSettingsGuard app_settings_;
  QTemporaryDir dir_;
  std::unique_ptr<QSettings> settings_;
  std::unique_ptr<FakeHttp> fake_;

  std::unique_ptr<UpdateChecker> make(bool with_client = true) {
    UpdateChecker::ClientFactory factory;
    if (with_client) {
      factory = [this] { return fake_->borrow(); };
    } else {
      factory = [] { return std::unique_ptr<pychron::http::IHttpClient>(); };
    }
    return std::make_unique<UpdateChecker>(*settings_, kRunning, [] { return kNow; }, factory);
  }

  void set_policy(const upd::CheckPolicy& policy) { pychron::ui::save_update_policy(*settings_, policy); }
  upd::CheckPolicy policy() const { return pychron::ui::load_update_policy(*settings_); }

 private slots:
  void init() {
    settings_ = std::make_unique<QSettings>(dir_.filePath(QStringLiteral("update.ini")), QSettings::IniFormat);
    settings_->clear();
    fake_ = std::make_unique<FakeHttp>();
  }

  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }

  void policyRoundTrip() {
    upd::CheckPolicy saved;
    saved.enabled = false;
    saved.last_check = kNow;
    saved.skipped = upd::Version{1, 2, 3};
    set_policy(saved);
    const auto loaded = policy();
    QCOMPARE(loaded.enabled, false);
    QVERIFY(loaded.last_check == kNow);
    QVERIFY(loaded.skipped == (upd::Version{1, 2, 3}));
    QCOMPARE(settings_->value(QStringLiteral("updates/skipped_version")).toString(), QStringLiteral("1.2.3"));
    QVERIFY(settings_->value(QStringLiteral("updates/last_check")).toString().endsWith(QLatin1Char('Z')));
  }

  void garbageSettingsReadAsNone() {
    settings_->setValue(QStringLiteral("updates/last_check"), QStringLiteral("yesterday"));
    settings_->setValue(QStringLiteral("updates/skipped_version"), QStringLiteral("banana"));
    const auto loaded = policy();
    QVERIFY(!loaded.last_check.has_value());
    QVERIFY(!loaded.skipped.has_value());
    QCOMPARE(loaded.enabled, true);
  }

  void startupNotDueSendsNothing() {
    upd::CheckPolicy p;
    p.last_check = kNow - std::chrono::hours(1);
    set_policy(p);
    auto checker = make();
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QVERIFY(!checker->busy());
    QCoreApplication::processEvents();
    QVERIFY(fake_->sent().empty());
    QCOMPARE(available.count(), 0);
    QCOMPARE(failed.count(), 0);
  }

  void startupDisabledSendsNothing() {
    upd::CheckPolicy p;
    p.enabled = false;
    set_policy(p);
    auto checker = make();
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QVERIFY(!checker->busy());
    QCoreApplication::processEvents();
    QVERIFY(fake_->sent().empty());
    QCOMPARE(available.count(), 0);
    QCOMPARE(failed.count(), 0);
  }

  void startupNewerEmitsAvailable() {
    fake_->answer(200, release_json("v999.0.0"));
    auto checker = make();
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    checker->check_on_startup();
    QVERIFY(checker->busy());
    QVERIFY(available.wait(kWaitMs));
    QCOMPARE(available.count(), 1);
    const auto release = available.at(0).at(0).value<upd::Release>();
    QVERIFY(release.version == (upd::Version{999, 0, 0}));
    QCOMPARE(available.at(0).at(1).toBool(), false);
    QVERIFY(!checker->busy());
    QVERIFY(policy().last_check == kNow);
    QCOMPARE(fake_->sent().size(), std::size_t{1});
  }

  void startupSkippedEmitsNothing() {
    upd::CheckPolicy p;
    p.skipped = upd::Version{999, 0, 0};
    set_policy(p);
    fake_->answer(200, release_json("v999.0.0"));
    auto checker = make();
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    QSignalSpy up_to_date(checker.get(), &UpdateChecker::up_to_date);
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QTRY_VERIFY_WITH_TIMEOUT(!checker->busy(), kWaitMs);
    QCOMPARE(available.count(), 0);
    QCOMPARE(up_to_date.count(), 0);
    QCOMPARE(failed.count(), 0);
    QVERIFY(policy().last_check == kNow);
  }

  void startupFailureIsSilentAndKeepsLastCheck() {
    const auto before = kNow - std::chrono::hours(48);
    upd::CheckPolicy p;
    p.last_check = before;
    set_policy(p);
    fake_->fail(ErrorKind::Io);
    auto checker = make();
    QStringList lines;
    checker->set_log([&lines](const QString& line) { lines << line; });
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    QSignalSpy up_to_date(checker.get(), &UpdateChecker::up_to_date);
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QVERIFY(failed.wait(kWaitMs));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(1).toBool(), false);
    QCOMPARE(available.count(), 0);
    QCOMPARE(up_to_date.count(), 0);
    QVERIFY(policy().last_check == before);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.front().startsWith(QStringLiteral("INFO [update] ")));
  }

  // An answer that cannot be read is something to look into: a warning.
  void startupUnreadableAnswerIsAWarning() {
    fake_->answer(200, "{not json");
    auto checker = make();
    QStringList lines;
    checker->set_log([&lines](const QString& line) { lines << line; });
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QVERIFY(failed.wait(kWaitMs));
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.front().startsWith(QStringLiteral("WARN [update] ")));
    QVERIFY(!policy().last_check.has_value());
  }

  // A bad status is the network's trouble, not ours: info.
  void startupBadStatusIsInfo() {
    fake_->answer(503, "");
    auto checker = make();
    QStringList lines;
    checker->set_log([&lines](const QString& line) { lines << line; });
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QVERIFY(failed.wait(kWaitMs));
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.front().startsWith(QStringLiteral("INFO [update] ")));
  }

  void manualIgnoresDueAndSkipped() {
    upd::CheckPolicy p;
    p.last_check = kNow;
    p.skipped = upd::Version{999, 0, 0};
    set_policy(p);
    fake_->answer(200, release_json("v999.0.0"));
    auto checker = make();
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    checker->check_now();
    QVERIFY(available.wait(kWaitMs));
    QCOMPARE(available.count(), 1);
    QCOMPARE(available.at(0).at(1).toBool(), true);
  }

  void manualWhileBusyDoesNothing() {
    fake_->hold();
    fake_->answer(200, release_json("v999.0.0"));
    auto checker = make();
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    checker->check_now();
    QVERIFY(checker->busy());
    checker->check_now();
    checker->check_on_startup();
    fake_->release();
    QVERIFY(available.wait(kWaitMs));
    QCoreApplication::processEvents();
    QCOMPARE(available.count(), 1);
    QCOMPARE(fake_->sent().size(), std::size_t{1});
  }

  void manualUpToDate() {
    fake_->answer(200, release_json("v0.4.0"));
    auto checker = make();
    QSignalSpy up_to_date(checker.get(), &UpdateChecker::up_to_date);
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    checker->check_now();
    QVERIFY(up_to_date.wait(kWaitMs));
    QCOMPARE(up_to_date.at(0).at(0).toBool(), false);
    QCOMPARE(available.count(), 0);
    QVERIFY(policy().last_check == kNow);
  }

  void manualNoRelease() {
    fake_->answer(404, "{}");
    auto checker = make();
    QSignalSpy up_to_date(checker.get(), &UpdateChecker::up_to_date);
    checker->check_now();
    QVERIFY(up_to_date.wait(kWaitMs));
    QCOMPARE(up_to_date.at(0).at(0).toBool(), true);
    QVERIFY(policy().last_check == kNow);
  }

  void manualFailureIsNotLogged() {
    fake_->fail(ErrorKind::Timeout);
    auto checker = make();
    QStringList lines;
    checker->set_log([&lines](const QString& line) { lines << line; });
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_now();
    QVERIFY(failed.wait(kWaitMs));
    QCOMPARE(failed.at(0).at(1).toBool(), true);
    QVERIFY(lines.isEmpty());
  }

  // What a failed check is said as (spec section 4): the network, a status,
  // or an answer that does not read.
  void failureReasonsNameWhatWentWrong() {
    const auto reason_for = [this](auto&& queue) {
      queue();
      auto checker = make();
      QSignalSpy failed(checker.get(), &UpdateChecker::failed);
      checker->check_now();
      if (!failed.wait(kWaitMs)) return QString();
      return failed.at(0).at(0).toString();
    };
    QVERIFY(reason_for([this] { fake_->fail(ErrorKind::Io); }).startsWith(QStringLiteral("Could not reach GitHub")));
    QVERIFY(
        reason_for([this] { fake_->fail(ErrorKind::Timeout); }).startsWith(QStringLiteral("Could not reach GitHub")));
    QCOMPARE(reason_for([this] { fake_->answer(503, ""); }), QStringLiteral("GitHub answered 503"));
    QVERIFY(reason_for([this] { fake_->answer(200, "{not json"); })
                .startsWith(QStringLiteral("Unexpected answer from GitHub")));
  }

  void busyChangedAroundACheck() {
    fake_->answer(200, release_json("v0.4.0"));
    auto checker = make();
    QSignalSpy busy(checker.get(), &UpdateChecker::busy_changed);
    QSignalSpy up_to_date(checker.get(), &UpdateChecker::up_to_date);
    checker->check_now();
    QCOMPARE(busy.count(), 1);
    QCOMPARE(busy.at(0).at(0).toBool(), true);
    QVERIFY(up_to_date.wait(kWaitMs));
    QCOMPARE(busy.count(), 2);
    QCOMPARE(busy.at(1).at(0).toBool(), false);
    QVERIFY(checker->running() == kRunning);
  }

  void skipIsStored() {
    auto checker = make(false);
    checker->skip(upd::Version{2, 0, 1});
    QVERIFY(policy().skipped == (upd::Version{2, 0, 1}));
  }

  void noClient() {
    auto checker = make(false);
    QSignalSpy available(checker.get(), &UpdateChecker::available);
    QSignalSpy up_to_date(checker.get(), &UpdateChecker::up_to_date);
    QSignalSpy failed(checker.get(), &UpdateChecker::failed);
    checker->check_on_startup();
    QVERIFY(!checker->busy());
    QCoreApplication::processEvents();
    QCOMPARE(available.count() + up_to_date.count() + failed.count(), 0);

    checker->check_now();
    QVERIFY(!checker->busy());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("this build has no network client"));
    QCOMPARE(failed.at(0).at(1).toBool(), true);
    QCOMPARE(available.count() + up_to_date.count(), 0);
  }

  // The answer may come before or during the destructor's join: either way
  // nothing is delivered to the dead checker.
  void destroyedWhileInFlight() {
    fake_->hold();
    fake_->answer(200, release_json("v999.0.0"));
    auto checker = make();
    std::atomic<int> delivered{0};
    connect(checker.get(), &UpdateChecker::available, [&delivered] { ++delivered; });
    checker->check_now();
    std::thread releaser([this] { fake_->release(); });
    checker.reset();
    releaser.join();
    QCoreApplication::processEvents();
    QCOMPARE(delivered.load(), 0);
    QCOMPARE(fake_->sent().size(), std::size_t{1});
  }
};

QTEST_MAIN(TestUpdateChecker)
#include "test_update_checker.moc"
