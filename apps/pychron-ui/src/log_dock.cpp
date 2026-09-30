#include "log_dock.hpp"

#include <QTime>

namespace pychron::ui {

namespace {

QString level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return QStringLiteral("TRACE");
    case LogLevel::Debug:
      return QStringLiteral("DEBUG");
    case LogLevel::Info:
      return QStringLiteral("INFO");
    case LogLevel::Warn:
      return QStringLiteral("WARN");
    case LogLevel::Error:
      return QStringLiteral("ERROR");
  }
  return QStringLiteral("?");
}

}  // namespace

LogDock::LogDock(QWidget* parent) : QDockWidget(tr("Log"), parent), view_(new QPlainTextEdit(this)) {
  setObjectName(QStringLiteral("LogDock"));
  view_->setReadOnly(true);
  view_->setMaximumBlockCount(kMaxLines);
  view_->setLineWrapMode(QPlainTextEdit::NoWrap);
  setWidget(view_);
}

void LogDock::append_log(const Log& log) {
  append_line(QStringLiteral("%1 [%2] %3")
                  .arg(level_name(log.level), QString::fromStdString(log.logger), QString::fromStdString(log.message)));
}

void LogDock::append_line(const QString& line) {
  view_->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line);
}

int LogDock::line_count() const { return view_->document()->isEmpty() ? 0 : view_->blockCount(); }

QString LogDock::text() const { return view_->toPlainText(); }

}  // namespace pychron::ui
