#pragma once

// DataBrowserWindow (data browsing and visualization design, section 11.2):
// a toolbar of what can be done with the selection, filter lists fed by the
// source's facets, a search box, a date preset or range, and the analyses
// table, newest first, paged with "Load more". Search and display design
// (2026-10-09): the date range, the Irradiation and Level lists, a text
// filter on every list (FacetBox), the row tint chosen under "Color by",
// and a separator row where a spectrometer ran nothing for longer than the
// time-break preference.
//
// Two lists: Filtered, what the query matches, and Selected, analyses kept
// across queries (browser selection design, 2026-10-19). A Filtered row
// already in Selected carries a check mark.
//
//   Add → / Enter (Filtered) the highlighted Filtered rows into Selected
//   ← Remove / Delete, Backspace (Selected)
//                            the highlighted Selected rows out
//   Clear                    Selected emptied
//   double-click / Space     recall_requested(uuid of the current row of that table)
//   Ctrl+N / Ctrl+B          select the next / previous row of the table with
//                            focus and recall it
//   toolbar, a figure        figure_requested(kind, Selected's uuids);
//                            kind: time_series, ideogram, spectrum, inverse_isochron,
//                            spectrum_isochron, then the fit windows isotope_evolution_fit, blank_fit,
//                            icfactor_fit
//   toolbar, Recall          recall_requested(uuid of the current row)
//   toolbar, Publish         publish_requested(Selected's uuids): Publish to EarthBank
//   toolbar, Export          export_requested(path, Selected's uuids): one
//                            click asks for a file name and the workspace writes the
//                            Schaen et al. (2021) data report there (.csv or .json)

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

#include <QStringList>
#include <QWidget>

#include "analysis_table_model.hpp"
#include "facet_box.hpp"
#include "preferences.hpp"
#include "selection_list.hpp"
#include "pychron/processing/source.hpp"

class QComboBox;
class QCheckBox;
class QDateTimeEdit;
class QDoubleSpinBox;
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

  // Rescans the source and reloads the first page. The analyses selected and
  // the current one stay so when they are still listed, and the table then
  // stays where it was scrolled to: a refresh can come while someone reads
  // (a run ended).
  void refresh();
  // What a run ending asks for (nobody pressed anything). The source is asked
  // to look again, and the table is loaded again only when the source then
  // has something it did not have at the last load: as many rows as were
  // loaded (not the first page only), the selection and the place kept, the
  // column widths left alone. While the window is not visible nothing is
  // asked at all: it is done once, when the window is next shown.
  void refresh_if_changed();
  // Why the source has less than was expected of it (a database that could
  // not be opened), on a banner over the table; empty: no banner.
  void set_unavailable(const QString& reason);
  // Analyses per page (File > Preferences…), at least 1; a change reloads the
  // first page.
  void set_page_size(int rows);
  int page_size() const noexcept { return page_size_; }
  // A separator row where a spectrometer ran nothing for longer than this
  // (File > Preferences…); zero or less: none.
  void set_gap_hours(double hours);
  // The colours of the analysis types under "Color by: Analysis type".
  void set_type_colors(const TypeColors& colors);
  // What the rows are tinted by; color_by_changed() when it changes.
  void set_color_by(ColorBy by);
  ColorBy color_by() const noexcept { return model_->color_by(); }
  processing::BrowseQuery query() const;

  // For tests. model() and table() are Filtered's.
  AnalysisTableModel* model() const noexcept { return model_; }
  QTableView* table() const noexcept { return table_; }
  AnalysisTableModel* selected_model() const noexcept { return selected_model_; }
  QTableView* selected_table() const noexcept { return selected_table_; }
  QPushButton* add_button() const noexcept { return add_; }
  QPushButton* add_all_button() const noexcept { return add_all_; }
  QPushButton* remove_button() const noexcept { return remove_; }
  QPushButton* clear_button() const noexcept { return clear_; }
  QLabel* selected_count() const noexcept { return selected_count_; }
  QLineEdit* search() const noexcept { return search_; }
  QComboBox* date_preset() const noexcept { return dates_; }
  QListWidget* facet_list(processing::Facet f) const;
  FacetBox* facet_box(processing::Facet f) const;
  QComboBox* color_by_box() const noexcept { return color_by_; }
  // The date range (shown when the preset is "Range…"): each end counts only
  // while its box is ticked.
  QWidget* date_range() const noexcept { return range_; }
  QCheckBox* from_enabled() const noexcept { return from_on_; }
  QCheckBox* to_enabled() const noexcept { return to_on_; }
  QDateTimeEdit* from_edit() const noexcept { return from_; }
  QDateTimeEdit* to_edit() const noexcept { return to_; }
  // The cached age range (Ma), under the dates: each end counts only while
  // its box is ticked.
  QCheckBox* age_min_enabled() const noexcept { return age_min_on_; }
  QCheckBox* age_max_enabled() const noexcept { return age_max_on_; }
  QDoubleSpinBox* age_min_edit() const noexcept { return age_min_; }
  QDoubleSpinBox* age_max_edit() const noexcept { return age_max_; }
  QPushButton* load_more_button() const noexcept { return more_; }
  QToolBar* toolbar() const noexcept { return toolbar_; }
  // The toolbar's action for a figure kind; null for a kind it does not offer.
  QAction* plot_action(const QString& kind) const;
  QAction* recall_action() const noexcept { return recall_; }
  QAction* export_action() const noexcept { return export_; }
  QAction* publish_action() const noexcept { return publish_; }  // enabled with a selection
  QLabel* status() const noexcept { return status_; }
  QLabel* unavailable() const noexcept { return unavailable_; }
  // A line for the status label (the workspace reports an export here).
  void show_message(const QString& text);
  // The file name Export asks for; tests replace the dialog. Empty: cancelled.
  std::function<QString(const QString& suggested)> ask_export_path;
  // Asked before Add all adds more than 1000 analyses; tests replace the
  // dialog. False: nothing is added.
  std::function<bool(std::size_t count)> confirm_add_all;
  // The highlighted rows of Filtered, in the table's order.
  QStringList highlighted_uuids() const;
  // Selected's uuids, sorted.
  QStringList selection_uuids() const;
  // Add →: the highlighted Filtered rows into Selected.
  void add_highlighted();
  // Add all: every analysis the current query matches, all pages.
  void add_all();
  // ← Remove: the highlighted Selected rows out of it.
  void remove_highlighted();
  // Clear: Selected emptied.
  void clear_selected();
  // By index among the analyses (model()->rows()), not by row of the table.
  void select_rows(const QList<int>& rows);
  // The same for Selected: by index among selected_model()->rows().
  void select_selected_rows(const QList<int>& rows);
  void recall_step(int delta);  // Ctrl+N (+1) / Ctrl+B (-1)

 protected:
  void showEvent(QShowEvent* event) override;
  // The keys of the two tables: Enter in Filtered, Delete and Backspace in
  // Selected, Space in either. Caught before the view's own handling (Enter
  // would otherwise emit activated, Space toggle the selection).
  bool eventFilter(QObject* watched, QEvent* event) override;

 signals:
  void recall_requested(const QString& uuid);
  void figure_requested(const QString& kind, const QStringList& uuids);
  void export_requested(const QString& path, const QStringList& uuids);
  void publish_requested(const QStringList& uuids);
  void color_by_changed(pychron::ui::ColorBy by);

 private:
  // What the user has in front of them, to be found again after a load.
  struct Place {
    QStringList selected;      // uuids
    std::string current;       // uuid; empty: none
    int scroll = 0;            // the vertical scroll bar's value
    int analyses = 0;
  };
  Place place() const;
  // Selects what of `was` is still listed; true when all of it is.
  bool select_again(const Place& was);
  // Selected's rows fetched again by uuid, for a store that moved; never on a filter change.
  void refetch_selected();
  void reload_changed();  // Filtered, everything loaded, fetched again for a store that moved
  void refresh_changed();  // refresh_if_changed() for a window that is visible
  void reload();     // first page for the current query
  void load_more();  // next page
  void update_facets();
  void show_count();
  // Each separator row of `view` over the whole width, from display row `first` on.
  void span_breaks(QTableView* view, int first);
  // Sets up a table of one of the two lists.
  void set_up_table(QTableView* view, AnalysisTableModel* model);
  AnalysisTableModel* model_of(const QTableView* view) const;
  // The table with focus: Selected's when it has it, else Filtered's.
  QTableView* focused_table() const;
  // By index among the analyses of `view`'s model.
  void select_in(QTableView* view, const QList<int>& rows);
  // Selected shown again after it changed: its table, the mark on Filtered, the count.
  void sync_selected();
  // Adds rows to Selected and says how many were new.
  void add_to_selected(std::vector<processing::AnalysisSummary> rows);
  // Figures, Export and Publish: on exactly while Selected holds something.
  void set_actions_enabled(bool on);
  void date_preset_changed();
  bool range_chosen() const;
  bool range_is_backwards() const;
  bool age_bound_set() const;
  bool age_is_backwards() const;
  // Either range low above high: nothing can be asked.
  bool query_is_backwards() const { return range_is_backwards() || age_is_backwards(); }
  void recall_current();  // in focused_table()
  void recall_in(const QTableView* view);
  void recall_step_in(QTableView* view, int delta);
  void request_figure(const QString& kind);
  void request_export();

  processing::IAnalysisSource& source_;
  AnalysisTableModel* model_;
  QTableView* table_;
  SelectionList selection_;
  AnalysisTableModel* selected_model_;
  QTableView* selected_table_;
  QLabel* selected_count_;
  QPushButton* add_;
  QPushButton* add_all_;
  QPushButton* remove_;
  QPushButton* clear_;
  QLineEdit* search_;
  QComboBox* dates_;
  QWidget* range_;
  QCheckBox* from_on_;
  QCheckBox* to_on_;
  QDateTimeEdit* from_;
  QDateTimeEdit* to_;
  bool range_seeded_ = false;
  QCheckBox* age_min_on_;
  QCheckBox* age_max_on_;
  QDoubleSpinBox* age_min_;
  QDoubleSpinBox* age_max_;
  QComboBox* color_by_;
  QCheckBox* exclude_invalid_;
  std::map<processing::Facet, FacetBox*> facets_;
  QPushButton* more_;
  QToolBar* toolbar_;
  QAction* recall_;
  QAction* export_;
  QAction* publish_;
  QLabel* status_;
  QLabel* unavailable_;
  int page_size_ = Preferences::kDefaultPageSize;
  TypeColors type_colors_;
  std::optional<processing::BrowseCursor> next_;
  std::optional<std::size_t> total_;
  std::uint64_t loaded_generation_ = 0;    // the source's generation at the last load
  std::uint64_t selected_generation_ = 0;  // ... and at the last fetch of Selected
  bool stale_ = false;                   // a refresh was asked for while the window was not visible
};

}  // namespace pychron::ui
