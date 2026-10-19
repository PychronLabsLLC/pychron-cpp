// FieldTableEditorWindow against the sim-integrated example spectrometer on a
// scratch copy of the example tables (configs/examples/tables, the same
// tables profiles/qtegra ships): open, edit, save through the bridge,
// conflicts, holds, Make Current, undo, and the window without a bridge.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QListWidget>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "field_table_editor_window.hpp"
#include "field_table_model.hpp"
#include "pychron/systems/spectrometer/field_table_store.hpp"
#include "spectrometer_bridge.hpp"
#include "spectrometer_fixture.hpp"

using namespace pychron;
using pychron::spectrometer::FieldTable;
using pychron::spectrometer::FieldTableStore;
using pychron::ui::FieldTableEditorWindow;
using pychron::ui::FieldTableModel;
using pychron::ui::SpectrometerBridge;
namespace fs = std::filesystem;

namespace {

constexpr int kWaitMs = 10000;

// The version the example `argon` table ships as current.
const QString kShipped = QStringLiteral("2026-09-30T000000");

int row_of(const FieldTableModel& m, const std::string& isotope) {
  const auto& points = m.table().points();
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (points[i].isotope == isotope) return static_cast<int>(i);
  }
  return -1;
}

double value_at(const FieldTable& t, const std::string& isotope, const std::string& det) {
  for (const auto& p : t.points()) {
    if (p.isotope == isotope) return p.values.at(det);
  }
  return -1;
}

void write_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

}  // namespace

class TestFieldTableEditor : public QObject {
  Q_OBJECT

 private:
  fs::path root() const { return fs::path(dir_->path().toStdString()); }
  FieldTableStore argon() const { return FieldTableStore(root() / "tables", "argon"); }
  QString current() const { return QString::fromStdString(argon().current_version().value()); }

  // Another writer (elctl, a second process) saving a new version.
  QString outside_save(int hours_ahead) {
    auto table = argon().load().value();
    table.mutable_points().front().values["H1"] += 0.001 * hours_ahead;
    const auto when = std::chrono::system_clock::now() + std::chrono::hours(hours_ahead);
    return QString::fromStdString(argon().save(table, when).value());
  }

  // A version saved through the core, as the editor or a peak center does:
  // the spectrometer then holds it in memory too. Waits for the bridge's
  // copy to follow.
  QString core_save(int hours_ahead) {
    QSignalSpy changed(bridge_.get(), &SpectrometerBridge::fieldTableChanged);
    auto table = argon().load().value();
    table.mutable_points().front().values["H1"] += 0.001 * hours_ahead;
    const auto when = std::chrono::system_clock::now() + std::chrono::hours(hours_ahead);
    auto v = sim_->spec->replace_table("argon", table, "", when);
    if (!v) qFatal("replace_table: %s", v.error().what.c_str());
    if (!changed.wait(kWaitMs)) qFatal("no fieldTableChanged");
    return QString::fromStdString(*v);
  }

  std::unique_ptr<FieldTableEditorWindow> make_window(bool with_bridge = true) {
    auto w = std::make_unique<FieldTableEditorWindow>(
        with_bridge ? bridge_.get() : nullptr, root(), sim_->spec->config(), sim_->spec->weights(),
        [] { return QDateTime(QDate(2026, 10, 19), QTime(12, 0), QTimeZone::utc()); });
    w->set_confirm([this](const QString& q) {
      questions_.append(q);
      return confirm_answer_;
    });
    w->set_show_error([this](const QString& e) { errors_.append(e); });
    w->set_ask_unsaved([this](const QString&) {
      ++unsaved_asked_;
      return unsaved_answer_;
    });
    w->set_ask_conflict([this](const QString& now) {
      conflict_now_.append(now);
      return conflict_answer_;
    });
    return w;
  }

  // Sets H1 at Ar40 in the grid.
  static void edit_h1(FieldTableEditorWindow& w, const QString& text) {
    auto* m = w.model();
    QVERIFY(m->setData(m->index(row_of(*m, "Ar40"), m->column_of("H1")), text));
  }

  // Saves and waits for the window to report it.
  static QString save_and_wait(FieldTableEditorWindow& w) {
    QSignalSpy saved(&w, &FieldTableEditorWindow::saved);
    w.save();
    if (saved.isEmpty() && !saved.wait(kWaitMs)) return {};
    return saved.takeFirst().at(1).toString();
  }

  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ui::test::SimSpectrometer> sim_;
  std::unique_ptr<SpectrometerBridge> bridge_;
  QStringList questions_;
  QStringList errors_;
  QStringList conflict_now_;
  bool confirm_answer_ = true;
  int unsaved_asked_ = 0;
  FieldTableEditorWindow::Unsaved unsaved_answer_ = FieldTableEditorWindow::Unsaved::Cancel;
  FieldTableEditorWindow::Conflict conflict_answer_ = FieldTableEditorWindow::Conflict::Cancel;

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "tables", root() / "tables", fs::copy_options::recursive);
    const fs::path r = root();
    sim_ = ui::test::make_sim_spectrometer([&r](spectrometer::cfg::SpectrometerData& data) { data.root = r; });
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    questions_.clear();
    errors_.clear();
    conflict_now_.clear();
    confirm_answer_ = true;
    unsaved_asked_ = 0;
    unsaved_answer_ = FieldTableEditorWindow::Unsaved::Cancel;
    conflict_answer_ = FieldTableEditorWindow::Conflict::Cancel;
  }

  void cleanup() {
    bridge_.reset();
    sim_.reset();
    dir_.reset();
  }

  void opensTheActiveTableAtItsCurrentVersion() {
    auto w = make_window();
    QCOMPARE(w->open_name(), QString("argon"));  // the active table, on construction
    w->open_table("argon");
    QCOMPARE(w->open_version(), kShipped);
    QCOMPARE(w->base_version(), kShipped);
    QCOMPARE(w->versions(), QStringList{kShipped});
    QVERIFY(w->table_names().contains("argon"));
    QVERIFY(w->table_names().contains("argon_hv"));
    QCOMPARE(w->model()->rowCount(), 3);
    QVERIFY(!w->modified());
    QVERIFY(!w->read_only());
    QCOMPARE(w->banner_text(), QString());
    // Names are marked in the list.
    auto* tables = w->findChild<QListWidget*>("tables");
    QVERIFY(tables);
    QStringList shown;
    for (int i = 0; i < tables->count(); ++i) shown << tables->item(i)->text();
    QVERIFY(shown.contains("argon (active)"));
    QVERIFY(shown.contains("argon_hv (HV)"));
    w->check_now();
    QVERIFY(!spectrometer::has_error(w->diagnostics()));
    QVERIFY(!w->save_enabled());  // nothing to save yet
    QVERIFY(w->save_disabled_reason().contains("no changes"));
  }

  void editCellSaveMakesANewCurrentVersionTheBridgeSees() {
    auto w = make_window();
    QSignalSpy changed(bridge_.get(), &SpectrometerBridge::fieldTableChanged);
    edit_h1(*w, "5.0123");
    QVERIFY(w->modified());
    w->check_now();
    QVERIFY2(w->save_enabled(), qPrintable(w->save_disabled_reason()));
    const QString v = save_and_wait(*w);
    QVERIFY(!v.isEmpty());
    QVERIFY(v != kShipped);
    QCOMPARE(current(), v);
    QCOMPARE(w->open_version(), v);
    QCOMPARE(w->base_version(), v);
    QVERIFY(!w->modified());
    QCOMPARE(w->versions().front(), v);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kWaitMs);
    QCOMPARE(changed.last().at(1).toString(), v);
    QCOMPARE(value_at(bridge_->table_copy("argon"), "Ar40", "H1"), 5.0123);
    QVERIFY(bridge_->isotopes_for("H1").contains("Ar40"));
    QCOMPARE(w->banner_text(), QString());
    QVERIFY(errors_.isEmpty());
    // Reopened, the saved table is what the spectrometer holds: no banner.
    w->open_table("argon");
    QCOMPARE(w->banner_text(), QString());
  }

  void duplicateIsotopeDisablesSave() {
    auto w = make_window();
    auto* m = w->model();
    QVERIFY(m->setData(m->index(row_of(*m, "Ar39"), FieldTableModel::kIsotopeColumn), QString("Ar40 ")));
    w->check_now();
    QVERIFY(!w->save_enabled());
    QVERIFY(w->save_disabled_reason().contains("duplicate"));
  }

  void removingARowBelowTheFitMinimumDisablesSave() {
    auto w = make_window();
    w->show();
    // Quadratic needs three points; the table has three.
    w->model()->remove_rows({0});
    QCOMPARE(w->model()->rowCount(), 2);
    w->check_now();
    QVERIFY(!w->save_enabled());
    const auto* first = spectrometer::first_error(w->diagnostics());
    QVERIFY(first);
    QCOMPARE(w->save_disabled_reason(), QString::fromStdString(spectrometer::to_string(*first)));
    QTest::qWait(1);  // a redraw after the edit must not fail either
  }

  void conflictReloadDiscardsEdits() {
    auto w = make_window();
    edit_h1(*w, "5.0123");
    const QString now = outside_save(1);
    conflict_answer_ = FieldTableEditorWindow::Conflict::Reload;
    w->save();
    QCOMPARE(conflict_now_, QStringList{now});
    QVERIFY(!w->modified());
    QCOMPARE(w->open_version(), now);
    QCOMPARE(w->base_version(), now);
    QCOMPARE(current(), now);
    QCOMPARE(argon().versions().value().size(), std::size_t{2});
  }

  void conflictSaveAnywayWrites() {
    auto w = make_window();
    edit_h1(*w, "5.0123");
    const QString now = outside_save(1);
    conflict_answer_ = FieldTableEditorWindow::Conflict::SaveAnyway;
    const QString v = save_and_wait(*w);
    QCOMPARE(conflict_now_, QStringList{now});
    QVERIFY(!v.isEmpty());
    QVERIFY(v != now);
    QCOMPARE(current(), v);
    QCOMPARE(argon().versions().value().size(), std::size_t{3});
    QCOMPARE(value_at(argon().load().value(), "Ar40", "H1"), 5.0123);
  }

  void conflictCancelKeepsEdits() {
    auto w = make_window();
    edit_h1(*w, "5.0123");
    const QString now = outside_save(1);
    conflict_answer_ = FieldTableEditorWindow::Conflict::Cancel;
    w->save();
    QCOMPARE(conflict_now_, QStringList{now});
    QVERIFY(w->modified());
    QCOMPARE(w->open_version(), kShipped);
    QCOMPARE(current(), now);
    QCOMPARE(argon().versions().value().size(), std::size_t{2});
  }

  void unsavedPromptOnSwitchingVersionAndOnClose() {
    const QString v2 = core_save(1);
    auto w = make_window();
    w->show();
    QCOMPARE(w->open_version(), v2);
    edit_h1(*w, "5.0123");

    unsaved_answer_ = FieldTableEditorWindow::Unsaved::Cancel;
    w->open_version("argon", kShipped);
    QCOMPARE(unsaved_asked_, 1);
    QCOMPARE(w->open_version(), v2);
    QVERIFY(w->modified());
    QVERIFY(!w->close());
    QCOMPARE(unsaved_asked_, 2);
    QVERIFY(w->isVisible());
    QVERIFY(w->modified());

    unsaved_answer_ = FieldTableEditorWindow::Unsaved::Discard;
    w->open_version("argon", kShipped);
    QCOMPARE(w->open_version(), kShipped);
    QVERIFY(!w->modified());

    edit_h1(*w, "5.0123");
    QVERIFY(w->close());
    QCOMPARE(unsaved_asked_, 4);
    QVERIFY(!w->isVisible());
    QVERIFY(!w->modified());  // discarded edits are gone when it shows again
  }

  void unsavedSaveSavesThenSwitches() {
    core_save(1);
    auto w = make_window();
    edit_h1(*w, "5.0123");
    unsaved_answer_ = FieldTableEditorWindow::Unsaved::Save;
    QSignalSpy saved(w.get(), &FieldTableEditorWindow::saved);
    w->open_version("argon", kShipped);
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, kWaitMs);
    QTRY_COMPARE_WITH_TIMEOUT(w->open_version(), kShipped, kWaitMs);
    QCOMPARE(current(), saved.first().at(1).toString());
    QVERIFY(!w->modified());
  }

  void saveAndMakeCurrentDisabledWhileHeld() {
    core_save(1);
    auto w = make_window();
    edit_h1(*w, "5.0123");
    w->check_now();
    QVERIFY(w->save_enabled());
    QVERIFY(w->make_current_enabled(kShipped));
    auto hold = sim_->spec->hold_table("queue");
    w->check_now();
    QVERIFY(!w->save_enabled());
    QCOMPARE(w->save_disabled_reason(), QString("table is held by queue"));
    QVERIFY(!w->make_current_enabled(kShipped));
    QCOMPARE(w->make_current_disabled_reason(kShipped), QString("table is held by queue"));
    hold = {};
    w->check_now();
    QVERIFY(w->save_enabled());
    QVERIFY(w->make_current_enabled(kShipped));
  }

  void makeCurrentDisabledOnVersionWithError() {
    // Two points: too few for every detector's quadratic.
    write_file(argon().directory() / "20260101T000000Z.toml",
               "fit = \"quadratic\"\naxis = \"dac\"\n[[points]]\nisotope = \"Ar40\"\nmass = 39.9623831237\nH1 = 5.0\n"
               "[[points]]\nisotope = \"Ar36\"\nmass = 35.967545105\nH1 = 4.5\n");
    write_file(argon().directory() / "20260102T000000Z.toml", "fit = \n");
    auto w = make_window();
    QVERIFY(w->versions().contains("20260101T000000Z"));
    QVERIFY(!w->make_current_enabled("20260101T000000Z"));
    QVERIFY(w->make_current_disabled_reason("20260101T000000Z").startsWith("error: "));
    QVERIFY(!w->make_current_enabled("20260102T000000Z"));
    QVERIFY(w->make_current_disabled_reason("20260102T000000Z").contains("does not parse"));
    QVERIFY(!w->make_current_enabled(kShipped));
    QVERIFY(w->make_current_disabled_reason(kShipped).contains("already current"));
    w->make_current("20260101T000000Z");
    QCOMPARE(current(), kShipped);
    QCOMPARE(errors_.size(), 1);
  }

  void makeCurrentConfirmsAndMovesPointer() {
    const QString v2 = core_save(1);
    auto w = make_window();
    confirm_answer_ = false;
    w->make_current(kShipped);
    QCOMPARE(questions_.size(), 1);
    QVERIFY(questions_.front().contains(kShipped));
    QVERIFY(questions_.front().contains(v2));
    QCOMPARE(current(), v2);

    confirm_answer_ = true;
    w->make_current(kShipped);
    QTRY_COMPARE_WITH_TIMEOUT(current(), kShipped, kWaitMs);
    // The window follows: it was unmodified, so it shows the new current.
    QTRY_COMPARE_WITH_TIMEOUT(w->open_version(), kShipped, kWaitMs);
    QVERIFY(errors_.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(bridge_->table_copy("argon"), argon().load(kShipped.toStdString()).value(), kWaitMs);
  }

  void makeCurrentRefusedWhenCurrentMovedMeanwhile() {
    const QString v2 = core_save(1);
    auto w = make_window();
    const QString v3 = outside_save(2);  // after the list was read
    w->make_current(kShipped);
    QCOMPARE(errors_, QStringList{QString("table changed since you opened it (now %1)").arg(v3)});
    QVERIFY(questions_.isEmpty());
    QCOMPARE(current(), v3);
    QCOMPARE(w->versions().front(), v3);  // the list was reread
    QVERIFY(w->versions().contains(v2));
  }

  void undoAndRedoShortcutsEditTheGrid() {
    auto w = make_window();
    w->show();
    w->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(w.get()));
    const double before = value_at(w->model()->table(), "Ar40", "H1");
    edit_h1(*w, "5.0123");
    QCOMPARE(value_at(w->model()->table(), "Ar40", "H1"), 5.0123);
    QTest::keyClick(w.get(), Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(value_at(w->model()->table(), "Ar40", "H1"), before);
    QVERIFY(!w->modified());
    QTest::keyClick(w.get(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(value_at(w->model()->table(), "Ar40", "H1"), 5.0123);
  }

  void aTableChangedWhileEditingShowsTheBannerAndKeepsEdits() {
    auto w = make_window();
    edit_h1(*w, "5.0123");
    // What a peak center does from the core side.
    std::string version;
    std::thread job([&] {
      if (!sim_->spec->update_table("AX", "Ar40", 4.9, false)) return;
      if (auto saved = sim_->spec->save_table(sim_->spec->clock().wall_now())) version = *saved;
    });
    job.join();
    QVERIFY(!version.empty());
    const QString v = QString::fromStdString(version);
    QTRY_COMPARE_WITH_TIMEOUT(w->banner_text(), QString("table changed since you opened it (now %1)").arg(v), kWaitMs);
    QVERIFY(w->modified());
    QCOMPARE(value_at(w->model()->table(), "Ar40", "H1"), 5.0123);
    QCOMPARE(w->base_version(), kShipped);
    QCOMPARE(w->versions().front(), v);
    // Save then goes through the conflict dialog.
    conflict_answer_ = FieldTableEditorWindow::Conflict::Cancel;
    w->save();
    QCOMPARE(conflict_now_, QStringList{v});
  }

  void aTableChangedWhileUnmodifiedReloads() {
    auto w = make_window();
    std::string version;
    std::thread job([&] {
      if (!sim_->spec->update_table("AX", "Ar40", 4.9, false)) return;
      if (auto saved = sim_->spec->save_table(sim_->spec->clock().wall_now())) version = *saved;
    });
    job.join();
    QVERIFY(!version.empty());
    QTRY_COMPARE_WITH_TIMEOUT(w->open_version(), QString::fromStdString(version), kWaitMs);
    QCOMPARE(w->base_version(), QString::fromStdString(version));
    QCOMPARE(value_at(w->model()->table(), "Ar40", "AX"), 4.9);
    QCOMPARE(w->banner_text(), QString());
  }

  void aStoreMovedOnByAnotherProcessOpensTheInMemoryTable() {
    // elctl saved; this process still holds the version it loaded.
    const QString v2 = outside_save(1);
    auto w = make_window();
    QCOMPARE(w->open_version(), v2);
    QCOMPARE(w->banner_text(), QString("in-memory table differs from saved version %1").arg(v2));
    QVERIFY(w->modified());  // saving makes it a version
    FieldTable copy = bridge_->table_copy("argon");
    ui::sort_by_mass(copy);
    QCOMPARE(w->model()->table(), copy);
    w->model()->undo_stack()->undo();  // the saved version underneath
    QCOMPARE(value_at(w->model()->table(), "Ar40", "H1"), value_at(argon().load().value(), "Ar40", "H1"));
  }

  void commentsLostIsAskedOnce() {
    // A hand-written version with a comment, made current.
    const auto table = argon().load().value();
    write_file(argon().directory() / "20260101T000000Z.toml", "# hand edited\n" + spectrometer::to_toml(table));
    QVERIFY(argon().restore("20260101T000000Z").has_value());
    auto w = make_window();
    QCOMPARE(w->open_version(), QString("20260101T000000Z"));
    edit_h1(*w, "5.0123");
    w->check_now();
    confirm_answer_ = false;
    w->save();
    QCOMPARE(questions_.size(), 1);
    QCOMPARE(questions_.front(), QString("comments in 20260101T000000Z are not kept"));
    QCOMPARE(current(), QString("20260101T000000Z"));
    QVERIFY(w->modified());

    confirm_answer_ = true;
    const QString v = save_and_wait(*w);
    QCOMPARE(questions_.size(), 2);
    QVERIFY(!v.isEmpty());
    edit_h1(*w, "5.0124");
    QVERIFY(!save_and_wait(*w).isEmpty());
    QCOMPARE(questions_.size(), 2);  // the new version is canonical
  }

  void unparseableVersionIsReadOnly() {
    write_file(argon().directory() / "20260102T000000Z.toml", "fit = \n");
    auto w = make_window();
    w->open_version("argon", "20260102T000000Z");
    QCOMPARE(w->open_version(), QString("20260102T000000Z"));
    QVERIFY(w->read_only());
    QVERIFY(!w->load_error().isEmpty());
    QCOMPARE(w->model()->rowCount(), 0);
    QVERIFY(!w->save_enabled());
    QVERIFY(w->save_disabled_reason().contains("does not parse"));
    w->open_table("argon");
    QVERIFY(!w->read_only());
    QCOMPARE(w->load_error(), QString());
  }

  void withoutBridgeSavesThroughTheStore() {
    auto w = make_window(false);
    QCOMPARE(w->open_name(), QString("argon"));
    edit_h1(*w, "5.0123");
    w->check_now();
    QVERIFY(w->save_enabled());
    QSignalSpy saved(w.get(), &FieldTableEditorWindow::saved);
    w->save();
    QCOMPARE(saved.count(), 1);  // no executor: at once
    QCOMPARE(saved.first().at(1).toString(), QString("20261019T120000Z"));
    QCOMPARE(current(), QString("20261019T120000Z"));
    QCOMPARE(value_at(argon().load().value(), "Ar40", "H1"), 5.0123);
    QVERIFY(!w->modified());
  }

  void diagnosticClickSelectsTheCell() {
    auto w = make_window();
    auto* m = w->model();
    QVERIFY(m->setData(m->index(row_of(*m, "Ar39"), m->column_of("AX")), QString("inf")) == false);
    QVERIFY(m->setData(m->index(row_of(*m, "Ar39"), FieldTableModel::kIsotopeColumn), QString("Ar40")));
    w->check_now();
    auto* list = w->findChild<QListWidget*>("diagnostics");
    auto* grid = w->findChild<QTableView*>("grid");
    QVERIFY(list && grid);
    QVERIFY(list->count() > 0);
    QCOMPARE(list->item(0)->text(), w->diagnostic_lines().front());
    emit list->itemClicked(list->item(0));
    QVERIFY(grid->currentIndex().isValid());
  }
};

QTEST_MAIN(TestFieldTableEditor)
#include "test_field_table_editor.moc"
