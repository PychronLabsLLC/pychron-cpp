#pragma once

// The samples of the Samples window (entry spec, section 9.1): stored rows
// whose cells can be edited, and new samples staged for the next save. Edits
// stay in the model until saved; to_batch() turns them into one catalog edit
// batch with the loaded values as expected (E2).

#include <map>
#include <optional>
#include <set>
#include <vector>

#include <QAbstractTableModel>

#include "entry_headers.hpp"

namespace pychron::ui {

// A sample entered in the new-sample form. PI, project and material are named
// by text: what the catalog lacks is created on save.
struct NewSample {
  std::string name, principal_investigator, project, material, grainsize;
  persistence::SampleFields fields;
};

class SampleTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column {
    Name,
    Project,
    PrincipalInvestigator,
    Material,
    Grainsize,
    Lat,
    Lon,
    Elevation,
    Unit,
    Lithology,
    Location,
    Storage,
    Igsn,
    Note,
    Positions,
    Analyses,
    ColumnCount
  };

  explicit SampleTableModel(QObject* parent = nullptr);

  // Replaces the stored rows; edits and new samples are dropped.
  void set_rows(std::vector<persistence::SampleRow> rows);
  const std::vector<persistence::SampleRow>& rows() const noexcept { return rows_; }
  // Stored rows first, then the new ones.
  int stored_count() const noexcept { return static_cast<int>(rows_.size()); }
  bool is_new(int row) const { return row >= stored_count(); }
  const persistence::SampleRow* stored(int row) const;

  void add_new(NewSample sample);
  const std::vector<NewSample>& new_samples() const noexcept { return new_; }
  // Marks a stored row for deletion (only rows with no positions or analyses).
  bool mark_delete(int row);
  bool dirty() const { return !edits_.empty() || !new_.empty() || !deletes_.empty(); }
  void revert() override;  // drops every edit and new sample

  // Rows another client changed: tinted, the other values in the tooltip.
  void mark_stale(const std::vector<persistence::StaleRow>& stale);
  bool is_stale(int row) const;

  // The edits as one batch. New PIs, projects and materials are inserted
  // first; `catalog` says which exist. Errors: a new sample that names a PI
  // the rules refuse, or an edit that is not a valid value.
  Result<persistence::CatalogEditBatch> to_batch(const entry::CatalogSnapshot& catalog,
                                                 const std::vector<std::string>& pi_names_allowed) const;

  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

 private:
  static const char* column_name(int column);  // the sample column a cell edits, or nullptr
  persistence::CatalogValue stored_value(const persistence::SampleRow& r, int column) const;
  persistence::CatalogValue current_value(int row, int column) const;

  std::vector<persistence::SampleRow> rows_;
  std::vector<NewSample> new_;
  std::map<persistence::Uuid, persistence::CatalogFields> edits_;  // column name -> new value
  std::set<persistence::Uuid> deletes_;
  std::map<persistence::Uuid, persistence::CatalogFields> stale_;
};

}  // namespace pychron::ui
