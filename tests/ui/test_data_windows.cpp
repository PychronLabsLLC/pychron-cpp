// Data browsing and visualization windows (design section 11) on an
// in-memory source: the browser filters and pages, recall shows every tab,
// the figure window computes on the bridge, a click on a point excludes the
// analysis and the statistics change, the options dock and presets drive the
// figure, and the main window tears the data windows down before the bridge.

#include <cmath>
#include <memory>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "data_browser_window.hpp"
#include "figure_window.hpp"
#include "main_window.hpp"
#include "options_editor.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/time_series.hpp"
#include "recall_window.hpp"
#include "scene_view.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
namespace pp = pychron::processing;
using pychron::ui::DataBrowserWindow;
using pychron::ui::FigureWindow;
using pychron::ui::OptionsEditor;
using pychron::ui::ProcessingBridge;
using pychron::ui::RecallWindow;

namespace {

constexpr int kWaitMs = 10000;

// Air i: Ar40/Ar36 = 295 + (i % 6), one hour apart; every 5th is a blank.
std::shared_ptr<pp::Analysis> analysis(int i) {
  auto a = std::make_shared<pp::Analysis>();
  const bool blank = i % 5 == 4;
  a->identifier = blank ? "bu" : "air";
  a->uuid = "uuid-" + std::to_string(i);
  a->aliquot = i + 1;
  a->runid = pp::make_runid(a->identifier, a->aliquot, -1);
  a->analysis_type = blank ? "blank_unknown" : "air";
  a->timestamp = 1'700'000'000.0 + 3600.0 * i;
  a->mass_spectrometer = i % 2 ? "jan" : "obama";
  a->sample = blank ? "" : "air";
  const double ratio = 295.0 + (i % 6);
  auto iso = [&](const char* name, const char* det, double v, double e) {
    pp::IsotopeData d;
    d.key = name;
    d.isotope = name;
    d.detector = det;
    d.intercept = {v, e};
    d.n = 20;
    reduction::FitSpec f;
    f.kind = reduction::FitKind::Linear;
    d.fit = f;
    a->isotopes.push_back(d);
  };
  iso("Ar40", "H1", 10.0 * ratio, 0.5);
  iso("Ar39", "AX", 1.0, 0.01);
  iso("Ar38", "L1", 2.0, 0.01);
  iso("Ar37", "L2", 0.5, 0.01);
  iso("Ar36", "CDD", 10.0, 0.02);
  a->gains = {{"H1", 1.0}};
  return a;
}

pp::RawData raw_for(const pp::Analysis& a) {
  pp::RawData raw;
  for (const auto& iso : a.isotopes) {
    pp::RawSeries s;
    s.kind = pp::SeriesKind::Signal;
    s.key = iso.key;
    s.detector = iso.detector;
    for (int k = 0; k < 20; ++k) {
      s.t.push_back(k * 2.0);
      s.v.push_back(iso.intercept.value - 0.01 * k);
    }
    raw.series.push_back(s);
  }
  return raw;
}

std::unique_ptr<pp::MemorySource> make_source(int n) {
  auto src = std::make_unique<pp::MemorySource>();
  for (int i = 0; i < n; ++i) {
    auto a = analysis(i);
    src->add(a, raw_for(*a));
  }
  return src;
}

QListWidgetItem* find_item(QListWidget* list, const QString& text) {
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->text() == text) return list->item(i);
  return nullptr;
}

bool wait_runs(FigureWindow& w, ProcessingBridge& bridge, int runs) {
  QElapsedTimer t;
  t.start();
  while (w.runs_completed() < runs && t.elapsed() < kWaitMs) {
    bridge.wait_idle(100);
    QTest::qWait(5);
  }
  return w.runs_completed() >= runs;
}

}  // namespace

class TestDataWindows : public QObject {
  Q_OBJECT

 private slots:
  void browser_lists_filters_and_pages() {
    auto src = make_source(260);
    DataBrowserWindow w(*src);
    QCOMPARE(w.model()->rowCount(), 200);  // first page
    QCOMPARE(w.model()->row(0).uuid, std::string("uuid-259"));  // newest first
    QVERIFY(w.load_more_button()->isEnabled());
    QVERIFY(w.status()->text().contains(QStringLiteral("200 of 260")));
    QTest::mouseClick(w.load_more_button(), Qt::LeftButton);
    QCOMPARE(w.model()->rowCount(), 260);
    QVERIFY(!w.load_more_button()->isEnabled());

    // Facet: only blanks.
    auto* types = w.facet_list(pp::Facet::AnalysisType);
    QVERIFY(types);
    QCOMPARE(types->count(), 2);
    find_item(types, QStringLiteral("blank_unknown"))->setCheckState(Qt::Checked);
    QCOMPARE(w.model()->rowCount(), 52);
    // The type list still offers both values (a facet ignores its own filter).
    QCOMPARE(w.facet_list(pp::Facet::AnalysisType)->count(), 2);
    // ... and the spectrometer list narrows to what blanks have.
    QCOMPARE(w.facet_list(pp::Facet::MassSpectrometer)->count(), 2);

    find_item(w.facet_list(pp::Facet::AnalysisType), QStringLiteral("blank_unknown"))->setCheckState(Qt::Unchecked);
    w.search()->setText(QStringLiteral("bu-0"));
    QVERIFY(w.model()->rowCount() > 0);
    for (const auto& r : w.model()->rows()) QVERIFY(r.runid.rfind("bu-0", 0) == 0);
    w.search()->clear();
    w.date_preset()->setCurrentIndex(1);  // last 24 hours of the newest
    QCOMPARE(w.model()->rowCount(), 25);
  }

  void browser_recall_and_time_series_signals() {
    auto src = make_source(10);
    DataBrowserWindow w(*src);
    QSignalSpy recall(&w, &DataBrowserWindow::recall_requested);
    QSignalSpy series(&w, &DataBrowserWindow::time_series_requested);
    w.recall_step(1);
    QCOMPARE(recall.count(), 1);
    QCOMPARE(recall.takeFirst().at(0).toString(), QStringLiteral("uuid-9"));
    w.recall_step(1);
    QCOMPARE(recall.takeFirst().at(0).toString(), QStringLiteral("uuid-8"));
    w.select_rows({0, 2});
    QCOMPARE(w.selected_uuids(), (QStringList{QStringLiteral("uuid-9"), QStringLiteral("uuid-7")}));
    for (auto* b : w.findChildren<QPushButton*>())
      if (b->text().startsWith(QStringLiteral("Time series"))) QTest::mouseClick(b, Qt::LeftButton);
    QCOMPARE(series.count(), 1);
    QCOMPARE(series.takeFirst().at(0).toStringList().size(), 2);
  }

  void recall_shows_every_tab() {
    auto src = make_source(3);
    RecallWindow w(*src);
    QVERIFY(w.show_analysis(QStringLiteral("uuid-1")));
    QVERIFY(w.title_label()->text().contains(QStringLiteral("air-02")));
    QVERIFY(w.computed_table()->rowCount() > 0);
    QCOMPARE(w.isotope_table()->rowCount(), 5);
    QCOMPARE(w.isotope_table()->item(0, 0)->text(), QStringLiteral("Ar40"));
    const int all_columns = w.isotope_table()->columnCount();
    w.stage_selector()->setCurrentIndex(1);  // intercept only
    QVERIFY(w.isotope_table()->columnCount() < all_columns);
    QCOMPARE(w.isotope_table()->item(0, 4)->text().toDouble(), 2960.0);
    QCOMPARE(w.evolutions()->panel_count(), 5);
    w.evolution_kind()->setCurrentIndex(1);  // no baselines in this data
    QCOMPARE(w.evolutions()->panel_count(), 0);
    QVERIFY(!w.show_analysis(QStringLiteral("nope")));
  }

  void figure_computes_and_click_excludes() {
    QTemporaryDir dir;
    auto src = make_source(12);
    ProcessingBridge bridge(*src);
    pp::PresetStore presets(dir.path().toStdString());
    QStringList ids;
    for (int i = 0; i < 12; ++i)
      if (i % 5 != 4) ids << QStringLiteral("uuid-%1").arg(i);
    FigureWindow w(bridge, presets, ids);
    w.resize(1000, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(wait_runs(w, bridge, 1));
    QVERIFY2(w.view()->scene(), qPrintable(w.status_label()->text()));
    QCOMPARE(w.view()->panel_count(), 2);  // Default preset: Ar40, Ar40/Ar36
    QCOMPARE(w.analyses_table()->rowCount(), 10);
    const QStringList before = w.view()->texts(1);
    QVERIFY(!before.isEmpty());

    // Click the analysis with the highest ratio in the ratio panel.
    w.view()->plot()->replot();
    const auto pos = w.view()->point_position("uuid-5", 1);
    QVERIFY(pos);
    QTest::mouseClick(w.view()->plot(), Qt::LeftButton, Qt::NoModifier, w.view()->plot()->mapFrom(w.view(), *pos));
    QVERIFY(wait_runs(w, bridge, 2));
    const auto ex = w.pipeline().find("edits")->options.get_strings("exclude");
    QCOMPARE(ex, std::vector<std::string>{"uuid-5"});
    bool excluded = false;
    for (const auto& it : w.dataset()->items())
      if (it.analysis->analysis->uuid == "uuid-5") excluded = it.exclusion.excluded();
    QVERIFY(excluded);
    QVERIFY(w.view()->texts(1) != before);  // the weighted mean moved

    // Toggling again includes it.
    w.toggle_exclusion({QStringLiteral("uuid-5")});
    QVERIFY(wait_runs(w, bridge, 3));
    QVERIFY(w.pipeline().find("edits")->options.get_strings("exclude").empty());
    QCOMPARE(w.view()->texts(1), before);
  }

  void figure_options_presets_and_export() {
    QTemporaryDir dir;
    auto src = make_source(12);
    ProcessingBridge bridge(*src);
    pp::PresetStore presets((dir.path() + QStringLiteral("/presets")).toStdString());
    QStringList ids;
    for (int i = 0; i < 12; ++i) ids << QStringLiteral("uuid-%1").arg(i);
    FigureWindow w(bridge, presets, ids);
    w.resize(900, 700);
    w.show();
    QVERIFY(wait_runs(w, bridge, 1));

    // The dock edits the figure: one panel, run-number axis.
    auto* editor = w.options_editor();
    QVERIFY(editor->tabs()->count() >= 5);
    QVERIFY(editor->apply(QStringLiteral("x.kind"), std::string("index")));
    QVERIFY(!editor->apply(QStringLiteral("x.kind"), std::string("sideways")));
    QVERIFY(wait_runs(w, bridge, 2));
    QCOMPARE(w.view()->scene()->graphs[0].x.format, pp::AxisFormat::Number);

    // Group by analysis type: two groups, two point layers per panel.
    w.set_group_key(QStringLiteral("analysis_type"));
    QVERIFY(wait_runs(w, bridge, 3));
    int layers = 0;
    for (const auto& l : w.view()->scene()->graphs[0].panels[0].layers) layers += std::holds_alternative<pp::PointLayer>(l);
    QCOMPARE(layers, 2);

    // Save as a user preset, switch to a factory one, come back.
    w.ask_preset_name = [] { return QStringLiteral("My Index"); };
    for (auto* b : w.findChildren<QPushButton*>())
      if (b->text() == QStringLiteral("Save as...")) QTest::mouseClick(b, Qt::LeftButton);
    QVERIFY(w.preset_combo()->findText(QStringLiteral("My Index")) >= 0);
    w.select_preset(QStringLiteral("Air monitor"));
    QVERIFY(wait_runs(w, bridge, 4));
    QCOMPARE(w.view()->panel_count(), 3);
    w.select_preset(QStringLiteral("My Index"));
    QVERIFY(wait_runs(w, bridge, 5));
    QCOMPARE(w.view()->scene()->graphs[0].x.format, pp::AxisFormat::Number);

    const QString png = dir.path() + QStringLiteral("/fig.png");
    const QString pdf = dir.path() + QStringLiteral("/fig.pdf");
    QVERIFY(w.export_figure(png));
    QVERIFY(w.export_figure(pdf));
    QVERIFY(QFileInfo(png).size() > 0);
    QVERIFY(QFileInfo(pdf).size() > 0);
  }

  void options_editor_rows() {
    OptionsEditor e;
    e.set_options(pp::Options(pp::time_series_schema()));
    auto* rows = e.rows(QStringLiteral("panels"));
    QVERIFY(rows);
    QCOMPARE(rows->count(), 1);
    QSignalSpy changed(&e, &OptionsEditor::changed);
    for (auto* b : rows->parentWidget()->findChildren<QPushButton*>())
      if (b->text() == QStringLiteral("Add")) {
        QTest::mouseClick(b, Qt::LeftButton);
        break;
      }
    QCOMPARE(changed.count(), 1);
    QCOMPARE(e.options().rows("panels").size(), 2u);
    QVERIFY(e.row_editor(QStringLiteral("panels"), QStringLiteral("quantity")));
    // Enabled-when: the fit error is disabled while fit = none.
    QVERIFY(!e.row_editor(QStringLiteral("panels"), QStringLiteral("fit_error"))->isEnabled());
    auto* fit = qobject_cast<QComboBox*>(e.row_editor(QStringLiteral("panels"), QStringLiteral("fit")));
    QVERIFY(fit);
    fit->setCurrentText(QStringLiteral("linear"));
    QVERIFY(e.row_editor(QStringLiteral("panels"), QStringLiteral("fit_error"))->isEnabled());
  }

  void main_window_opens_and_tears_down_data_windows() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    auto src = make_source(6);
    pp::PresetStore presets(dir.path().toStdString());
    {
      ui::MainWindow window(*line);
      QVERIFY(!window.data_action()->isEnabled());
      window.set_data(src.get(), &presets);
      QVERIFY(window.data_action()->isEnabled());
      window.data_action()->trigger();
      QVERIFY(window.data_window());
      QCOMPARE(window.data_window()->model()->rowCount(), 6);
      QVERIFY(window.open_recall(QStringLiteral("uuid-0")));
      QVERIFY(window.open_time_series({QStringLiteral("uuid-0"), QStringLiteral("uuid-1")}));
      // Destroyed with a figure window open: data windows go before the bridge.
    }
  }
};

QTEST_MAIN(TestDataWindows)
#include "test_data_windows.moc"
