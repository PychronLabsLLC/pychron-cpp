#include "code_editor.hpp"
#include "theme.hpp"

#include <algorithm>

#include <QAbstractItemView>
#include <QCompleter>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QToolTip>

#include "pychron/experiment/lab/scripts.hpp"

namespace pychron::ui {

namespace {

constexpr int kMinCompletionPrefix = 2;

class Gutter : public QWidget {
 public:
  explicit Gutter(CodeEditor* editor) : QWidget(editor), editor_(editor) {}
  QSize sizeHint() const override { return {editor_->gutter_width(), 0}; }

 protected:
  void paintEvent(QPaintEvent* event) override { editor_->paint_gutter(event); }

 private:
  CodeEditor* editor_;
};

}  // namespace

CodeEditor::CodeEditor(QWidget* parent)
    : QPlainTextEdit(parent), gutter_(new Gutter(this)), completer_(new QCompleter(this)), words_(new QStringListModel(this)) {
  setFont(style::mono_font());
  setLineWrapMode(QPlainTextEdit::NoWrap);
  setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
  setMouseTracking(true);

  completer_->setModel(words_);
  completer_->setWidget(this);
  completer_->setCompletionMode(QCompleter::PopupCompletion);
  completer_->setCaseSensitivity(Qt::CaseSensitive);
  connect(completer_, qOverload<const QString&>(&QCompleter::activated), this, &CodeEditor::insert_completion);

  connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutter_width(), 0, 0, 0); });
  connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
    if (dy != 0) gutter_->scroll(0, dy);
    else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
  });
  setViewportMargins(gutter_width(), 0, 0, 0);
}

void CodeEditor::set_completion_words(const QStringList& words) { words_->setStringList(words); }
QStringList CodeEditor::completion_words() const { return words_->stringList(); }

QStringList CodeEditor::completions_for(const QString& prefix) const {
  QStringList out;
  for (const auto& w : words_->stringList())
    if (w.startsWith(prefix) && w != prefix) out.append(w);
  return out;
}

int CodeEditor::gutter_width() const {
  const int digits = static_cast<int>(QString::number(std::max(1, blockCount())).size());
  return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(3, digits);
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
  QPlainTextEdit::resizeEvent(event);
  const QRect cr = contentsRect();
  gutter_->setGeometry(QRect(cr.left(), cr.top(), gutter_width(), cr.height()));
}

void CodeEditor::paint_gutter(QPaintEvent* event) {
  QPainter p(gutter_);
  p.fillRect(event->rect(), theme().gutter);
  QTextBlock block = firstVisibleBlock();
  int number = block.blockNumber() + 1;
  int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  const int h = fontMetrics().height();
  while (block.isValid() && top <= event->rect().bottom()) {
    if (block.isVisible()) {
      // A dot for the worst diagnostic on the line.
      std::optional<bool> mark;
      for (const auto& d : diagnostics_)
        if (d.line == number) mark = mark.value_or(false) || d.error;
      if (mark) {
        p.setPen(Qt::NoPen);
        p.setBrush(*mark ? theme().error : theme().warning);
        p.drawEllipse(QPoint(6, top + h / 2), 3, 3);
      }
      p.setPen(theme().faint_text);
      p.drawText(0, top, gutter_->width() - 4, h, Qt::AlignRight, QString::number(number));
    }
    block = block.next();
    top += qRound(blockBoundingRect(block).height());
    ++number;
  }
}

void CodeEditor::set_diagnostics(std::vector<EditorDiagnostic> diagnostics) {
  diagnostics_ = std::move(diagnostics);
  refresh_selections();
  gutter_->update();
}

void CodeEditor::refresh_selections() {
  QList<QTextEdit::ExtraSelection> selections;
  for (const auto& d : diagnostics_) {
    const QTextBlock block = document()->findBlockByNumber(d.line - 1);
    if (!block.isValid()) continue;
    QTextEdit::ExtraSelection s;
    s.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    s.format.setUnderlineColor(d.error ? theme().error : theme().warning);
    s.format.setToolTip(d.message);
    QTextCursor c(block);
    // The line's text without its indentation.
    const QString text = block.text();
    int indent = 0;
    while (indent < text.size() && text[indent].isSpace()) ++indent;
    c.setPosition(block.position() + indent);
    c.setPosition(block.position() + static_cast<int>(text.size()), QTextCursor::KeepAnchor);
    s.cursor = c;
    selections.append(s);
  }
  setExtraSelections(selections);
}

void CodeEditor::go_to_line(int line) {
  const QTextBlock block = document()->findBlockByNumber(std::max(0, line - 1));
  if (!block.isValid()) return;
  QTextCursor c(block);
  setTextCursor(c);
  centerCursor();
  setFocus();
}

std::optional<QString> CodeEditor::gosub_at(int line, int column) const {
  const QTextBlock block = document()->findBlockByNumber(line - 1);
  if (!block.isValid()) return std::nullopt;
  for (const auto& g : experiment::lab::find_gosubs(block.text().toStdString()))
    if (column >= g.column && column <= g.column + g.length) return QString::fromStdString(g.name);
  return std::nullopt;
}

std::optional<QString> CodeEditor::gosub_under_cursor() const {
  const QTextCursor c = textCursor();
  return gosub_at(c.blockNumber() + 1, c.positionInBlock());
}

QString CodeEditor::word_before_cursor() const {
  QTextCursor c = textCursor();
  const QString text = c.block().text().left(c.positionInBlock());
  int i = static_cast<int>(text.size());
  while (i > 0 && (text[i - 1].isLetterOrNumber() || text[i - 1] == QLatin1Char('_'))) --i;
  return text.mid(i);
}

void CodeEditor::insert_completion(const QString& word) {
  QTextCursor c = textCursor();
  const QString prefix = completer_->completionPrefix();
  c.insertText(word.mid(prefix.size()));
  setTextCursor(c);
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
  QAbstractItemView* popup = completer_->popup();
  if (popup->isVisible()) {
    switch (event->key()) {
      case Qt::Key_Enter:
      case Qt::Key_Return:
      case Qt::Key_Escape:
      case Qt::Key_Tab:
      case Qt::Key_Backtab: event->ignore(); return;  // the completer takes them
      default: break;
    }
  }
  const bool force = event->modifiers().testFlag(Qt::ControlModifier) && event->key() == Qt::Key_Space;
  if (!force) {
    // Return keeps the indentation of the line it ends.
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !popup->isVisible()) {
      const QString line = textCursor().block().text();
      QString indent;
      for (QChar ch : line) {
        if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t')) break;
        indent += ch;
      }
      if (line.trimmed().endsWith(QLatin1Char(':'))) indent += QStringLiteral("    ");
      QPlainTextEdit::keyPressEvent(event);
      insertPlainText(indent);
      return;
    }
    QPlainTextEdit::keyPressEvent(event);
  }
  const QString prefix = word_before_cursor();
  if (!force && (event->text().isEmpty() || prefix.size() < kMinCompletionPrefix)) {
    popup->hide();
    return;
  }
  if (prefix != completer_->completionPrefix()) {
    completer_->setCompletionPrefix(prefix);
    popup->setCurrentIndex(completer_->completionModel()->index(0, 0));
  }
  if (completer_->completionCount() == 0 ||
      (completer_->completionCount() == 1 && completer_->currentCompletion() == prefix)) {
    popup->hide();
    return;
  }
  QRect r = cursorRect();
  r.setWidth(popup->sizeHintForColumn(0) + popup->verticalScrollBar()->sizeHint().width());
  completer_->complete(r);
}

void CodeEditor::mouseReleaseEvent(QMouseEvent* event) {
  QPlainTextEdit::mouseReleaseEvent(event);
  if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {
    if (auto name = gosub_under_cursor()) emit gosubActivated(*name);
  }
}

bool CodeEditor::event(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    auto* help = static_cast<QHelpEvent*>(event);
    const QTextCursor c = cursorForPosition(help->pos() - QPoint(gutter_width(), 0));
    const int line = c.blockNumber() + 1;
    QStringList messages;
    for (const auto& d : diagnostics_)
      if (d.line == line) messages.append(d.message);
    if (!messages.isEmpty()) QToolTip::showText(help->globalPos(), messages.join(QLatin1Char('\n')), this);
    else QToolTip::hideText();
    return true;
  }
  return QPlainTextEdit::event(event);
}

}  // namespace pychron::ui
