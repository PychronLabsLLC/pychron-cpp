// The flux window (flux window design, sections 5.3 and 5.4) on a SQLite store
// holding the seeded level of flux_seed.hpp: the level tree and its status
// column, loading and fitting a level, the monitor selection, a level that
// cannot be loaded or fitted, a selection superseded while it loads, and a
// window closed while it loads.

#include <QtTest/QtTest>

#ifndef PYCHRON_UI_HAS_STORE

class FluxWindowTest : public QObject {
  Q_OBJECT
 private Q_SLOTS:
  void skipped() { QSKIP("built without the DVC store"); }
};

#else

#include <memory>
#include <string>
#include <variant>

#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "entry_bridge.hpp"
#include "flux_analysis_model.hpp"
#include "flux_monitor_model.hpp"
#include "flux_unknown_model.hpp"
#include "flux_window.hpp"
#include "pychron/processing/flux_view.hpp"
#include "pychron/processing/options.hpp"
#include "scene_view.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "flux_seed.hpp"
#include "pychron/processing/store_source.hpp"
#pragma pop_macro("signals")

using namespace pychron;
namespace ps = pychron::persistence;
namespace pp = pychron::processing;
namespace pt = pychron::processing::testing;
using pychron::ui::EntryBridge;
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

// What a window needs, in the order it must die in: the bridge (whose worker
// may still be reading through the source) before the source.
struct Rig {
  std::unique_ptr<pp::StoreSource> source;
  pp::PresetStore presets;
  std::unique_ptr<EntryBridge> bridge;
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
    FluxWindow w(*r.bridge, *r.source, r.presets);
    // Every level the monitors table ever shows, by its first identifier.
    QStringList shown;
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
