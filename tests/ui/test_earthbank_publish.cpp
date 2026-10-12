// Data browser, Publish to EarthBank: the action and its dialog. One row per
// sample with the ages it could report, the three things it can do with them
// (check the terms, write the workbooks, upload) on a worker thread, and what
// it says afterwards. The service is the in-memory fake: nothing here reaches
// EarthBank.

#include <cstddef>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include "analyses.hpp"
#include "data_browser_window.hpp"
#include "data_workspace.hpp"
#include "earthbank_fixture.hpp"
#include "earthbank_package_dialog.hpp"
#include "earthbank_publish_dialog.hpp"
#include "earthbank_worker.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"
#include "settings_guard.hpp"

using namespace pychron;
namespace eb = pychron::earthbank;
namespace pp = pychron::processing;
using eb::testing::AnalysisList;
using pychron::ui::DataBrowserWindow;
using pychron::ui::EarthbankPublishDialog;
using Action = pychron::ui::EarthbankPublishDialog::Action;
using Column = pychron::ui::EarthbankPublishDialog::Column;

namespace {

// The two fixture samples with every term one the fake's lists hold: the
// heated sample's stratigraphic unit is not, and is taken off.
AnalysisList publishable() {
  return eb::testing::with_edit_all(eb::testing::both_samples(), [](pp::Analysis& a) { a.sample_info.unit.clear(); });
}

}  // namespace

class EarthbankPublishTest : public QObject {
  Q_OBJECT

  struct World : ui::test::EarthbankWorld {
    std::function<std::unique_ptr<QSettings>()> settings() const {
      return [path = dir.filePath(QStringLiteral("ui.ini"))] {
        return std::make_unique<QSettings>(path, QSettings::IniFormat);
      };
    }
    // A dialog on `analyses`, surveyed, that says yes to an upload.
    std::unique_ptr<EarthbankPublishDialog> dialog(const AnalysisList& analyses = publishable()) const {
      auto d = std::make_unique<EarthbankPublishDialog>(analyses, setup(), settings());
      d->ask_confirm = [](const QString&) { return true; };
      d->setup_name()->setText(QStringLiteral("Noble gas lab"));
      return d;
    }
  };

  static int row_of(const EarthbankPublishDialog& d, const QString& sample) {
    for (int r = 0; r < d.table()->rowCount(); ++r) {
      if (d.table()->item(r, static_cast<int>(Column::Sample))->text() == sample) return r;
    }
    return -1;
  }
  static QString cell(const EarthbankPublishDialog& d, int row, Column column) {
    return d.table()->item(row, static_cast<int>(column))->text();
  }
  static void set_action(EarthbankPublishDialog& d, Action action) {
    d.action()->setCurrentIndex(d.action()->findData(static_cast<int>(action)));
  }
  // Presses Run and waits for the dialog to be done with it.
  static void run(EarthbankPublishDialog& d) {
    QVERIFY(d.run_button()->isEnabled());
    d.run_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(d.ready(), 20000);
  }
  static std::size_t record_posts(const eb::testing::FakeEarthBank& bank) {
    return bank.count("POST", "/api/arar/") + bank.count("POST", "/api/core/");
  }

  ui::test::ApplicationSettingsGuard app_settings_;

 private slots:
  void the_action_is_enabled_only_with_a_selection() {
    pp::MemorySource source(eb::testing::both_samples());
    DataBrowserWindow w(source);
    QVERIFY(w.publish_action() != nullptr);
    QVERIFY(!w.publish_action()->isEnabled());
    QSignalSpy asked(&w, &DataBrowserWindow::publish_requested);

    w.select_rows({0, 1});
    QVERIFY(w.publish_action()->isEnabled());
    w.publish_action()->trigger();
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.first().first().toStringList(), w.selected_uuids());
    QCOMPARE(w.selected_uuids().size(), 2);

    w.select_rows({});
    QVERIFY(!w.publish_action()->isEnabled());
  }

  void one_row_per_sample_with_candidates_and_the_rules_pick() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QVERIFY(!d->ready());  // the survey is on the worker's thread
    QVERIFY(!d->run_button()->isEnabled());
    QTRY_VERIFY(d->ready());

    QCOMPARE(d->table()->rowCount(), 2);
    const int fus = row_of(*d, QStringLiteral("S-FUS")), step = row_of(*d, QStringLiteral("S-STEP"));
    QVERIFY(fus >= 0 && step >= 0);
    QCOMPARE(cell(*d, fus, Column::Analyses), QStringLiteral("6"));
    QCOMPARE(cell(*d, step, Column::Analyses), QStringLiteral("10"));
    QVERIFY(cell(*d, step, Column::FluxMonitor).contains(QStringLiteral("FC-2")));

    // Left to the rule, which is said with its age: a plateau for the heated
    // sample, the weighted mean for the fusions.
    QComboBox* ages = d->preferred_age(step);
    QVERIFY(ages != nullptr);
    QCOMPARE(ages->currentIndex(), 0);
    QVERIFY2(ages->currentText().startsWith(QStringLiteral("By the rule: plateau")), qPrintable(ages->currentText()));
    QVERIFY(ages->currentText().contains(QStringLiteral(" ± ")));
    QVERIFY(ages->currentText().endsWith(QStringLiteral(" Ma")));
    QVERIFY2(d->preferred_age(fus)->currentText().startsWith(QStringLiteral("By the rule: weighted mean")),
             qPrintable(d->preferred_age(fus)->currentText()));
    // and every kind after it, each with its own age
    QCOMPARE(ages->count(), 5);
    QStringList kinds;
    for (int i = 1; i < ages->count(); ++i) kinds << ages->itemText(i).section(QLatin1Char(':'), 0, 0);
    QCOMPARE(kinds, QStringList({QStringLiteral("Plateau"), QStringLiteral("Weighted mean"), QStringLiteral("Isochron"),
                                 QStringLiteral("Integrated")}));
    QVERIFY(ages->itemText(2).contains(QStringLiteral(" ± ")));
    QVERIFY(ages->itemText(2) != ages->itemText(4));
    // the fusions have no plateau: the choice is there, greyed, and says why
    const int plateau = 1;
    QVERIFY(!(d->preferred_age(fus)->model()->flags(d->preferred_age(fus)->model()->index(plateau, 0)) &
              Qt::ItemIsEnabled));
    QCOMPARE(w.bank.requests().size(), std::size_t{0});  // none of this asked the service anything
  }

  void the_profile_and_its_package_are_shown() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    QCOMPARE(d->profile()->currentText(), QStringLiteral("mylab"));
    QCOMPARE(d->package()->text(), QString::number(w.bank.writable_package()));
  }

  void with_no_profile_only_the_workbooks_can_be_written() {
    World w;  // no profiles file
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    set_action(*d, Action::Upload);
    QVERIFY(!d->run_button()->isEnabled());
    QVERIFY(d->hint()->text().contains(QStringLiteral("Preferences")));
    set_action(*d, Action::ExportXlsx);
    d->folder()->setText(w.dir.filePath(QStringLiteral("out")));
    QVERIFY(d->run_button()->isEnabled());
  }

  void preflight_marks_the_rows_with_misses() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    const int fus = row_of(*d, QStringLiteral("S-FUS")), step = row_of(*d, QStringLiteral("S-STEP"));
    d->table()->item(step, static_cast<int>(Column::Interpretation))->setText(QStringLiteral("No Such Interpretation"));
    set_action(*d, Action::Preflight);
    run(*d);

    QVERIFY2(cell(*d, step, Column::Notes).contains(QStringLiteral("No Such Interpretation")),
             qPrintable(cell(*d, step, Column::Notes)));
    QVERIFY(!cell(*d, fus, Column::Notes).contains(QStringLiteral("No Such Interpretation")));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("'No Such Interpretation' is not known to EarthBank")));
    QCOMPARE(record_posts(w.bank), std::size_t{0});

    // Corrected, the row is clean again.
    d->table()->item(step, static_cast<int>(Column::Interpretation))->setText(QString());
    run(*d);
    QVERIFY(!cell(*d, step, Column::Notes).contains(QStringLiteral("No Such Interpretation")));
  }

  void export_writes_the_workbooks() {
    World w;  // no profile, no password: an export needs neither
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    QString shown;
    d->show_folder = [&shown](const QString& folder) { shown = folder; };
    const QString out = w.dir.filePath(QStringLiteral("workbooks"));
    set_action(*d, Action::ExportXlsx);
    d->folder()->setText(out);
    d->per_sample()->setChecked(true);
    QVERIFY(!d->show_folder_button()->isEnabled());
    run(*d);

    const QStringList files = QDir(out).entryList(QDir::Files, QDir::Name);
    QCOMPARE(files.size(), 3);
    QCOMPARE(files[0], QStringLiteral("ArArDataPoint_S-FUS.xlsx"));
    QCOMPARE(files[1], QStringLiteral("ArArDataPoint_S-STEP.xlsx"));
    QVERIFY(files[2].startsWith(QStringLiteral("Samples_")));
    for (const QString& file : files) QVERIFY2(d->result()->toPlainText().contains(file), qPrintable(file));
    QVERIFY(d->show_folder_button()->isEnabled());
    d->show_folder_button()->click();
    QCOMPARE(shown, out);
    QCOMPARE(w.bank.requests().size(), std::size_t{0});

    // Without the sample workbook, and all the data points in one.
    const QString again = w.dir.filePath(QStringLiteral("again"));
    d->folder()->setText(again);
    d->per_sample()->setChecked(false);
    d->include_samples()->setChecked(false);
    run(*d);
    QCOMPARE(QDir(again).entryList(QDir::Files).size(), 1);
  }

  void an_export_with_no_folder_is_not_run() {
    World w;
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    set_action(*d, Action::ExportXlsx);
    d->folder()->clear();
    QVERIFY(!d->run_button()->isEnabled());
  }

  void upload_asks_first_and_writes_on_yes() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    QString asked;
    std::size_t posts_when_asked = 99;
    d->ask_confirm = [&](const QString& text) {
      asked = text;
      posts_when_asked = record_posts(w.bank);
      return true;
    };
    set_action(*d, Action::Upload);
    run(*d);

    // Who, where and how much, before anything was written.
    QCOMPARE(posts_when_asked, std::size_t{0});
    QVERIFY2(asked.contains(QStringLiteral("Robin Example (robin.example)")), qPrintable(asked));
    QVERIFY(asked.contains(QStringLiteral("Example Lab Ar-Ar 2026")));
    QVERIFY(asked.contains(QString::number(w.bank.writable_package())));
    QVERIFY(asked.contains(QStringLiteral("S-STEP: 1 data point, 1 aliquot, 10 measurements")));
    QVERIFY(asked.contains(QStringLiteral("S-FUS: 1 data point, 6 aliquots, 6 measurements")));
    QVERIFY(asked.contains(QStringLiteral("cannot be changed or removed")));

    const auto store = w.bank.store();
    QCOMPARE(store.samples.size(), std::size_t{2});
    QCOMPARE(store.data_points.size(), std::size_t{2});
    QCOMPARE(store.measurements.size(), std::size_t{16});
    const QString said = d->result()->toPlainText();
    QVERIFY2(said.contains(QStringLiteral("S-FUS: complete, data point %1").arg(store.data_points[0].id)),
             qPrintable(said));
    QVERIFY(said.contains(QStringLiteral("S-STEP: complete, data point %1").arg(store.data_points[1].id)));
    QVERIFY(said.contains(QStringLiteral("S-STEP: age summary: saved, id ")));  // a line for each record
    QCOMPARE(d->progress()->value(), d->progress()->maximum());

    // Again: it is all there, and nothing is sent twice.
    const std::size_t sent = record_posts(w.bank);
    run(*d);
    QCOMPARE(record_posts(w.bank), sent);
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-STEP: complete")));
  }

  void upload_writes_nothing_on_no() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    d->ask_confirm = [](const QString&) { return false; };
    set_action(*d, Action::Upload);
    run(*d);
    QCOMPARE(record_posts(w.bank), std::size_t{0});
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("Nothing was sent")));
    QTRY_COMPARE(d->worker()->outstanding(), 0);
  }

  void an_upload_with_an_unknown_term_is_not_offered() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    bool asked = false;
    d->ask_confirm = [&asked](const QString&) {
      asked = true;
      return true;
    };
    const int step = row_of(*d, QStringLiteral("S-STEP"));
    d->table()->item(step, static_cast<int>(Column::Interpretation))->setText(QStringLiteral("No Such Interpretation"));
    set_action(*d, Action::Upload);
    run(*d);
    QVERIFY(!asked);
    QCOMPARE(record_posts(w.bank), std::size_t{0});
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("Nothing was sent")));
    QVERIFY(cell(*d, step, Column::Notes).contains(QStringLiteral("No Such Interpretation")));

    // Allowed, it is asked and goes without that field.
    d->allow_unresolved()->setChecked(true);
    run(*d);
    QVERIFY(asked);
    QCOMPARE(w.bank.store().data_points.size(), std::size_t{2});
  }

  void upload_with_no_package_says_so_and_sends_nothing() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    d->package()->clear();
    set_action(*d, Action::Upload);
    run(*d);
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("No data package is chosen")));
    QCOMPARE(w.bank.requests().size(), std::size_t{0});
  }

  void a_wrong_password_is_said_and_nothing_is_sent() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    d->password()->setText(QStringLiteral("not-the-password"));
    set_action(*d, Action::Upload);
    run(*d);
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("Nothing was sent")));
    QVERIFY(!d->result()->toPlainText().contains(QStringLiteral("not-the-password")));
    QCOMPARE(record_posts(w.bank), std::size_t{0});
  }

  void cancel_stops_and_a_rerun_finishes() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    set_action(*d, Action::Upload);
    QVERIFY(!d->cancel_button()->isEnabled());

    w.gate.hold_at("POST", "/api/arar/ArArMeasurement");
    d->run_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(w.gate.waiting(), 20000);  // the first measurement is on its way
    QVERIFY(d->cancel_button()->isEnabled());
    QVERIFY(!d->run_button()->isEnabled());
    d->cancel_button()->click();
    w.gate.release();
    QTRY_VERIFY_WITH_TIMEOUT(d->ready(), 20000);

    // The record that was out is in; nothing after it was sent.
    QCOMPARE(w.bank.store().measurements.size(), std::size_t{1});
    QCOMPARE(w.bank.store().data_points.size(), std::size_t{1});
    QVERIFY2(d->result()->toPlainText().contains(QStringLiteral("S-FUS: stopped, data point")),
             qPrintable(d->result()->toPlainText()));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-STEP: stopped\n")));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("run the upload again")));

    run(*d);
    const auto store = w.bank.store();
    QCOMPARE(store.samples.size(), std::size_t{2});
    QCOMPARE(store.data_points.size(), std::size_t{2});
    QCOMPARE(store.aliquots.size(), std::size_t{7});
    QCOMPARE(store.measurements.size(), std::size_t{16});  // none of them twice
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-FUS: complete")));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-STEP: complete")));
  }

  void an_in_doubt_record_that_is_there_is_finished_with_its_id() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    set_action(*d, Action::Upload);
    QVERIFY(!d->there_button()->isVisibleTo(d.get()));

    w.bank.drop_next("POST", "/api/arar/ArArAliquot");  // saved, and the answer lost
    run(*d);
    QVERIFY2(d->result()->toPlainText().contains(QStringLiteral("S-FUS: in doubt")),
             qPrintable(d->result()->toPlainText()));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("may or may not have been saved")));
    QVERIFY(d->there_button()->isVisibleTo(d.get()));
    QVERIFY(d->not_there_button()->isVisibleTo(d.get()));
    QVERIFY(!d->there_button()->isEnabled());  // not without its id
    // Nothing more of that sample was sent; the other sample went on, with its one aliquot.
    QCOMPARE(w.bank.store().aliquots.size(), std::size_t{2});
    QCOMPARE(w.bank.store().data_points.size(), std::size_t{2});
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-STEP: complete")));

    // The user looks on EarthBank and finds it.
    bool asked_again = false;
    d->ask_confirm = [&asked_again](const QString&) {
      asked_again = true;
      return true;
    };
    d->in_doubt_id()->setText(QString::number(w.bank.store().aliquots[0].id));
    QVERIFY(d->there_button()->isEnabled());
    d->there_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(d->ready(), 20000);
    QVERIFY(!asked_again);  // it finishes the upload that was agreed to
    QCOMPARE(w.bank.store().aliquots.size(), std::size_t{7});  // the one in doubt was not sent again
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-FUS: complete")));
    QVERIFY(!d->there_button()->isVisibleTo(d.get()));
  }

  void an_in_doubt_record_that_is_not_there_is_sent_again() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    set_action(*d, Action::Upload);
    w.bank.lose_next("POST", "/api/arar/ArArAliquot");  // never saved, and no answer
    run(*d);
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-FUS: in doubt")));
    QCOMPARE(w.bank.store().aliquots.size(), std::size_t{1});  // the other sample's

    d->not_there_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(d->ready(), 20000);
    QCOMPARE(w.bank.store().aliquots.size(), std::size_t{7});
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-FUS: complete")));
  }

  void the_in_doubt_answer_is_withdrawn_when_what_would_be_sent_changes() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    int asked = 0;
    d->ask_confirm = [&asked](const QString&) {
      ++asked;
      return true;
    };
    set_action(*d, Action::Upload);
    w.bank.lose_next("POST", "/api/arar/ArArAliquot");
    run(*d);
    QCOMPARE(asked, 1);
    QVERIFY(d->not_there_button()->isVisibleTo(d.get()));

    // The answer finishes the upload that was agreed to. Once something of
    // it is changed (where it goes, what a row says, what is done), it is no
    // longer that upload, and the buttons go: Run asks again. Each change is
    // put back before the run, so the same record is still in doubt after it.
    const int step = row_of(*d, QStringLiteral("S-STEP"));
    QTableWidgetItem* interpretation = d->table()->item(step, static_cast<int>(Column::Interpretation));
    const QString package = d->package()->text();
    struct Change {
      std::function<void()> make, undo;
    };
    const std::vector<Change> changes{
        {[&] { interpretation->setText(QStringLiteral("Something else")); }, [&] { interpretation->setText(QString()); }},
        {[&] { d->package()->setText(QStringLiteral("610002")); }, [&] { d->package()->setText(package); }},
        {[&] { d->preferred_age(step)->setCurrentIndex(2); }, [&] { d->preferred_age(step)->setCurrentIndex(0); }},
        {[&] { d->allow_unresolved()->setChecked(true); }, [&] { d->allow_unresolved()->setChecked(false); }},
        {[&] { d->decay_constant()->setText(QStringLiteral("Min (2000)")); }, [&] { d->decay_constant()->clear(); }},
        {[&] { set_action(*d, Action::Preflight); }, [&] { set_action(*d, Action::Upload); }},
    };
    for (const Change& change : changes) {
      QVERIFY(d->not_there_button()->isVisibleTo(d.get()));
      change.make();
      QVERIFY(!d->not_there_button()->isVisibleTo(d.get()));
      QVERIFY(!d->there_button()->isVisibleTo(d.get()));
      change.undo();
      QVERIFY(!d->not_there_button()->isVisibleTo(d.get()));  // put back is not agreed to either
      const int before = asked;
      run(*d);
      QCOMPARE(asked, before + 1);
    }
    // Untouched since, the answer finishes it without another question.
    QVERIFY(d->not_there_button()->isVisibleTo(d.get()));
    d->not_there_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(d->ready(), 20000);
    QCOMPARE(asked, 1 + static_cast<int>(changes.size()));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-FUS: complete")));
  }

  void a_package_list_that_fails_late_is_not_taken_for_the_run() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    set_action(*d, Action::Preflight);
    w.bank.fail_next("GET", "/api/management/data-packages", 403, R"({"detail":"packages are closed today"})");
    w.gate.hold_at("GET", "/api/management/data-packages");
    d->package_button()->click();
    QVERIFY(d->package_dialog() != nullptr);
    QTRY_VERIFY(w.gate.waiting());
    d->package_dialog()->reject();  // closed with its list still on the way
    QTRY_VERIFY(d->package_dialog() == nullptr);

    // Nothing is run while a request is out: its answer is not the run's.
    QVERIFY(!d->run_button()->isEnabled());
    w.gate.release();
    QTRY_VERIFY(d->run_button()->isEnabled());
    QVERIFY2(!d->result()->toPlainText().contains(QStringLiteral("EarthBank does not let")),
             qPrintable(d->result()->toPlainText()));
    run(*d);
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("Every term the upload would send is known")));
  }

  void a_disagreeing_sample_is_shown_and_left_out() {
    World w;
    AnalysisList analyses = eb::testing::fusion_sample();
    const AnalysisList steps =
        eb::testing::with_edit(eb::testing::step_sample(), 3, [](pp::Analysis& a) { a.material = "biotite"; });
    analyses.insert(analyses.end(), steps.begin(), steps.end());
    auto d = w.dialog(analyses);
    QTRY_VERIFY(d->ready());

    const int fus = row_of(*d, QStringLiteral("S-FUS")), step = row_of(*d, QStringLiteral("S-STEP"));
    QVERIFY2(cell(*d, step, Column::Notes).contains(QStringLiteral("material")), qPrintable(cell(*d, step, Column::Notes)));
    QVERIFY(cell(*d, step, Column::Notes).contains(QStringLiteral("biotite")));
    QVERIFY(!(d->table()->item(step, static_cast<int>(Column::Interpretation))->flags() & Qt::ItemIsEnabled));
    QVERIFY(d->table()->item(fus, static_cast<int>(Column::Interpretation))->flags() & Qt::ItemIsEnabled);

    const QString out = w.dir.filePath(QStringLiteral("workbooks"));
    set_action(*d, Action::ExportXlsx);
    d->folder()->setText(out);
    d->per_sample()->setChecked(true);
    run(*d);
    const QStringList files = QDir(out).entryList({QStringLiteral("ArArDataPoint_*")}, QDir::Files);
    QCOMPARE(files, QStringList({QStringLiteral("ArArDataPoint_S-FUS.xlsx")}));
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-STEP: left out")));
  }

  void choices_are_remembered_per_sample() {
    World w;
    {
      auto d = w.dialog();
      QTRY_VERIFY(d->ready());
      const int step = row_of(*d, QStringLiteral("S-STEP"));
      d->table()->item(step, static_cast<int>(Column::Interpretation))->setText(QStringLiteral("Eruption age"));
      d->table()->item(step, static_cast<int>(Column::FluxMonitor))->setText(QStringLiteral("FC-2 28.201 Ma (Kuiper 2008)"));
      d->preferred_age(step)->setCurrentIndex(2);  // weighted mean
      d->decay_constant()->setText(QStringLiteral("Min (2000)"));
      set_action(*d, Action::ExportXlsx);
      d->folder()->setText(w.dir.filePath(QStringLiteral("out")));
      run(*d);
    }
    auto d = w.dialog();
    d->setup_name()->clear();  // World::dialog types one; what was kept is asked for below
    QTRY_VERIFY(d->ready());
    const int fus = row_of(*d, QStringLiteral("S-FUS")), step = row_of(*d, QStringLiteral("S-STEP"));
    QCOMPARE(cell(*d, step, Column::Interpretation), QStringLiteral("Eruption age"));
    QCOMPARE(cell(*d, step, Column::FluxMonitor), QStringLiteral("FC-2 28.201 Ma (Kuiper 2008)"));
    QCOMPARE(d->preferred_age(step)->currentIndex(), 2);
    QCOMPARE(d->decay_constant()->text(), QStringLiteral("Min (2000)"));
    // the other sample was left alone
    QCOMPARE(cell(*d, fus, Column::Interpretation), QString());
    QCOMPARE(d->preferred_age(fus)->currentIndex(), 0);
    // in the UI's settings, by sample name; nothing of it went to EarthBank
    QVERIFY(w.settings()()->allKeys().filter(QStringLiteral("earthbank")).size() >= 3);
    QCOMPARE(w.bank.requests().size(), std::size_t{0});
  }

  void the_chosen_age_is_what_is_sent() {
    World w;
    w.signed_up();
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    const int step = row_of(*d, QStringLiteral("S-STEP"));
    d->preferred_age(step)->setCurrentIndex(4);  // integrated
    set_action(*d, Action::Upload);
    run(*d);
    const auto store = w.bank.store();
    QCOMPARE(store.age_summaries.size(), std::size_t{2});
    const auto integrated = w.bank.lookup_id("/api/arar/LAgeType", eb::age_type_name(eb::AgeKind::Integrated));
    QVERIFY(integrated.has_value());
    QCOMPARE(store.age_summaries[1].body.value("ageCalcTypeId", std::int64_t{0}), *integrated);
  }

  void no_password_or_token_appears_in_the_result_pane() {
    World w;
    eb::Profile p = w.profile();
    p.data_package_id = w.bank.writable_package();
    w.write(p);  // no password kept: it is typed
    auto d = w.dialog();
    QTRY_VERIFY(d->ready());
    QString asked;
    d->ask_confirm = [&asked](const QString& text) {
      asked = text;
      return true;
    };
    d->password()->setText(QString::fromStdString(w.bank.password()));
    set_action(*d, Action::Upload);
    w.bank.expire_tokens();
    w.bank.fail_next("POST", "/api/arar/ArArAgeSummary", 500, R"({"detail":"boom"})");
    run(*d);
    set_action(*d, Action::Preflight);
    run(*d);

    QVERIFY(!w.spy.secrets().empty());
    QString everything = asked + d->result()->toPlainText();
    for (int r = 0; r < d->table()->rowCount(); ++r) everything += cell(*d, r, Column::Notes);
    for (const std::string& secret : w.spy.secrets()) {
      QVERIFY2(!everything.contains(QString::fromStdString(secret)), "a token is shown");
    }
    QVERIFY(!everything.contains(QString::fromStdString(w.bank.password())));
    QCOMPARE(d->password()->echoMode(), QLineEdit::Password);
  }

  void many_samples_show_progress() {
    World w;
    AnalysisList analyses;
    for (int i = 0; i < 300; ++i) {
      for (int k = 0; k < 2; ++k) {
        auto a = eb::testing::fusion_analysis(k);
        a->sample = "S-" + std::to_string(1000 + i);
        a->identifier = std::to_string(30000 + i);
        a->uuid = "many-" + std::to_string(i) + "-" + std::to_string(k);
        a->runid = pp::make_runid(a->identifier, a->aliquot, -1);
        analyses.push_back(a);
      }
    }
    auto d = w.dialog(analyses);
    QSignalSpy progressed(d->worker(), &ui::EarthbankWorker::progress);
    QVERIFY(!d->ready());

    // this thread's event loop turns while the worker's goes through them
    int ticks = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, &timer, [&ticks] { ++ticks; });
    timer.start(0);
    QTRY_VERIFY_WITH_TIMEOUT(d->ready(), 60000);
    QVERIFY(ticks > 0);
    QCOMPARE(progressed.count(), 300);
    QCOMPARE(d->table()->rowCount(), 300);
    QCOMPARE(d->progress()->value(), d->progress()->maximum());
  }

  void closing_during_the_survey_does_not_wait_for_it() {
    World w;
    AnalysisList analyses;
    for (int i = 0; i < 300; ++i) {
      auto a = eb::testing::fusion_analysis(0);
      a->sample = "S-" + std::to_string(1000 + i);
      analyses.push_back(a);
    }
    auto d = w.dialog(analyses);
    QElapsedTimer clock;
    clock.start();
    d.reset();
    QVERIFY(clock.elapsed() < 5000);
  }

  void the_workspace_opens_the_dialog_on_the_selection() {
    World w;
    pp::MemorySource source(eb::testing::both_samples());
    pp::PresetStore presets(w.folder() / "presets");
    QWidget owner;
    ui::DataWorkspace workspace(&owner, {});
    workspace.set_source(&source, &presets);
    workspace.set_settings(w.settings());
    workspace.set_earthbank_setup(w.setup());
    DataBrowserWindow* browser = workspace.browser();
    QVERIFY(browser != nullptr);
    QVERIFY(workspace.publish_dialog() == nullptr);

    QList<int> rows;
    for (int r = 0; r < 16; ++r) rows << r;
    browser->select_rows(rows);
    QCOMPARE(browser->selected_uuids().size(), 16);
    browser->publish_action()->trigger();
    QTRY_VERIFY(workspace.publish_dialog() != nullptr);
    EarthbankPublishDialog* d = workspace.publish_dialog();
    QTRY_VERIFY(d->ready());
    QCOMPARE(d->table()->rowCount(), 2);
    QCOMPARE(cell(*d, row_of(*d, QStringLiteral("S-STEP")), Column::Analyses), QStringLiteral("10"));
  }

  void a_second_publish_does_not_replace_a_dialog_that_is_uploading() {
    World w;
    w.signed_up();
    pp::MemorySource source(publishable());
    pp::PresetStore presets(w.folder() / "presets");
    QWidget owner;
    ui::DataWorkspace workspace(&owner, {});
    workspace.set_source(&source, &presets);
    workspace.set_settings(w.settings());
    workspace.set_earthbank_setup(w.setup());
    DataBrowserWindow* browser = workspace.browser();
    QList<int> rows;
    for (int r = 0; r < 16; ++r) rows << r;
    browser->select_rows(rows);
    browser->publish_action()->trigger();
    QTRY_VERIFY(workspace.publish_dialog() != nullptr);
    QPointer<EarthbankPublishDialog> d = workspace.publish_dialog();
    QTRY_VERIFY(d->ready());
    d->ask_confirm = [](const QString&) { return true; };
    d->setup_name()->setText(QStringLiteral("Noble gas lab"));
    set_action(*d, Action::Upload);
    w.gate.hold_at("POST", "/api/arar/ArArMeasurement");
    d->run_button()->click();
    QTRY_VERIFY_WITH_TIMEOUT(w.gate.waiting(), 20000);

    browser->select_rows({0, 1});
    browser->publish_action()->trigger();
    QTRY_VERIFY2(browser->status()->text().contains(QStringLiteral("still running")), qPrintable(browser->status()->text()));
    QVERIFY(!d.isNull());
    QCOMPARE(workspace.publish_dialog(), d.data());

    w.gate.release();
    QTRY_VERIFY_WITH_TIMEOUT(d->ready(), 20000);
    QCOMPARE(w.bank.store().measurements.size(), std::size_t{16});
    QVERIFY(d->result()->toPlainText().contains(QStringLiteral("S-STEP: complete")));

    // Idle, it is replaced by one on the new selection.
    browser->publish_action()->trigger();
    QTRY_VERIFY(workspace.publish_dialog() != nullptr && workspace.publish_dialog() != d.data());
    EarthbankPublishDialog* next = workspace.publish_dialog();
    QTRY_VERIFY(next->ready());
    int analyses = 0;
    for (int r = 0; r < next->table()->rowCount(); ++r) analyses += cell(*next, r, Column::Analyses).toInt();
    QCOMPARE(analyses, 2);
  }

  void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
};

QTEST_MAIN(EarthbankPublishTest)
#include "test_earthbank_publish.moc"
