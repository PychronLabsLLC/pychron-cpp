#pragma once

// Python highlighting for pyscripts: keywords, builtins, the host's commands,
// run-context globals, numbers, strings (triple-quoted ones across lines),
// comments and the `#! pychron:` header.

#include <QRegularExpression>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include "pychron/scripting/script.hpp"

namespace pychron::ui {

class ScriptHighlighter : public QSyntaxHighlighter {
  Q_OBJECT

 public:
  enum class Role { Keyword, Builtin, Command, Context, Number, String, Comment, Header };

  ScriptHighlighter(QTextDocument* document, scripting::ScriptKind kind);

  // The role a word gets, nullopt for none (for tests).
  std::optional<Role> role_of(const QString& word) const;
  QTextCharFormat format_for(Role role) const;

 protected:
  void highlightBlock(const QString& text) override;

 private:
  QSet<QString> keywords_, builtins_, commands_, context_;
  QTextCharFormat formats_[8];
  QRegularExpression word_, number_;
};

}  // namespace pychron::ui
