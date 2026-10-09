// The three table models of the flux window on a hand-built LevelFit (no store):
// cells, check boxes that emit and do not change, tints, rows without a fit.

#include <QBrush>
#include <QSignalSpy>
#include <QtTest/QtTest>

#ifndef PYCHRON_UI_HAS_STORE

class FluxModelsTest : public QObject {
  Q_OBJECT
 private Q_SLOTS:
  void skipped() { QSKIP("built without the DVC store"); }
};

#else

#include "flux_analysis_model.hpp"
#include "flux_monitor_model.hpp"
#include "flux_unknown_model.hpp"
#include "theme.hpp"

namespace pp = pychron::processing;
using pychron::ui::FluxAnalysisModel;
using pychron::ui::FluxMonitorModel;
using pychron::ui::FluxUnknownModel;

namespace {

pp::FittedPosition monitor(int hole) {
  pp::FittedPosition p;
  p.hole = hole;
  p.identifier = "NM-" + std::to_string(hole);
  p.sample = "FC-2";
  p.monitor = true;
  p.n = 4;
  p.saved_j = 1.0e-3;
  p.saved_j_err = 1.0e-5;
  p.mean_j = 1.01e-3;
  p.mean_j_err = 2.02e-5;
  p.mean_j_mswd = 1.234;
  p.j = 1.02e-3;
  p.j_err = 5.1e-6;
  p.dev_percent = -1.5;
  p.used_in_fit = true;
  return p;
}

pp::FittedPosition unknown(int hole) {
  pp::FittedPosition p;
  p.hole = hole;
  p.identifier = "26-" + std::to_string(hole);
  p.sample = "Alpha";
  p.saved_j = 2.0e-3;
  p.saved_j_err = 2.0e-5;
  p.j = 2.5e-3;
  p.j_err = 5.0e-5;
  p.dev_percent = -20.0;
  return p;
}

pp::FittedPosition::UsedAnalysis analysis(const char* uuid, pp::AnalysisState state) {
  pp::FittedPosition::UsedAnalysis a;
  a.uuid = uuid;
  a.record_id = std::string("26-001-") + uuid;
  a.tag = "ok";
  a.state = state;
  a.j = 1.0e-3;
  a.j_err = 1.0e-5;
  return a;
}

QString cell(const QAbstractItemModel& m, int row, int col, int role = Qt::DisplayRole) {
  return m.data(m.index(row, col), role).toString();
}

pp::LevelFit sample_fit() {
  pp::LevelFit fit;
  fit.positions = {monitor(3), unknown(4), monitor(5)};
  return fit;
}

}  // namespace

class FluxModelsTest : public QObject {
  Q_OBJECT

 private Q_SLOTS:
  void monitor_headers_and_cells() {
    pp::LevelFit fit = sample_fit();
    FluxMonitorModel m;
    m.set_fit(&fit);
    QCOMPARE(m.rowCount(), 2);  // the unknown is not a monitor row
    QCOMPARE(m.columnCount(), int(FluxMonitorModel::ColumnCount));
    const QStringList headers{"Fit", "Save", "Hole", "Identifier", "Sample", "N", "Saved J", "±",
                              "Mean J", "±", "%", "MSWD", "Pred. J", "±", "%", "Dev %"};
    for (int c = 0; c < headers.size(); ++c)
      QCOMPARE(m.headerData(c, Qt::Horizontal).toString(), headers[c]);
    QCOMPARE(m.data(m.index(0, FluxMonitorModel::Fit), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QCOMPARE(m.data(m.index(0, FluxMonitorModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QCOMPARE(cell(m, 0, FluxMonitorModel::Hole), QString("3"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::Identifier), QString("NM-3"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::Sample), QString("FC-2"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::N), QString("4"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::SavedJ), QString("1.0000e-03"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::SavedJErr), QString("1.0000e-05"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::MeanJ), QString("1.0100e-03"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::MeanJErr), QString("2.0200e-05"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::MeanPercent), QString("2.00"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::Mswd), QString("1.23"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::PredJ), QString("1.0200e-03"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::PredJErr), QString("5.1000e-06"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::PredPercent), QString("0.50"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::Dev), QString("-1.50"));
    QCOMPARE(m.data(m.index(0, FluxMonitorModel::MeanJ), Qt::TextAlignmentRole).toInt(),
             int(Qt::AlignRight | Qt::AlignVCenter));
    QCOMPARE(m.hole_at(1), 5);
    QCOMPARE(m.row_of(5), 1);
    QCOMPARE(m.row_of(4), -1);
    // an absent value is an empty cell, not "-"
    fit.positions[0].mean_j_mswd.reset();
    fit.positions[0].dev_percent.reset();
    m.set_fit(&fit);
    QCOMPARE(cell(m, 0, FluxMonitorModel::Mswd), QString());
    QCOMPARE(cell(m, 0, FluxMonitorModel::Dev), QString());
  }

  void monitor_fit_box_emits_and_does_not_change() {
    pp::LevelFit fit = sample_fit();
    FluxMonitorModel m;
    m.set_fit(&fit);
    QSignalSpy fit_spy(&m, &FluxMonitorModel::fit_toggled);
    QSignalSpy save_spy(&m, &FluxMonitorModel::save_toggled);
    QVERIFY(m.flags(m.index(0, FluxMonitorModel::Fit)) & Qt::ItemIsUserCheckable);
    QVERIFY(m.setData(m.index(0, FluxMonitorModel::Fit), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(fit_spy.count(), 1);
    QCOMPARE(fit_spy.at(0).at(0).toInt(), 3);
    QCOMPARE(fit_spy.at(0).at(1).toBool(), false);
    QCOMPARE(m.data(m.index(0, FluxMonitorModel::Fit), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QVERIFY(m.setData(m.index(1, FluxMonitorModel::Save), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(save_spy.count(), 1);
    QCOMPARE(save_spy.at(0).at(0).toInt(), 5);
    QCOMPARE(save_spy.at(0).at(1).toBool(), false);
    QCOMPARE(m.data(m.index(1, FluxMonitorModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QVERIFY(!m.setData(m.index(0, FluxMonitorModel::Hole), 9, Qt::EditRole));
  }

  void monitor_without_usable_analysis_is_not_checkable() {
    pp::LevelFit fit = sample_fit();
    fit.positions[0].used_in_fit = false;
    fit.positions[0].n = 0;
    fit.positions[0].notes = {pp::PositionNote::NoUsableAnalysis};
    FluxMonitorModel m;
    m.set_fit(&fit);
    const QModelIndex box = m.index(0, FluxMonitorModel::Fit);
    QVERIFY(!(m.flags(box) & Qt::ItemIsUserCheckable));
    QCOMPARE(m.data(box, Qt::ToolTipRole).toString(), QString("No usable analysis"));
    QCOMPARE(m.data(box, Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    QSignalSpy spy(&m, &FluxMonitorModel::fit_toggled);
    QVERIFY(!m.setData(box, Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 0);
    // the other monitor is checkable and has no tooltip
    QVERIFY(m.flags(m.index(1, FluxMonitorModel::Fit)) & Qt::ItemIsUserCheckable);
    QVERIFY(!m.data(m.index(1, FluxMonitorModel::Fit), Qt::ToolTipRole).isValid());
  }

  void monitor_tints() {
    pp::LevelFit fit = sample_fit();
    fit.positions[0].notes = {pp::PositionNote::MeanMswdOutsideLimits};
    fit.positions[2].used_in_fit = false;
    fit.positions[2].excluded = true;
    fit.positions[2].notes = {pp::PositionNote::LeftOutOfFit};
    FluxMonitorModel m;
    m.set_fit(&fit);
    // outside its limits: the warning row tint, text as usual
    for (int c = 0; c < FluxMonitorModel::ColumnCount; ++c) {
      QCOMPARE(m.data(m.index(0, c), Qt::BackgroundRole).value<QBrush>().color(), pychron::ui::theme().warning_bg);
      QVERIFY(!m.data(m.index(0, c), Qt::ForegroundRole).isValid());
    }
    // not in the fit: muted text across the row, no background
    for (int c = 0; c < FluxMonitorModel::ColumnCount; ++c) {
      QCOMPARE(m.data(m.index(1, c), Qt::ForegroundRole).value<QBrush>().color(), pychron::ui::theme().muted_text);
      QVERIFY(!m.data(m.index(1, c), Qt::BackgroundRole).isValid());
    }
  }

  void monitor_rows_without_a_fit() {
    pp::LevelInputs inputs;
    pp::LevelPosition a, u, b;
    a.hole = 2; a.identifier = "NM-2"; a.sample = "FC-2"; a.monitor = true;
    pp::SavedFlux saved;
    saved.j = 1.0e-3;
    saved.j_err = 1.0e-5;
    a.saved = saved;
    u.hole = 3; u.identifier = "26-3"; u.sample = "Alpha";
    b.hole = 7; b.identifier = "NM-7"; b.sample = "FC-2"; b.monitor = true;
    inputs.positions = {a, u, b};

    FluxMonitorModel m;
    QCOMPARE(m.rowCount(), 0);  // both null: nothing
    m.set_fit(nullptr);
    m.set_inputs(&inputs);
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(cell(m, 0, FluxMonitorModel::Hole), QString("2"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::Identifier), QString("NM-2"));
    QCOMPARE(cell(m, 1, FluxMonitorModel::Identifier), QString("NM-7"));
    QCOMPARE(cell(m, 0, FluxMonitorModel::SavedJ), QString("1.0000e-03"));
    QCOMPARE(cell(m, 1, FluxMonitorModel::SavedJ), QString());
    for (int c : {static_cast<int>(FluxMonitorModel::N), static_cast<int>(FluxMonitorModel::MeanJ), static_cast<int>(FluxMonitorModel::MeanJErr),
                  static_cast<int>(FluxMonitorModel::MeanPercent), static_cast<int>(FluxMonitorModel::Mswd), static_cast<int>(FluxMonitorModel::PredJ),
                  static_cast<int>(FluxMonitorModel::PredJErr), static_cast<int>(FluxMonitorModel::PredPercent), static_cast<int>(FluxMonitorModel::Dev)})
      QCOMPARE(cell(m, 0, c), QString());
    QCOMPARE(m.data(m.index(0, FluxMonitorModel::Fit), Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    QVERIFY(!(m.flags(m.index(0, FluxMonitorModel::Fit)) & Qt::ItemIsUserCheckable));
    QCOMPARE(m.hole_at(1), 7);
    QCOMPARE(m.hole_at(2), 0);
    QCOMPARE(m.row_of(7), 1);
    QCOMPARE(m.row_of(3), -1);  // an unknown is no monitor row

    FluxUnknownModel um;
    um.set_inputs(&inputs);
    QCOMPARE(um.rowCount(), 1);
    QCOMPARE(cell(um, 0, FluxUnknownModel::Hole), QString("3"));
    QCOMPARE(cell(um, 0, FluxUnknownModel::PredJ), QString());
    QCOMPARE(cell(um, 0, FluxUnknownModel::Dev), QString());
    QCOMPARE(um.hole_at(0), 3);
    QCOMPARE(um.hole_at(1), 0);
  }

  // Ruling R15: no fit, and the positions as fit_level would have counted them
  // (processing::evaluate_position): the Fit boxes say what the edits say and
  // can be changed, the means are shown, the predictions are not.
  void monitor_rows_from_evaluated_positions() {
    std::vector<pp::FittedPosition> evaluated = {monitor(3), unknown(4), monitor(5), monitor(6)};
    evaluated[2].used_in_fit = false;  // excluded by the edits
    evaluated[2].excluded = true;
    evaluated[2].notes = {pp::PositionNote::LeftOutOfFit};
    evaluated[3].used_in_fit = false;  // nothing to fit with
    evaluated[3].n = 0;
    evaluated[3].mean_j.reset();
    evaluated[3].mean_j_err.reset();
    evaluated[3].mean_j_mswd.reset();
    evaluated[3].notes = {pp::PositionNote::NoUsableAnalysis};

    FluxMonitorModel m;
    QSignalSpy resets(&m, &QAbstractItemModel::modelReset);
    m.set_unfitted(&evaluated);
    QCOMPARE(resets.count(), 1);
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(m.hole_at(0), 3);
    QCOMPARE(m.row_of(5), 1);
    QCOMPARE(m.row_of(4), -1);
    const auto fit_box = [&](int row) { return m.data(m.index(row, FluxMonitorModel::Fit), Qt::CheckStateRole).toInt(); };
    const auto checkable = [&](int row) {
      return bool(m.flags(m.index(row, FluxMonitorModel::Fit)) & Qt::ItemIsUserCheckable);
    };
    QCOMPARE(fit_box(0), int(Qt::Checked));
    QCOMPARE(fit_box(1), int(Qt::Unchecked));
    QCOMPARE(fit_box(2), int(Qt::Unchecked));
    QVERIFY(checkable(0));
    QVERIFY(checkable(1));
    QVERIFY(!checkable(2));
    QCOMPARE(cell(m, 2, FluxMonitorModel::Fit, Qt::ToolTipRole), QString("No usable analysis"));
    // What does not depend on the fit is there...
    for (int row : {0, 1}) {
      QCOMPARE(cell(m, row, FluxMonitorModel::Identifier), QString("NM-%1").arg(row == 0 ? 3 : 5));
      QCOMPARE(cell(m, row, FluxMonitorModel::N), QString("4"));
      QCOMPARE(cell(m, row, FluxMonitorModel::SavedJ), QString("1.0000e-03"));
      QCOMPARE(cell(m, row, FluxMonitorModel::MeanJ), QString("1.0100e-03"));
      QCOMPARE(cell(m, row, FluxMonitorModel::MeanJErr), QString("2.0200e-05"));
      QCOMPARE(cell(m, row, FluxMonitorModel::MeanPercent), QString("2.00"));
      QCOMPARE(cell(m, row, FluxMonitorModel::Mswd), QString("1.23"));
    }
    QCOMPARE(cell(m, 2, FluxMonitorModel::MeanJ), QString());
    // ... and what the fit predicts is not, whatever the positions hold there.
    for (int row = 0; row < 3; ++row)
      for (int c : {static_cast<int>(FluxMonitorModel::PredJ), static_cast<int>(FluxMonitorModel::PredJErr), static_cast<int>(FluxMonitorModel::PredPercent),
                    static_cast<int>(FluxMonitorModel::Dev)})
        QCOMPARE(cell(m, row, c), QString());

    // A click is reported as ever; the model changes nothing by itself.
    QSignalSpy toggled(&m, &FluxMonitorModel::fit_toggled);
    QVERIFY(m.setData(m.index(1, FluxMonitorModel::Fit), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(toggled.at(0).at(0).toInt(), 5);
    QCOMPARE(toggled.at(0).at(1).toBool(), true);
    QCOMPARE(fit_box(1), int(Qt::Unchecked));
    QVERIFY(!m.setData(m.index(2, FluxMonitorModel::Fit), Qt::Checked, Qt::CheckStateRole));

    FluxUnknownModel um;
    um.set_unfitted(&evaluated);
    QCOMPARE(um.rowCount(), 1);
    QCOMPARE(cell(um, 0, FluxUnknownModel::Identifier), QString("26-4"));
    QCOMPARE(cell(um, 0, FluxUnknownModel::SavedJ), QString("2.0000e-03"));
    for (int c : {static_cast<int>(FluxUnknownModel::PredJ), static_cast<int>(FluxUnknownModel::PredJErr), static_cast<int>(FluxUnknownModel::PredPercent),
                  static_cast<int>(FluxUnknownModel::Dev)})
      QCOMPARE(cell(um, 0, c), QString());

    // A fit takes over, and gives way again.
    pp::LevelFit fit = sample_fit();
    m.set_fit(&fit);
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(cell(m, 0, FluxMonitorModel::PredJ), QString("1.0200e-03"));
    m.set_fit(nullptr);
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(cell(m, 0, FluxMonitorModel::PredJ), QString());
    m.set_unfitted(nullptr);
    QCOMPARE(m.rowCount(), 0);
  }

  void unknown_cells_and_save_box() {
    pp::LevelFit fit = sample_fit();
    FluxUnknownModel m;
    m.set_fit(&fit);
    QCOMPARE(m.rowCount(), 1);
    const QStringList headers{"Save", "Hole", "Identifier", "Sample", "Saved J", "±", "Pred. J", "±", "%", "Dev %"};
    QCOMPARE(m.columnCount(), headers.size());
    for (int c = 0; c < headers.size(); ++c)
      QCOMPARE(m.headerData(c, Qt::Horizontal).toString(), headers[c]);
    QCOMPARE(cell(m, 0, FluxUnknownModel::Hole), QString("4"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::Identifier), QString("26-4"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::Sample), QString("Alpha"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::SavedJ), QString("2.0000e-03"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::SavedJErr), QString("2.0000e-05"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::PredJ), QString("2.5000e-03"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::PredJErr), QString("5.0000e-05"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::PredPercent), QString("2.00"));
    QCOMPARE(cell(m, 0, FluxUnknownModel::Dev), QString("-20.00"));
    QCOMPARE(m.hole_at(0), 4);
    QSignalSpy spy(&m, &FluxUnknownModel::save_toggled);
    QVERIFY(m.setData(m.index(0, FluxUnknownModel::Save), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 4);
    QCOMPARE(spy.at(0).at(1).toBool(), false);
    QCOMPARE(m.data(m.index(0, FluxUnknownModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QVERIFY(!m.setData(m.index(0, FluxUnknownModel::Save), Qt::Unchecked, Qt::EditRole));
    QVERIFY(!m.setData(m.index(0, FluxUnknownModel::Hole), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 1);
  }

  void analysis_states_and_use_box() {
    pp::FittedPosition p = monitor(3);
    p.analyses = {analysis("a", pp::AnalysisState::Used),        analysis("b", pp::AnalysisState::OmittedByTag),
                  analysis("c", pp::AnalysisState::OmittedBySavedFit), analysis("d", pp::AnalysisState::OmittedByEdit),
                  analysis("e", pp::AnalysisState::NotReduced),  analysis("f", pp::AnalysisState::NoJ)};
    p.analyses[1].tag = "bad";
    p.analyses[4].j.reset();
    p.analyses[4].j_err.reset();
    p.analyses[4].reduction_error = "no baseline";
    FluxAnalysisModel m;
    QCOMPARE(m.rowCount(), 0);
    m.set_position(&p);
    QCOMPARE(m.rowCount(), 6);
    QCOMPARE(cell(m, 5, FluxAnalysisModel::J), QString("1.0000e-03"));
    QCOMPARE(cell(m, 5, FluxAnalysisModel::State), QString("no J"));
    QCOMPARE(m.data(m.index(5, FluxAnalysisModel::Use), Qt::ToolTipRole).toString(), QString("No J"));
    const QStringList headers{"Use", "Record", "Tag", "J", "±", "State"};
    for (int c = 0; c < headers.size(); ++c)
      QCOMPARE(m.headerData(c, Qt::Horizontal).toString(), headers[c]);
    const QStringList states{"used", "omitted (tag bad)", "omitted (saved fit)", "omitted (here)", "not reduced", "no J"};
    for (int r = 0; r < states.size(); ++r) QCOMPARE(cell(m, r, FluxAnalysisModel::State), states[r]);
    QCOMPARE(cell(m, 0, FluxAnalysisModel::Record), QString("26-001-a"));
    QCOMPARE(cell(m, 0, FluxAnalysisModel::Tag), QString("ok"));
    QCOMPARE(cell(m, 0, FluxAnalysisModel::J), QString("1.0000e-03"));
    QCOMPARE(cell(m, 0, FluxAnalysisModel::JErr), QString("1.0000e-05"));
    QCOMPARE(cell(m, 4, FluxAnalysisModel::J), QString());
    for (int r = 0; r < 6; ++r)
      QCOMPARE(m.data(m.index(r, FluxAnalysisModel::Use), Qt::CheckStateRole).toInt(),
               int(r == 0 ? Qt::Checked : Qt::Unchecked));
    for (int r : {0, 1, 2, 3}) QVERIFY(m.flags(m.index(r, FluxAnalysisModel::Use)) & Qt::ItemIsUserCheckable);
    for (int r : {4, 5}) QVERIFY(!(m.flags(m.index(r, FluxAnalysisModel::Use)) & Qt::ItemIsUserCheckable));
    QCOMPARE(m.data(m.index(4, FluxAnalysisModel::Use), Qt::ToolTipRole).toString(), QString("no baseline"));
    p.analyses[4].reduction_error.clear();
    m.set_position(&p);
    QCOMPARE(m.data(m.index(4, FluxAnalysisModel::Use), Qt::ToolTipRole).toString(), QString("Not reduced"));

    QSignalSpy spy(&m, &FluxAnalysisModel::use_toggled);
    QVERIFY(m.setData(m.index(0, FluxAnalysisModel::Use), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString("a"));
    QCOMPARE(spy.at(0).at(1).toBool(), false);
    QCOMPARE(m.data(m.index(0, FluxAnalysisModel::Use), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QVERIFY(m.setData(m.index(2, FluxAnalysisModel::Use), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toString(), QString("c"));
    QCOMPARE(spy.at(1).at(1).toBool(), true);
    QVERIFY(!m.setData(m.index(5, FluxAnalysisModel::Use), Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 2);
    // neither another role nor another column is a check-box click
    QVERIFY(!m.setData(m.index(0, FluxAnalysisModel::Use), Qt::Unchecked, Qt::EditRole));
    QVERIFY(!m.setData(m.index(0, FluxAnalysisModel::Tag), Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(spy.count(), 2);
    m.set_position(nullptr);
    QCOMPARE(m.rowCount(), 0);
  }

  void skip_positions_untick_save() {
    pp::LevelFit fit = sample_fit();
    FluxMonitorModel mm;
    FluxUnknownModel um;
    mm.set_fit(&fit);
    um.set_fit(&fit);
    mm.set_skip({5, 4});
    um.set_skip({5, 4});
    QCOMPARE(mm.data(mm.index(0, FluxMonitorModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Checked));
    QCOMPARE(mm.data(mm.index(1, FluxMonitorModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    QCOMPARE(um.data(um.index(0, FluxUnknownModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
    mm.set_skip({});
    QCOMPARE(mm.data(mm.index(1, FluxMonitorModel::Save), Qt::CheckStateRole).toInt(), int(Qt::Checked));
  }
};

#endif

QTEST_MAIN(FluxModelsTest)
#include "test_flux_models.moc"
