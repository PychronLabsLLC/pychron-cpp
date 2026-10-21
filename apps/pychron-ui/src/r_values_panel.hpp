#pragma once

// The R-values of an ideogram (libs/processing r_values.hpp) as tables, one
// per graph with two groups or more, with the note on correlated errors under
// them and Copy / Export CSV. Computes nothing: the figure window hands it
// the matrices after each run.

#include <vector>

#include <QList>
#include <QWidget>

#include "pychron/processing/options.hpp"
#include "pychron/processing/r_values.hpp"

class QLabel;
class QTableWidget;
class QVBoxLayout;

namespace pychron::ui {

class RValuesPanel : public QWidget {
  Q_OBJECT

 public:
  explicit RValuesPanel(QWidget* parent = nullptr);

  void set_matrices(std::vector<processing::RValueMatrix> matrices, const processing::Options& ideogram_options);
  // Some graph has two groups or more.
  bool has_tables() const;
  const QList<QTableWidget*>& tables() const { return tables_; }
  QString note_text() const;
  // The CSV export (r_values_csv), marked as the user's file; false when it
  // could not be written.
  bool export_csv(const QString& path) const;
  // The tables as shown, tab-separated, to the clipboard.
  void copy() const;

 private:
  std::vector<processing::RValueMatrix> matrices_;
  processing::Options options_;
  int nsigma_ = 1;
  QVBoxLayout* tables_layout_;
  QList<QWidget*> table_widgets_;  // per table: its title and the table
  QList<QTableWidget*> tables_;
  QLabel* note_;
};

}  // namespace pychron::ui
