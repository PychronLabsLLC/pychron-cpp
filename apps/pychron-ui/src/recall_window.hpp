#pragma once

// RecallWindow (data browsing and visualization design, section 11.3): one
// analysis in depth. Tabs: Summary (computed values and corrected ratios),
// Isotopes (any correction stage, or all side by side), Evolutions (raw
// signal, baseline and sniff with the stored fit), Error budget, Identity,
// Extraction, Spectrometer. Everything shown comes from processing::
// RecallModel and make_evolution_scene.

#include <QString>
#include <QWidget>

#include "pychron/processing/recall.hpp"
#include "pychron/processing/source.hpp"

class QComboBox;
class QLabel;
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
  // the source cannot load it.
  bool show_analysis(const QString& uuid);
  QString uuid() const { return uuid_; }

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

 private:
  void fill_isotopes();
  void fill_evolutions();

  processing::IAnalysisSource& source_;
  QString uuid_;
  processing::AnalysisPtr analysis_;
  processing::RawData raw_;
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
};

}  // namespace pychron::ui
