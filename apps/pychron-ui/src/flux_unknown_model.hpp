#pragma once

// The unknowns table of the flux window (flux window design, section 5.3): the
// positions that are not monitors, with the saved J and the J the fit predicts.
// A click on Save is reported as a signal; the window refits and resets the model.

#include "flux_model_support.hpp"

namespace pychron::ui {

class FluxUnknownModel : public FluxPositionModel {
  Q_OBJECT

 public:
  enum Column { Save, Hole, Identifier, Sample, SavedJ, SavedJErr, PredJ, PredJErr, PredPercent, Dev, ColumnCount };

  explicit FluxUnknownModel(QObject* parent = nullptr);

  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 Q_SIGNALS:
  void save_toggled(int hole, bool save);
};

}  // namespace pychron::ui
