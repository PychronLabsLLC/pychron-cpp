#pragma once

// DataBrowserWindow (data browsing and visualization design, section 11.2):
// a toolbar of what can be done with the selection, filter lists fed by the
// source's facets, a search box, a date preset, and the analyses table,
// newest first, paged with "Load more".
//
//   double-click / Enter     recall_requested(uuid)
//   Ctrl+N / Ctrl+B          select the next / previous row and recall it
//   toolbar, a figure        figure_requested(kind, selected uuids or all shown);
//                            kind: time_series, ideogram, spectrum, inverse_isochron,
//                            then the fit windows isotope_evolution_fit, blank_fit,
//                            icfactor_fit
//   toolbar, Recall          recall_requested(uuid of the current row)
//   toolbar, Export          export_requested(path, selected uuids or all shown): one
//                            click asks for a file name and the workspace writes the
//                            Schaen et al. (2021) data report there (.csv or .json)

#include <functional>
#include <map>
#include <optional>

#include <QStringList>
#include <QWidget>

#include "analysis_table_model.hpp"
#include "preferences.hpp"
#include "pychron/processing/source.hpp"

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableView;
class QToolBar;
class QAction;

namespace pychron::ui {

class DataBrowserWindow : public QWidget {
  Q_OBJECT

 public:
  // `source` must outlive the window.
  explicit DataBrowserWindow(processing::IAnalysisSource& source, QWidget* parent = nullptr);

  // Rescans the source and reloads the first page.
  void refresh();
  // Analyses per page (File > Preferences…), at least 1; a change reloads the
  // first page.
  void set_page_size(int rows);
  int page_size() const noexcept { return page_size_; }
  processing::BrowseQuery query() const;

  // For tests.
  AnalysisTableModel* model() const noexcept { return model_; }
  QTableView* table() const noexcept { return table_; }
  QLineEdit* search() const noexcept { return search_; }
  QComboBox* date_preset() const noexcept { return dates_; }
  QListWidget* facet_list(processing::Facet f) const;
  QPushButton* load_more_button() const noexcept { return more_; }
  QToolBar* toolbar() const noexcept { return toolbar_; }
  // The toolbar's action for a figure kind; null for a kind it does not offer.
  QAction* plot_action(const QString& kind) const;
  QAction* recall_action() const noexcept { return recall_; }
  QAction* export_action() const noexcept { return export_; }
  QLabel* status() const noexcept { return status_; }
  // A line for the status label (the workspace reports an export here).
  void show_message(const QString& text);
  // The file name Export asks for; tests replace the dialog. Empty: cancelled.
  std::function<QString(const QString& suggested)> ask_export_path;
  QStringList selected_uuids() const;
  void select_rows(const QList<int>& rows);
  void recall_step(int delta);  // Ctrl+N (+1) / Ctrl+B (-1)

 signals:
  void recall_requested(const QString& uuid);
  void figure_requested(const QString& kind, const QStringList& uuids);
  void export_requested(const QString& path, const QStringList& uuids);

 private:
  void reload();     // first page for the current query
  void load_more();  // next page
  void update_facets();
  void recall_current();
  void request_figure(const QString& kind);
  void request_export();

  processing::IAnalysisSource& source_;
  AnalysisTableModel* model_;
  QTableView* table_;
  QLineEdit* search_;
  QComboBox* dates_;
  QCheckBox* exclude_invalid_;
  std::map<processing::Facet, QListWidget*> facets_;
  QPushButton* more_;
  QToolBar* toolbar_;
  QAction* recall_;
  QAction* export_;
  QLabel* status_;
  int page_size_ = Preferences::kDefaultPageSize;
  std::optional<processing::BrowseCursor> next_;
  std::optional<std::size_t> total_;
  bool updating_ = false;
};

}  // namespace pychron::ui
