#pragma once

// AnalysisTableModel (design section 11.2): browser rows, newest first.

#include <vector>

#include <QAbstractTableModel>
#include <QStringList>

#include "pychron/processing/source.hpp"

namespace pychron::ui {

class AnalysisTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { RunId, Type, Sample, Identifier, Spectrometer, Date, Extract, Tag, Project, Irradiation, Uuid, ColumnCount };

  explicit AnalysisTableModel(QObject* parent = nullptr);

  void set_rows(std::vector<processing::AnalysisSummary> rows);
  void append_rows(std::vector<processing::AnalysisSummary> rows);
  const std::vector<processing::AnalysisSummary>& rows() const noexcept { return rows_; }
  const processing::AnalysisSummary& row(int r) const { return rows_.at(static_cast<std::size_t>(r)); }

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  static QString format_time(double utc_seconds);

 private:
  std::vector<processing::AnalysisSummary> rows_;
};

}  // namespace pychron::ui
