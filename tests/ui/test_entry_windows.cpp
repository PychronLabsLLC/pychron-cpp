// Sample and package entry windows (entry spec, section 9) on a SQLite store:
// the samples table edits and saves, shows another client's change as stale
// and writes nothing; the import dialog previews and imports pasted rows;
// the packages window assigns samples, saves, asks before leaving unsaved
// edits, generates identifiers (re-planning when the counter moved), and
// writes the level sheets as PDF.

#include <QtTest/QtTest>

#ifndef PYCHRON_UI_HAS_STORE

class EntryWindowsTest : public QObject {
  Q_OBJECT
 private Q_SLOTS:
  void skipped() { QSKIP("built without the DVC store"); }
};

#else

#include <QCheckBox>
#include <QComboBox>
#include <QMainWindow>
#include <QTableView>
#include <QTableWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTreeWidget>

#include "entry_actions.hpp"
#include "entry_bridge.hpp"
#include "holder_view.hpp"
#include "level_grid_model.hpp"
#include "menu_hub.hpp"
#include "package_dialogs.hpp"
#include "packages_window.hpp"
#include "sample_import_dialog.hpp"
#include "sample_table_model.hpp"
#include "samples_window.hpp"

using namespace pychron;
namespace ps = pychron::persistence;
namespace en = pychron::entry;
using pychron::ui::EntryBridge;

namespace {

constexpr int kWaitMs = 10000;

struct Seeded {
  ps::Uuid client, user;
  ps::Uuid ross, alpha, sanidine, bt1, fc2;
  ps::Uuid package, level_a, level_b;
};

Seeded seed(ps::IStore& s) {
  Seeded d;
  d.client = *s.register_client({"seed", "reduction", std::nullopt, "test"});
  d.user = *s.ensure_user(d.client, "seeder");
  d.ross = *s.add_principal_investigator(d.client, {"Ross", "J", std::nullopt, std::nullopt, std::nullopt});
  d.alpha = *s.add_project(d.client, {"Alpha", d.ross});
  d.sanidine = *s.add_material(d.client, {"sanidine", "", std::nullopt});
  d.bt1 = *s.add_sample(d.client, {"bt-1", d.alpha, d.sanidine});
  d.fc2 = *s.add_sample(d.client, {"FC-2", d.alpha, d.sanidine});
  const ps::Actor actor{d.user, d.client};
  auto holder = en::read_holder("circle,0.02\n0,0\n1,0\n2,0\n3,0\n");
  const ps::Uuid h = *en::save_holder(s, actor, "4-hole", *holder);
  en::NewPackage p;
  p.name = "P-1";
  p.kind = "package";
  p.levels = {{"A", h, std::nullopt, std::nullopt}, {"B", h, std::nullopt, std::nullopt}};
  auto made = en::create_package(s, actor, p);
  if (!made) qFatal("seed: %s", to_string(made.error()).c_str());
  d.package = made->package;
  d.level_a = made->levels[0];
  d.level_b = made->levels[1];
  return d;
}

}  // namespace

class EntryWindowsTest : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir dir_;
  std::string url_;
  std::unique_ptr<ps::IStore> store_;  // the "other client"
  Seeded seeded_;

  std::unique_ptr<EntryBridge> bridge() {
    auto b = EntryBridge::open({url_, "tester", "test-host"});
    if (!b) qFatal("bridge: %s", to_string(b.error()).c_str());
    return std::move(*b);
  }

  ps::Actor other() const { return ps::Actor{seeded_.user, seeded_.client}; }

 private Q_SLOTS:
  void init() {
    QVERIFY(dir_.isValid());
    static int n = 0;
    url_ = "sqlite:" + dir_.filePath(QStringLiteral("store-%1.db").arg(++n)).toStdString();
    auto s = ps::open_store(ps::StoreConfig{url_, true});
    QVERIFY2(s.has_value(), s ? "" : to_string(s.error()).c_str());
    store_ = std::move(*s);
    seeded_ = seed(*store_);
  }

  void cleanup() { store_.reset(); }

  void samples_edit_and_save() {
    auto b = bridge();
    pychron::ui::SamplesWindow w(*b);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    auto* model = w.model();
    QCOMPARE(model->stored_count(), 2);
    int row = -1;
    for (int r = 0; r < model->stored_count(); ++r)
      if (model->stored(r)->name == "bt-1") row = r;
    QVERIFY(row >= 0);
    QVERIFY(model->setData(model->index(row, pychron::ui::SampleTableModel::Note), QStringLiteral("dated")));
    QVERIFY(model->setData(model->index(row, pychron::ui::SampleTableModel::Lat), QStringLiteral("34.5")));
    QVERIFY(model->setData(model->index(row, pychron::ui::SampleTableModel::Lon), QStringLiteral("-106")));
    QVERIFY(!model->setData(model->index(row, pychron::ui::SampleTableModel::Lat), QStringLiteral("north")));
    QVERIFY(model->dirty());
    w.save();
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && !model->dirty(), kWaitMs);
    const auto row_now = *store_->catalog_row(ps::CatalogTable::Sample, seeded_.bt1);
    QCOMPARE(row_now->at("note"), ps::CatalogValue{std::string("dated")});
    QCOMPARE(row_now->at("lat"), ps::CatalogValue{34.5});
  }

  void samples_stale_edit_writes_nothing() {
    auto b = bridge();
    pychron::ui::SamplesWindow w(*b);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    auto* model = w.model();
    int row = -1;
    for (int r = 0; r < model->stored_count(); ++r)
      if (model->stored(r)->name == "bt-1") row = r;
    QVERIFY(model->setData(model->index(row, pychron::ui::SampleTableModel::Note), QStringLiteral("mine")));
    // Another client changes the same field first.
    ps::CatalogEditBatch theirs;
    theirs.edits = {ps::CatalogUpdate{ps::CatalogTable::Sample, seeded_.bt1, {}, {{"note", std::string("theirs")}}}};
    QVERIFY(std::holds_alternative<ps::CatalogApplied>(*store_->apply_catalog_edits(seeded_.client, theirs)));
    w.save();
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY(model->is_stale(row));
    QVERIFY2(w.message().contains(QStringLiteral("another client")), qPrintable(w.message()));
    QCOMPARE((*store_->catalog_row(ps::CatalogTable::Sample, seeded_.bt1))->at("note"), ps::CatalogValue{std::string("theirs")});
  }

  void samples_new_with_new_project() {
    auto b = bridge();
    pychron::ui::SamplesWindow w(*b);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    pychron::ui::NewSample s;
    s.name = "new-1";
    s.principal_investigator = "Doe, A";
    s.project = "Delta";
    s.material = "biotite";
    s.grainsize = "20-40";
    w.set_form(s);
    QVERIFY(w.add_from_form());
    s.principal_investigator = "jake doe";  // refused by the PI rule
    w.set_form(s);
    QVERIFY(!w.add_from_form());
    w.save();
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && !w.model()->dirty(), kWaitMs);
    ps::SampleQuery q;
    q.text = "new-1";
    auto rows = *store_->samples(q);
    QCOMPARE(rows.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(rows[0].project_name), QStringLiteral("Delta"));
    QCOMPARE(QString::fromStdString(rows[0].principal_investigator_name), QStringLiteral("Doe, A"));
    QCOMPARE(QString::fromStdString(rows[0].grainsize), QStringLiteral("20-40"));
  }

  void import_pasted_rows() {
    auto b = bridge();
    pychron::ui::SampleImportDialog d(*b);
    d.set_text(QStringLiteral("sample\tproject\tPI\tmaterial\nbt-1\tAlpha\tRoss, J\tsanidine\nz-1\tAlpha\tRoss, J\tsanidine\n"
                              "z-2\t9bad\tRoss, J\tsanidine\n"));
    QVERIFY(d.preview());
    QCOMPARE(d.plan()->exists, 1);
    QCOMPARE(d.plan()->creates, 1);  // z-1; z-2's project is not a project name
    QCOMPARE(d.plan()->errors, 1);
    d.filter()->setCurrentIndex(4);  // Error
    QCOMPARE(d.preview_table()->rowCount(), 1);
    // Fix the row and import.
    d.set_text(QStringLiteral("sample\tproject\tPI\tmaterial\nbt-1\tAlpha\tRoss, J\tsanidine\nz-1\tAlpha\tRoss, J\tsanidine\n"));
    QVERIFY(d.preview());
    QSignalSpy spy(&d, &pychron::ui::SampleImportDialog::imported);
    d.import_rows();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, kWaitMs);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    ps::SampleQuery q;
    q.text = "z-1";
    QCOMPARE(store_->samples(q)->size(), std::size_t{1});
  }

  void packages_assign_save_and_generate() {
    auto b = bridge();
    pychron::ui::PackagesWindow w(*b);
    int asked = 0;
    w.set_confirm([&](const QString&) {
      ++asked;
      return false;
    });
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QCOMPARE(w.tree()->topLevelItemCount(), 1);
    w.open_level(seeded_.level_a);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit(), kWaitMs);
    QCOMPARE(w.grid()->rowCount(), 4);  // the holder's holes

    ps::SampleRow bt1 = (*store_->samples({"bt-1", std::nullopt, std::nullopt, std::nullopt, 10})).front();
    w.select_positions({1, 3});
    QCOMPARE(w.holder_view()->selected(), (std::set<int>{1, 3}));
    w.assign(bt1);
    QVERIFY(w.grid()->edit().dirty());
    QVERIFY(w.grid()->setData(w.grid()->index(0, pychron::ui::LevelGridModel::Packet), QStringLiteral("P1")));

    // Leaving with unsaved edits asks; "no" drops them.
    w.open_level(seeded_.level_b);
    QCOMPARE(asked, 1);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->edit().loaded().level.uuid == seeded_.level_b, kWaitMs);
    QVERIFY(store_->level_sheet(seeded_.level_a)->value().positions.empty());

    w.open_level(seeded_.level_a);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->edit().loaded().level.uuid == seeded_.level_a, kWaitMs);
    w.select_positions({1, 3});
    w.assign(bt1);
    w.save();
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit() && !w.grid()->edit().dirty(), kWaitMs);
    const auto sheet = store_->level_sheet(seeded_.level_a)->value();
    QCOMPARE(sheet.positions.size(), std::size_t{2});
    QCOMPARE(sheet.positions[1].position, 3);

    // Identifiers: preview equals what is written.
    pychron::ui::IdentifierDialog d(*b, *w.current_package());
    QCOMPARE(d.plan().assignments.size(), std::size_t{2});
    const auto planned = d.plan().assignments;
    QSignalSpy allocated(&d, &pychron::ui::IdentifierDialog::allocated);
    d.commit();
    QTRY_COMPARE_WITH_TIMEOUT(allocated.count(), 1, kWaitMs);
    const auto after = store_->level_sheet(seeded_.level_a)->value();
    for (std::size_t i = 0; i < planned.size(); ++i)
      QCOMPARE(after.positions[i].identifier, std::optional<std::string>(std::to_string(planned[i].number)));
  }

  void identifiers_replan_when_the_counter_moved() {
    auto b = bridge();
    // Two filled positions in A, one in B.
    {
      auto sheet = store_->level_sheet(seeded_.level_a)->value();
      en::LevelSheetEdit e(sheet, std::nullopt);
      e.add_row(1);
      e.add_row(2);
      ps::SampleRow bt1 = (*store_->samples({"bt-1", std::nullopt, std::nullopt, std::nullopt, 10})).front();
      e.assign_sample({1, 2}, bt1);
      QVERIFY(std::holds_alternative<ps::CatalogApplied>(*store_->apply_catalog_edits(seeded_.client, e.to_batch())));
    }
    auto pkg = *store_->irradiations();
    pychron::ui::IdentifierDialog d(*b, pkg.front());
    QCOMPARE(d.plan().expected_last, std::int64_t{0});
    // Another client numbers position 1 first.
    auto sheets = *en::package_sheets(*store_, seeded_.package);
    auto first = en::plan_identifiers(sheets, 0, false);
    first.assignments.resize(1);
    first.last = 1;
    QVERIFY(std::holds_alternative<ps::CatalogApplied>(*store_->allocate_identifiers(seeded_.client, first.allocation())));
    d.commit();
    QTRY_VERIFY_WITH_TIMEOUT(d.message().contains(QStringLiteral("new plan")), kWaitMs);
    QCOMPARE(d.plan().expected_last, std::int64_t{1});
    QCOMPARE(d.plan().assignments.size(), std::size_t{1});
    QSignalSpy allocated(&d, &pychron::ui::IdentifierDialog::allocated);
    d.commit();
    QTRY_COMPARE_WITH_TIMEOUT(allocated.count(), 1, kWaitMs);
    QCOMPARE(*store_->identifier_counter(std::string(ps::kIdentifierScope)), std::optional<std::int64_t>(2));
  }

  void pdf_has_a_page_per_level() {
    auto b = bridge();
    pychron::ui::PackagesWindow w(*b);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    w.open_level(seeded_.level_a);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit(), kWaitMs);
    const QString path = dir_.filePath(QStringLiteral("p1.pdf"));
    auto pages = w.save_pdf(path);
    QVERIFY2(pages.has_value(), pages ? "" : pages.error().what.c_str());
    QCOMPARE(*pages, 3);  // a summary and two levels
    QVERIFY(QFileInfo(path).size() > 1000);
  }

  void packages_fit_flux_asks_for_the_level() {
    auto b = bridge();
    pychron::ui::PackagesWindow w(*b);
    QSignalSpy asked(&w, &pychron::ui::PackagesWindow::flux_requested);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QAction* fit = w.fit_flux_action();
    QVERIFY(fit != nullptr);
    QCOMPARE(fit->text(), QStringLiteral("Fit flux…"));
    QVERIFY(w.findChild<QToolBar*>(QStringLiteral("packages_toolbar"))->actions().contains(fit));
    // No level open: disabled, and nothing is asked.
    QVERIFY(!fit->isEnabled());
    fit->trigger();
    QCOMPARE(asked.count(), 0);

    w.open_level(seeded_.level_a);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit(), kWaitMs);
    QVERIFY(fit->isEnabled());
    fit->trigger();
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.at(0).at(0).toString(), QStringLiteral("P-1"));
    QCOMPARE(asked.at(0).at(1).toString(), QStringLiteral("A"));
  }

  void show_level_selects_and_opens_it() {
    auto b = bridge();
    pychron::ui::PackagesWindow w(*b);
    int asked = 0;
    w.set_confirm([&](const QString&) {
      ++asked;
      return false;
    });
    // Asked for while the tree is still being read: opened once it is.
    QVERIFY(w.busy());
    w.show_level(QStringLiteral("P-1"), QStringLiteral("B"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit(), kWaitMs);
    QCOMPARE(w.grid()->edit().loaded().level.uuid, seeded_.level_b);
    QVERIFY(w.tree()->currentItem() != nullptr);
    QCOMPARE(w.tree()->currentItem()->text(0), QStringLiteral("B"));
    QCOMPARE(w.windowTitle(), QStringLiteral("Packages — P-1 B"));

    // With edits pending it asks, as a click on the level does.
    ps::SampleRow bt1 = (*store_->samples({"bt-1", std::nullopt, std::nullopt, std::nullopt, 10})).front();
    w.select_positions({1});
    w.assign(bt1);
    QVERIFY(w.grid()->edit().dirty());
    w.show_level(QStringLiteral("P-1"), QStringLiteral("A"));
    QCOMPARE(asked, 1);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->edit().loaded().level.uuid == seeded_.level_a, kWaitMs);
    QCOMPARE(w.tree()->currentItem()->text(0), QStringLiteral("A"));

    // The level already open is only selected; one that is not there is said.
    w.select_positions({1});
    w.assign(bt1);
    w.show_level(QStringLiteral("P-1"), QStringLiteral("A"));
    QCOMPARE(asked, 1);
    QVERIFY(!w.busy());
    QVERIFY(w.grid()->edit().dirty());
    w.show_level(QStringLiteral("P-1"), QStringLiteral("Z"));
    QCOMPARE(w.message(), QStringLiteral("There is no level P-1 Z"));
    QCOMPARE(asked, 1);
    QVERIFY(!w.busy());
  }

  void a_change_elsewhere_reloads_the_open_level() {
    auto b = bridge();
    pychron::ui::PackagesWindow w(*b);
    w.set_confirm([](const QString&) { return false; });
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    w.open_level(seeded_.level_a);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit(), kWaitMs);
    const auto sample_at = [&](int row) {
      return w.grid()->index(row, pychron::ui::LevelGridModel::Sample).data().toString();
    };
    QCOMPARE(sample_at(0), QString());

    // Another client fills position 1; the window is told and shows it.
    {
      auto sheet = store_->level_sheet(seeded_.level_a)->value();
      en::LevelSheetEdit e(sheet, std::nullopt);
      e.add_row(1);
      ps::SampleRow bt1 = (*store_->samples({"bt-1", std::nullopt, std::nullopt, std::nullopt, 10})).front();
      e.assign_sample({1}, bt1);
      QVERIFY(std::holds_alternative<ps::CatalogApplied>(*store_->apply_catalog_edits(seeded_.client, e.to_batch())));
    }
    b->notify_changed();
    QVERIFY(w.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy() && w.grid()->has_edit(), kWaitMs);
    QCOMPARE(sample_at(0), QStringLiteral("bt-1"));

    // With edits pending the level is left as it is.
    QVERIFY(w.grid()->setData(w.grid()->index(1, pychron::ui::LevelGridModel::Note), QStringLiteral("mine")));
    QVERIFY(w.grid()->edit().dirty());
    b->notify_changed();
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY(w.grid()->edit().dirty());
    QCOMPARE(w.grid()->index(1, pychron::ui::LevelGridModel::Note).data().toString(), QStringLiteral("mine"));
  }

  void entry_menu_opens_the_windows() {
    pychron::ui::MenuHub::reset(pychron::ui::MenuHub::Bars::PerWindow);
    QMainWindow main;
    auto* actions = new pychron::ui::EntryActions(&main, url_);
    main.show();
    actions->samples_action()->trigger();
    QVERIFY(actions->samples() != nullptr);
    QVERIFY(actions->samples()->isVisible());
    actions->packages_action()->trigger();
    QVERIFY(actions->packages()->isVisible());
    QVERIFY(actions->bridge() != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!actions->samples()->busy() && !actions->packages()->busy(), kWaitMs);
  }
};

#endif

QTEST_MAIN(EntryWindowsTest)
#include "test_entry_windows.moc"
