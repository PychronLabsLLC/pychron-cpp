// FieldTableModel: the field table editor's grid (isotope, mass, one column
// per detector) over an edit copy of a FieldTable, every edit undoable.

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <QSignalSpy>
#include <QUndoStack>
#include <QtTest/QtTest>

#include "field_table_model.hpp"

using pychron::spectrometer::ControlPoint;
using pychron::spectrometer::FieldTable;
using pychron::spectrometer::FitKind;
using pychron::spectrometer::MolecularWeights;
using pychron::spectrometer::TableAxis;
using pychron::ui::FieldTableModel;

namespace {

constexpr double kAr40 = 39.9623831237;
constexpr double kAr39 = 38.964313;
constexpr double kAr36 = 35.967545105;

MolecularWeights weights() {
  return MolecularWeights({{"Ar40", kAr40}, {"Ar39", kAr39}, {"Ar38", 37.9627322}, {"Ar36", kAr36}});
}

// Heaviest first, as the shipped tables are; H2 is not in the config below.
FieldTable table() {
  return FieldTable(FitKind::Quadratic, TableAxis::Dac,
                    {ControlPoint{"Ar40", kAr40, {{"H1", 5.001}, {"AX", 4.874}, {"H2", 5.123}}},
                     ControlPoint{"Ar39", kAr39, {{"H1", 4.874}, {"AX", 4.750}}},
                     ControlPoint{"Ar36", kAr36, {{"H1", 4.505}, {"AX", 4.383}}}});
}

std::vector<std::string> config_detectors() { return {"AX", "H1", "L1"}; }

QString cell(const FieldTableModel& m, int row, int col, int role = Qt::DisplayRole) {
  return m.data(m.index(row, col), role).toString();
}

int row_of(const FieldTableModel& m, const std::string& isotope) {
  const auto& points = m.table().points();
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (points[i].isotope == isotope) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

class TestFieldTableModel : public QObject {
  Q_OBJECT

 private slots:
  void columnsAreConfigOrderThenTableOnly() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    QCOMPARE(m.detector_columns(), (std::vector<std::string>{"AX", "H1", "L1", "H2"}));
    QCOMPARE(m.columnCount(), 6);
    QCOMPARE(m.column_of("AX"), FieldTableModel::kFirstDetectorColumn);
    QCOMPARE(m.column_of("H2"), FieldTableModel::kFirstDetectorColumn + 3);
    QCOMPARE(m.column_of("nope"), -1);
    QVERIFY(m.in_config("L1"));
    QVERIFY(!m.in_config("H2"));
    QCOMPARE(m.headerData(FieldTableModel::kIsotopeColumn, Qt::Horizontal).toString(), QString("Isotope"));
    QCOMPARE(m.headerData(FieldTableModel::kMassColumn, Qt::Horizontal).toString(), QString("Mass"));
    QCOMPARE(m.headerData(m.column_of("AX"), Qt::Horizontal).toString(), QString("AX"));
    QCOMPARE(m.headerData(m.column_of("H2"), Qt::Horizontal).toString(), QString("H2 (not in config)"));
    QVERIFY(m.headerData(m.column_of("H2"), Qt::Horizontal, Qt::ToolTipRole).toString().contains("not in config"));
    // L1 is in config but the table has no value for it: an empty column.
    QCOMPARE(cell(m, 0, m.column_of("L1")), QString());
  }

  void valuesDisplayFiveDecimalsEditFullPrecision() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    const int r = row_of(m, "Ar40");
    const int c = m.column_of("H1");
    QVERIFY(m.setData(m.index(r, c), QString("5.0012345678901")));
    QCOMPARE(cell(m, r, c), QString("5.00123"));
    QCOMPARE(m.data(m.index(r, c), Qt::EditRole).toString().toDouble(), 5.0012345678901);
    QCOMPARE(m.table().points()[static_cast<std::size_t>(r)].values.at("H1"), 5.0012345678901);
    QCOMPARE(cell(m, r, FieldTableModel::kIsotopeColumn), QString("Ar40"));
  }

  void residualTooltipWhenTheFitSucceeds() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    const int r = row_of(m, "Ar39");
    // Three points, quadratic: the fit goes through each, residual ~0.
    QVERIFY(cell(m, r, m.column_of("H1"), Qt::ToolTipRole).startsWith("residual"));
    // L1 has no points: no fit, no tooltip.
    QCOMPARE(cell(m, r, m.column_of("L1"), Qt::ToolTipRole), QString());
  }

  void clearingACellRemovesTheValue() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    const int r = row_of(m, "Ar40");
    QVERIFY(m.setData(m.index(r, m.column_of("H2")), QString("  ")));
    QVERIFY(!m.table().points()[static_cast<std::size_t>(r)].values.contains("H2"));
    QCOMPARE(cell(m, r, m.column_of("H2")), QString());
    // The column stays: the column set is fixed by config and the table loaded.
    QCOMPARE(m.column_of("H2"), FieldTableModel::kFirstDetectorColumn + 3);
  }

  void nonFiniteTextIsRefused() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    const int c = m.column_of("H1");
    QVERIFY(!m.setData(m.index(0, c), QString("abc")));
    QVERIFY(!m.setData(m.index(0, c), QString("inf")));
    QVERIFY(!m.setData(m.index(0, c), QString("nan")));
    QVERIFY(!m.setData(m.index(0, FieldTableModel::kMassColumn), QString("-1")));
    QVERIFY(!m.setData(m.index(0, FieldTableModel::kMassColumn), QString("0")));
    QVERIFY(!m.setData(m.index(0, FieldTableModel::kMassColumn), QString("x")));
    QVERIFY(!m.modified());
  }

  void isotopeCommitFillsMassFromWeights() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    // Mass equals the old isotope's weight: follows the new name.
    QVERIFY(m.setData(m.index(row_of(m, "Ar39"), FieldTableModel::kIsotopeColumn), QString("  Ar38 ")));
    const int r = row_of(m, "Ar38");
    QVERIFY(r >= 0);
    QCOMPARE(m.table().points()[static_cast<std::size_t>(r)].mass, 37.9627322);
    // A row added for an unknown isotope has mass 0; naming it fills it.
    m.add_row("X1");
    QCOMPARE(m.table().points()[static_cast<std::size_t>(row_of(m, "X1"))].mass, 0.0);
    QVERIFY(m.setData(m.index(row_of(m, "X1"), FieldTableModel::kIsotopeColumn), QString("Ar39")));
    QCOMPARE(m.table().points()[static_cast<std::size_t>(row_of(m, "Ar39"))].mass, kAr39);
  }

  void isotopeCommitKeepsAHandTypedMass() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    const int r = row_of(m, "Ar39");
    QVERIFY(m.setData(m.index(r, FieldTableModel::kMassColumn), QString("38.5")));
    QVERIFY(m.setData(m.index(row_of(m, "Ar39"), FieldTableModel::kIsotopeColumn), QString("Ar38")));
    QCOMPARE(m.table().points()[static_cast<std::size_t>(row_of(m, "Ar38"))].mass, 38.5);
    // An unknown name keeps the mass too.
    QVERIFY(m.setData(m.index(row_of(m, "Ar40"), FieldTableModel::kIsotopeColumn), QString("Q40")));
    QCOMPARE(m.table().points()[static_cast<std::size_t>(row_of(m, "Q40"))].mass, kAr40);
  }

  void rowsStaySortedByMass() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    // Loaded heaviest first, shown lightest first.
    QCOMPARE(cell(m, 0, FieldTableModel::kIsotopeColumn), QString("Ar36"));
    QCOMPARE(cell(m, 2, FieldTableModel::kIsotopeColumn), QString("Ar40"));
    QVERIFY(m.setData(m.index(0, FieldTableModel::kMassColumn), QString("41")));
    QCOMPARE(cell(m, 2, FieldTableModel::kIsotopeColumn), QString("Ar36"));
    m.add_row("Ar38");  // 37.96: lighter than Ar39 now that Ar36 sits at 41
    QCOMPARE(cell(m, 0, FieldTableModel::kIsotopeColumn), QString("Ar38"));
    QCOMPARE(cell(m, 3, FieldTableModel::kIsotopeColumn), QString("Ar36"));
  }

  void addAndRemoveRows() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    m.add_row(" Ar38 ");
    QCOMPARE(m.rowCount(), 4);
    const int r = row_of(m, "Ar38");
    QVERIFY(r >= 0);
    QVERIFY(m.table().points()[static_cast<std::size_t>(r)].values.empty());
    m.remove_rows({row_of(m, "Ar36"), row_of(m, "Ar40")});
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(row_of(m, "Ar36"), -1);
    QCOMPARE(row_of(m, "Ar40"), -1);
    QVERIFY(row_of(m, "Ar39") >= 0);
    m.remove_rows({});
    m.remove_rows({99});
    QCOMPARE(m.rowCount(), 2);
  }

  void setValuePropagates() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    FieldTable expected = m.table();
    QVERIFY(expected.update("H1", "Ar40", 5.011, true).has_value());
    QVERIFY(m.set_value("H1", "Ar40", 5.011, true).has_value());
    QCOMPARE(m.table(), expected);
    QVERIFY(m.modified());

    FieldTable plain = m.table();
    QVERIFY(plain.update("AX", "Ar36", 4.4, false).has_value());
    QVERIFY(m.set_value("AX", "Ar36", 4.4, false).has_value());
    QCOMPARE(m.table(), plain);

    const int depth = m.undo_stack()->count();
    QVERIFY(!m.set_value("H1", "Ar99", 1.0, false).has_value());
    QVERIFY(!m.set_value("H1", "Ar40", std::numeric_limits<double>::infinity(), false).has_value());
    QCOMPARE(m.undo_stack()->count(), depth);
  }

  void undoRedoRestoresTableExactly() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    const FieldTable start = m.table();
    QVERIFY(m.setData(m.index(0, m.column_of("H1")), QString("4.5051234")));
    const FieldTable edited = m.table();
    m.add_row("Ar38");
    m.remove_rows({0});
    QCOMPARE(m.undo_stack()->count(), 3);
    m.undo_stack()->undo();
    m.undo_stack()->undo();
    QCOMPARE(m.table(), edited);
    m.undo_stack()->undo();
    QCOMPARE(m.table(), start);
    m.undo_stack()->redo();
    QCOMPARE(m.table(), edited);
    QCOMPARE(cell(m, 0, m.column_of("H1")), QString("4.50512"));
  }

  void modifiedFollowsTheCleanState() {
    FieldTableModel m(config_detectors(), weights());
    QSignalSpy edited(&m, &FieldTableModel::tableEdited);
    m.set_table(table());
    QVERIFY(!m.modified());
    QVERIFY(m.setData(m.index(0, m.column_of("H1")), QString("4.6")));
    QVERIFY(m.modified());
    QVERIFY(edited.count() >= 1);
    m.mark_saved();
    QVERIFY(!m.modified());
    m.undo_stack()->undo();
    QVERIFY(m.modified());
    m.undo_stack()->redo();
    QVERIFY(!m.modified());
    m.set_table(table());
    QVERIFY(!m.modified());
    QCOMPARE(m.undo_stack()->count(), 0);
    // Committing the same text is no edit.
    QVERIFY(m.setData(m.index(0, FieldTableModel::kIsotopeColumn), QString("Ar36")));
    QCOMPARE(m.undo_stack()->count(), 0);
  }

  void fitChangesAreUndoable() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    m.set_default_fit(FitKind::Linear);
    QCOMPARE(m.table().default_fit(), FitKind::Linear);
    m.set_detector_fit("H1", FitKind::Cubic);
    QCOMPARE(m.table().fit("H1"), FitKind::Cubic);
    m.set_detector_fit("H1", std::nullopt);
    QVERIFY(!m.table().fit_overrides().contains("H1"));
    QCOMPARE(m.undo_stack()->count(), 3);
    m.undo_stack()->undo();
    QCOMPARE(m.table().fit("H1"), FitKind::Cubic);
    m.undo_stack()->undo();
    m.undo_stack()->undo();
    QCOMPARE(m.table().default_fit(), FitKind::Quadratic);
    QVERIFY(!m.modified());
  }

  void anEditBringingANewDetectorAddsItsColumnAndUndoTakesItAway() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    FieldTable imported = m.table();
    imported.mutable_points().front().values["CDD"] = 4.0;  // in neither config nor table
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
    m.edit_table(imported, "Import CSV");
    QCOMPARE(reset.count(), 1);
    QCOMPARE(m.detector_columns(), (std::vector<std::string>{"AX", "H1", "L1", "H2", "CDD"}));
    QCOMPARE(m.columnCount(), 7);
    const int c = m.column_of("CDD");
    QCOMPARE(cell(m, 0, c), QString("4.00000"));
    QCOMPARE(m.headerData(c, Qt::Horizontal).toString(), QString("CDD (not in config)"));
    QVERIFY(m.setData(m.index(0, c), QString("4.5")));
    m.undo_stack()->undo();
    m.undo_stack()->undo();
    // Back to the table as loaded: its own columns, H2 kept though it is in the table only.
    QCOMPARE(m.detector_columns(), (std::vector<std::string>{"AX", "H1", "L1", "H2"}));
    // Clearing a table-only column's last value keeps the column while editing.
    for (int r = 0; r < m.rowCount(); ++r) QVERIFY(m.setData(m.index(r, m.column_of("H2")), QString()));
    QCOMPARE(m.column_of("H2"), FieldTableModel::kFirstDetectorColumn + 3);
  }

  void readOnlyRefusesEdits() {
    FieldTableModel m(config_detectors(), weights());
    m.set_table(table());
    m.set_read_only(true);
    QVERIFY(!(m.flags(m.index(0, 0)) & Qt::ItemIsEditable));
    QVERIFY(!m.setData(m.index(0, m.column_of("H1")), QString("4.6")));
    m.set_read_only(false);
    QVERIFY(m.flags(m.index(0, 0)) & Qt::ItemIsEditable);
  }
};

QTEST_MAIN(TestFieldTableModel)
#include "test_field_table_model.moc"
