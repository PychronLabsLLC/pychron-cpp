#pragma once

// The monitors table of the flux window (flux window design, section 5.3): one
// row per monitor position of a level, with the saved J, the mean of the
// monitor's analyses and the J the fit predicts there. The window owns the fit
// and the inputs; a check-box click is reported as a signal and the window
// refits and resets the model, so data() never changes on setData().

#include "flux_model_support.hpp"

namespace pychron::ui {

class FluxMonitorModel : public FluxPositionModel {
  Q_OBJECT

 public:
  enum Column { Fit, Save, Hole, Identifier, Sample, N, SavedJ, SavedJErr, MeanJ, MeanJErr, MeanPercent, Mswd,
                PredJ, PredJErr, PredPercent, Dev, ColumnCount };

  explicit FluxMonitorModel(QObject* parent = nullptr);

  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 Q_SIGNALS:
  void fit_toggled(int hole, bool in_fit);
  void save_toggled(int hole, bool save);
};

}  // namespace pychron::ui
