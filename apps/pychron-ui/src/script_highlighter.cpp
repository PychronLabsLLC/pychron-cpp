#include "script_highlighter.hpp"
#include "theme.hpp"

#include "pychron/experiment/lab/scripts.hpp"
#include "pychron/scripting/vocabulary.hpp"

namespace pychron::ui {

namespace {

enum BlockState { kNormal = 0, kInSingleTriple = 1, kInDoubleTriple = 2 };

QTextCharFormat make(const QColor& color, bool bold = false, bool italic = false) {
  QTextCharFormat f;
  f.setForeground(color);
  if (bold) f.setFontWeight(QFont::Bold);
  f.setFontItalic(italic);
  return f;
}

}  // namespace

ScriptHighlighter::ScriptHighlighter(QTextDocument* document, scripting::ScriptKind kind)
    : QSyntaxHighlighter(document),
      word_(QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*\\b")),
      number_(QStringLiteral("\\b(0[xX][0-9a-fA-F]+|[0-9]+\\.?[0-9]*([eE][-+]?[0-9]+)?|\\.[0-9]+([eE][-+]?[0-9]+)?)\\b")) {
  for (const auto& w : experiment::lab::python_keywords()) keywords_.insert(QString::fromStdString(w));
  for (const auto& w : experiment::lab::script_builtins()) builtins_.insert(QString::fromStdString(w));
  for (const auto& c : scripting::vocabulary())
    if (scripting::command_allowed(c, kind)) commands_.insert(QString::fromLatin1(c.name.data(), static_cast<qsizetype>(c.name.size())));
  for (const auto& [name, value] : scripting::default_context()) context_.insert(QString::fromStdString(name));
  context_.insert(QStringLiteral("opt"));

  const auto& syntax = theme().syntax;
  formats_[static_cast<int>(Role::Keyword)] = make(syntax.keyword, true);
  formats_[static_cast<int>(Role::Builtin)] = make(syntax.builtin);
  formats_[static_cast<int>(Role::Command)] = make(syntax.command, true);
  formats_[static_cast<int>(Role::Context)] = make(syntax.context);
  formats_[static_cast<int>(Role::Number)] = make(syntax.number);
  formats_[static_cast<int>(Role::String)] = make(syntax.string);
  formats_[static_cast<int>(Role::Comment)] = make(syntax.comment, false, true);
  formats_[static_cast<int>(Role::Header)] = make(syntax.header, true);
}

std::optional<ScriptHighlighter::Role> ScriptHighlighter::role_of(const QString& word) const {
  if (keywords_.contains(word)) return Role::Keyword;
  if (commands_.contains(word)) return Role::Command;
  if (context_.contains(word)) return Role::Context;
  if (builtins_.contains(word)) return Role::Builtin;
  return std::nullopt;
}

QTextCharFormat ScriptHighlighter::format_for(Role role) const { return formats_[static_cast<int>(role)]; }

void ScriptHighlighter::highlightBlock(const QString& text) {
  // Words and numbers first; strings and comments paint over them.
  for (auto it = word_.globalMatch(text); it.hasNext();) {
    const auto m = it.next();
    if (auto r = role_of(m.captured())) setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(m.capturedLength()), format_for(*r));
  }
  for (auto it = number_.globalMatch(text); it.hasNext();) {
    const auto m = it.next();
    setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(m.capturedLength()), format_for(Role::Number));
  }

  const QTextCharFormat str = format_for(Role::String);
  int i = 0;
  int state = previousBlockState() < 0 ? kNormal : previousBlockState();
  // Inside a triple-quoted string carried over from the previous block.
  if (state != kNormal) {
    const QString close = state == kInSingleTriple ? QStringLiteral("'''") : QStringLiteral("\"\"\"");
    const int end = static_cast<int>(text.indexOf(close));
    if (end < 0) {
      setFormat(0, static_cast<int>(text.size()), str);
      setCurrentBlockState(state);
      return;
    }
    setFormat(0, end + 3, str);
    i = end + 3;
    state = kNormal;
  }
  setCurrentBlockState(kNormal);
  while (i < text.size()) {
    const QChar ch = text[i];
    if (ch == QLatin1Char('#')) {
      const bool header = text.mid(i).startsWith(QStringLiteral("#! pychron:"));
      setFormat(i, static_cast<int>(text.size()) - i, format_for(header ? Role::Header : Role::Comment));
      return;
    }
    if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
      const QString triple(3, ch);
      if (text.mid(i, 3) == triple) {
        const int end = static_cast<int>(text.indexOf(triple, i + 3));
        if (end < 0) {
          setFormat(i, static_cast<int>(text.size()) - i, str);
          setCurrentBlockState(ch == QLatin1Char('\'') ? kInSingleTriple : kInDoubleTriple);
          return;
        }
        setFormat(i, end + 3 - i, str);
        i = end + 3;
        continue;
      }
      int j = i + 1;
      while (j < text.size() && text[j] != ch) j += text[j] == QLatin1Char('\\') ? 2 : 1;
      setFormat(i, std::min(j + 1, static_cast<int>(text.size())) - i, str);
      i = j + 1;
      continue;
    }
    ++i;
  }
}

}  // namespace pychron::ui
