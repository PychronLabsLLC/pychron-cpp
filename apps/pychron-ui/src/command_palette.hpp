#pragma once

// The command palette (Help > Command Palette…, Ctrl+Shift+P): every command
// in the unified menu bar that can run now, found by typing a few letters of
// it ("qsv" finds Queue › Save). Enter runs the highlighted one.
//
// It is a popup, not a dialog, so the window it was opened over stays the
// active window: that window's own commands stay enabled and are offered.

#include <QFrame>
#include <QList>
#include <QPointer>
#include <QString>

class QAction;
class QLineEdit;
class QTreeWidget;

namespace pychron::ui {

// How well `query` matches `text` as a subsequence, case-insensitively;
// negative for no match. Matches at word starts and runs of consecutive
// letters score higher; an empty query matches everything with 0.
int fuzzy_score(const QString& text, const QString& query);

class CommandPalette : public QFrame {
  Q_OBJECT

 public:
  explicit CommandPalette(QWidget* parent = nullptr);

  // Lists what can run now and shows the palette over `over`.
  void open_over(QWidget* over);

  QLineEdit* filter() const { return filter_; }
  QTreeWidget* list() const { return list_; }
  // The commands listed, best first ("Queue › Save").
  QStringList shown() const;

  // The label the palette gives a command: menu, then the action's text
  // without mnemonics or a trailing ellipsis.
  static QString label(const QAction* action, const QString& menu_title);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct Entry {
    QPointer<QAction> action;
    QString label;
  };

  void collect();
  void refill();
  void run_current();

  QLineEdit* filter_;
  QTreeWidget* list_;
  QList<Entry> entries_;
};

// Help > Command Palette…, owned by `window` (the caller puts it in the
// unified bar); the palette is built on first use as its child.
QAction* make_command_palette_action(QWidget* window);

}  // namespace pychron::ui
