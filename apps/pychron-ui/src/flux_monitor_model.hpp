#pragma once

// The monitors table of the flux window (flux window design, section 5.3): one
// row per monitor position of a level, with the saved J, the mean of the
// monitor's analyses and the J the fit predicts there. The window owns the fit
// and the inputs; a check-box click is reported as a signal and the window
// refits and resets the model, so data() never changes on setData().

#include <set>

#include <QAbstractTableModel>

#include "pychron/processing/flux_fit.hpp"

namespace pychron::ui {

class FluxMonitorModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { Fit, Save, Hole, Identifier, Sample, N, SavedJ, SavedJErr, MeanJ, MeanJErr, MeanPercent, Mswd,
                PredJ, PredJErr, PredPercent, Dev, ColumnCount };

  explicit FluxMonitorModel(QObject* parent = nullptr);

  // The pointers stay the window's; each call resets the model. A null fit
  // shows the monitors of the inputs with the fit's cells blank.
  void set_fit(const processing::LevelFit* fit);
  void set_inputs(const processing::LevelInputs* inputs);
  void set_skip(const std::set<int>& skip_positions);

  int hole_at(int row) const;
  int row_of(int hole) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 Q_SIGNALS:
  void fit_toggled(int hole, bool in_fit);
  void save_toggled(int hole, bool save);

 private:
  // The rows: indexes into the fit's positions, or into the inputs' when there is no fit.
  void rebuild();
  const processing::FittedPosition* fitted(int row) const;
  const processing::LevelPosition* input(int row) const;

  const processing::LevelFit* fit_ = nullptr;
  const processing::LevelInputs* inputs_ = nullptr;
  std::set<int> skip_;
  std::vector<std::size_t> rows_;
};

}  // namespace pychron::ui
