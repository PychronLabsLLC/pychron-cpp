#pragma once

// RecallWindow (data browsing and visualization design, section 11.3): one
// analysis in depth. Tabs: Summary (computed values and corrected ratios),
// Isotopes (any correction stage, or all side by side), Evolutions (raw
// signal, baseline and sniff with the stored fit), Error budget, Identity,
// Extraction, Spectrometer, History. Everything shown comes from processing::
// RecallModel and make_evolution_scene.
//
// Fit editing (Evolutions, signals): change an isotope's fit, error type or
// outlier filter, or click points to leave them out; the window refits and
// recomputes everything as a pending edit. Save commits the edits as a new
// intercepts revision when the source keeps revisions (the database), on top
// of the head they were made on; if someone else saved first nothing is
// written and the status says who. History lists the revisions of each kind
// and shows one, or the differences between two.

#include <map>

#include <QString>
#include <QWidget>

#include "pychron/processing/fit_edit.hpp"
#include "pychron/processing/recall.hpp"
#include "pychron/processing/revisions.hpp"
#include "pychron/processing/source.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabBar;
class QTabWidget;
class QTableWidget;

namespace pychron::ui {

class SceneView;

class RecallWindow : public QWidget {
  Q_OBJECT

 public:
  // `source` must outlive the window.
  RecallWindow(processing::IAnalysisSource& source, QWidget* parent = nullptr);

  // Loads, reduces and shows `uuid`; false (and the error in the title) when
  // the source cannot load it. Pending fit edits are dropped.
  bool show_analysis(const QString& uuid);
  QString uuid() const { return uuid_; }

  bool has_pending_edits() const noexcept { return !edits_.empty(); }
  // Saves the pending fit edits; false when nothing was saved (the reason is
  // in edit_status()).
  bool save_edits();
  void revert_edits();

  // For tests.
  QTabWidget* tabs() const noexcept { return tabs_; }
  QTableWidget* computed_table() const noexcept { return computed_; }
  QTableWidget* isotope_table() const noexcept { return isotopes_; }
  QTableWidget* budget_table() const noexcept { return budget_; }
  QComboBox* stage_selector() const noexcept { return stage_; }
  SceneView* evolutions() const noexcept { return evolutions_; }
  QTabBar* evolution_kind() const noexcept { return kind_; }
  QLabel* title_label() const noexcept { return title_; }
  const processing::RecallModel& model() const noexcept { return model_; }
  const processing::AnalysisPtr& shown() const noexcept { return shown_; }
  QGroupBox* fit_editor() const noexcept { return fit_box_; }
  QComboBox* fit_isotope() const noexcept { return fit_isotope_; }
  QComboBox* fit_kind() const noexcept { return fit_kind_; }
  QComboBox* fit_error() const noexcept { return fit_error_; }
  QCheckBox* fit_outliers() const noexcept { return fit_outliers_; }
  QPushButton* save_button() const noexcept { return save_; }
  QPushButton* revert_button() const noexcept { return revert_; }
  QLabel* edit_status() const noexcept { return edit_status_; }
  QWidget* history_page() const noexcept { return history_page_; }
  QComboBox* history_kind() const noexcept { return history_kind_; }
  QTableWidget* revision_list() const noexcept { return revisions_; }
  QTableWidget* revision_content() const noexcept { return revision_content_; }
  QLabel* history_note() const noexcept { return history_note_; }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void display();  // everything from shown_
  void recompute();
  void fill_isotopes();
  void fill_evolutions();
  void fill_fit_editor();
  void fit_controls_changed();
  void toggle_points(const QStringList& refs);
  void update_edit_state();
  void fill_history();
  void show_revisions();
  processing::FitEdit current_edit(const std::string& key) const;

  processing::IAnalysisSource& source_;
  QString uuid_;
  processing::AnalysisPtr analysis_;  // as loaded
  processing::AnalysisPtr shown_;     // with pending edits
  std::map<std::string, processing::FitEdit> edits_;
  std::vector<processing::EditedIsotope> edited_;
  processing::RawData raw_;
  std::vector<processing::RevisionSummary> history_;
  bool history_stale_ = true;
  processing::RecallModel model_;
  QLabel* title_;
  QTabWidget* tabs_;
  QTableWidget* computed_;
  QTableWidget* ratios_;
  QComboBox* stage_;
  QTableWidget* isotopes_;
  QTabBar* kind_;
  SceneView* evolutions_;
  QTableWidget* budget_;
  QTableWidget* identity_;
  QTableWidget* extraction_;
  QTableWidget* spectrometer_;
  QGroupBox* fit_box_;
  QComboBox* fit_isotope_;
  QComboBox* fit_kind_;
  QComboBox* fit_error_;
  QCheckBox* fit_outliers_;
  QSpinBox* fit_iterations_;
  QDoubleSpinBox* fit_std_devs_;
  QPushButton* clear_excluded_;
  QPushButton* revert_;
  QPushButton* save_;
  QLabel* edit_status_;
  QWidget* history_page_;
  QComboBox* history_kind_;
  QTableWidget* revisions_;
  QTableWidget* revision_content_;
  QLabel* history_note_;
};

}  // namespace pychron::ui
