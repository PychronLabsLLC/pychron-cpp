#pragma once

// ScriptEditorWindow (experiment-window design 5.8): edits the lab's scripts.
//
//   left    the lab's scripts by kind (double-click opens)
//   center  one tab per open script: highlighting, completion from the host's
//           command table for that kind, diagnostics in place
//   bottom  the static check's diagnostics (click to go to the line) and the
//           estimate, refreshed 600 ms after the text stops changing
//
// Ctrl+click (or F2) on gosub('name') opens the script it runs. Scripts are
// checked as `elctl`/a run would, without hardware (lab::editor_environment).
// Closing a tab or the window with unsaved edits asks first.

#include <functional>
#include <optional>
#include <memory>
#include <vector>

#include <QMainWindow>
#include <QSettings>
#include <QTimer>

#include "code_editor.hpp"
#include "pychron/experiment/lab/scripts.hpp"
#include "script_highlighter.hpp"

class QAction;
class QLabel;
class QListWidget;
class QTabWidget;
class QTreeWidget;

namespace pychron::ui {

class ScriptEditorWindow : public QMainWindow {
  Q_OBJECT

 public:
  enum class Unsaved { Save, Discard, Cancel };
  static constexpr int kCheckDelayMs = 600;

  // `lab` must outlive the window.
  explicit ScriptEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr,
                              QWidget* parent = nullptr);
  ~ScriptEditorWindow() override;

  // Opens (or switches to) a script. false when the file cannot be read.
  bool open(const experiment::lab::ScriptFile& file);
  bool open(scripting::ScriptKind kind, const QString& name);
  // Creates <kind>/<name>.py with a main() skeleton and opens it. false (with
  // `error`) for a bad name or an existing file.
  bool new_script(scripting::ScriptKind kind, const QString& name, QString* error = nullptr);
  // File > Open: asks which of the lab's scripts, then opens it or switches
  // to its tab. false on cancel, with no scripts, or for a file that cannot
  // be read.
  bool open_picked();
  // Which of `names` (script_names()) to open; nullopt: cancelled. A dialog
  // by default.
  using PickScript = std::function<std::optional<QString>(const QStringList& names)>;
  void set_pick_script(PickScript pick) { pick_script_ = std::move(pick); }

  int document_count() const;
  QString current_name() const;  // "extraction/sim_extract"; empty without a tab
  CodeEditor* current_editor() const;
  bool modified() const;  // the current tab
  bool save(QString* error = nullptr);  // the current tab
  bool close_current();                 // asks when modified; false on Cancel
  // Opens the script a gosub in the current tab runs; false when it does not resolve.
  bool follow_gosub(const QString& name);

  void check_now();  // the current tab, at once
  QStringList diagnostic_lines() const;  // "3: error: ..." as listed
  QString estimate_text() const;
  QStringList script_names() const;  // the tree, "kind/name"

  void set_ask_unsaved(std::function<Unsaved(const QString& name)> ask) { ask_unsaved_ = std::move(ask); }

 signals:
  // A script was created or saved (the lab's script lists may have changed).
  void scriptsChanged();

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  struct Document {
    experiment::lab::ScriptFile file;
    CodeEditor* editor = nullptr;
    ScriptHighlighter* highlighter = nullptr;
    bool modified = false;
    experiment::lab::ScriptCheck check;
    QString checked_text;  // what `check` was run on
  };

  Document* current() const;
  Document* find(const experiment::lab::ScriptFile& file) const;
  bool resolve_unsaved(Document& doc);
  bool close_document(int index);
  void fill_tree();
  void show_check();  // the current tab's last check in the list and status
  void update_tab_title(Document& doc);
  static QString label(const experiment::lab::ScriptFile& file);

  const experiment::lab::Lab& lab_;
  std::unique_ptr<QSettings> settings_;
  std::unique_ptr<scripting::IScriptHost> host_;
  std::vector<std::unique_ptr<Document>> docs_;  // tab order
  QTreeWidget* tree_;
  QTabWidget* tabs_;
  QListWidget* problems_;
  QLabel* status_;
  QAction* new_ = nullptr;   // what File > New, Open and Save do here
  QAction* open_ = nullptr;  // enabled while the lab has a script
  QAction* save_ = nullptr;  // enabled while a tab is open
  PickScript pick_script_;
  QTimer check_timer_;
  std::function<Unsaved(const QString&)> ask_unsaved_;
};

}  // namespace pychron::ui
