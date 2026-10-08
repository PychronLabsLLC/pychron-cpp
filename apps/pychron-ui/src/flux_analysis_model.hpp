#pragma once

// The analyses of the monitor selected in the flux window (flux window design,
// section 5.3): each with its J and why it is in the mean or out of it. A click
// on Use is reported as a signal; the window refits and resets the model.

#include <QAbstractTableModel>

#include "pychron/processing/flux_fit.hpp"

namespace pychron::ui {

class FluxAnalysisModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { Use, Record, Tag, J, JErr, State, ColumnCount };

  explicit FluxAnalysisModel(QObject* parent = nullptr);

  // The position stays the window's; each call resets the model. nullptr: empty.
  void set_position(const processing::FittedPosition* position);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 Q_SIGNALS:
  void use_toggled(const QString& uuid, bool use);

 private:
  const processing::FittedPosition::UsedAnalysis* at(int row) const;

  const processing::FittedPosition* position_ = nullptr;
};

}  // namespace pychron::ui
