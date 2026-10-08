// The flux window (flux window design, sections 5.3 and 5.4) on a SQLite store
// holding the seeded level of flux_seed.hpp: the level tree and its status
// column, loading and fitting a level, the monitor selection, a level that
// cannot be loaded or fitted, a selection superseded while it loads, and a
// window closed while it loads. Then the edits (plot clicks and check boxes),
// the options dock with its presets, the monitor group, Revert, Reload and
// Reset omissions, the unsaved question, and a change made in another window.
// Then saving (one changeset, a conflict, the Save answer of the unsaved
// question, a save that outlives its level or its window) and the CSV export.
// Then the way in (FitActions): Fit > Flux, the Packages window's "Fit flux…",
// Open in Packages, Recall, a store that cannot be opened, and the two owners
// destroyed in either order while a level is read.

#include <QtTest/QtTest>

#include <qcustomplot.h>

#ifndef PYCHRON_UI_HAS_STORE

class FluxWindowTest : public QObject {
  Q_OBJECT
 private Q_SLOTS:
  void skipped() { QSKIP("built without the DVC store"); }
};

#else

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTreeWidget>

#include "command_palette.hpp"
#include "entry_actions.hpp"
#include "entry_bridge.hpp"
#include "fit_actions.hpp"
#include "flux_analysis_model.hpp"
#include "flux_monitor_model.hpp"
#include "flux_unknown_model.hpp"
#include "flux_window.hpp"
#include "level_grid_model.hpp"
#include "menu_hub.hpp"
#include "options_editor.hpp"
#include "packages_window.hpp"
#include "preset_bar.hpp"
#include "pychron/processing/flux_view.hpp"
#include "pychron/processing/options.hpp"
#include "scene_view.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "flux_seed.hpp"
#include "pychron/processing/flux_store.hpp"
#include "pychron/processing/store_source.hpp"
#pragma pop_macro("signals")

using namespace pychron;
namespace ps = pychron::persistence;
namespace pp = pychron::processing;
namespace pt = pychron::processing::testing;
using pychron::ui::EntryActions;
using pychron::ui::EntryBridge;
using pychron::ui::FitActions;
using pychron::ui::MenuHub;
using pychron::ui::PackagesWindow;
using pychron::ui::FluxAnalysisModel;
using pychron::ui::FluxMonitorModel;
using pychron::ui::FluxWindow;

namespace {

constexpr int kWaitMs = 10000;

const std::vector<pp::Layer>& layers_of(const pp::Scene& scene) { return scene.graphs.at(0).panels.at(0).layers; }

const pp::PointLayer* points_labelled(const pp::Scene& scene, const std::string& label) {
  for (const auto& layer : layers_of(scene))
    if (const auto* points = std::get_if<pp::PointLayer>(&layer); points && points->label == label) return points;
  return nullptr;
}

const pp::FittedPosition& fitted_at(const pp::LevelFit& fit, int hole) {
  for (const auto& p : fit.positions)
    if (p.hole == hole) return p;
  qFatal("no hole %d", hole);
}

// Whether the scene draws the analysis as excluded; nullopt when it is not drawn.
std::optional<bool> drawn_excluded(const pp::Scene& scene, const std::string& uuid) {
  const auto* points = points_labelled(scene, "Analyses");
  if (!points) return std::nullopt;
  for (std::size_t i = 0; i < points->refs.size(); ++i)
    if (points->refs[i].analysis == uuid) return points->excluded.at(i);
  return std::nullopt;
}

pp::FluxOptions nearest3() {
  pp::FluxOptions o;
  o.fit.kind = reduction::ModelKind::NearestNeighbors;
  o.fit.n_neighbors = 3;
  return o;
}

// What a window needs, in the order it must die in: the bridge (whose worker
// may still be reading through the source) before the source.
struct Rig {
  std::unique_ptr<pp::StoreSource> source;
  pp::PresetStore presets;
  std::unique_ptr<EntryBridge> bridge;
};

// The two owners under a main window, as main.cpp makes them: Entry first.
struct Owners {
  pp::PresetStore presets;  // outlives the window
  QMainWindow main;
  EntryActions* entry;
  FitActions* fit;
  QStringList recalled, errors;

  Owners(const std::string& url, const QString& presets_dir)
      : presets(presets_dir.toStdString()),
        entry(new EntryActions(&main, url)),
        fit(new FitActions(&main, url, *entry, presets, [this](const QString& uuid) { recalled.append(uuid); })) {
    fit->set_report_error([this](const QString& text) { errors.append(text); });
  }
};

}  // namespace

class FluxWindowTest : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir dir_;
  std::string url_;
  std::unique_ptr<ps::IStore> store_;  // the seeder's own connection
  ps::Actor actor_;
  pt::SeededLevel seeded_;

  // Opened after everything a test seeds, so the source sees it.
  Rig rig() {
    auto source = pp::StoreSource::open(ps::StoreConfig{url_, false}, {});
    if (!source) qFatal("source: %s", to_string(source.error()).c_str());
    auto bridge = EntryBridge::open({url_, "tester", "test-host"});
    if (!bridge) qFatal("bridge: %s", to_string(bridge.error()).c_str());
    return Rig{std::move(*source), pp::PresetStore(dir_.filePath(QStringLiteral("presets")).toStdString()),
               std::move(*bridge)};
  }

  // The worker has run every job posted so far and their results were delivered.
  static void drain(EntryBridge& bridge) {
    auto ran = bridge.run_sync<bool>([](ps::IStore&, const ps::Actor&) -> Result<bool> { return true; });
    QVERIFY(ran.has_value());
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
  }

  // A saved fit of a hole of level A, under the default monitor set.
  void save_fit(int hole, const pp::FluxOptions& options, bool excluded = false, double j = 1.0e-3) {
    const pp::MonitorSet set = *pp::default_monitor_sets().find("");
    ps::FluxValue value;
    value.j = j;
    value.j_err = 1.0e-6;
    value.mean_j = 1.0e-3;
    value.mean_j_err = 1.0e-6;
    value.options_json = pp::flux_options_json(options, set, !excluded, excluded, false, 1.0, 5, "test");
    auto saved = pt::seed_save_flux(*store_, actor_, seeded_, hole, std::move(value));
    QVERIFY2(saved.has_value(), saved ? "" : to_string(saved.error()).c_str());
  }

  // The window on level A, loaded and fitted. The questions it asks are
  // recorded in `asked` and answered with `answer`.
  std::unique_ptr<FluxWindow> opened(Rig& r, QStringList* asked = nullptr,
                                     FluxWindow::Unsaved* answer = nullptr) {
    auto w = std::make_unique<FluxWindow>(*r.bridge, *r.source, r.presets);
    w->set_ask_unsaved([asked, answer](const QString& question) {
      if (asked) asked->append(question);
      return answer ? *answer : FluxWindow::Unsaved::Cancel;
    });
    w->open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    if (!QTest::qWaitFor([&] { return !w->busy(); }, kWaitMs)) qFatal("the level did not load");
    return w;
  }

  // The head flux_position revision of a hole; nullopt when it has none.
  std::optional<ps::Uuid> head(int hole, const std::string& level = "A") {
    auto object = store_->find_catalog_row(ps::CatalogTable::RefObject,
                                           {std::string("flux_position"), "NM-300/" + level + "/" + std::to_string(hole)});
    if (!object) qFatal("%s", to_string(object.error()).c_str());
    if (!*object) return std::nullopt;
    auto revision = store_->head(**object, ps::Kind::RefValue);
    if (!revision) qFatal("%s", to_string(revision.error()).c_str());
    return *revision;
  }

  std::map<int, std::optional<ps::Uuid>> heads() {
    std::map<int, std::optional<ps::Uuid>> out;
    for (int hole = 1; hole <= 12; ++hole) out[hole] = head(hole);
    return out;
  }

  // The flux at a hole's head; nullopt when nothing was saved there.
  std::optional<ps::FluxValue> head_value(int hole, const std::string& level = "A") {
    const auto revision = head(hole, level);
    if (!revision) return std::nullopt;
    auto payload = store_->load_payload(*revision);
    if (!payload || !*payload) qFatal("hole %d has no payload", hole);
    const auto* ref = std::get_if<ps::RefPayload>(&**payload);
    const auto* flux = ref ? std::get_if<ps::FluxValue>(ref) : nullptr;
    if (!flux) qFatal("hole %d is not a flux", hole);
    return *flux;
  }

  // Whether the saved fit of a hole left it out; nullopt when nothing was saved there.
  std::optional<bool> saved_excluded(int hole, const std::string& level = "A") {
    const auto value = head_value(hole, level);
    if (!value || !value->options_json) return std::nullopt;
    return pp::parse_flux_options(*value->options_json).excluded;
  }

  // The changeset that wrote a hole's head.
  ps::Uuid head_changeset(int hole) {
    auto object = store_->find_catalog_row(ps::CatalogTable::RefObject,
                                           {std::string("flux_position"), "NM-300/A/" + std::to_string(hole)});
    if (!object || !*object) qFatal("hole %d has no flux", hole);
    auto history = store_->history(**object, ps::Kind::RefValue);
    if (!history || history->size() != 1) qFatal("hole %d was not saved once", hole);
    return history->front().changeset.uuid;
  }

  ps::ChangeSeq change_seq() {
    auto seq = store_->latest_change_seq();
    if (!seq) qFatal("%s", to_string(seq.error()).c_str());
    return *seq;
  }

  static bool settle(FluxWindow& w) {
    return QTest::qWaitFor([&] { return !w.busy(); }, kWaitMs);
  }

  static Qt::CheckState check(const QAbstractItemModel* model, int row, int column) {
    return model->index(row, column).data(Qt::CheckStateRole).value<Qt::CheckState>();
  }

  static QTreeWidgetItem* level_item(const FluxWindow& w, const QString& irradiation, const QString& level) {
    for (int i = 0; i < w.tree()->topLevelItemCount(); ++i) {
      auto* top = w.tree()->topLevelItem(i);
      if (top->text(0) != irradiation) continue;
      for (int c = 0; c < top->childCount(); ++c)
        if (top->child(c)->text(0) == level) return top->child(c);
    }
    return nullptr;
  }

 private Q_SLOTS:
  void init() {
    QVERIFY(dir_.isValid());
    static int n = 0;
    url_ = "sqlite:" + dir_.filePath(QStringLiteral("flux-%1.db").arg(++n)).toStdString();
    auto s = ps::open_store(ps::StoreConfig{url_, true});
    QVERIFY2(s.has_value(), s ? "" : to_string(s.error()).c_str());
    store_ = std::move(*s);
    auto client = store_->register_client({"red-1", "reduction", std::nullopt, "test"});
    QVERIFY2(client.has_value(), client ? "" : to_string(client.error()).c_str());
    auto user = store_->ensure_user(*client, "jsmith");
    QVERIFY2(user.has_value(), user ? "" : to_string(user.error()).c_str());
    actor_ = ps::Actor{*user, *client};
    auto seeded = pt::seed_flux_level(*store_, actor_);
    QVERIFY2(seeded.has_value(), seeded ? "" : to_string(seeded.error()).c_str());
    seeded_ = std::move(*seeded);
  }

  void cleanup() { store_.reset(); }

  void tree_lists_levels_with_their_status() {
    auto r = rig();
    FluxWindow w(*r.bridge, *r.source, r.presets);
    QCOMPARE(w.objectName(), QStringLiteral("flux_window"));
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux"));
    QCOMPARE(w.tree()->objectName(), QStringLiteral("flux_tree"));
    QCOMPARE(w.tree()->columnCount(), 2);
    QVERIFY(w.busy());  // the tree is being read
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QCOMPARE(w.tree()->topLevelItemCount(), 1);
    auto* irradiation = w.tree()->topLevelItem(0);
    QCOMPARE(irradiation->text(0), QStringLiteral("NM-300"));
    QCOMPARE(irradiation->text(1), QString());
    QCOMPARE(irradiation->childCount(), 1);
    QCOMPARE(irradiation->child(0)->text(0), QStringLiteral("A"));
    QCOMPARE(irradiation->child(0)->text(1), QStringLiteral("not fitted"));
    // Nothing is loaded until a level is picked; picking the irradiation loads nothing.
    w.tree()->setCurrentItem(irradiation);
    QVERIFY(!w.busy());
    QCOMPARE(w.inputs(), nullptr);

    // Levels by name under an irradiation, irradiations newest (by name) first.
    QVERIFY(pt::seed_level_without_monitors(*store_, seeded_, "B").has_value());
    auto later = store_->add_irradiation(seeded_.acquisition_client, "NM-301");
    QVERIFY2(later.has_value(), later ? "" : to_string(later.error()).c_str());
    for (int hole = 1; hole <= 8; ++hole) {
      ps::FluxValue value;
      value.j = 1.0e-3;
      value.j_err = 1.0e-6;
      QVERIFY(pt::seed_save_flux(*store_, actor_, seeded_, hole, value).has_value());
    }
    w.reload_tree();
    QVERIFY(w.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QCOMPARE(w.tree()->topLevelItemCount(), 2);
    QCOMPARE(w.tree()->topLevelItem(0)->text(0), QStringLiteral("NM-301"));
    QCOMPARE(w.tree()->topLevelItem(0)->childCount(), 0);
    irradiation = w.tree()->topLevelItem(1);
    QCOMPARE(irradiation->text(0), QStringLiteral("NM-300"));
    QCOMPARE(irradiation->childCount(), 2);
    QCOMPARE(irradiation->child(0)->text(0), QStringLiteral("A"));
    QCOMPARE(irradiation->child(0)->text(1), QStringLiteral("fitted"));
    QCOMPARE(irradiation->child(1)->text(0), QStringLiteral("B"));
    QCOMPARE(irradiation->child(1)->text(1), QStringLiteral("no monitors"));
    QVERIFY(!w.status_is_error());
  }

  void opening_a_level_fills_tables_plot_and_status() {
    auto r = rig();
    FluxWindow w(*r.bridge, *r.source, r.presets);
    QCOMPARE(w.size(), QSize(1400, 820));
    QCOMPARE(w.monitor_table()->objectName(), QStringLiteral("flux_monitors"));
    QVERIFY(w.findChild<QTableView*>(QStringLiteral("flux_analyses")));
    QVERIFY(w.findChild<QTableView*>(QStringLiteral("flux_unknowns")));
    QVERIFY(w.findChild<QWidget*>(QStringLiteral("flux_status")));
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QVERIFY(w.busy());
    QCOMPARE(w.status(), QStringLiteral("Loading NM-300 A…"));
    QVERIFY(!w.status_is_error());
    QCOMPARE(w.inputs(), nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);

    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 A"));
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->level, std::string("A"));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.options(), pp::FluxOptions{});
    QCOMPARE(w.monitors()->rowCount(), 8);
    QCOMPARE(w.unknowns()->rowCount(), 4);
    QCOMPARE(w.analyses()->rowCount(), 0);  // no monitor is selected
    QCOMPARE(w.monitors()->index(0, FluxMonitorModel::Identifier).data().toString(), QStringLiteral("66001"));
    QVERIFY(!w.monitors()->index(0, FluxMonitorModel::PredJ).data().toString().isEmpty());
    QVERIFY(w.monitor_table()->isEnabled());

    const auto scene = w.view()->scene();
    QVERIFY(scene);
    QCOMPARE(scene->kind, std::string("flux"));
    const auto* analyses = points_labelled(*scene, "Analyses");
    QVERIFY(analyses);
    QCOMPARE(analyses->x.size(), std::size_t{24});
    QVERIFY(points_labelled(*scene, "Unknowns"));

    QVERIFY(!w.status_is_error());
    // flux_status_line's text: the model's first clause, the fit MSWD, the J range.
    QVERIFY2(w.status().startsWith(QStringLiteral("plane, unweighted · fit MSWD")), qPrintable(w.status()));
    QVERIFY2(w.status().contains(QStringLiteral(" · J ")), qPrintable(w.status()));
    QVERIFY2(w.status().contains(QStringLiteral("(5 dof)")), qPrintable(w.status()));  // 8 monitors, 3 parameters
    // The warnings are counted in the status and listed in its tooltip.
    const QStringList expected = [&] {
      QStringList lines;
      for (const auto& line : pp::flux_warnings(*w.inputs(), *w.fit())) lines << QString::fromStdString(line);
      return lines;
    }();
    QCOMPARE(w.warnings(), expected);
    QCOMPARE(w.status(), expected.isEmpty()
                             ? QString::fromStdString(pp::flux_status_line(*w.fit()))
                             : QStringLiteral("%1 · %2 %3")
                                   .arg(QString::fromStdString(pp::flux_status_line(*w.fit())))
                                   .arg(expected.size())
                                   .arg(expected.size() == 1 ? QStringLiteral("warning") : QStringLiteral("warnings")));

    // The tree shows what is open.
    QTRY_VERIFY_WITH_TIMEOUT(level_item(w, QStringLiteral("NM-300"), QStringLiteral("A")), kWaitMs);
    QCOMPARE(w.tree()->currentItem(), level_item(w, QStringLiteral("NM-300"), QStringLiteral("A")));
  }

  void picking_a_level_in_the_tree_opens_it() {
    auto r = rig();
    FluxWindow w(*r.bridge, *r.source, r.presets);
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    auto* item = level_item(w, QStringLiteral("NM-300"), QStringLiteral("A"));
    QVERIFY(item);
    w.tree()->setCurrentItem(item);
    QVERIFY(w.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 A"));
    QCOMPARE(w.monitors()->rowCount(), 8);
    QVERIFY(w.fit());
  }

  void selecting_a_monitor_shows_its_analyses() {
    auto r = rig();
    FluxWindow w(*r.bridge, *r.source, r.presets);
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY2(w.fit(), qPrintable(w.status()));
    const auto plain = w.view()->scene();
    QVERIFY(plain);
    const std::size_t plain_layers = layers_of(*plain).size();

    w.select_monitor(3);
    QCOMPARE(w.analyses()->rowCount(), 3);
    QCOMPARE(w.analyses()->index(0, FluxAnalysisModel::Record).data().toString(), QStringLiteral("66003-01"));
    QCOMPARE(w.monitor_table()->currentIndex().row(), w.monitors()->row_of(3));
    // The highlight: the hole's analyses and its mean drawn again on top, unlabelled.
    auto scene = w.view()->scene();
    QVERIFY(scene);
    QCOMPARE(layers_of(*scene).size(), plain_layers + 2);
    const auto* on_top = std::get_if<pp::PointLayer>(&layers_of(*scene)[plain_layers]);
    QVERIFY(on_top);
    QVERIFY(on_top->label.empty());
    QCOMPARE(on_top->refs.size(), std::size_t{3});
    for (const auto& ref : on_top->refs)
      QVERIFY(ref.analysis == seeded_.analyses.at("66003-01").str() || ref.analysis == seeded_.analyses.at("66003-02").str() ||
              ref.analysis == seeded_.analyses.at("66003-03").str());

    // A row of the table, as a click on it.
    w.monitor_table()->selectRow(w.monitors()->row_of(5));
    QCOMPARE(w.analyses()->rowCount(), 3);
    QCOMPARE(w.analyses()->index(2, FluxAnalysisModel::Record).data().toString(), QStringLiteral("66005-03"));
    scene = w.view()->scene();
    QCOMPARE(layers_of(*scene).size(), plain_layers + 2);

    // A hole that is not a monitor changes nothing.
    w.select_monitor(9);
    QCOMPARE(w.analyses()->index(2, FluxAnalysisModel::Record).data().toString(), QStringLiteral("66005-03"));
  }

  void a_level_that_cannot_load_says_why() {
    QVERIFY(pt::seed_level_without_monitors(*store_, seeded_, "B").has_value());
    auto r = rig();
    FluxWindow w(*r.bridge, *r.source, r.presets);
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY(w.fit());

    // B reads, and has no monitor to fit: the error is fit_level's, and the
    // level stays on show without predictions.
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("B"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("no monitor positions")), qPrintable(w.status()));
    QCOMPARE(w.status(), QStringLiteral("flux: NM-300B has no monitor positions"));
    QCOMPARE(w.fit(), nullptr);
    QVERIFY(w.warnings().isEmpty());
    QCOMPARE(w.monitors()->rowCount(), 0);
    QCOMPARE(w.unknowns()->rowCount(), 1);
    QCOMPARE(w.unknowns()->index(0, pychron::ui::FluxUnknownModel::Identifier).data().toString(),
             QStringLiteral("66201"));
    QVERIFY(w.unknowns()->index(0, pychron::ui::FluxUnknownModel::PredJ).data().toString().isEmpty());
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 B"));

    // A level that is not there cannot be read at all: nothing is left on show.
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("Z"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY(w.status_is_error());
    QCOMPARE(w.status(), QStringLiteral("flux: no level Z of NM-300"));
    QCOMPARE(w.inputs(), nullptr);
    QCOMPARE(w.fit(), nullptr);
    QCOMPARE(w.monitors()->rowCount(), 0);
    QCOMPARE(w.unknowns()->rowCount(), 0);
    QCOMPARE(w.analyses()->rowCount(), 0);
    QVERIFY(!w.view()->scene());
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux"));

    // And a level that reads puts the window right again.
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QVERIFY(!w.status_is_error());
    QVERIFY(w.fit());
    QCOMPARE(w.monitors()->rowCount(), 8);
  }

  void the_latest_selection_wins() {
    auto second = pt::seed_second_flux_level(*store_, actor_, seeded_, "C");
    QVERIFY2(second.has_value(), second ? "" : to_string(second.error()).c_str());
    auto r = rig();
    // Every level the monitors table ever shows, by its first identifier.
    // Before the window: the window resets its models as it is destroyed.
    QStringList shown;
    FluxWindow w(*r.bridge, *r.source, r.presets);
    connect(w.monitors(), &QAbstractItemModel::modelReset, &w, [&] {
      if (w.monitors()->rowCount() > 0)
        shown << w.monitors()->index(0, FluxMonitorModel::Identifier).data().toString();
    });
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    w.select_monitor(3);  // nothing to select yet
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("C"));
    QCOMPARE(w.status(), QStringLiteral("Loading NM-300 C…"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    drain(*r.bridge);  // A's result came and went
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 C"));
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->level, std::string("C"));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.fit()->level, std::string("C"));
    QCOMPARE(w.monitors()->rowCount(), 8);
    for (int row = 0; row < 8; ++row)
      QCOMPARE(w.monitors()->index(row, FluxMonitorModel::Identifier).data().toString(),
               QString::number(67001 + row));
    QCOMPARE(w.unknowns()->index(0, pychron::ui::FluxUnknownModel::Identifier).data().toString(),
             QStringLiteral("67101"));
    QCOMPARE(points_labelled(*w.view()->scene(), "Analyses")->x.size(), std::size_t{24});
    QCOMPARE(w.tree()->currentItem(), level_item(w, QStringLiteral("NM-300"), QStringLiteral("C")));
    // A was read too, and its result dropped: it was never on show.
    QVERIFY(!shown.isEmpty());
    QVERIFY2(!shown.contains(QStringLiteral("66001")), qPrintable(shown.join(QLatin1Char(' '))));

    // The other way round, the first being the one that fails.
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("Z"));
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    drain(*r.bridge);
    QVERIFY2(!w.status_is_error(), qPrintable(w.status()));
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 A"));
    QCOMPARE(w.monitors()->index(0, FluxMonitorModel::Identifier).data().toString(), QStringLiteral("66001"));
  }

  // ---- Edits ------------------------------------------------------------------

  void unticking_fit_drops_a_degree_of_freedom() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.fit()->dof, 5);
    QVERIFY(!w.edited());
    QVERIFY(!w.status().contains(QStringLiteral("edited")));
    const auto actions = w.findChild<QToolBar*>(QStringLiteral("flux_toolbar"))->actions();
    QCOMPARE(actions.size(), 6);
    QCOMPARE(actions[5], w.packages_action());
    QCOMPARE(actions[0], w.save_action());
    QCOMPARE(actions[1], w.revert_action());
    QCOMPARE(actions[2], w.reload_action());
    QCOMPARE(actions[3], w.reset_omissions_action());
    QCOMPARE(actions[4], w.export_action());
    QCOMPARE(w.save_action()->text(), QStringLiteral("Save"));
    QCOMPARE(w.export_action()->text(), QStringLiteral("Export CSV…"));
    QCOMPARE(w.revert_action()->text(), QStringLiteral("Revert"));
    QCOMPARE(w.reload_action()->text(), QStringLiteral("Reload"));
    QCOMPARE(w.reset_omissions_action()->text(), QStringLiteral("Reset omissions"));

    w.set_in_fit(3, false);
    QVERIFY(w.busy());  // W5: the refit follows, debounced
    QVERIFY(w.edited());
    QVERIFY(settle(w));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.fit()->dof, 4);
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
    const int row = w.monitors()->row_of(3);
    QCOMPARE(check(w.monitors(), row, FluxMonitorModel::Fit), Qt::Unchecked);
    QVERIFY(fitted_at(*w.fit(), 3).excluded);
    QVERIFY2(w.status().startsWith(QStringLiteral("plane, unweighted · fit MSWD")), qPrintable(w.status()));
    QVERIFY2(w.status().contains(QStringLiteral("(4 dof)")), qPrintable(w.status()));
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));

    // The Fit box of the table is the same edit; ticking it back leaves nothing pending.
    QVERIFY(w.monitors()->setData(w.monitors()->index(row, FluxMonitorModel::Fit), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(settle(w));
    QCOMPARE(w.fit()->dof, 5);
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(w.edits().include_positions.empty());
    QVERIFY(!w.edited());
    QVERIFY(!w.status().contains(QStringLiteral("edited")));

    // A Save box is no part of the fit, and is an edit all the same.
    QVERIFY(w.monitors()->setData(w.monitors()->index(row, FluxMonitorModel::Save), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(check(w.monitors(), row, FluxMonitorModel::Save), Qt::Unchecked);
    QVERIFY(w.edited());
    QVERIFY(!w.busy());
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));
    QVERIFY(w.unknowns()->setData(w.unknowns()->index(0, pychron::ui::FluxUnknownModel::Save), Qt::Unchecked,
                                  Qt::CheckStateRole));
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Unchecked);
    QVERIFY(w.monitors()->setData(w.monitors()->index(row, FluxMonitorModel::Save), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(w.edited());  // the unknown's is still unticked
    QVERIFY(w.unknowns()->setData(w.unknowns()->index(0, pychron::ui::FluxUnknownModel::Save), Qt::Checked,
                                  Qt::CheckStateRole));
    QVERIFY(!w.edited());

    // A hole that is not a monitor of the level is no edit.
    w.set_in_fit(9, false);
    w.set_in_fit(99, false);
    QVERIFY(!w.edited());
    QVERIFY(!w.busy());

    // Reset omissions with no saved fit to forget: nothing is pending for it,
    // and the edits made here are dropped as ever.
    w.reset_omissions();
    QVERIFY(!w.edits().reset_omits);
    QVERIFY(!w.edited());
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    w.reset_omissions();
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(!w.edits().reset_omits);
    QVERIFY(!w.edited());
    QCOMPARE(w.fit()->dof, 5);
  }

  void a_plot_click_omits_an_analysis_everywhere() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    w.resize(1400, 820);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    // The second of the hole's three is the golden J itself: it is drawn where
    // the hole's mean is, with it and without it. The click reaches it under
    // the mean (ruling R8), and omitting it changes the mean's error, not the mean.
    const std::string uuid = seeded_.analyses.at("66003-02").str();
    const double mean_before = *fitted_at(*w.fit(), 3).mean_j;
    const double error_before = *fitted_at(*w.fit(), 3).mean_j_err;
    QCOMPARE(fitted_at(*w.fit(), 3).n, 3);
    w.select_monitor(3);
    QCOMPARE(drawn_excluded(*w.view()->scene(), uuid), std::optional<bool>(false));

    const auto click = [&] {
      w.view()->plot()->replot();
      const auto pos = w.view()->point_position(uuid);
      QVERIFY(pos);
      QTest::mouseClick(w.view()->plot(), Qt::LeftButton, Qt::NoModifier, w.view()->plot()->mapFrom(w.view(), *pos));
    };
    click();
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.edits().omit, std::set<std::string>{"66003-02"});
    const int row = w.monitors()->row_of(3);
    QCOMPARE(w.monitors()->index(row, FluxMonitorModel::N).data().toInt(), 2);
    QCOMPARE(fitted_at(*w.fit(), 3).n, 2);
    QVERIFY(*fitted_at(*w.fit(), 3).mean_j_err != error_before);
    // The selection survived the refit: the analyses table shows the same monitor.
    QCOMPARE(w.analyses()->rowCount(), 3);
    QCOMPARE(w.analyses()->index(1, FluxAnalysisModel::Record).data().toString(), QStringLiteral("66003-02"));
    QCOMPARE(check(w.analyses(), 1, FluxAnalysisModel::Use), Qt::Unchecked);
    QCOMPARE(w.analyses()->index(1, FluxAnalysisModel::State).data().toString(), QStringLiteral("omitted (here)"));
    QCOMPARE(drawn_excluded(*w.view()->scene(), uuid), std::optional<bool>(true));
    QVERIFY(w.edited());
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));

    click();
    QVERIFY(settle(w));
    QCOMPARE(w.monitors()->index(row, FluxMonitorModel::N).data().toInt(), 3);
    QCOMPARE(*fitted_at(*w.fit(), 3).mean_j, mean_before);
    QCOMPARE(*fitted_at(*w.fit(), 3).mean_j_err, error_before);
    QCOMPARE(check(w.analyses(), 1, FluxAnalysisModel::Use), Qt::Checked);
    QCOMPARE(drawn_excluded(*w.view()->scene(), uuid), std::optional<bool>(false));
    QVERIFY(w.edits().omit.empty());
    QVERIFY(w.edits().include.empty());
    QVERIFY(!w.edited());
  }

  void the_use_box_and_the_plot_click_are_the_same_edit() {
    auto r = rig();
    const std::string uuid = seeded_.analyses.at("66003-02").str();
    // By the plot: a click on the analysis, which lies under its hole's mean.
    auto clicked = opened(r);
    clicked->resize(1400, 820);
    clicked->show();
    QVERIFY(QTest::qWaitForWindowExposed(clicked.get()));
    clicked->view()->plot()->replot();
    const auto pos = clicked->view()->point_position(uuid);
    QVERIFY(pos);
    QTest::mouseClick(clicked->view()->plot(), Qt::LeftButton, Qt::NoModifier,
                      clicked->view()->plot()->mapFrom(clicked->view(), *pos));
    QVERIFY(clicked->busy());
    QVERIFY(settle(*clicked));
    QVERIFY2(clicked->fit(), qPrintable(clicked->status()));
    // By the Use box.
    auto ticked = opened(r);
    ticked->select_monitor(3);
    QCOMPARE(ticked->analyses()->index(1, FluxAnalysisModel::Record).data().toString(), QStringLiteral("66003-02"));
    QVERIFY(ticked->analyses()->setData(ticked->analyses()->index(1, FluxAnalysisModel::Use), Qt::Unchecked,
                                        Qt::CheckStateRole));
    QVERIFY(settle(*ticked));
    QVERIFY2(ticked->fit(), qPrintable(ticked->status()));

    QCOMPARE(ticked->edits().omit, clicked->edits().omit);
    QCOMPARE(ticked->edits().include, clicked->edits().include);
    QCOMPARE(ticked->fit()->parameters, clicked->fit()->parameters);
    QCOMPARE(ticked->fit()->positions.size(), clicked->fit()->positions.size());
    for (std::size_t i = 0; i < ticked->fit()->positions.size(); ++i) {
      const auto& a = ticked->fit()->positions[i];
      const auto& b = clicked->fit()->positions[i];
      QCOMPARE(a.hole, b.hole);
      QCOMPARE(a.n, b.n);
      QCOMPARE(a.mean_j, b.mean_j);
      QCOMPARE(a.j, b.j);
      QCOMPARE(a.j_err, b.j_err);
      QCOMPARE(a.used_in_fit, b.used_in_fit);
    }
    QCOMPARE(fitted_at(*ticked->fit(), 3).n, 2);
    QCOMPARE(fitted_at(*ticked->fit(), 3).analyses.at(1).state, pp::AnalysisState::OmittedByEdit);

    // The box asks for a state: asked again for the one it has, nothing changes
    // (two quick clicks answered by one refit do not net to something else).
    QVERIFY(ticked->analyses()->setData(ticked->analyses()->index(1, FluxAnalysisModel::Use), Qt::Unchecked,
                                        Qt::CheckStateRole));
    QVERIFY(!ticked->busy());
    QCOMPARE(ticked->edits().omit, std::set<std::string>{"66003-02"});
    Q_EMIT ticked->analyses()->use_toggled(QString::fromStdString(seeded_.analyses.at("66003-01").str()), true);
    QVERIFY(!ticked->busy());
    QCOMPARE(ticked->edits().omit, std::set<std::string>{"66003-02"});
    QVERIFY(ticked->edits().include.empty());
    // Ticked back: nothing pending.
    QVERIFY(ticked->analyses()->setData(ticked->analyses()->index(1, FluxAnalysisModel::Use), Qt::Checked,
                                        Qt::CheckStateRole));
    QVERIFY(settle(*ticked));
    QVERIFY(!ticked->edited());

    // A rubber band over two analyses is one edit, answered by one refit.
    int fits = 0;  // the unknowns' model is given the fit once per refit
    const auto counting = connect(ticked->unknowns(), &QAbstractItemModel::modelReset, ticked.get(), [&] {
      if (ticked->fit()) ++fits;
    });
    ticked->set_in_fit(7, false);
    QVERIFY(settle(*ticked));
    const int one_fit = fits;
    QVERIFY(one_fit > 0);
    fits = 0;
    Q_EMIT ticked->view()->points_toggled({QString::fromStdString(seeded_.analyses.at("66001-01").str()),
                                           QString::fromStdString(seeded_.analyses.at("66002-03").str())});
    QCOMPARE(ticked->edits().omit, (std::set<std::string>{"66001-01", "66002-03"}));
    QVERIFY(ticked->busy());
    QVERIFY(settle(*ticked));
    QCOMPARE(fits, one_fit);
    disconnect(counting);  // the window outlives the counter
    QCOMPARE(fitted_at(*ticked->fit(), 1).n, 2);
    QCOMPARE(fitted_at(*ticked->fit(), 2).n, 2);
    QCOMPARE(fitted_at(*ticked->fit(), 1).analyses.at(0).state, pp::AnalysisState::OmittedByEdit);
    QCOMPARE(fitted_at(*ticked->fit(), 2).analyses.at(2).state, pp::AnalysisState::OmittedByEdit);

    // Several at once, one of them unknown to the level: the others are toggled.
    clicked->toggle_analyses({QString::fromStdString(seeded_.analyses.at("66004-01").str()), QStringLiteral("nothing"),
                              QString::fromStdString(uuid)});
    QVERIFY(settle(*clicked));
    QCOMPARE(clicked->edits().omit, std::set<std::string>{"66004-01"});
    QCOMPARE(fitted_at(*clicked->fit(), 3).n, 3);
    QCOMPARE(fitted_at(*clicked->fit(), 4).n, 2);
  }

  void a_saved_exclusion_can_be_ticked_back() {
    for (int hole = 1; hole <= 8; ++hole) save_fit(hole, pp::FluxOptions{}, hole == 5);
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    const int row = w.monitors()->row_of(5);
    QCOMPARE(check(w.monitors(), row, FluxMonitorModel::Fit), Qt::Unchecked);
    QVERIFY(fitted_at(*w.fit(), 5).excluded);
    QCOMPARE(w.fit()->dof, 4);
    QVERIFY(!w.edited());

    w.set_in_fit(5, true);
    QVERIFY(settle(w));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.edits().include_positions, std::set<int>{5});
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(fitted_at(*w.fit(), 5).used_in_fit);
    QVERIFY(!fitted_at(*w.fit(), 5).excluded);
    QCOMPARE(check(w.monitors(), row, FluxMonitorModel::Fit), Qt::Checked);
    QCOMPARE(w.fit()->dof, 5);
    QVERIFY(w.edited());

    // And out again is what the saved fit says: nothing is pending.
    w.set_in_fit(5, false);
    QVERIFY(settle(w));
    QVERIFY(w.edits().include_positions.empty());
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(fitted_at(*w.fit(), 5).excluded);
    QVERIFY(!w.edited());
  }

  // ---- Options and presets ------------------------------------------------------

  void a_preset_applies() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    auto* dock = w.findChild<QDockWidget*>(QStringLiteral("flux_dock"));
    QVERIFY(dock);
    QCOMPARE(dock->windowTitle(), QStringLiteral("Fit"));
    QCOMPARE(w.dockWidgetArea(dock), Qt::RightDockWidgetArea);
    QCOMPARE(w.options_editor()->sections(), QStringList({QStringLiteral("Model"), QStringLiteral("Errors")}));
    // No saved fit: the level opens on the preset in use.
    QCOMPARE(w.preset_bar()->current_name(), QStringLiteral("Default"));
    QCOMPARE(w.preset_bar()->combo()->findText(QStringLiteral("(saved fit)")), -1);
    QVERIFY(!w.options().fit.weighted);

    QVERIFY(w.preset_bar()->select(QStringLiteral("Weighted plane")));
    QVERIFY(w.options().fit.weighted);
    QVERIFY(w.options_editor()->options().get_bool("model.weighted"));
    QVERIFY(w.edited());
    QVERIFY(settle(w));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(w.fit()->options.fit.weighted);
    QVERIFY2(w.status().startsWith(QStringLiteral("plane, weighted · fit MSWD")), qPrintable(w.status()));
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));

    // The bar saves what the editor shows.
    w.options_editor()->apply(QStringLiteral("model.kind"), std::string("nearest"));
    QVERIFY(settle(w));
    QCOMPARE(w.options().fit.kind, reduction::ModelKind::NearestNeighbors);
    w.preset_bar()->ask_name = [] { return QStringLiteral("Mine"); };
    QVERIFY(w.preset_bar()->save(true));
    auto mine = r.presets.load(pp::flux_options_schema(), "Mine");
    QVERIFY(mine.has_value());
    QCOMPARE(mine->options.get_string("model.kind"), std::string("nearest"));
    QVERIFY(mine->options.get_bool("model.weighted"));

    // A level without a saved fit opens on the preset in use.
    w.set_ask_unsaved([](const QString&) { return FluxWindow::Unsaved::Discard; });
    w.reload();
    QVERIFY(settle(w));
    QCOMPARE(w.preset_bar()->current_name(), QStringLiteral("Mine"));
    QCOMPARE(w.options().fit.kind, reduction::ModelKind::NearestNeighbors);
    QVERIFY(!w.edited());
  }

  void a_saved_fit_is_where_a_level_starts() {
    for (int hole = 1; hole <= 8; ++hole) save_fit(hole, nearest3());
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.options(), nearest3());
    QCOMPARE(w.fit()->options, nearest3());
    QCOMPARE(w.options_editor()->options().get_string("model.kind"), std::string("nearest"));
    QCOMPARE(w.options_editor()->options().get_int("model.neighbors"), std::int64_t{3});
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    QCOMPARE(w.preset_bar()->combo()->currentIndex(), 0);
    QVERIFY(w.preset_bar()->pinned_selected());
    QVERIFY(!w.edited());
    QVERIFY2(w.status().startsWith(QStringLiteral("nearest")), qPrintable(w.status()));

    // "(saved fit)" is not a preset: nothing to delete, and Save asks for a name.
    QVERIFY(!w.preset_bar()->remove());
    QVERIFY(!w.preset_bar()->factory_reset());
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    QCOMPARE(w.options(), nearest3());

    // An option edited leaves the entry there; choosing it again is the saved fit again.
    QVERIFY(w.options_editor()->apply(QStringLiteral("model.neighbors"), std::int64_t{2}));
    QVERIFY(w.edited());
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    Q_EMIT w.preset_bar()->combo()->activated(0);
    QCOMPARE(w.options(), nearest3());
    QCOMPARE(w.options_editor()->options().get_int("model.neighbors"), std::int64_t{3});
    QVERIFY(!w.edited());
    QVERIFY(settle(w));

    // A preset applies over it, and the entry goes.
    QVERIFY(w.preset_bar()->select(QStringLiteral("Default")));
    QCOMPARE(w.options(), pp::FluxOptions{});
    QCOMPARE(w.preset_bar()->combo()->findText(QStringLiteral("(saved fit)")), -1);
    QCOMPARE(w.preset_bar()->current_name(), QStringLiteral("Default"));
    QVERIFY(w.edited());
    QVERIFY(settle(w));

    // Revert is the saved fit again.
    w.revert();
    QCOMPARE(w.options(), nearest3());
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    QVERIFY(!w.edited());

    // Save with "(saved fit)" selected asks for a name instead of writing a preset of that name.
    int asked = 0;
    w.preset_bar()->ask_name = [&] {
      ++asked;
      return QStringLiteral("From A");
    };
    QVERIFY(w.preset_bar()->save(false));
    QCOMPARE(asked, 1);
    QVERIFY(r.presets.load(pp::flux_options_schema(), "From A").has_value());
    QVERIFY(!r.presets.load(pp::flux_options_schema(), "(saved fit)").has_value());
    QVERIFY(!w.edited());  // the options are still the saved fit's

    // While the level is read again its options are nobody's: the defaults.
    w.reload();
    QVERIFY(w.busy());
    QCOMPARE(w.options(), pp::FluxOptions{});
    QVERIFY(settle(w));
    QCOMPARE(w.options(), nearest3());
  }

  void a_model_the_monitors_cannot_fit() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    w.select_monitor(3);

    auto bowl = pp::to_options(pp::FluxOptions{});
    QVERIFY(bowl.set("model.kind", std::string("bowl")).has_value());
    w.set_options(bowl);
    QVERIFY(settle(w));
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("do not determine a bowl")), qPrintable(w.status()));
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));
    QCOMPARE(w.fit(), nullptr);
    QCOMPARE(w.options().fit.kind, reduction::ModelKind::Bowl);
    QCOMPARE(w.options_editor()->options().get_string("model.kind"), std::string("bowl"));
    // The data stays in view: the monitors without predictions, the analyses, the plot.
    QVERIFY(w.inputs());
    QCOMPARE(w.monitors()->rowCount(), 8);
    QCOMPARE(w.monitors()->index(0, FluxMonitorModel::Identifier).data().toString(), QStringLiteral("66001"));
    for (int row = 0; row < 8; ++row)
      QVERIFY(w.monitors()->index(row, FluxMonitorModel::PredJ).data().toString().isEmpty());
    QCOMPARE(w.unknowns()->rowCount(), 4);
    QCOMPARE(w.analyses()->rowCount(), 3);
    const auto scene = w.view()->scene();
    QVERIFY(scene);
    QCOMPARE(points_labelled(*scene, "Analyses")->x.size(), std::size_t{24});

    // An edit while there is no fit is decided as fit_level would have.
    const std::string uuid = seeded_.analyses.at("66003-02").str();
    w.toggle_analyses({QString::fromStdString(uuid)});
    QVERIFY(settle(w));
    QCOMPARE(w.edits().omit, std::set<std::string>{"66003-02"});
    QCOMPARE(w.analyses()->index(1, FluxAnalysisModel::State).data().toString(), QStringLiteral("omitted (here)"));
    QCOMPARE(drawn_excluded(*w.view()->scene(), uuid), std::optional<bool>(true));
    w.toggle_analyses({QString::fromStdString(uuid)});
    QVERIFY(settle(w));
    QVERIFY(w.edits().omit.empty());
    QVERIFY(w.edits().include.empty());

    // The model changed in the dock: fitted again, with nothing read from the
    // store (a reload would show the J saved meanwhile).
    save_fit(1, pp::FluxOptions{});
    QVERIFY(w.options_editor()->apply(QStringLiteral("model.kind"), std::string("plane")));
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QVERIFY2(!w.status_is_error(), qPrintable(w.status()));
    QVERIFY(w.fit());
    QCOMPARE(w.fit()->dof, 5);
    QVERIFY(!w.monitors()->index(0, FluxMonitorModel::PredJ).data().toString().isEmpty());
    QCOMPARE(w.analyses()->rowCount(), 3);
    QVERIFY(!w.edited());
    QVERIFY(w.monitors()->index(w.monitors()->row_of(1), FluxMonitorModel::SavedJ).data().toString().isEmpty());
    QCOMPARE(w.status(), QString::fromStdString(pp::flux_status_line(*w.fit())) +
                             (w.warnings().isEmpty() ? QString()
                              : w.warnings().size() == 1 ? QStringLiteral(" · 1 warning")
                                                         : QStringLiteral(" · %1 warnings").arg(w.warnings().size())));

    // A preset whose options cannot fit the level, the same.
    auto custom = pp::to_options(pp::FluxOptions{});
    QVERIFY(custom.set("model.kind", std::string("bowl")).has_value());
    QVERIFY(r.presets.save("Bowl", custom).has_value());
    w.preset_bar()->reload(QStringLiteral("Default"));
    QVERIFY(w.preset_bar()->select(QStringLiteral("Bowl")));
    QVERIFY(settle(w));
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("do not determine a bowl")), qPrintable(w.status()));
    QCOMPARE(w.monitors()->rowCount(), 8);
    QVERIFY(w.preset_bar()->select(QStringLiteral("Default")));
    QVERIFY(settle(w));
    QVERIFY2(!w.status_is_error(), qPrintable(w.status()));
    QVERIFY(w.fit());
  }

  // Ruling R10: what the preset bar says follows the status; it does not cover it.
  void a_preset_message_follows_the_status() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    const QString fitted = w.status();
    QVERIFY(!w.preset_bar()->select(QStringLiteral("No such preset")));
    QVERIFY2(w.status().startsWith(fitted + QStringLiteral(" · Preset: ")), qPrintable(w.status()));
    QVERIFY(!w.status_is_error());

    auto bowl = pp::to_options(pp::FluxOptions{});
    QVERIFY(bowl.set("model.kind", std::string("bowl")).has_value());
    w.set_options(bowl);  // the user's next change: the message has been read
    QVERIFY(!w.status().contains(QStringLiteral("Preset:")));
    QVERIFY(!w.status().isEmpty());
    QVERIFY(settle(w));
    QVERIFY(w.status_is_error());
    const QString failed = w.status();
    QVERIFY2(failed.contains(QStringLiteral("do not determine a bowl")), qPrintable(failed));
    QVERIFY2(failed.endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(failed));

    // With a fit error on show: the error stays, the message after it, "edited" last.
    QVERIFY(!w.preset_bar()->select(QStringLiteral("No such preset")));
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("do not determine a bowl")), qPrintable(w.status()));
    QVERIFY2(w.status().contains(QStringLiteral(" · Preset: ")), qPrintable(w.status()));
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));
    QVERIFY(w.statusBar()->currentMessage().isEmpty());  // nothing hides the label
    QVERIFY(!w.findChild<QWidget*>(QStringLiteral("flux_status"))->isHidden());

    // The next status update: the error is still what is reported.
    w.set_in_fit(3, false);
    QVERIFY(!w.status().isEmpty());
    QVERIFY(settle(w));
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("bowl")), qPrintable(w.status()));
    QVERIFY(!w.status().contains(QStringLiteral("Preset:")));
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));

    // A message that comes with the preset it loaded outlives the refit that follows.
    QVERIFY(w.preset_bar()->select(QStringLiteral("Default")));
    Q_EMIT w.preset_bar()->message(QStringLiteral("Preset \"Default\" loaded with warnings"), QStringLiteral("one\ntwo"));
    QVERIFY(settle(w));
    QVERIFY2(!w.status_is_error(), qPrintable(w.status()));
    QVERIFY2(w.status().contains(QStringLiteral(" · Preset \"Default\" loaded with warnings")), qPrintable(w.status()));
    QVERIFY(w.findChild<QWidget*>(QStringLiteral("flux_status"))->toolTip().contains(QStringLiteral("one\ntwo")));
    w.revert();
    QVERIFY(!w.status().contains(QStringLiteral("Preset")));
    QVERIFY(!w.findChild<QWidget*>(QStringLiteral("flux_status"))->toolTip().contains(QStringLiteral("one")));
    QVERIFY(!w.status().isEmpty());
  }

  void a_saved_fit_the_monitors_cannot_fit() {
    // Saved as a bowl (by another program, or before a monitor lost its analyses).
    pp::FluxOptions bowl;
    bowl.fit.kind = reduction::ModelKind::Bowl;
    for (int hole = 1; hole <= 8; ++hole) save_fit(hole, bowl);
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("do not determine a bowl")), qPrintable(w.status()));
    QCOMPARE(w.fit(), nullptr);
    QVERIFY(!w.edited());
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    QCOMPARE(w.monitors()->rowCount(), 8);
    QVERIFY(!w.monitors()->index(0, FluxMonitorModel::SavedJ).data().toString().isEmpty());
    QVERIFY(w.monitors()->index(0, FluxMonitorModel::PredJ).data().toString().isEmpty());
    QVERIFY(w.view()->scene());
    QVERIFY(w.options_editor()->apply(QStringLiteral("model.kind"), std::string("plane")));
    QVERIFY(settle(w));
    QVERIFY2(!w.status_is_error(), qPrintable(w.status()));
    QVERIFY(w.fit());
    QVERIFY(w.edited());
  }

  void sd_with_a_surface_is_shown_on_the_field() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));

    QVERIFY(w.options_editor()->apply(QStringLiteral("fit.error"), std::string("sd")));
    QVERIFY(!w.busy());  // nothing to fit: no refit is pending
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("sd is not an error kind of a fitted surface")), qPrintable(w.status()));
    QCOMPARE(w.fit(), nullptr);
    QCOMPARE(w.monitors()->rowCount(), 8);
    QVERIFY(w.monitors()->index(0, FluxMonitorModel::PredJ).data().toString().isEmpty());
    QVERIFY(w.view()->scene());
    QVERIFY(w.edited());
    QVERIFY(w.options_editor()->editor(QStringLiteral("fit.error"))->property("invalid").toBool());
    QCOMPARE(w.options().fit.error, reduction::MeanErrorKind::Msem);  // the last options that were a fit's

    // An edit meanwhile is kept, and still does not fit.
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("sd is not an error kind")), qPrintable(w.status()));
    QCOMPARE(w.fit(), nullptr);

    // A mean model takes sd: the field is right again and the level fits.
    QVERIFY(w.options_editor()->apply(QStringLiteral("model.kind"), std::string("weighted-mean")));
    QVERIFY(settle(w));
    QVERIFY2(!w.status_is_error(), qPrintable(w.status()));
    QVERIFY(w.fit());
    QCOMPARE(w.options().fit.error, reduction::MeanErrorKind::Sd);
    QVERIFY(!w.options_editor()->editor(QStringLiteral("fit.error"))->property("invalid").toBool());
    QVERIFY(!fitted_at(*w.fit(), 3).used_in_fit);
  }

  // ---- The monitor group --------------------------------------------------------

  void another_monitor_set_reloads() {
    auto sets = pp::load_monitor_sets(*store_);
    QVERIFY2(sets.has_value(), sets ? "" : to_string(sets.error()).c_str());
    pp::MonitorSets edited = sets->sets;
    pp::MonitorSet alt = *edited.find("");
    const std::string standard = alt.name;
    alt.name = "Alt";
    alt.age_ma *= 1.01;
    edited.sets.push_back(alt);
    auto committed = pp::save_monitor_sets(*store_, actor_, edited, *sets);
    QVERIFY2(committed.has_value(), committed ? "" : to_string(committed.error()).c_str());

    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Cancel;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    auto* group = w.findChild<QWidget*>(QStringLiteral("flux_monitor_group"));
    QVERIFY(group);
    // The group shows what the load used, and that is no edit.
    QCOMPARE(w.monitor_set_combo()->count(), static_cast<int>(edited.sets.size()));
    QCOMPARE(w.monitor_set_combo()->currentText(), QString::fromStdString(standard));
    QVERIFY(w.sample_edit()->text().isEmpty());
    QCOMPARE(w.sample_edit()->placeholderText(), QString::fromStdString(alt.sample));
    QVERIFY(!w.all_positions_box()->isChecked());
    QVERIFY(!w.edited());
    QVERIFY2(w.monitor_set_combo()->toolTip().startsWith(QStringLiteral("sample %1 · ").arg(QString::fromStdString(alt.sample))),
             qPrintable(w.monitor_set_combo()->toolTip()));
    QVERIFY2(w.monitor_set_combo()->toolTip().contains(QStringLiteral(" Ma · lambda_k ")),
             qPrintable(w.monitor_set_combo()->toolTip()));
    const double j_standard = w.fit()->max_j;

    // Another set: asks nothing when nothing is pending, and the level is read again.
    w.monitor_set_combo()->setCurrentText(QStringLiteral("Alt"));
    QVERIFY(w.busy());
    QCOMPARE(w.status(), QStringLiteral("Loading NM-300 A…"));
    QVERIFY(settle(w));
    QVERIFY(asked.isEmpty());
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->monitor_set.name, std::string("Alt"));
    QCOMPARE(w.monitor_set_combo()->currentText(), QStringLiteral("Alt"));
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(w.fit()->max_j != j_standard);  // another age, another J
    // Ruling R9: the level read with the choice is still not the level as saved.
    QVERIFY(w.edited());
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));
    // Reload asks for it, and keeps the choice.
    answer = FluxWindow::Unsaved::Discard;
    w.reload();
    QCOMPARE(asked, QStringList{QStringLiteral("Save the flux of NM-300 A?")});
    QVERIFY(settle(w));
    QCOMPARE(w.inputs()->monitor_set.name, std::string("Alt"));
    QVERIFY(w.edited());

    // Revert is the discard: the level as its saved fit chooses, read again, nothing asked.
    const int loads = w.loads_started();
    w.revert();
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QCOMPARE(w.loads_started(), loads + 1);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(w.inputs()->monitor_set.name, standard);
    QCOMPARE(w.monitor_set_combo()->currentText(), QString::fromStdString(standard));
    QVERIFY(!w.edited());
    QVERIFY(!w.status().contains(QStringLiteral("edited")));
    QCOMPARE(w.fit()->max_j, j_standard);

    // With an edit pending a change of the group asks; Cancel puts back
    // whichever of the three was changed and keeps everything.
    w.monitor_set_combo()->setCurrentText(QStringLiteral("Alt"));
    QVERIFY(settle(w));
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    answer = FluxWindow::Unsaved::Cancel;
    const auto untouched = [&] {
      return !w.busy() && w.monitor_set_combo()->currentText() == QStringLiteral("Alt") &&
             w.sample_edit()->text().isEmpty() && !w.all_positions_box()->isChecked() &&
             w.inputs()->monitor_set.name == "Alt" && w.inputs()->monitor_set.sample == alt.sample &&
             !w.inputs()->all_positions && w.edits().exclude_positions == std::set<int>{3};
    };
    w.monitor_set_combo()->setCurrentText(QString::fromStdString(standard));
    QCOMPARE(asked.size(), 2);
    QVERIFY(untouched());
    w.all_positions_box()->setChecked(true);
    QCOMPARE(asked.size(), 3);
    QVERIFY(untouched());
    w.sample_edit()->setText(QStringLiteral("unk"));
    Q_EMIT w.sample_edit()->editingFinished();
    QCOMPARE(asked.size(), 4);
    QVERIFY(untouched());

    // Discard reads it with the other group.
    answer = FluxWindow::Unsaved::Discard;
    w.all_positions_box()->setChecked(true);
    QCOMPARE(asked.size(), 5);
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QVERIFY(w.inputs()->all_positions);
    QVERIFY(w.all_positions_box()->isChecked());
    QCOMPARE(w.inputs()->monitor_set.name, std::string("Alt"));
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(w.edited());  // the group
    // From one choice to another nothing is asked: the choice is what is being changed.
    w.all_positions_box()->setChecked(false);
    QVERIFY(settle(w));
    QVERIFY(!w.inputs()->all_positions);
    QCOMPARE(asked.size(), 5);

    // Another sample: the unknowns' holes become the monitors (they have no analyses).
    w.sample_edit()->setText(QStringLiteral("unk"));
    QVERIFY(w.edited());
    Q_EMIT w.sample_edit()->editingFinished();
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 5);
    QCOMPARE(w.inputs()->monitor_set.sample, std::string("unk"));
    QCOMPARE(w.sample_edit()->text(), QStringLiteral("unk"));
    QCOMPARE(w.monitors()->rowCount(), 4);
    QVERIFY(w.edited());
    // Cleared: the set's own sample again.
    w.sample_edit()->clear();
    Q_EMIT w.sample_edit()->editingFinished();
    QVERIFY(settle(w));
    QCOMPARE(w.inputs()->monitor_set.sample, alt.sample);
    QCOMPARE(w.monitors()->rowCount(), 8);
    QVERIFY(w.sample_edit()->text().isEmpty());
    QVERIFY(w.fit());
    QVERIFY(w.edited());  // still the other set

    // Chosen back to what the level gives by itself: nothing is pending.
    w.monitor_set_combo()->setCurrentText(QString::fromStdString(standard));
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 5);
    QCOMPARE(w.inputs()->monitor_set.name, standard);
    QVERIFY(!w.edited());
    QCOMPARE(w.fit()->max_j, j_standard);
  }

  void a_chosen_monitor_set_is_an_edit() {
    auto second = pt::seed_second_flux_level(*store_, actor_, seeded_, "C");
    QVERIFY2(second.has_value(), second ? "" : to_string(second.error()).c_str());
    auto sets = pp::load_monitor_sets(*store_);
    QVERIFY2(sets.has_value(), sets ? "" : to_string(sets.error()).c_str());
    pp::MonitorSets with_alt = sets->sets;
    pp::MonitorSet alt = *with_alt.find("");
    const QString standard = QString::fromStdString(alt.name);
    alt.name = "Alt";
    alt.age_ma *= 1.01;
    with_alt.sets.push_back(alt);
    QVERIFY(pp::save_monitor_sets(*store_, actor_, with_alt, *sets).has_value());

    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Cancel;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    auto* a_item = level_item(w, QStringLiteral("NM-300"), QStringLiteral("A"));
    auto* c_item = level_item(w, QStringLiteral("NM-300"), QStringLiteral("C"));
    QVERIFY(a_item && c_item);

    w.monitor_set_combo()->setCurrentText(QStringLiteral("Alt"));
    QVERIFY(settle(w));
    QVERIFY(asked.isEmpty());
    QVERIFY(w.edited());

    // Another level: asked once; Cancel stays, with the set chosen.
    w.tree()->setCurrentItem(c_item);
    QCOMPARE(asked, QStringList{QStringLiteral("Save the flux of NM-300 A?")});
    QVERIFY(!w.busy());
    QCOMPARE(w.tree()->currentItem(), a_item);
    QCOMPARE(w.inputs()->level, std::string("A"));
    QCOMPARE(w.monitor_set_combo()->currentText(), QStringLiteral("Alt"));
    QVERIFY(w.edited());
    // Discard moves, and the other level's monitors are its own.
    answer = FluxWindow::Unsaved::Discard;
    w.tree()->setCurrentItem(c_item);
    QCOMPARE(asked.size(), 2);
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 2);
    QCOMPARE(w.inputs()->level, std::string("C"));
    QCOMPARE(w.monitor_set_combo()->currentText(), standard);
    QVERIFY(!w.edited());
    // And back: the choice was discarded with the level.
    w.tree()->setCurrentItem(a_item);
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 2);
    QCOMPARE(w.inputs()->monitor_set.name, standard.toStdString());
    QVERIFY(!w.edited());

    // Ruling R10: closed with Discard, the window is not still edited when shown again.
    w.set_in_fit(3, false);
    QVERIFY(w.preset_bar()->select(QStringLiteral("Weighted plane")));
    QVERIFY(settle(w));
    w.show();
    QVERIFY(w.close());
    QCOMPARE(asked.size(), 3);
    w.show();
    QVERIFY(!w.edited());
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(!w.options().fit.weighted);
    QCOMPARE(w.fit()->dof, 5);
    QVERIFY(!w.status().contains(QStringLiteral("edited")));
    QVERIFY(w.close());  // nothing pending, nothing asked
    QCOMPARE(asked.size(), 3);
    // The same with another monitor set chosen: the level is read again as saved.
    w.show();
    w.monitor_set_combo()->setCurrentText(QStringLiteral("Alt"));
    QVERIFY(settle(w));
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    QVERIFY(w.close());
    QCOMPARE(asked.size(), 4);
    w.show();
    QVERIFY(!w.edited());
    QVERIFY(settle(w));
    QVERIFY(!w.edited());
    QCOMPARE(w.inputs()->monitor_set.name, standard.toStdString());
    QCOMPARE(w.monitor_set_combo()->currentText(), standard);
    QVERIFY(w.edits().exclude_positions.empty());
  }

  // ---- Leaving edits behind -----------------------------------------------------

  void switching_level_with_edits_asks_once() {
    auto second = pt::seed_second_flux_level(*store_, actor_, seeded_, "C");
    QVERIFY2(second.has_value(), second ? "" : to_string(second.error()).c_str());
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Cancel;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    auto* a_item = level_item(w, QStringLiteral("NM-300"), QStringLiteral("A"));
    auto* c_item = level_item(w, QStringLiteral("NM-300"), QStringLiteral("C"));
    QVERIFY(a_item && c_item);

    // Nothing pending: nothing asked.
    w.tree()->setCurrentItem(c_item);
    QVERIFY(settle(w));
    QVERIFY(asked.isEmpty());
    QCOMPARE(w.inputs()->level, std::string("C"));
    w.tree()->setCurrentItem(a_item);
    QVERIFY(settle(w));
    QCOMPARE(w.inputs()->level, std::string("A"));

    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    QVERIFY(w.edited());

    // Cancel: stays, with the edits, and the tree is back on A.
    w.tree()->setCurrentItem(c_item);
    QCOMPARE(asked, QStringList{QStringLiteral("Save the flux of NM-300 A?")});
    QVERIFY(!w.busy());
    QCOMPARE(w.tree()->currentItem(), a_item);
    QVERIFY(a_item->isSelected());
    QVERIFY(!c_item->isSelected());
    QCOMPARE(w.inputs()->level, std::string("A"));
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
    QCOMPARE(w.fit()->dof, 4);
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 A"));

    // Reload and closing ask the same.
    w.reload();
    QCOMPARE(asked.size(), 2);
    QVERIFY(!w.busy());
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
    w.show();
    QVERIFY(!w.close());
    QCOMPARE(asked.size(), 3);
    QVERIFY(w.isVisible());

    // Discard: moves, once asked.
    answer = FluxWindow::Unsaved::Discard;
    w.tree()->setCurrentItem(c_item);
    QCOMPARE(asked.size(), 4);
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 4);
    QCOMPARE(w.inputs()->level, std::string("C"));
    QCOMPARE(w.tree()->currentItem(), c_item);
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(!w.edited());
    QCOMPARE(w.fit()->dof, 5);

    // Closing with edits and Discard closes; with none it asks nothing.
    w.set_in_fit(2, false);
    QVERIFY(settle(w));
    QVERIFY(w.close());
    QCOMPARE(asked.size(), 5);
    QVERIFY(!w.isVisible());
    FluxWindow plain(*r.bridge, *r.source, r.presets);
    plain.set_ask_unsaved([&](const QString& q) {
      asked << q;
      return FluxWindow::Unsaved::Cancel;
    });
    plain.show();
    QVERIFY(plain.close());
    QCOMPARE(asked.size(), 5);
  }

  void revert_and_reset_omissions() {
    for (int hole = 1; hole <= 8; ++hole) save_fit(hole, nearest3(), hole == 5);
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(!fitted_at(*w.fit(), 5).used_in_fit);
    // Saved meanwhile by someone else: Revert and Reset omissions do not read it.
    const int row1 = w.monitors()->row_of(1);
    QCOMPARE(w.monitors()->index(row1, FluxMonitorModel::SavedJ).data().toString(), QStringLiteral("1.0000e-03"));
    save_fit(1, nearest3(), false, 2.0e-3);

    // Every kind of edit.
    w.set_in_fit(3, false);
    w.toggle_analyses({QString::fromStdString(seeded_.analyses.at("66001-01").str())});
    QVERIFY(w.preset_bar()->select(QStringLiteral("Weighted plane")));
    const int row = w.monitors()->row_of(2);
    QVERIFY(w.monitors()->setData(w.monitors()->index(row, FluxMonitorModel::Save), Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(settle(w));
    QVERIFY(w.edited());
    QCOMPARE(w.options().fit.kind, reduction::ModelKind::Plane);

    w.revert();
    QVERIFY(!w.busy());  // no store call, and the fit is at once
    QVERIFY(!w.edited());
    QCOMPARE(w.options(), nearest3());
    QCOMPARE(w.options_editor()->options().get_string("model.kind"), std::string("nearest"));
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY(w.edits().omit.empty());
    QCOMPARE(check(w.monitors(), row, FluxMonitorModel::Save), Qt::Checked);
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QCOMPARE(w.fit()->options, nearest3());
    QVERIFY(fitted_at(*w.fit(), 3).used_in_fit);
    QVERIFY(!fitted_at(*w.fit(), 5).used_in_fit);  // the saved exclusion is not an edit
    QCOMPARE(fitted_at(*w.fit(), 1).n, 3);
    QVERIFY(!w.status().contains(QStringLiteral("edited")));
    QCOMPARE(w.monitors()->index(row1, FluxMonitorModel::SavedJ).data().toString(), QStringLiteral("1.0000e-03"));

    // Reset omissions: what the saved fit left out is forgotten, and so are the edits made here.
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    w.reset_omissions();
    QVERIFY(!w.busy());
    QVERIFY(w.edits().reset_omits);
    QVERIFY(w.edits().exclude_positions.empty());
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(fitted_at(*w.fit(), 5).used_in_fit);
    QVERIFY(fitted_at(*w.fit(), 3).used_in_fit);
    QCOMPARE(check(w.monitors(), w.monitors()->row_of(5), FluxMonitorModel::Fit), Qt::Checked);
    QVERIFY(w.edited());
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));
    QCOMPARE(w.options(), nearest3());

    // The tool bar's actions are these.
    w.revert_action()->trigger();
    QVERIFY(!w.edited());
    QVERIFY(!fitted_at(*w.fit(), 5).used_in_fit);
    w.reset_omissions_action()->trigger();
    QVERIFY(w.edits().reset_omits);
    QCOMPARE(w.monitors()->index(row1, FluxMonitorModel::SavedJ).data().toString(), QStringLiteral("1.0000e-03"));
    w.set_ask_unsaved([](const QString&) { return FluxWindow::Unsaved::Discard; });
    w.reload_action()->trigger();
    QVERIFY(w.busy());
    QVERIFY(!w.reload_action()->isEnabled());  // while the level is read
    QVERIFY(!w.revert_action()->isEnabled());
    QVERIFY(settle(w));
    QVERIFY(w.reload_action()->isEnabled());
    QVERIFY(w.revert_action()->isEnabled());
    QVERIFY(!w.edited());
    QVERIFY(!fitted_at(*w.fit(), 5).used_in_fit);
    QCOMPARE(w.monitors()->index(row1, FluxMonitorModel::SavedJ).data().toString(), QStringLiteral("2.0000e-03"));
  }

  void a_change_elsewhere_reloads_only_when_not_edited() {
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Discard;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    const int row1 = w.monitors()->row_of(1), row2 = w.monitors()->row_of(2);
    QVERIFY(w.monitors()->index(row1, FluxMonitorModel::SavedJ).data().toString().isEmpty());

    // Not edited: the level is read again, and shows what was saved elsewhere.
    save_fit(1, nearest3());
    r.bridge->notify_changed();
    QVERIFY(w.busy());
    QCOMPARE(w.status(), QStringLiteral("Loading NM-300 A…"));
    QVERIFY(settle(w));
    QVERIFY(asked.isEmpty());
    QVERIFY(!w.monitors()->index(row1, FluxMonitorModel::SavedJ).data().toString().isEmpty());
    QCOMPARE(w.options(), nearest3());
    QVERIFY(!w.status().contains(QStringLiteral("changed elsewhere")));
    QVERIFY(!w.edited());

    // Edited: the tree only; the edits stay and the status says what to do.
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    save_fit(2, nearest3());
    r.bridge->notify_changed();
    QVERIFY(w.busy());  // the tree
    QVERIFY(w.inputs());
    QVERIFY2(w.status().contains(QStringLiteral(" · level changed elsewhere, Reload to see it")), qPrintable(w.status()));
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(asked.isEmpty());
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
    QVERIFY(!fitted_at(*w.fit(), 3).used_in_fit);
    QVERIFY(w.monitors()->index(row2, FluxMonitorModel::SavedJ).data().toString().isEmpty());
    QVERIFY2(w.status().contains(QStringLiteral(" · level changed elsewhere, Reload to see it")), qPrintable(w.status()));
    QVERIFY2(w.status().endsWith(QStringLiteral(" · edited (not saved)")), qPrintable(w.status()));
    // It survives a refit.
    w.set_in_fit(4, false);
    QVERIFY(settle(w));
    QVERIFY2(w.status().contains(QStringLiteral(" · level changed elsewhere, Reload to see it")), qPrintable(w.status()));

    // Reload (the edits discarded) shows it.
    w.reload();
    QCOMPARE(asked.size(), 1);
    QVERIFY(settle(w));
    QVERIFY(!w.monitors()->index(row2, FluxMonitorModel::SavedJ).data().toString().isEmpty());
    QVERIFY(!w.status().contains(QStringLiteral("changed elsewhere")));
    QVERIFY(!w.edited());
  }

  // ---- Saving -------------------------------------------------------------------

  void save_writes_and_says_so() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(w.save_action()->isEnabled());
    QVERIFY(w.export_action()->isEnabled());
    std::map<int, double> predicted;
    for (const auto& p : w.fit()->positions) predicted[p.hole] = p.j;
    QCOMPARE(predicted.size(), std::size_t{12});
    QSignalSpy saved(&w, &FluxWindow::saved);
    QSignalSpy changed(r.bridge.get(), &EntryBridge::changed);
    const int loads = w.loads_started();
    QCOMPARE(loads, 1);
    const ps::ChangeSeq before = change_seq();

    w.save();
    QVERIFY(w.busy());
    QCOMPARE(w.status(), QStringLiteral("Saving…"));
    QVERIFY(!w.status_is_error());
    // The tool bar waits for it.
    for (QAction* action : w.findChild<QToolBar*>(QStringLiteral("flux_toolbar"))->actions())
      QVERIFY2(!action->isEnabled(), qPrintable(action->text()));
    QCOMPARE(w.save_action()->toolTip(), QStringLiteral("Saving…"));
    QVERIFY(!w.monitor_table()->isEnabled());
    w.save();  // again while it runs: nothing more
    QVERIFY(settle(w));

    QCOMPARE(w.status(), QStringLiteral("Saved 12 positions (0 unchanged)"));
    QVERIFY(!w.status_is_error());
    QVERIFY(change_seq() > before);
    for (int hole = 1; hole <= 12; ++hole) {
      QCOMPARE(head_changeset(hole), head_changeset(1));  // one changeset, and each hole written once
      const auto value = head_value(hole);
      QVERIFY2(value.has_value(), qPrintable(QString::number(hole)));
      QVERIFY(value->j.has_value());
      QCOMPARE(*value->j, predicted.at(hole));
      QVERIFY(value->options_json.has_value());
      QVERIFY2(QString::fromStdString(*value->options_json).contains(QStringLiteral("pychron-ui")),
               value->options_json->c_str());
    }
    // The level was read again, once: it shows what was saved, and nothing is pending.
    QCOMPARE(w.loads_started(), loads + 1);
    QCOMPARE(changed.count(), 1);  // the other windows were told; this one did not answer itself
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.at(0).at(0).toString(), QStringLiteral("NM-300"));
    QCOMPARE(saved.at(0).at(1).toString(), QStringLiteral("A"));
    QVERIFY(w.inputs());
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(!w.edited());
    QVERIFY(!w.monitors()->index(0, FluxMonitorModel::SavedJ).data().toString().isEmpty());
    QCOMPARE(w.preset_bar()->combo()->currentText(), QStringLiteral("(saved fit)"));
    QVERIFY(w.save_action()->isEnabled());
    QVERIFY(w.monitor_table()->isEnabled());
    auto* item = level_item(w, QStringLiteral("NM-300"), QStringLiteral("A"));
    QVERIFY(item);
    QCOMPARE(item->text(1), QStringLiteral("fitted"));
    QCOMPARE(w.tree()->currentItem(), item);

    // The outcome stands until the level is fitted again.
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    QVERIFY2(w.status().startsWith(QStringLiteral("plane, unweighted · fit MSWD")), qPrintable(w.status()));
  }

  void a_second_save_has_nothing_to_write() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    w.save();
    QVERIFY(settle(w));
    QCOMPARE(w.status(), QStringLiteral("Saved 12 positions (0 unchanged)"));

    QSignalSpy saved(&w, &FluxWindow::saved);
    QSignalSpy changed(r.bridge.get(), &EntryBridge::changed);
    const ps::ChangeSeq before = change_seq();
    const auto before_heads = heads();
    const int loads = w.loads_started();
    w.save_action()->trigger();
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    drain(*r.bridge);
    QCOMPARE(w.status(), QStringLiteral("Nothing to save: 12 positions unchanged"));
    QVERIFY(!w.status_is_error());
    QCOMPARE(change_seq(), before);
    QCOMPARE(heads(), before_heads);
    QCOMPARE(w.loads_started(), loads);  // nothing changed: nothing is read again
    QCOMPARE(changed.count(), 0);
    QCOMPARE(saved.count(), 0);
    QVERIFY(w.fit());
    QVERIFY(w.save_action()->isEnabled());

    // With a box unticked the count says so.
    w.set_save(9, false);
    w.save();
    QVERIFY(settle(w));
    QCOMPARE(w.status(),
             QStringLiteral("Nothing to save: 11 positions unchanged, 1 not saved · edited (not saved)"));
    QCOMPARE(change_seq(), before);
  }

  void an_unticked_save_box_leaves_its_head() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    w.save();
    QVERIFY(settle(w));
    const auto before = heads();
    QVERIFY(before.at(9).has_value());

    w.set_in_fit(3, false);  // another J everywhere
    w.set_save(9, false);
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Unchecked);
    QVERIFY(settle(w));
    w.save();
    QVERIFY(settle(w));
    QVERIFY2(w.status().contains(QStringLiteral("1 not saved")), qPrintable(w.status()));
    QCOMPARE(w.status(), QStringLiteral("Saved 11 positions (0 unchanged), 1 not saved"));
    const auto after = heads();
    QCOMPARE(after.at(9), before.at(9));
    for (int hole = 1; hole <= 12; ++hole)
      if (hole != 9) QVERIFY2(after.at(hole) != before.at(hole), qPrintable(QString::number(hole)));
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));

    // R11: read again after that save, the box is still unticked, and that is
    // no edit: nothing differs from what was saved but the choice.
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Unchecked);
    QCOMPARE(check(w.unknowns(), 1, pychron::ui::FluxUnknownModel::Save), Qt::Checked);
    QVERIFY(!w.edited());
    // So a second save leaves hole 9's head alone again.
    w.set_in_fit(3, true);
    w.save();
    QVERIFY(settle(w));
    QCOMPARE(w.status(), QStringLiteral("Saved 11 positions (0 unchanged), 1 not saved"));
    QCOMPARE(heads().at(9), before.at(9));
    QVERIFY(heads().at(10) != after.at(10));
    QCOMPARE(saved_excluded(3), std::optional<bool>(false));
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Unchecked);
    QVERIFY(!w.edited());
    // Ticked back, it is an edit until saved.
    w.set_save(9, true);
    QVERIFY(w.edited());
    w.set_save(9, false);
    QVERIFY(!w.edited());

    // A manual Reload, Revert and another level tick every box again.
    w.reload();
    QVERIFY(settle(w));
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Checked);
    QVERIFY(!w.edited());
    w.set_save(9, false);
    w.set_in_fit(3, false);
    w.save();
    QVERIFY(settle(w));
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Unchecked);
    w.revert();
    QCOMPARE(check(w.unknowns(), 0, pychron::ui::FluxUnknownModel::Save), Qt::Checked);
    QVERIFY(!w.edited());
  }

  void save_commits_what_was_typed_before_it_fits() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(w.options_editor()->apply(QStringLiteral("model.kind"), std::string("nearest")));
    QVERIFY(settle(w));
    QCOMPARE(w.options().fit.n_neighbors, 2);
    auto* spin = qobject_cast<QSpinBox*>(w.options_editor()->editor(QStringLiteral("model.neighbors")));
    QVERIFY(spin);
    auto* text = spin->findChild<QLineEdit*>();
    QVERIFY(text);

    // Typed and not yet taken (as a field that commits when it is left).
    spin->setKeyboardTracking(false);
    text->selectAll();
    QTest::keyClicks(text, QStringLiteral("3"));
    QCOMPARE(text->text(), QStringLiteral("3"));
    QCOMPARE(w.options().fit.n_neighbors, 2);
    QVERIFY(!w.busy());
    QCOMPARE(w.fit()->options.fit.n_neighbors, 2);

    w.save();
    // Committed and fitted before the fit was taken.
    QCOMPARE(w.options().fit.n_neighbors, 3);
    QVERIFY(w.fit());
    QCOMPARE(w.fit()->options.fit.n_neighbors, 3);
    QVERIFY(settle(w));
    QVERIFY2(w.status().startsWith(QStringLiteral("Saved 12 positions")), qPrintable(w.status()));
    for (int hole : {1, 9}) {
      const auto value = head_value(hole);
      QVERIFY(value.has_value() && value->options_json.has_value());
      const auto doc = pp::parse_flux_options(*value->options_json);
      QVERIFY(doc.options.has_value());
      QCOMPARE(doc.options->fit.kind, reduction::ModelKind::NearestNeighbors);
      QCOMPARE(doc.options->fit.n_neighbors, 3);
    }
    // And the reload did not take it back.
    QCOMPARE(w.options().fit.n_neighbors, 3);
    QCOMPARE(w.options_editor()->options().get_int("model.neighbors"), std::int64_t{3});
    QVERIFY(!w.edited());

    // Typed with the box taking each key: the refit is pending, and saved all the same.
    spin = qobject_cast<QSpinBox*>(w.options_editor()->editor(QStringLiteral("model.neighbors")));
    QVERIFY(spin);
    text = spin->findChild<QLineEdit*>();
    text->selectAll();
    QTest::keyClicks(text, QStringLiteral("4"));
    QVERIFY(w.busy());
    w.save();
    QVERIFY(settle(w));
    QCOMPARE(pp::parse_flux_options(*head_value(1)->options_json).options->fit.n_neighbors, 4);
    QCOMPARE(w.options().fit.n_neighbors, 4);
  }

  void a_commit_that_cannot_fit_starts_no_save() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    const ps::ChangeSeq before = change_seq();
    QSignalSpy saved(&w, &FluxWindow::saved);

    // Options that are no fit's (sd with a plane): no job, and the error is said.
    QVERIFY(w.options_editor()->apply(QStringLiteral("fit.error"), std::string("sd")));
    w.save();
    QVERIFY(!w.busy());
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("sd is not an error kind of a fitted surface")), qPrintable(w.status()));
    QVERIFY(!w.save_action()->isEnabled());
    QVERIFY2(w.save_action()->toolTip().contains(QStringLiteral("sd is not an error kind")),
             qPrintable(w.save_action()->toolTip()));
    QVERIFY(w.monitor_table()->isEnabled());  // nothing waits for a save that did not start
    QVERIFY(w.options_editor()->isEnabled());
    drain(*r.bridge);
    QCOMPARE(change_seq(), before);
    QVERIFY(w.options_editor()->apply(QStringLiteral("fit.error"), std::string("msem")));
    QVERIFY(settle(w));
    QVERIFY(w.fit());

    // A sample typed and not asked for is other monitors than the fit's: Save
    // says so and saves nothing, and the text stays.
    w.sample_edit()->setText(QStringLiteral("unk"));
    QVERIFY(w.edited());
    w.save();
    QVERIFY(!w.busy());
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().startsWith(QStringLiteral("Not saved: the monitor selection changed, fit again")),
             qPrintable(w.status()));
    QCOMPARE(w.sample_edit()->text(), QStringLiteral("unk"));
    QVERIFY(w.sample_edit()->isEnabled());
    drain(*r.bridge);
    QCOMPARE(change_seq(), before);
    QCOMPARE(saved.count(), 0);
    // Taken back, the fit on show is saved.
    w.sample_edit()->clear();
    QVERIFY(!w.edited());
    w.save();
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    QCOMPARE(w.status(), QStringLiteral("Saved 12 positions (0 unchanged)"));
    QCOMPARE(saved.count(), 1);
  }

  void a_change_or_a_reload_during_a_save_still_says_saved() {
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Discard;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QSignalSpy saved(&w, &FluxWindow::saved);
    int loads = w.loads_started();

    // Another window's change while the save runs: the save reads the level again, once.
    w.save();
    r.bridge->notify_changed();
    QCOMPARE(w.loads_started(), loads);
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(settle(w));
    QCOMPARE(w.status(), QStringLiteral("Saved 12 positions (0 unchanged)"));
    QCOMPARE(saved.count(), 1);
    QCOMPARE(w.loads_started(), loads + 1);
    QVERIFY(w.fit());
    QVERIFY(!w.monitors()->index(0, FluxMonitorModel::SavedJ).data().toString().isEmpty());

    // With nothing to write the level is not read, and the status says there is something to see.
    loads = w.loads_started();
    w.save();
    r.bridge->notify_changed();
    QVERIFY(settle(w));
    drain(*r.bridge);
    QCOMPARE(w.status(), QStringLiteral("Nothing to save: 12 positions unchanged · level changed elsewhere, Reload to see it"));
    QCOMPARE(w.loads_started(), loads);
    QCOMPARE(saved.count(), 1);
    w.reload();
    QVERIFY(settle(w));

    // The level asked for again while its save runs: that read shows what was
    // saved, nothing is read a second time, and the outcome is said.
    w.set_in_fit(3, false);
    loads = w.loads_started();
    w.save();
    w.reload();  // the edits are being saved; Discard lets it through
    QCOMPARE(asked.size(), 1);
    QCOMPARE(w.status(), QStringLiteral("Loading NM-300 A…"));
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(settle(w));
    QVERIFY2(w.status().startsWith(QStringLiteral("Saved 12 positions")), qPrintable(w.status()));
    QVERIFY(!w.status_is_error());
    QCOMPARE(saved.count(), 2);
    QCOMPARE(w.loads_started(), loads + 1);
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));
    QVERIFY(fitted_at(*w.fit(), 3).excluded);
    QVERIFY(!w.edited());
    QVERIFY(w.save_action()->isEnabled());

    // And a conflict the same: said once the level is on show again, in the error tone.
    w.set_in_fit(3, true);
    QVERIFY(settle(w));
    save_fit(7, pp::FluxOptions{}, false, 2.0e-3);
    w.save();
    w.reload();
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(settle(w));
    QVERIFY2(w.status().startsWith(QStringLiteral("Not saved: hole 7 was saved by jsmith at ")), qPrintable(w.status()));
    QVERIFY(w.status_is_error());
    QCOMPARE(saved.count(), 2);
  }

  void closing_during_a_save_waits_for_it() {
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Save;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    w.show();
    w.set_in_fit(3, false);
    w.save();
    QVERIFY(w.busy());
    QVERIFY(!w.close());  // asked, and Save: the save in flight is that save
    QCOMPARE(asked.size(), 1);
    QVERIFY(w.isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!w.isVisible(), kWaitMs);
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 1);
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));
    QVERIFY(!w.edited());

    // A save in flight that fails keeps the window open.
    w.show();
    w.set_in_fit(3, true);
    QVERIFY(settle(w));
    save_fit(7, pp::FluxOptions{}, false, 2.0e-3);
    w.save();
    QVERIFY(!w.close());
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(w.isVisible());
    QVERIFY(w.status_is_error());
    QVERIFY(w.edited());
  }

  void save_outcomes_are_said_verbatim() {
    pp::FluxSaveOutcome outcome;
    outcome.written = 12;
    QCOMPARE(FluxWindow::save_text(outcome, "", ""), QStringLiteral("Saved 12 positions (0 unchanged)"));
    outcome.written = 7;
    outcome.unchanged = 3;
    outcome.skipped = 2;
    QCOMPARE(FluxWindow::save_text(outcome, "", ""), QStringLiteral("Saved 7 positions (3 unchanged), 2 not saved"));
    outcome.written = 0;
    outcome.unchanged = 12;
    outcome.skipped = 0;
    QCOMPARE(FluxWindow::save_text(outcome, "", ""), QStringLiteral("Nothing to save: 12 positions unchanged"));
    outcome.unchanged = 11;
    outcome.skipped = 1;
    QCOMPARE(FluxWindow::save_text(outcome, "", ""),
             QStringLiteral("Nothing to save: 11 positions unchanged, 1 not saved"));

    // A conflict: nothing was written, whatever the counts say.
    outcome.conflict = ps::Conflict{};
    outcome.conflict_position = "hole 7";
    outcome.conflict_hole = 7;
    QCOMPARE(FluxWindow::save_text(outcome, "jsmith", "2026-10-07 14:02:11"),
             QStringLiteral("Not saved: hole 7 was saved by jsmith at 2026-10-07 14:02:11 UTC since this level was "
                            "loaded. Reload and fit again."));
    // The head could not be read back: who and when are not known.
    QCOMPARE(FluxWindow::save_text(outcome, "", ""),
             QStringLiteral("Not saved: hole 7 was saved by someone else since this level was loaded. Reload and fit "
                            "again."));
    QCOMPARE(FluxWindow::save_text(outcome, "jsmith", ""),
             QStringLiteral("Not saved: hole 7 was saved by jsmith since this level was loaded. Reload and fit again."));
    QCOMPARE(FluxWindow::save_text(outcome, "", "2026-10-07 14:02:11"),
             QStringLiteral("Not saved: hole 7 was saved by someone else at 2026-10-07 14:02:11 UTC since this level "
                            "was loaded. Reload and fit again."));

    // Any other failure is the store's own words.
    QCOMPARE(FluxWindow::save_error_text("flux: hole 3 of NM-300A has J nan: nothing was saved"),
             QStringLiteral("Not saved: flux: hole 3 of NM-300A has J nan: nothing was saved"));
  }

  void save_inside_the_debounce_saves_what_is_shown() {
    auto r = rig();
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QVERIFY(fitted_at(*w.fit(), 3).used_in_fit);
    w.set_in_fit(3, false);
    QVERIFY(w.busy());                            // the refit is still to come
    QVERIFY(fitted_at(*w.fit(), 3).used_in_fit);  // the fit on show is the old one
    QVERIFY(w.save_action()->isEnabled());
    w.save();  // no wait
    QVERIFY(w.fit());
    QVERIFY(fitted_at(*w.fit(), 3).excluded);  // fitted first
    const double j1 = fitted_at(*w.fit(), 1).j;
    QVERIFY(settle(w));
    QCOMPARE(w.status(), QStringLiteral("Saved 12 positions (0 unchanged)"));
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));
    QCOMPARE(saved_excluded(4), std::optional<bool>(false));
    QCOMPARE(*head_value(1)->j, j1);
    // Read again, the exclusion is the saved fit's and no edit.
    QVERIFY(fitted_at(*w.fit(), 3).excluded);
    QVERIFY(!w.edited());

    // A refit pending that cannot fit: nothing is saved, and the error is said.
    const ps::ChangeSeq before = change_seq();
    auto bowl = pp::to_options(pp::FluxOptions{});
    QVERIFY(bowl.set("model.kind", std::string("bowl")).has_value());
    w.set_options(bowl);
    QVERIFY(w.busy());
    w.save();
    QVERIFY(!w.busy());
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().contains(QStringLiteral("do not determine a bowl")), qPrintable(w.status()));
    drain(*r.bridge);
    QCOMPARE(change_seq(), before);
  }

  void a_conflict_writes_nothing_and_keeps_the_edits() {
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Discard;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    const pp::Edits edits = w.edits();

    save_fit(7, pp::FluxOptions{}, false, 2.0e-3);  // another client, after the level was loaded
    const auto before_heads = heads();
    QSignalSpy saved(&w, &FluxWindow::saved);
    QSignalSpy changed(r.bridge.get(), &EntryBridge::changed);
    const int loads = w.loads_started();

    w.save();
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY2(w.status().startsWith(QStringLiteral("Not saved: hole 7 was saved by")), qPrintable(w.status()));
    static const QRegularExpression line(QStringLiteral(
        "^Not saved: hole 7 was saved by jsmith at \\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d UTC since this level was "
        "loaded\\. Reload and fit again\\. · edited \\(not saved\\)$"));
    QVERIFY2(line.match(w.status()).hasMatch(), qPrintable(w.status()));
    QVERIFY(w.status_is_error());
    QCOMPARE(heads(), before_heads);  // no head moved, and no hole got one
    QCOMPARE(w.edits().exclude_positions, edits.exclude_positions);
    QCOMPARE(w.edits().omit, edits.omit);
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
    QVERIFY(w.edited());
    QVERIFY(w.fit());
    QVERIFY(!fitted_at(*w.fit(), 3).used_in_fit);
    QCOMPARE(w.loads_started(), loads);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(saved.count(), 0);
    QVERIFY(w.save_action()->isEnabled());
    QVERIFY(w.monitor_table()->isEnabled());

    // Reload, fit again, and it saves.
    w.reload();
    QVERIFY(settle(w));
    QCOMPARE(w.monitors()->index(w.monitors()->row_of(7), FluxMonitorModel::SavedJ).data().toString(),
             QStringLiteral("2.0000e-03"));
    w.set_in_fit(3, false);
    w.save();
    QVERIFY(settle(w));
    QVERIFY2(w.status().startsWith(QStringLiteral("Saved ")), qPrintable(w.status()));
    QVERIFY(!w.status_is_error());
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));
  }

  void save_is_disabled_with_the_reason() {
    auto r = rig();
    FluxWindow w(*r.bridge, *r.source, r.presets);
    QVERIFY(!w.save_action()->isEnabled());
    QCOMPARE(w.save_action()->toolTip(), QStringLiteral("No level open"));
    QVERIFY(!w.export_action()->isEnabled());
    w.save();  // nothing to save: nothing happens
    QTRY_VERIFY_WITH_TIMEOUT(!w.busy(), kWaitMs);
    QCOMPARE(w.save_action()->toolTip(), QStringLiteral("No level open"));

    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QVERIFY(!w.save_action()->isEnabled());
    QCOMPARE(w.save_action()->toolTip(), QStringLiteral("Loading…"));
    QVERIFY(settle(w));
    QVERIFY(w.save_action()->isEnabled());
    QVERIFY(!w.save_action()->toolTip().isEmpty());
    QVERIFY(w.export_action()->isEnabled());

    // A fit that failed: the error is the reason.
    auto bowl = pp::to_options(pp::FluxOptions{});
    QVERIFY(bowl.set("model.kind", std::string("bowl")).has_value());
    w.set_options(bowl);
    QVERIFY(settle(w));
    QCOMPARE(w.fit(), nullptr);
    QVERIFY(!w.save_action()->isEnabled());
    QVERIFY2(w.save_action()->toolTip().contains(QStringLiteral("do not determine a bowl")),
             qPrintable(w.save_action()->toolTip()));
    QVERIFY(!w.export_action()->isEnabled());
    const ps::ChangeSeq before = change_seq();
    w.save();
    QVERIFY(!w.busy());
    QVERIFY(w.status_is_error());
    drain(*r.bridge);
    QCOMPARE(change_seq(), before);

    QVERIFY(w.options_editor()->apply(QStringLiteral("model.kind"), std::string("plane")));
    QVERIFY(settle(w));
    QVERIFY(w.save_action()->isEnabled());

    // A level that cannot be read leaves none open.
    w.set_ask_unsaved([](const QString&) { return FluxWindow::Unsaved::Discard; });
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("Z"));
    QVERIFY(settle(w));
    QVERIFY(!w.save_action()->isEnabled());
    QCOMPARE(w.save_action()->toolTip(), QStringLiteral("No level open"));
  }

  void the_unsaved_question_can_save_then_move() {
    auto second = pt::seed_second_flux_level(*store_, actor_, seeded_, "C");
    QVERIFY2(second.has_value(), second ? "" : to_string(second.error()).c_str());
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Save;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    QSignalSpy saved(&w, &FluxWindow::saved);
    w.set_in_fit(3, false);
    QVERIFY(settle(w));

    w.tree()->setCurrentItem(level_item(w, QStringLiteral("NM-300"), QStringLiteral("C")));
    QCOMPARE(asked, QStringList{QStringLiteral("Save the flux of NM-300 A?")});
    QVERIFY(w.busy());
    QCOMPARE(w.status(), QStringLiteral("Saving…"));
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->level, std::string("A"));  // until it is saved
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 1);
    // A was saved, with the edit.
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));
    for (int hole = 1; hole <= 12; ++hole) QVERIFY(head(hole).has_value());
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.at(0).at(1).toString(), QStringLiteral("A"));
    // And the window is on C, with nothing pending and C's own status.
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->level, std::string("C"));
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 C"));
    QCOMPARE(w.tree()->currentItem(), level_item(w, QStringLiteral("NM-300"), QStringLiteral("C")));
    QCOMPARE(level_item(w, QStringLiteral("NM-300"), QStringLiteral("A"))->text(1), QStringLiteral("fitted"));
    QVERIFY(!w.edited());
    QVERIFY(!w.status_is_error());
    QVERIFY2(w.status().startsWith(QStringLiteral("plane, unweighted · fit MSWD")), qPrintable(w.status()));
    // What became of A is said after it, in the label, until the next change.
    QVERIFY2(w.status().endsWith(QStringLiteral(" · Saved 12 positions (0 unchanged)")), qPrintable(w.status()));
    QVERIFY(w.statusBar()->currentMessage().isEmpty());
    QVERIFY(!head(1, "C").has_value());

    // Reload with Save: saved, then read again.
    w.set_in_fit(2, false);
    QVERIFY(settle(w));
    const int loads = w.loads_started();
    w.reload();
    QCOMPARE(asked.size(), 2);
    QVERIFY(settle(w));
    QCOMPARE(saved_excluded(2, "C"), std::optional<bool>(true));
    QCOMPARE(w.loads_started(), loads + 1);
    QVERIFY(!w.edited());
    QVERIFY(fitted_at(*w.fit(), 2).excluded);

    // Closing with Save: stays until it is saved, then closes.
    w.set_in_fit(2, true);
    QVERIFY(settle(w));
    QVERIFY(w.edited());
    w.show();
    QVERIFY(!w.close());
    QCOMPARE(asked.size(), 3);
    QVERIFY(w.isVisible());
    QVERIFY(w.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!w.isVisible(), kWaitMs);
    QVERIFY(settle(w));
    QCOMPARE(asked.size(), 3);
    QCOMPARE(saved_excluded(2, "C"), std::optional<bool>(false));
    QCOMPARE(saved.count(), 3);
  }

  void a_failed_save_cancels_the_move() {
    auto second = pt::seed_second_flux_level(*store_, actor_, seeded_, "C");
    QVERIFY2(second.has_value(), second ? "" : to_string(second.error()).c_str());
    auto r = rig();
    QStringList asked;
    auto answer = FluxWindow::Unsaved::Save;
    auto wp = opened(r, &asked, &answer);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    w.set_in_fit(3, false);
    QVERIFY(settle(w));
    save_fit(7, pp::FluxOptions{}, false, 2.0e-3);  // someone else, meanwhile
    const auto before_heads = heads();
    const int loads = w.loads_started();

    w.tree()->setCurrentItem(level_item(w, QStringLiteral("NM-300"), QStringLiteral("C")));
    QCOMPARE(asked.size(), 1);
    QVERIFY(w.busy());
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(w.status_is_error());
    QVERIFY2(w.status().startsWith(QStringLiteral("Not saved: hole 7 was saved by jsmith at ")), qPrintable(w.status()));
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->level, std::string("A"));
    QCOMPARE(w.windowTitle(), QStringLiteral("Flux — NM-300 A"));
    QCOMPARE(w.tree()->currentItem(), level_item(w, QStringLiteral("NM-300"), QStringLiteral("A")));
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
    QCOMPARE(w.loads_started(), loads);
    QCOMPARE(heads(), before_heads);
    QCOMPARE(asked.size(), 1);

    // Closing the same: it stays open.
    w.show();
    QVERIFY(!w.close());
    QCOMPARE(asked.size(), 2);
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(w.isVisible());
    QVERIFY(w.status_is_error());
    QCOMPARE(w.edits().exclude_positions, std::set<int>{3});
  }

  void export_csv_writes_the_level() {
    auto r = rig();
    const QString path = dir_.filePath(QStringLiteral("export/flux.csv"));
    QVERIFY(QDir(dir_.path()).mkpath(QStringLiteral("export")));
    QFile::remove(path);
    {
      // Nothing fitted: nothing to write.
      FluxWindow none(*r.bridge, *r.source, r.presets);
      auto written = none.export_csv(path);
      QVERIFY(!written.has_value());
      QCOMPARE(QString::fromStdString(written.error().what), QStringLiteral("Nothing fitted to export"));
      QVERIFY(!QFile::exists(path));
      QTRY_VERIFY_WITH_TIMEOUT(!none.busy(), kWaitMs);
    }
    auto wp = opened(r);
    FluxWindow& w = *wp;
    QVERIFY2(w.fit(), qPrintable(w.status()));
    const auto read = [&] {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QString();
      return QString::fromUtf8(file.readAll());
    };
    auto written = w.export_csv(path);
    QVERIFY2(written.has_value(), written ? "" : to_string(written.error()).c_str());
    QString text = read();
    QCOMPARE(text, QString::fromStdString(pp::flux_csv_header() + pp::flux_csv_rows(*w.fit())));
    QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 13);  // the header and the twelve positions
    QCOMPARE(lines.at(0) + QLatin1Char('\n'), QString::fromStdString(pp::flux_csv_header()));
    // Only the file is left beside it.
    QCOMPARE(QDir(dir_.filePath(QStringLiteral("export"))).entryList(QDir::Files | QDir::Hidden),
             QStringList{QStringLiteral("flux.csv")});

    // Over an earlier file, and what is on screen: a refit pending is fitted first.
    w.set_in_fit(3, false);
    written = w.export_csv(path);
    QVERIFY2(written.has_value(), written ? "" : to_string(written.error()).c_str());
    QVERIFY(fitted_at(*w.fit(), 3).excluded);
    QVERIFY(read() != text);
    QCOMPARE(read(), QString::fromStdString(pp::flux_csv_header() + pp::flux_csv_rows(*w.fit())));

    // A place that cannot be written is an error, and nothing is made.
    const QString nowhere = dir_.filePath(QStringLiteral("no-such-directory/flux.csv"));
    written = w.export_csv(nowhere);
    QVERIFY(!written.has_value());
    QCOMPARE(written.error().kind, ErrorKind::Io);
    QVERIFY2(QString::fromStdString(written.error().what).contains(nowhere), written.error().what.c_str());
    QVERIFY(!QFile::exists(nowhere));
  }

  void closing_while_saving_does_not_crash() {
    auto second = pt::seed_second_flux_level(*store_, actor_, seeded_, "C");
    QVERIFY2(second.has_value(), second ? "" : to_string(second.error()).c_str());
    auto r = rig();
    QSignalSpy changed(r.bridge.get(), &EntryBridge::changed);
    {
      auto w = opened(r);
      QVERIFY2(w->fit(), qPrintable(w->status()));
      w->save();
      QVERIFY(w->busy());
    }  // destroyed while the save runs
    drain(*r.bridge);
    for (int hole = 1; hole <= 12; ++hole) QVERIFY(head(hole).has_value());
    QCOMPARE(changed.count(), 1);  // it was written: the other windows are told all the same

    // The user moved to another level while the save ran: the result is not that level's.
    auto wp = opened(r);
    FluxWindow& w = *wp;
    w.set_ask_unsaved([](const QString&) { return FluxWindow::Unsaved::Discard; });
    QSignalSpy saved(&w, &FluxWindow::saved);
    w.set_in_fit(3, false);
    w.save();
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("C"));
    QCOMPARE(w.status(), QStringLiteral("Loading NM-300 C…"));
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(settle(w));
    QCOMPARE(saved_excluded(3), std::optional<bool>(true));  // A was saved
    QCOMPARE(changed.count(), 2);
    QVERIFY(w.inputs());
    QCOMPARE(w.inputs()->level, std::string("C"));
    QVERIFY(!w.status_is_error());
    QVERIFY2(w.status().startsWith(QStringLiteral("plane, unweighted · fit MSWD")), qPrintable(w.status()));
    QVERIFY(!w.edited());
    QVERIFY(w.edits().exclude_positions.empty());
    QCOMPARE(w.loads_started(), 2);  // A, then C: the dropped result read nothing again
    QVERIFY(w.save_action()->isEnabled());
    QVERIFY(w.monitor_table()->isEnabled());
    QCOMPARE(level_item(w, QStringLiteral("NM-300"), QStringLiteral("A"))->text(1), QStringLiteral("fitted"));
    QVERIFY(!head(1, "C").has_value());

    // The same with a conflict: C's status is not told of A's.
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QVERIFY(settle(w));
    w.set_in_fit(3, true);
    QVERIFY(settle(w));
    save_fit(7, pp::FluxOptions{}, false, 2.0e-3);
    w.save();
    w.open_level(QStringLiteral("NM-300"), QStringLiteral("C"));
    QVERIFY(settle(w));
    drain(*r.bridge);
    QVERIFY(!w.status_is_error());
    QVERIFY2(!w.status().contains(QStringLiteral("Not saved")), qPrintable(w.status()));
    QCOMPARE(w.inputs()->level, std::string("C"));
    QCOMPARE(changed.count(), 2);
  }

  void the_menu_action_opens_the_window() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    Owners o(url_, dir_.filePath(QStringLiteral("presets")));
    o.main.show();
    QCOMPARE(o.fit->flux_action()->text(), QStringLiteral("Flux…"));
    QStringList commands;
    for (const MenuHub::Command& c : MenuHub::instance().commands())
      commands << pychron::ui::CommandPalette::label(c.action, MenuHub::title(c.menu));
    QVERIFY2(commands.contains(QStringLiteral("Fit › Flux")), qPrintable(commands.join(QStringLiteral(", "))));
    // The bar shows Fit, after Entry.
    QStringList titles;
    for (const QAction* a : MenuHub::instance().bar_for(&o.main)->actions())
      if (a->isVisible()) titles << a->text().remove(QLatin1Char('&'));
    QVERIFY2(titles.indexOf(QStringLiteral("Fit")) == titles.indexOf(QStringLiteral("Entry")) + 1,
             qPrintable(titles.join(QStringLiteral(", "))));
    QVERIFY(titles.contains(QStringLiteral("Entry")));

    QVERIFY(o.entry->bridge() == nullptr);  // nothing is opened until asked
    o.fit->flux_action()->trigger();
    FluxWindow* w = o.fit->flux();
    QVERIFY(w != nullptr);
    QVERIFY(w->isVisible());
    QVERIFY(w->isWindow());
    QCOMPARE(w->parentWidget(), &o.main);
    QVERIFY(o.entry->bridge() != nullptr);  // the bridge is Entry's
    QVERIFY(o.errors.isEmpty());
    QVERIFY(settle(*w));
    QVERIFY(level_item(*w, QStringLiteral("NM-300"), QStringLiteral("A")) != nullptr);
    // Made once and kept.
    o.fit->flux_action()->trigger();
    QCOMPARE(o.fit->flux(), w);
    // Before a level is open there is nothing to show in Packages.
    QVERIFY(!w->packages_action()->isEnabled());
  }

  void packages_opens_the_flux_window_on_its_level() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    Owners o(url_, dir_.filePath(QStringLiteral("presets")));
    PackagesWindow* p = o.entry->packages();
    QVERIFY(p != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!p->busy(), kWaitMs);
    QVERIFY(!p->fit_flux_action()->isEnabled());
    p->show_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QTRY_VERIFY_WITH_TIMEOUT(!p->busy() && p->grid()->has_edit(), kWaitMs);
    QVERIFY(p->fit_flux_action()->isEnabled());

    p->fit_flux_action()->trigger();
    FluxWindow* w = o.fit->flux();
    QVERIFY(w != nullptr);
    QVERIFY(w->isVisible());
    QVERIFY(settle(*w));
    QCOMPARE(w->windowTitle(), QStringLiteral("Flux — NM-300 A"));
    QVERIFY2(w->fit(), qPrintable(w->status()));
    QCOMPARE(w->loads_started(), 1);
    QCOMPARE(w->tree()->currentItem(), level_item(*w, QStringLiteral("NM-300"), QStringLiteral("A")));
  }

  void packages_sees_the_new_j_after_a_save() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    Owners o(url_, dir_.filePath(QStringLiteral("presets")));
    PackagesWindow* p = o.entry->packages();
    QVERIFY(p != nullptr);
    p->show_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    QTRY_VERIFY_WITH_TIMEOUT(!p->busy() && p->grid()->has_edit(), kWaitMs);
    using Grid = pychron::ui::LevelGridModel;
    const auto j_of = [p](int hole) -> std::optional<QString> {
      for (int row = 0; row < p->grid()->rowCount(); ++row)
        if (p->grid()->index(row, Grid::Position).data().toInt() == hole)
          return p->grid()->index(row, Grid::J).data().toString();
      return std::nullopt;
    };
    QCOMPARE(j_of(9), std::optional<QString>(QString()));  // nothing saved yet

    p->fit_flux_action()->trigger();
    FluxWindow* w = o.fit->flux();
    QVERIFY(w != nullptr);
    QVERIFY(settle(*w));
    QVERIFY2(w->fit(), qPrintable(w->status()));
    const double predicted = fitted_at(*w->fit(), 9).j;
    QVERIFY(predicted > 0.0);
    w->save();
    QVERIFY(settle(*w));
    QCOMPARE(w->status(), QStringLiteral("Saved 12 positions (0 unchanged)"));

    // The Packages window was told through the bridge, and read the level again.
    QTRY_VERIFY_WITH_TIMEOUT(!p->busy() && p->grid()->has_edit(), kWaitMs);
    QCOMPARE(j_of(9), std::optional<QString>(QString::number(predicted, 'E', 6)));
    QCOMPARE(p->windowTitle(), QStringLiteral("Packages — NM-300 A"));
  }

  void open_in_packages_shows_the_level() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    Owners o(url_, dir_.filePath(QStringLiteral("presets")));
    o.fit->open_flux(QStringLiteral("NM-300"), QStringLiteral("A"));
    FluxWindow* w = o.fit->flux();
    QVERIFY(w != nullptr);
    QVERIFY(w->isVisible());
    QVERIFY(settle(*w));
    QAction* open = w->packages_action();
    QCOMPARE(open->text(), QStringLiteral("Open in Packages"));
    QCOMPARE(w->findChild<QToolBar*>(QStringLiteral("flux_toolbar"))->actions().constLast(), open);  // the last one
    QVERIFY(open->isEnabled());
    QSignalSpy asked(w, &FluxWindow::packages_requested);

    open->trigger();
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.at(0).at(0).toString(), QStringLiteral("NM-300"));
    QCOMPARE(asked.at(0).at(1).toString(), QStringLiteral("A"));
    PackagesWindow* p = o.entry->packages();
    QVERIFY(p != nullptr);
    QVERIFY(p->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!p->busy() && p->grid()->has_edit(), kWaitMs);
    QCOMPARE(p->windowTitle(), QStringLiteral("Packages — NM-300 A"));
    QVERIFY(p->tree()->currentItem() != nullptr);
    QCOMPARE(p->tree()->currentItem()->text(0), QStringLiteral("A"));
    QCOMPARE(p->tree()->currentItem()->parent()->text(0), QStringLiteral("NM-300"));
    QVERIFY(p->fit_flux_action()->isEnabled());
  }

  void recall_reaches_the_callback() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    {
      Owners o(url_, dir_.filePath(QStringLiteral("presets")));
      FluxWindow* w = o.fit->flux();
      QVERIFY(w != nullptr);
      Q_EMIT w->view()->recall_requested(QStringLiteral("an-analysis"));
      QCOMPARE(o.recalled, QStringList{QStringLiteral("an-analysis")});
      QVERIFY(settle(*w));
    }
    // Without one, Recall does nothing.
    auto r = rig();
    {
      FluxWindow w(*r.bridge, *r.source, r.presets);
      Q_EMIT w.view()->recall_requested(QStringLiteral("an-analysis"));
      QVERIFY(settle(w));
    }
    drain(*r.bridge);
  }

  void a_store_that_cannot_open_gives_no_window() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    const std::string missing = "sqlite:" + dir_.filePath(QStringLiteral("nowhere/missing.db")).toStdString();
    Owners o(missing, dir_.filePath(QStringLiteral("presets")));
    QVERIFY(o.fit->flux() == nullptr);
    QCOMPARE(o.errors.size(), 1);
    QVERIFY2(o.errors.front().startsWith(QStringLiteral("The store could not be opened:\n")), qPrintable(o.errors.front()));
    QVERIFY(o.errors.front().size() > QStringLiteral("The store could not be opened:\n").size());
    QVERIFY(o.entry->bridge() == nullptr);  // Entry was not asked: one message, not two
    // Asked again, said again; and the action shows nothing.
    o.fit->flux_action()->trigger();
    QCOMPARE(o.errors.size(), 2);
    o.fit->open_flux(QStringLiteral("NM-300"), QStringLiteral("A"));
    QCOMPARE(o.errors.size(), 3);
    QVERIFY(o.fit->flux() == nullptr);
  }

  void owners_torn_down_in_either_order_with_a_load_in_flight() {
    MenuHub::reset(MenuHub::Bars::PerWindow);
    for (const bool entry_first : {true, false}) {
      pp::PresetStore presets(dir_.filePath(QStringLiteral("presets")).toStdString());
      QMainWindow main;
      auto* entry = new EntryActions(&main, url_);
      auto* fit = new FitActions(&main, url_, *entry, presets);
      fit->open_flux(QStringLiteral("NM-300"), QStringLiteral("A"));
      const QPointer<FluxWindow> w = fit->flux();
      QVERIFY(w != nullptr);
      QVERIFY(w->busy());  // the tree and the level are being read through the source
      if (entry_first) {
        // As the main window deletes them: the window goes before the bridge.
        delete entry;
        QVERIFY(w == nullptr);
        QVERIFY(fit->flux() == nullptr);  // and none is made without Entry
        fit->open_flux(QStringLiteral("NM-300"), QStringLiteral("A"));
        delete fit;
      } else {
        delete fit;  // waits for the worker before its source goes
        QVERIFY(w == nullptr);
        QVERIFY(entry->bridge() != nullptr);
        delete entry;
      }
      QCoreApplication::processEvents();  // results of jobs whose window is gone
    }
    // The same with the main window deleting both.
    {
      Owners o(url_, dir_.filePath(QStringLiteral("presets")));
      o.fit->open_flux(QStringLiteral("NM-300"), QStringLiteral("A"));
      QVERIFY(o.fit->flux()->busy());
    }
    QCoreApplication::processEvents();
  }

  void closing_while_loading_does_not_crash() {
    auto r = rig();
    {
      auto* w = new FluxWindow(*r.bridge, *r.source, r.presets);
      w->open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
      QVERIFY(w->busy());
      delete w;  // the tree and the level are still being read
    }
    drain(*r.bridge);
    {
      // Closed between the load and the fit, and after a selection.
      auto w = std::make_unique<FluxWindow>(*r.bridge, *r.source, r.presets);
      w->open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
      QTRY_VERIFY_WITH_TIMEOUT(!w->busy(), kWaitMs);
      w->select_monitor(2);
      w->open_level(QStringLiteral("NM-300"), QStringLiteral("A"));
    }
    drain(*r.bridge);
  }
};

#endif

QTEST_MAIN(FluxWindowTest)
#include "test_flux_window.moc"
