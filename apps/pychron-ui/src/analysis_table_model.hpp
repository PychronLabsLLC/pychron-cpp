#pragma once

// AnalysisTableModel (design section 11.2; search and display design,
// sections 3.4 and 3.5): browser rows, newest first, tinted by what the user
// chose, with a separator row wherever a spectrometer ran nothing for longer
// than the gap threshold. A separator is a row of the model that is no
// analysis: it cannot be selected and analysis_at() gives null for it.

#include <cstddef>
#include <string>
#include <vector>

#include <QAbstractTableModel>
#include <QStringList>

#include "pychron/processing/source.hpp"
#include "pychron/processing/time_breaks.hpp"
#include "row_colors.hpp"

namespace pychron::ui {

class AnalysisTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { RunId, Type, Sample, Identifier, Spectrometer, Date, Extract, Tag, Project, Irradiation, Uuid, ColumnCount };

  explicit AnalysisTableModel(QObject* parent = nullptr);

  void set_rows(std::vector<processing::AnalysisSummary> rows);
  // Older rows, after those there. Rows already shown keep their place: a
  // break is drawn above the older of its two analyses.
  void append_rows(std::vector<processing::AnalysisSummary> rows);
  // The analyses, without the separators.
  const std::vector<processing::AnalysisSummary>& rows() const noexcept { return rows_; }
  int analysis_count() const noexcept { return static_cast<int>(rows_.size()); }

  // A row of the model (a display row) is an analysis or a separator.
  bool is_break(int display_row) const;
  // Null for a separator and for a row that is not there.
  const processing::AnalysisSummary* analysis_at(int display_row) const;
  // The display row of rows()[analysis_index]; -1 when there is none.
  int display_row_of(std::size_t analysis_index) const;
  // The display rows that are separators, ascending.
  QList<int> break_rows() const;

  void set_coloring(ColorBy by, const TypeColors& types);
  ColorBy color_by() const noexcept { return color_by_; }
  // Zero or less: no separators.
  void set_gap_threshold(double seconds);
  double gap_threshold() const noexcept { return gap_seconds_; }

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

  static QString format_time(double utc_seconds);

 private:
  struct Entry {
    bool is_break = false;
    std::size_t index = 0;  // into breaks_ or rows_
  };

  // Finds the breaks of rows_ again; the entries of rows_[first..] and of the
  // breaks above them.
  std::vector<Entry> entries_from(std::size_t first);
  // The distinct colour keys and whether more than one spectrometer is shown;
  // true when either changed.
  bool update_keys();
  QVariant break_data(const processing::TimeBreak& b, int column, int role) const;

  std::vector<processing::AnalysisSummary> rows_;
  std::vector<processing::TimeBreak> breaks_;
  std::vector<Entry> display_;
  std::vector<std::string> keys_;
  bool several_spectrometers_ = false;
  ColorBy color_by_ = ColorBy::AnalysisType;
  TypeColors type_colors_;
  double gap_seconds_ = 0.0;
};

}  // namespace pychron::ui
