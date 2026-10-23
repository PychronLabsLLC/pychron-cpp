#include "error_report.hpp"

#include <string>

#include "pychron/core/log_error.hpp"
#include "pychron/core/process_log.hpp"

namespace pychron::ui {
namespace {

Logger area_logger(std::string_view area) {
  std::string name = "ui.";
  name.append(area);
  return process_logger(std::move(name));
}

}  // namespace

void log_shown(std::string_view area, const QString& title, const QString& text) {
  const Logger logger = area_logger(area);
  if (!logger.enabled(LogLevel::Error)) return;
  logger.error((title + QStringLiteral(": ") + text).toStdString());
}

void log_shown(std::string_view area, const QString& title, const Error& error) {
  log_error(area_logger(area), error, title.toStdString());
}

QMessageBox::StandardButton show_error(QWidget* parent, std::string_view area, const QString& title,
                                       const QString& text, QMessageBox::StandardButtons buttons) {
  log_shown(area, title, text);
  return QMessageBox::critical(parent, title, text, buttons);
}

QMessageBox::StandardButton show_warning(QWidget* parent, std::string_view area, const QString& title,
                                         const QString& text, QMessageBox::StandardButtons buttons) {
  log_shown(area, title, text);
  return QMessageBox::warning(parent, title, text, buttons);
}

void show_error(QWidget* parent, std::string_view area, const QString& title, const Error& error) {
  log_shown(area, title, error);
  QMessageBox::critical(parent, title, QString::fromStdString(error.what));
}

void log_action(const QString& what) {
  const Logger logger = process_logger("ui.action");
  if (logger.enabled(LogLevel::Info)) logger.info(what.toStdString());
}

}  // namespace pychron::ui
