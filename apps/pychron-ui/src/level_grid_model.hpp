#pragma once

// The positions grid of the Packages window (entry spec, section 9.3): one
// row per hole of the level's holder, plus orphans, over a LevelSheetEdit.
// Weight, packet and note are edited in the cell; everything else through
// the window's actions.

#include <optional>

#include <QAbstractTableModel>

#include "entry_headers.hpp"

namespace pychron::ui {

class LevelGridModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { Analyzed, Position, Packet, Identifier, Sample, Project, PI, Material, Grainsize, Weight, J, JErr, Note, ColumnCount };

  explicit LevelGridModel(QObject* parent = nullptr);

  // Takes the level being edited; nullopt empties the grid.
  void set_edit(std::optional<entry::LevelSheetEdit> edit);
  bool has_edit() const noexcept { return edit_.has_value(); }
  const entry::LevelSheetEdit& edit() const { return *edit_; }
  // Runs `change` on the edit and refreshes the grid.
  template <class F>
  void modify(F&& change) {
    if (!edit_) return;
    beginResetModel();
    change(*edit_);
    endResetModel();
    Q_EMIT edited();
  }
  // The position (1-based hole) of a grid row; 0 for none.
  int position_at(int row) const;
  int row_of(int position) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 Q_SIGNALS:
  void edited();

 private:
  std::optional<entry::LevelSheetEdit> edit_;
};

}  // namespace pychron::ui
