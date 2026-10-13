#include "flux_model_support.hpp"

#include <utility>

namespace pychron::ui {

void FluxPositionModel::rebuild() {
  rows_.clear();
  if (const auto* positions = evaluated()) {
    for (std::size_t i = 0; i < positions->size(); ++i)
      if ((*positions)[i].monitor == monitors_) rows_.push_back(i);
  } else if (inputs_) {
    for (std::size_t i = 0; i < inputs_->positions.size(); ++i)
      if (inputs_->positions[i].monitor == monitors_) rows_.push_back(i);
  }
}

void FluxPositionModel::set_fit(const processing::LevelFit* fit) {
  beginResetModel();
  fit_ = fit;
  rebuild();
  endResetModel();
}

void FluxPositionModel::set_unfitted(const std::vector<processing::FittedPosition>* positions) {
  beginResetModel();
  unfitted_ = positions;
  rebuild();
  endResetModel();
}

void FluxPositionModel::set_inputs(const processing::LevelInputs* inputs) {
  beginResetModel();
  inputs_ = inputs;
  rebuild();
  endResetModel();
}

void FluxPositionModel::set_skip(const std::set<int>& skip_positions) {
  beginResetModel();
  skip_ = skip_positions;
  endResetModel();
}

const processing::FittedPosition* FluxPositionModel::fitted(int row) const {
  const auto* positions = evaluated();
  if (!positions || row < 0 || std::cmp_greater_equal(row, rows_.size())) return nullptr;
  return &(*positions)[rows_[static_cast<std::size_t>(row)]];
}

const processing::LevelPosition* FluxPositionModel::input(int row) const {
  if (evaluated() || !inputs_ || row < 0 || std::cmp_greater_equal(row, rows_.size())) return nullptr;
  return &inputs_->positions[rows_[static_cast<std::size_t>(row)]];
}

int FluxPositionModel::hole_at(int row) const {
  if (const auto* f = fitted(row)) return f->hole;
  if (const auto* i = input(row)) return i->hole;
  return 0;
}

int FluxPositionModel::row_of(int hole) const {
  for (int r = 0; std::cmp_less(r, rows_.size()); ++r)
    if (hole_at(r) == hole) return r;
  return -1;
}

int FluxPositionModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

}  // namespace pychron::ui
