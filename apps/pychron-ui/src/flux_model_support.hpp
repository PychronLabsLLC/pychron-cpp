#pragma once

// What the flux window's table models share: the cell text (the shared
// formatters give "-" for an absent value, a cell shows nothing) and the base of
// the two models that list a level's positions.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QAbstractTableModel>
#include <QString>

#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/flux_view.hpp"

namespace pychron::ui {

inline QString flux_cell(const std::string& formatted) {
  return formatted == "-" ? QString() : QString::fromStdString(formatted);
}
// `%.4e`, empty when absent.
inline QString flux_j_cell(const std::optional<double>& v) { return flux_cell(processing::flux_j_text(v)); }
// `%.2f`, empty when absent (MSWD, Dev %).
inline QString flux_fixed2_cell(const std::optional<double>& v) { return flux_cell(processing::flux_pct_text(v)); }
// err / value x 100 as `%.2f`, empty when either is absent.
inline QString flux_percent_cell(const std::optional<double>& err, const std::optional<double>& value) {
  return flux_cell(processing::flux_percent_of(err, value));
}

// The rows of a level's monitors or of its unknowns: from the fit when there is
// one, else from the inputs (the fit's cells are then blank). The pointers stay
// the window's; each set_* resets the model.
class FluxPositionModel : public QAbstractTableModel {
 public:
  void set_fit(const processing::LevelFit* fit);
  void set_inputs(const processing::LevelInputs* inputs);
  void set_skip(const std::set<int>& skip_positions);

  int hole_at(int row) const;
  int row_of(int hole) const;

  int rowCount(const QModelIndex& parent = {}) const override;

 protected:
  FluxPositionModel(bool monitors, QObject* parent) : QAbstractTableModel(parent), monitors_(monitors) {}

  // Exactly one is non-null for a valid row: the fitted position, or the input one when there is no fit.
  const processing::FittedPosition* fitted(int row) const;
  const processing::LevelPosition* input(int row) const;
  bool saved(int hole) const { return skip_.count(hole) == 0; }

 private:
  void rebuild();

  bool monitors_;
  const processing::LevelFit* fit_ = nullptr;
  const processing::LevelInputs* inputs_ = nullptr;
  std::set<int> skip_;
  std::vector<std::size_t> rows_;
};

}  // namespace pychron::ui
