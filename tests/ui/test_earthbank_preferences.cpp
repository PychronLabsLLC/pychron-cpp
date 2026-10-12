// File > Preferences, EarthBank: the profiles as a table kept in
// profiles.toml, the password kept in the secret store and nowhere else, Test
// signing in on a worker thread, and the package dialog. The service is the
// in-memory fake, the store a memory store and the file one in a temp folder:
// nothing here reaches EarthBank, the keychain or the user's own profiles.

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include "earthbank_package_dialog.hpp"
#include "earthbank_preferences_page.hpp"
#include "earthbank_fixture.hpp"
#include "earthbank_worker.hpp"
#include "preferences_dialog.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "settings_guard.hpp"

using namespace pychron;
namespace eb = pychron::earthbank;
using pychron::ui::EarthbankPackageDialog;
using pychron::ui::EarthbankPreferencesPage;
using pychron::ui::EarthbankSetup;
using Column = pychron::ui::EarthbankPreferencesPage::Column;

namespace {

std::string slurp(const std::filesystem::path& file) {
  const std::ifstream in(file);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

class EarthbankPreferencesTest : public QObject {
  Q_OBJECT

  using World = ui::test::EarthbankWorld;

  static void type(QTableWidget* table, int row, Column column, const QString& text) {
    table->item(row, static_cast<int>(column))->setText(text);
  }
  static QString cell(QTableWidget* table, int row, Column column) {
    return table->item(row, static_cast<int>(column))->text();
  }
  // A new row holding `p`.
  static void add(EarthbankPreferencesPage& page, const eb::Profile& p) {
    page.add_button()->click();
    const int row = page.table()->rowCount() - 1;
    type(page.table(), row, Column::Name, QString::fromStdString(p.name));
    type(page.table(), row, Column::Server, QString::fromStdString(p.base_url));
    type(page.table(), row, Column::Username, QString::fromStdString(p.username));
    if (p.institution_id) type(page.table(), row, Column::Institution, QString::number(*p.institution_id));
    page.table()->selectRow(row);
  }

  ui::test::ApplicationSettingsGuard app_settings_;

 private slots:
  void adding_a_profile_and_applying_writes_the_file() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    QCOMPARE(page.table()->rowCount(), 0);

    add(page, w.profile());
    QCOMPARE(page.apply(), QString());

    const auto saved = eb::load_profiles(w.file());
    QVERIFY(saved);
    QCOMPARE(saved->profiles.size(), std::size_t{1});
    QCOMPARE(saved->profiles[0], w.profile());
    QCOMPARE(saved->active, std::string("mylab"));  // the only profile is the one used
  }

  void a_saved_file_is_shown_and_an_untouched_page_writes_nothing() {
    World w;
    eb::Profile p = w.profile();
    p.data_package_id = 610001;
    w.write(p);
    const auto before = std::filesystem::last_write_time(w.file());

    EarthbankPreferencesPage page(w.setup());
    page.load();
    QCOMPARE(page.table()->rowCount(), 1);
    QCOMPARE(cell(page.table(), 0, Column::Name), QStringLiteral("mylab"));
    QCOMPARE(cell(page.table(), 0, Column::Package), QStringLiteral("610001"));
    QCOMPARE(page.table()->item(0, static_cast<int>(Column::Active))->checkState(), Qt::Checked);

    QCOMPARE(page.apply(), QString());
    QCOMPARE(std::filesystem::last_write_time(w.file()), before);
  }

  void a_page_never_shown_reads_and_writes_nothing() {
    World w;
    bool asked = false;
    EarthbankSetup setup = w.setup();
    setup.secrets = [&] {
      asked = true;
      return w.store;
    };
    EarthbankPreferencesPage page(setup);
    QCOMPARE(page.apply(), QString());
    QVERIFY(!asked);
    QVERIFY(!std::filesystem::exists(w.file()));
  }

  void a_file_that_cannot_be_read_is_said_and_never_written_over() {
    World w;
    {
      std::ofstream out(w.file());
      out << "active = \"x\"\n[profiles.x]\npassword = \"oops\"\n";
    }
    const std::string before = slurp(w.file());

    EarthbankPreferencesPage page(w.setup());
    page.load();
    QVERIFY(page.status()->text().contains(QStringLiteral("password")));
    QVERIFY(!page.status()->text().contains(QStringLiteral("oops")));
    QVERIFY(!page.add_button()->isEnabled());
    QCOMPARE(page.apply(), QString());
    QCOMPARE(slurp(w.file()), before);
  }

  void a_wrong_field_is_refused_and_nothing_is_written() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    type(page.table(), 0, Column::Institution, QStringLiteral("melbourne"));
    QVERIFY(page.apply().contains(QStringLiteral("institution id")));
    QVERIFY(!std::filesystem::exists(w.file()));

    type(page.table(), 0, Column::Institution, QStringLiteral("9301"));
    type(page.table(), 0, Column::Server, QStringLiteral("http://elsewhere.test"));
    QVERIFY(!page.apply().isEmpty());  // save_profiles' own sentence
    QVERIFY(!std::filesystem::exists(w.file()));
  }

  void only_one_profile_is_active() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile("one"));
    add(page, w.profile("two"));
    auto active = [&](int row) { return page.table()->item(row, static_cast<int>(Column::Active)); };
    QCOMPARE(active(0)->checkState(), Qt::Checked);
    active(1)->setCheckState(Qt::Checked);
    QCOMPARE(active(0)->checkState(), Qt::Unchecked);
    QCOMPARE(page.apply(), QString());
    QCOMPARE(eb::load_profiles(w.file())->active, std::string("two"));
  }

  void the_password_is_not_in_the_file_and_is_in_the_store() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.password()->setText(QStringLiteral("typed-but-never-tested"));
    QCOMPARE(page.apply(), QString());

    QTRY_COMPARE(w.stored(w.profile()), std::optional<std::string>("typed-but-never-tested"));
    QVERIFY(slurp(w.file()).find("typed-but-never-tested") == std::string::npos);
    QCOMPARE(page.password()->text(), QString());
    QCOMPARE(page.password()->echoMode(), QLineEdit::Password);
    // not in the table either
    for (int c = 0; c < page.table()->columnCount(); ++c) {
      QVERIFY(!page.table()->item(0, c)->text().contains(QStringLiteral("typed-but")));
    }
    QCOMPARE(w.bank.requests().size(), std::size_t{0});  // applying asks the service nothing
  }

  void the_store_in_use_is_named() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    QCOMPARE(page.store_line()->text(), QStringLiteral("Passwords are not stored: this session only."));
  }

  void test_shows_the_accounts_name() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.password()->setText(QString::fromStdString(w.bank.password()));
    page.test_button()->click();

    QTRY_VERIFY(page.status()->text().contains(QStringLiteral("Robin Example")));
    QVERIFY(page.status()->text().contains(QStringLiteral("robin.example")));
    // a password that signed in is kept, and the field emptied
    QCOMPARE(w.stored(w.profile()), std::optional<std::string>(w.bank.password()));
    QCOMPARE(page.password()->text(), QString());
    QVERIFY(!page.status()->text().contains(QString::fromStdString(w.bank.password())));
  }

  void test_uses_the_stored_password_when_none_is_typed() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.test_button()->click();
    QTRY_VERIFY(page.status()->text().contains(QStringLiteral("Robin Example")));
  }

  void test_with_no_password_anywhere_says_to_type_one_and_sends_nothing() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.test_button()->click();
    QTRY_VERIFY(page.status()->text().contains(QStringLiteral("password")));
    QCOMPARE(w.bank.requests().size(), std::size_t{0});
  }

  void test_with_a_wrong_password_shows_the_sentence_and_stores_nothing() {
    World w;
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.password()->setText(QStringLiteral("not-the-password"));
    page.test_button()->click();

    QVERIFY(!page.test_button()->isEnabled());  // until the answer is in
    QTRY_VERIFY(page.test_button()->isEnabled());
    QVERIFY2(page.status()->text().contains(QStringLiteral("password"), Qt::CaseInsensitive),
             qPrintable(page.status()->text()));
    QVERIFY(!page.status()->text().contains(QStringLiteral("not-the-password")));
    QVERIFY(!page.status()->text().contains(QStringLiteral("Robin")));
    QCOMPARE(w.stored(w.profile()), std::optional<std::string>());
  }

  void selecting_a_package_sets_its_id() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.package_button()->click();

    EarthbankPackageDialog* dialog = page.package_dialog();
    QVERIFY(dialog != nullptr);
    QTRY_VERIFY(dialog->packages()->rowCount() >= 2);
    QVERIFY(!dialog->buttons()->button(QDialogButtonBox::Ok)->isEnabled());  // nothing chosen yet

    dialog->filter()->setText(QStringLiteral("archive"));
    int shown = 0, row = -1;
    for (int r = 0; r < dialog->packages()->rowCount(); ++r) {
      if (dialog->packages()->isRowHidden(r)) continue;
      ++shown;
      row = r;
    }
    QCOMPARE(shown, 1);
    QCOMPARE(dialog->packages()->item(row, 0)->text(), QStringLiteral("Example Lab archive"));
    dialog->packages()->selectRow(row);
    QVERIFY(dialog->buttons()->button(QDialogButtonBox::Ok)->isEnabled());
    dialog->buttons()->button(QDialogButtonBox::Ok)->click();

    QCOMPARE(cell(page.table(), 0, Column::Package), QStringLiteral("610002"));
    QCOMPARE(page.apply(), QString());
    QCOMPARE(eb::load_profiles(w.file())->profiles[0].data_package_id, std::optional<std::int64_t>(610002));
  }

  void cancelling_the_package_dialog_changes_nothing() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.package_button()->click();
    EarthbankPackageDialog* dialog = page.package_dialog();
    QTRY_VERIFY(dialog->packages()->rowCount() >= 2);
    dialog->packages()->selectRow(0);
    dialog->buttons()->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(cell(page.table(), 0, Column::Package), QString());
  }

  void creating_a_package_selects_it() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.package_button()->click();
    EarthbankPackageDialog* dialog = page.package_dialog();
    QTRY_VERIFY(dialog->packages()->rowCount() >= 2);
    QVERIFY(!dialog->create_button()->isEnabled());  // no name yet

    dialog->name()->setText(QStringLiteral("Tuff of somewhere 2026"));
    dialog->create_button()->click();
    QTRY_COMPARE(dialog->selected_id(), std::optional<std::int64_t>(620001));
    QCOMPARE(w.bank.count("POST", "/api/management/data-packages"), std::size_t{1});
    const int row = dialog->packages()->currentRow();
    QCOMPARE(dialog->packages()->item(row, 0)->text(), QStringLiteral("Tuff of somewhere 2026"));

    dialog->buttons()->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(cell(page.table(), 0, Column::Package), QStringLiteral("620001"));
  }

  void a_package_that_could_not_be_listed_says_why_in_the_dialog() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    w.bank.fail_next("GET", "/api/management/data-packages", 403, R"({"detail":"no"})");
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());
    page.package_button()->click();
    EarthbankPackageDialog* dialog = page.package_dialog();
    QTRY_VERIFY(!dialog->status()->text().isEmpty() && !dialog->status()->text().contains(QStringLiteral("Asking")));
    QCOMPARE(dialog->packages()->rowCount(), 0);
  }

  void removing_a_profile_erases_its_password() {
    World w;
    w.write(w.profile());
    QVERIFY(w.store->set(eb::secret_account(w.profile()), "kept-until-removed"));
    EarthbankPreferencesPage page(w.setup());
    page.load();
    page.table()->selectRow(0);
    page.remove_button()->click();
    QCOMPARE(page.table()->rowCount(), 0);
    // not before it is applied: Cancel must be able to drop the removal
    QCOMPARE(w.stored(w.profile()), std::optional<std::string>("kept-until-removed"));

    QCOMPARE(page.apply(), QString());
    QTRY_COMPARE(w.stored(w.profile()), std::optional<std::string>());
    QCOMPARE(eb::load_profiles(w.file())->profiles.size(), std::size_t{0});
  }

  void a_password_applied_just_before_the_page_closes_is_still_kept() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    {
      EarthbankPreferencesPage page(w.setup());
      page.load();
      add(page, w.profile());
      // The worker is busy (a Test waiting for the service) when OK is pressed.
      w.gate.hold();
      page.test_button()->click();
      QTRY_VERIFY(w.gate.waiting());
      page.password()->setText(QStringLiteral("typed-then-ok"));
      QCOMPARE(page.apply(), QString());
    }  // OK closes the dialog at once: the page is gone before the store was reached
    w.gate.release();
    QTRY_COMPARE(w.stored(w.profile()), std::optional<std::string>("typed-then-ok"));
  }

  void a_removal_applied_just_before_the_page_closes_still_erases_the_password() {
    World w;
    w.write(w.profile());
    QVERIFY(w.store->set(eb::secret_account(w.profile()), "kept-until-removed"));
    {
      EarthbankPreferencesPage page(w.setup());
      page.load();
      page.table()->selectRow(0);
      w.gate.hold();
      page.test_button()->click();
      QTRY_VERIFY(w.gate.waiting());
      page.remove_button()->click();
      QCOMPARE(page.apply(), QString());
    }
    w.gate.release();
    QTRY_COMPARE(w.stored(w.profile()), std::optional<std::string>());
  }

  void the_ui_stays_responsive_while_a_request_is_outstanding() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    EarthbankPreferencesPage page(w.setup());
    page.load();
    add(page, w.profile());

    w.gate.hold();
    page.test_button()->click();
    QTRY_VERIFY(w.gate.waiting());  // the request is out, on the worker's thread
    QVERIFY(!page.test_button()->isEnabled());

    // this thread's event loop goes on turning
    int ticks = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, &timer, [&ticks] { ++ticks; });
    timer.start(1);
    QTRY_VERIFY(ticks >= 3);
    QVERIFY(w.gate.waiting());
    QVERIFY(!page.status()->text().contains(QStringLiteral("Robin")));

    w.gate.release();
    QTRY_VERIFY(page.status()->text().contains(QStringLiteral("Robin Example")));
    QVERIFY(page.test_button()->isEnabled());
  }

  void a_page_closed_with_a_request_outstanding_does_not_wait_for_it() {
    World w;
    QVERIFY(w.store->set(eb::secret_account(w.profile()), w.bank.password()));
    auto page = std::make_unique<EarthbankPreferencesPage>(w.setup());
    page->load();
    add(*page, w.profile());
    w.gate.hold();
    page->test_button()->click();
    QTRY_VERIFY(w.gate.waiting());

    QElapsedTimer clock;
    clock.start();
    page.reset();
    QVERIFY(clock.elapsed() < 5000);
    w.gate.release();  // the worker's thread ends on its own; it shares the service
  }

  void the_preferences_dialog_has_the_page_and_ok_saves_it() {
    World w;
    ui::PreferencesDialog dialog(ui::PreferencesDialog::Values{}, {});
    EarthbankPreferencesPage* page = dialog.earthbank_page();
    QVERIFY(page != nullptr);
    page->set_setup(w.setup());
    const int index = dialog.stack()->indexOf(page);
    QVERIFY(index >= 0);
    QCOMPARE(dialog.pages()->item(index)->text(), QStringLiteral("EarthBank"));

    dialog.show();
    dialog.pages()->setCurrentRow(index);  // showing the page loads it
    QTRY_VERIFY(page->loaded());
    add(*page, w.profile());
    type(page->table(), 0, Column::Institution, QStringLiteral("not a number"));
    dialog.buttons()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(dialog.isVisible());  // refused: the dialog stays and says why
    QVERIFY(dialog.problem()->text().contains(QStringLiteral("institution id")));

    type(page->table(), 0, Column::Institution, QStringLiteral("9301"));
    dialog.buttons()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(!dialog.isVisible());
    QCOMPARE(eb::load_profiles(w.file())->profiles.size(), std::size_t{1});
  }

  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(EarthbankPreferencesTest)
#include "test_earthbank_preferences.moc"
