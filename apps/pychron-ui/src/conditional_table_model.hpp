#pragma once

// ConditionalTableModel (conditionals-editor design 5.1): the conditionals of
// one file, grouped by kind in the order the file is written.
//
// Conditionals are held as authored: an empty name means the default name, and
// `expr` is not kept. Every row carries the error finalize() gives it, or
// "duplicate name"; such rows are drawn in the error colour and keep the file
// from being saved.

#include <vector>

#include <QAbstractTableModel>
#include <QStringList>

#include "pychron/experiment/conditionals/conditional.hpp"

namespace pychron::ui {

class ConditionalTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { Kind, Name, Check, Action, Count };

  explicit ConditionalTableModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  // Replaces everything; this becomes the clean state.
  void set(experiment::ConditionalSet set);
  const experiment::ConditionalSet& conditionals() const noexcept { return set_; }
  void mark_clean();
  bool modified() const;

  // Row operations return the affected row's index, or -1 / false when refused.
  int add(experiment::ConditionalKind kind);  // at the end of its kind
  bool remove(int row);
  int duplicate(int row);   // below `row`; a set name gets "_copy"
  bool move_up(int row);    // within its kind
  bool move_down(int row);
  int replace(int row, experiment::Conditional c);  // moves when the kind changed

  QString effective_name(int row) const;
  int row_of(const QString& effective_name) const;  // -1 when absent

  void set_disable(const QStringList& names);
  QStringList disable() const;

  QString error(int row) const;  // empty when the row is fine
  bool has_errors() const;

 signals:
  void changed();

 private:
  bool valid(int row) const { return row >= 0 && row < rowCount(); }
  void recheck();
  void touched();

  experiment::ConditionalSet set_, clean_;
  std::vector<QString> errors_;
};

// The fields a person writes (not level, location or the compiled check).
bool same_authored(const experiment::Conditional& a, const experiment::Conditional& b);

}  // namespace pychron::ui
