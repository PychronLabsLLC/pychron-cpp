#pragma once

// The editor of one setup question (a profile's `[[questions]]`), shared by
// the setup wizard and Preferences > Data source (data source editing spec,
// docs/superpowers/specs/2026-10-18-data-source-editing-design.md, 3.5): the
// same widgets on both, not copies.
//
//   Bool                 a check box with the prompt
//   Port, Int            a spin box (a port 1 to 65535)
//   Choice               radio buttons in a QButtonGroup (a combo box past
//                        four choices); each radio named "<id>-<choice>"
//   Path, Folder         a line edit named "text" and a "Browse…" button
//   String, Host, Float,
//   Secret, List         a line edit (a secret not echoed)
//   Table                a table named "table" with Add row / Remove row

#include <functional>

#include "pychron/core/error.hpp"
#include "pychron/setup/profile.hpp"

class QWidget;

namespace pychron::ui {

// A new editor for `q`, child of `parent` (may be null: the caller parents
// it). `changed`, when given, is called whenever its value changes, by the
// user or by set_question_value.
QWidget* make_question_editor(const setup::Question& q, QWidget* parent, const std::function<void()>& changed = {});

// Shows `v` in an editor make_question_editor made for `q`. A number the
// editor cannot hold (a port of 70000 from a hand-edited record) is kept as
// text, and question_value refuses it.
void set_question_value(QWidget* editor, const setup::Question& q, const setup::Value& v);

// What the editor holds, typed as `q` asks (parse_answer); Config with the
// reason, without the question id, when it is not an answer. A secret is
// returned as typed, not trimmed.
Result<setup::Value> question_value(const QWidget* editor, const setup::Question& q);

}  // namespace pychron::ui
