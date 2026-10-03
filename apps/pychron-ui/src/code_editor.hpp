#pragma once

// CodeEditor: a QPlainTextEdit for pyscripts with a line-number gutter,
// diagnostics drawn in place (wavy underline on the line, a dot in the gutter,
// the message as the tooltip), word completion (Ctrl+Space, or after two
// letters) and gosub links (Ctrl+click on a gosub('name') emits
// gosubActivated).

#include <optional>
#include <vector>

#include <QPlainTextEdit>
#include <QString>
#include <QStringList>

class QCompleter;
class QStringListModel;

namespace pychron::ui {

struct EditorDiagnostic {
  int line = 0;  // 1-based
  bool error = true;
  QString message;
};

class CodeEditor : public QPlainTextEdit {
  Q_OBJECT

 public:
  explicit CodeEditor(QWidget* parent = nullptr);

  void set_completion_words(const QStringList& words);
  QStringList completion_words() const;
  // Completions for `prefix` (case-sensitive prefix match), for tests.
  QStringList completions_for(const QString& prefix) const;

  void set_diagnostics(std::vector<EditorDiagnostic> diagnostics);
  const std::vector<EditorDiagnostic>& diagnostics() const noexcept { return diagnostics_; }
  void go_to_line(int line);  // 1-based

  // The gosub name at a position (1-based line, 0-based column), if the
  // position is on one. gosubActivated is emitted for it on Ctrl+click.
  std::optional<QString> gosub_at(int line, int column) const;
  std::optional<QString> gosub_under_cursor() const;

  int gutter_width() const;
  void paint_gutter(QPaintEvent* event);

 signals:
  void gosubActivated(const QString& name);

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  bool event(QEvent* event) override;  // tooltips for diagnostics
  void changeEvent(QEvent* event) override;  // tab stops and gutter follow the font

 private:
  QString word_before_cursor() const;
  void insert_completion(const QString& word);
  void refresh_selections();

  QWidget* gutter_;
  QCompleter* completer_;
  QStringListModel* words_;
  std::vector<EditorDiagnostic> diagnostics_;
};

}  // namespace pychron::ui
