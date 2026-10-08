#pragma once

// The unknowns table of the flux window (flux window design, section 5.3): the
// positions that are not monitors, with the saved J and the J the fit predicts.
// A click on Save is reported as a signal; the window refits and resets the model.

#include <set>

#include <QAbstractTableModel>

#include "pychron/processing/flux_fit.hpp"

namespace pychron::ui {

class FluxUnknownModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { Save, Hole, Identifier, Sample, SavedJ, SavedJErr, PredJ, PredJErr, PredPercent, Dev, ColumnCount };

  explicit FluxUnknownModel(QObject* parent = nullptr);

  // The pointers stay the window's; each call resets the model. A null fit
  // shows the unknowns of the inputs with the predicted cells blank.
  void set_fit(const processing::LevelFit* fit);
  void set_inputs(const processing::LevelInputs* inputs);
  void set_skip(const std::set<int>& skip_positions);

  int hole_at(int row) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 Q_SIGNALS:
  void save_toggled(int hole, bool save);

 private:
  void rebuild();
  const processing::FittedPosition* fitted(int row) const;
  const processing::LevelPosition* input(int row) const;

  const processing::LevelFit* fit_ = nullptr;
  const processing::LevelInputs* inputs_ = nullptr;
  std::set<int> skip_;
  std::vector<std::size_t> rows_;
};

}  // namespace pychron::ui
