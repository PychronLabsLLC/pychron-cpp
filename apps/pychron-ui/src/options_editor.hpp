#pragma once

// OptionsEditor (data browsing and visualization design, sections 6.1, 11.4):
// a form generated from a processing::Schema. One tab per section; one editor
// per field type; each list (panels, groups, ...) is a row list with add,
// remove and move buttons and a form for the selected row. Every edit is
// validated through Options::set; an invalid value is marked and not applied.
//
//   Bool -> check box        Int -> spin box          Double -> line edit
//   Enum -> combo box        Color -> swatch button   Quantity -> editable combo
//   String -> line edit      StringList -> comma-separated line edit
// Optional fields accept an empty entry (unset).

#include <functional>
#include <map>

#include <QStringList>
#include <QWidget>

#include "pychron/processing/options.hpp"

class QFormLayout;
class QListWidget;
class QTabWidget;

namespace pychron::ui {

class OptionsEditor : public QWidget {
  Q_OBJECT

 public:
  explicit OptionsEditor(QWidget* parent = nullptr);

  // Rebuilds the form for `options` (keeps the current tab).
  void set_options(const processing::Options& options);
  const processing::Options& options() const noexcept { return options_; }

  // Completion list for Quantity fields (available_quantities of the data).
  void set_quantity_choices(const QStringList& choices);

  // For tests: the editor of a top-level field, or of `row`'s field in a list.
  QWidget* editor(const QString& key) const;
  QWidget* row_editor(const QString& list, const QString& key) const;
  QListWidget* rows(const QString& list) const;
  QTabWidget* tabs() const noexcept { return tabs_; }
  // Applies a value as if typed by the user; false if invalid.
  bool apply(const QString& key, const processing::OptionValue& value);

 signals:
  void changed();

 private:
  struct ListUi {
    QListWidget* list = nullptr;
    QWidget* form_host = nullptr;
    std::map<QString, QWidget*> editors;
  };

  void rebuild();
  void refresh_enabled();
  void build_row_form(const QString& list_key);
  QWidget* make_editor(const processing::FieldSpec& field, const processing::Options& values,
                       std::function<Result<void>(processing::OptionValue)> set);
  QString row_label(const processing::Options& row, int index) const;

  processing::Options options_;
  QStringList quantities_;
  QTabWidget* tabs_;
  std::map<QString, QWidget*> editors_;
  std::map<QString, ListUi> lists_;
  bool building_ = false;
};

}  // namespace pychron::ui
