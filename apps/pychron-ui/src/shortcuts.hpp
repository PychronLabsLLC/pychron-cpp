#pragma once

// Every keyboard shortcut in pychron-ui, in one place. Windows take their keys
// from here (key(Shortcut::SaveQueue)) rather than spelling them out, so
// Help > Keyboard Shortcuts, which lists this catalog, is always what the
// keys do. tests/ui/test_shortcuts.cpp checks that no two entries that can be
// live at once share a key, and that the menus use these keys.

#include <vector>

#include <QDialog>
#include <QKeySequence>
#include <QString>

class QAction;
class QLineEdit;
class QTreeWidget;

namespace pychron::ui {

enum class Shortcut {
  // Everywhere (the unified menu bar).
  Preferences,
  Quit,
  ExtractionLineWindow,
  SpectrometerWindow,
  ExperimentWindow,
  DataWindow,
  KeyboardShortcuts,
  CommandPalette,
  // The experiment window.
  OpenQueue,
  SaveQueue,
  MoveRowsUp,
  MoveRowsDown,
  DuplicateRows,
  DeleteRows,
  ToggleSkip,
  EndAfter,
  AddRuns,
  StartQueue,
  ScriptEditor,
  // The script editor.
  NewScript,
  SaveScript,
  CloseScriptTab,
  CheckScript,
  GoToGosub,
  // The conditionals editor.
  SaveConditionals,
  // The data browser.
  RecallNext,
  RecallPrevious,
};

// Where a shortcut works. Everywhere overlaps every window; the others are
// separate windows, so their keys may repeat between them.
enum class ShortcutContext { Everywhere, ExperimentWindow, ScriptEditor, ConditionalsEditor, DataBrowser };

struct ShortcutEntry {
  Shortcut id;
  ShortcutContext context;
  QString command;  // as the menu names it, without the mnemonic
  QKeySequence key;
};

const std::vector<ShortcutEntry>& shortcut_catalog();
QKeySequence key(Shortcut id);
QString context_name(ShortcutContext context);

// Help > Keyboard Shortcuts: the catalog by where it works, with a filter.
class ShortcutsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit ShortcutsDialog(QWidget* parent = nullptr);

  QLineEdit* filter() const { return filter_; }
  QTreeWidget* tree() const { return tree_; }

 private:
  void apply_filter(const QString& text);

  QLineEdit* filter_;
  QTreeWidget* tree_;
};

// Help > Keyboard Shortcuts, owned by `window` (the caller puts it in the
// unified bar); the dialog is built on first use as its child.
QAction* make_shortcuts_action(QWidget* window);

}  // namespace pychron::ui
