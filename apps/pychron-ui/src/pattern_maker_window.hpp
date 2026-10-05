#pragma once

// PatternMakerWindow (laser window design, section 6): a laser pattern made
// on screen. Pick a kind, set its fields (only its own are shown), and the
// path is drawn with its length and the time it takes; what could not run is
// said as the parser says it, and cannot be saved. Saving writes
// <dir>/<name>.toml and puts the pattern in the library, so the next run of
// that name uses it (one already running keeps the path it started with).

#include <filesystem>
#include <optional>

#include <QMainWindow>
#include <QString>

#include "pychron/laser/pattern.hpp"

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace pychron::ui {

class PatternPreview;

class PatternMakerWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `library` must outlive the window; `dir` is the lab's patterns directory.
  PatternMakerWindow(laser::PatternLibrary& library, std::filesystem::path dir, QWidget* parent = nullptr);

  // Fills the form from a pattern of the library; nothing for an unknown name.
  void open(const QString& name);
  // What the form holds.
  laser::Pattern pattern() const;
  // Points of the path drawn (every iteration, and the return); 0 for none.
  int preview_points() const;

 signals:
  void saved(const QString& name);

 private:
  void show_pattern(const laser::Pattern& pattern);
  void build_fields(const laser::Pattern& pattern);  // the form, for its kind
  void changed();                                    // the form was edited
  void refresh();                                    // preview, summary, problem, save
  void refresh_open_list();
  void save();

  laser::PatternLibrary& library_;
  std::filesystem::path dir_;
  laser::PatternKind kind_ = laser::PatternKind::Polygon;
  bool filling_ = false;  // the form is being built: not an edit
  QString note_;          // "saved ...", until the next edit

  QComboBox* open_list_ = nullptr;
  QLineEdit* name_ = nullptr;
  QComboBox* kind_choice_ = nullptr;
  QWidget* fields_ = nullptr;  // rebuilt for each kind
  QFormLayout* form_ = nullptr;
  PatternPreview* preview_ = nullptr;
  QLabel* summary_ = nullptr;
  QLabel* problem_ = nullptr;
  QPushButton* save_ = nullptr;
};

}  // namespace pychron::ui
