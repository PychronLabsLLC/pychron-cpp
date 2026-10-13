#pragma once

// OptionsEditor (data browsing and visualization design, sections 6.1, 11.4):
// a form generated from a processing::Schema. One page per section, chosen
// from a row of buttons that wraps onto further rows in a narrow dock, so
// every section is always in sight (a tab bar scrolls the ones that do not
// fit out of view); one editor
// per field type; each list (panels, groups, ...) is a row list with add,
// remove and move buttons and a form for the selected row. Every edit is
// validated through Options::set; an invalid value is marked and not applied.
//
//   Bool -> check box        Int -> spin box          Double -> line edit
//   Enum -> combo box        Color -> swatch button   Quantity -> editable combo
//   String -> line edit      StringList -> comma-separated line edit
//   Font -> combo box of the installed families ("Default" is unset)
// Optional fields accept an empty entry (unset).

#include <functional>
#include <map>

#include <QStringList>
#include <QWidget>

#include "pychron/processing/options.hpp"

class QFormLayout;
class QListWidget;
class QAbstractButton;
class QButtonGroup;
class QStackedWidget;

namespace pychron::ui {

class OptionsEditor : public QWidget {
  Q_OBJECT

 public:
  explicit OptionsEditor(QWidget* parent = nullptr);

  // Rebuilds the form for `options` (keeps the current section).
  void set_options(const processing::Options& options);
  const processing::Options& options() const noexcept { return options_; }

  // Completion list for Quantity fields (available_quantities of the data).
  void set_quantity_choices(const QStringList& choices);

  // For tests: the editor of a top-level field, or of `row`'s field in a list.
  QWidget* editor(const QString& key) const;
  QWidget* row_editor(const QString& list, const QString& key) const;
  QListWidget* rows(const QString& list) const;
  // Sections, in schema order; the button of each, and which one is shown.
  QStringList sections() const;
  QAbstractButton* section_button(int index) const;
  int current_section() const;
  void set_current_section(int index);
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
  QWidget* section_bar_;
  QButtonGroup* section_buttons_;
  QStackedWidget* pages_;
  std::map<QString, QWidget*> editors_;
  std::map<QString, ListUi> lists_;
  bool building_ = false;
};

}  // namespace pychron::ui
