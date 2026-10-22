#pragma once

// What a window shows of the update check (update check spec 2.3): a button
// in the status bar once an automatic check has found a release worth
// showing, the update dialog it opens, and Help > Check for Updates…, whose
// answer is always a dialog. An automatic check never opens one by itself.

#include <QDialog>
#include <QString>
#include <QToolButton>

#include "pychron/update/update.hpp"

class QLabel;
class QMainWindow;
class QPushButton;
class QTextBrowser;

namespace pychron::ui {

class UpdateChecker;

// One answer of a check. Release: its version, notes and Download / Skip this
// version / Close. Otherwise a line of text and Close.
class UpdateDialog : public QDialog {
  Q_OBJECT

 public:
  // Deleted when closed.
  static UpdateDialog* release(const update::Release& release, QWidget* parent);
  static UpdateDialog* up_to_date(update::Version running, bool no_release, QWidget* parent);
  static UpdateDialog* failure(const QString& reason, QWidget* parent);

  // The line at the top.
  QString message() const;
  // Release only; null otherwise.
  QTextBrowser* notes() const noexcept { return notes_; }
  QPushButton* download_button() const noexcept { return download_; }
  QPushButton* skip_button() const noexcept { return skip_; }

 signals:
  // Skip this version was pressed (the dialog then closes).
  void skipped();

 private:
  UpdateDialog(const QString& title, const QString& message, QWidget* parent);

  QLabel* message_;
  QTextBrowser* notes_ = nullptr;
  QPushButton* download_ = nullptr;
  QPushButton* skip_ = nullptr;
};

// "Update available: v<x.y.z>" in the status bar; hidden until a release is
// shown, and again once it is skipped. Clicked, opens the release's dialog.
class UpdateIndicator : public QToolButton {
  Q_OBJECT

 public:
  // `checker` outlives the indicator's dialogs' Skip.
  explicit UpdateIndicator(UpdateChecker& checker, QWidget* parent = nullptr);

  void show_release(const update::Release& release);

 private:
  UpdateChecker& checker_;
  update::Release release_;
};

// Puts the indicator in `window`'s status bar and Check for Updates… in the
// Help menu (MenuHub, every window), greyed while a check is out. An
// automatic check's release goes to the indicator only; a manual check's
// every answer to a dialog over `window`. `checker` outlives `window`'s
// update widgets or is destroyed before them.
void install_update_check(QMainWindow* window, UpdateChecker& checker);

}  // namespace pychron::ui
