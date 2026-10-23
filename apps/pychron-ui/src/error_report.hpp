#pragma once

// Every error that reaches the user is in the log (spec 2.4), and every user
// command that changes state or starts work too (spec 2.2, `ui.action`).
// A box goes through show_error/show_warning; a banner, status line or label
// that shows an error in its own way calls log_shown first.

#include <string_view>

#include <QMessageBox>
#include <QString>

#include "pychron/core/error.hpp"

class QWidget;

namespace pychron::ui {

// Logs at error on `ui.<area>` ("<title>: <text>"), then shows the box as before.
// Returns what QMessageBox returned.
QMessageBox::StandardButton show_error(QWidget* parent, std::string_view area, const QString& title,
                                       const QString& text, QMessageBox::StandardButtons buttons = QMessageBox::Ok);
QMessageBox::StandardButton show_warning(QWidget* parent, std::string_view area, const QString& title,
                                         const QString& text, QMessageBox::StandardButtons buttons = QMessageBox::Ok);
// For an Error object: logs through log_error (attempted = title), shows its text.
void show_error(QWidget* parent, std::string_view area, const QString& title, const Error& error);

// The log record alone, for an error shown some other way (banner, status
// line, a box built by hand): the same line show_error writes.
void log_shown(std::string_view area, const QString& title, const QString& text);
void log_shown(std::string_view area, const QString& title, const Error& error);

// A user command, at info on `ui.action`: "<verb> <object>" with what
// identifies it ("stop queue", "open valve A", "save queue <path>").
void log_action(const QString& what);

}  // namespace pychron::ui
