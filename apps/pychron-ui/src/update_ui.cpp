#include "update_ui.hpp"

#include <QAction>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStatusBar>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include "menu_hub.hpp"
#include "update_checker.hpp"

namespace pychron::ui {

namespace {

constexpr QSize kReleaseSize{560, 420};  // room for a release's notes

QString version_text(update::Version version) { return QString::fromStdString(update::to_string(version)); }

// One answer over `window` at a time: the one before is closed.
void show_over(QWidget* window, UpdateDialog* dialog) {
  for (UpdateDialog* before : window->findChildren<UpdateDialog*>(QString(), Qt::FindDirectChildrenOnly)) {
    if (before != dialog) before->close();
  }
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

// A release's dialog over `window`, whose Skip is remembered by `checker`.
UpdateDialog* show_release_dialog(QWidget* window, const update::Release& release, UpdateChecker& checker) {
  auto* dialog = UpdateDialog::release(release, window);
  QObject::connect(dialog, &UpdateDialog::skipped, &checker,
                   [&checker, version = release.version] { checker.skip(version); });
  show_over(window, dialog);
  return dialog;
}

}  // namespace

UpdateDialog::UpdateDialog(const QString& title, const QString& message, QWidget* parent)
    : QDialog(parent), message_(new QLabel(message)) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(title);
  message_->setWordWrap(true);
  message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(message_);
}

UpdateDialog* UpdateDialog::release(const update::Release& release, QWidget* parent) {
  const QString title = tr("pychron %1 is available").arg(version_text(release.version));
  auto* dialog = new UpdateDialog(title, title, parent);
  dialog->notes_ = new QTextBrowser;
  dialog->notes_->setOpenExternalLinks(true);
  dialog->notes_->setMarkdown(QString::fromStdString(release.notes));
  auto* buttons = new QDialogButtonBox;
  dialog->download_ = buttons->addButton(tr("Download"), QDialogButtonBox::ActionRole);
  dialog->skip_ = buttons->addButton(tr("Skip this version"), QDialogButtonBox::ActionRole);
  buttons->addButton(QDialogButtonBox::Close);
  // The release's page in the browser: nothing is downloaded here.
  connect(dialog->download_, &QPushButton::clicked, dialog,
          [url = QUrl(QString::fromStdString(release.html_url))] { QDesktopServices::openUrl(url); });
  connect(dialog->skip_, &QPushButton::clicked, dialog, [dialog] {
    emit dialog->skipped();
    dialog->close();
  });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  dialog->layout()->addWidget(dialog->notes_);
  dialog->layout()->addWidget(buttons);
  dialog->resize(kReleaseSize);
  return dialog;
}

UpdateDialog* UpdateDialog::up_to_date(update::Version running, bool no_release, QWidget* parent) {
  const QString message = no_release ? tr("No release has been published yet")
                                     : tr("pychron %1 is up to date").arg(version_text(running));
  auto* dialog = new UpdateDialog(tr("Check for Updates"), message, parent);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  dialog->layout()->addWidget(buttons);
  return dialog;
}

UpdateDialog* UpdateDialog::failure(const QString& reason, QWidget* parent) {
  auto* dialog = new UpdateDialog(tr("Check for Updates"), tr("Could not check for updates: %1").arg(reason), parent);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  dialog->layout()->addWidget(buttons);
  return dialog;
}

QString UpdateDialog::message() const { return message_->text(); }

UpdateIndicator::UpdateIndicator(UpdateChecker& checker, QWidget* parent) : QToolButton(parent), checker_(checker) {
  setAutoRaise(true);
  setToolButtonStyle(Qt::ToolButtonTextOnly);
  hide();
  connect(this, &QToolButton::clicked, this, [this] {
    UpdateDialog* dialog = show_release_dialog(window(), release_, checker_);
    connect(dialog, &UpdateDialog::skipped, this, &QWidget::hide);
  });
}

void UpdateIndicator::show_release(const update::Release& release) {
  release_ = release;
  setText(tr("Update available: v%1").arg(version_text(release.version)));
  setToolTip(tr("pychron %1 is available: click for its notes").arg(version_text(release.version)));
  show();
}

void install_update_check(QMainWindow* window, UpdateChecker& checker) {
  auto* indicator = new UpdateIndicator(checker, window);
  window->statusBar()->addPermanentWidget(indicator);
  indicator->hide();  // until a release is shown

  auto* check = new QAction(QObject::tr("Check for Updates…"), window);
  check->setEnabled(!checker.busy());
  MenuHub::instance().contribute(window, MenuHub::Menu::Help, {check}, MenuHub::Scope::App);
  QObject::connect(check, &QAction::triggered, &checker, [&checker] { checker.check_now(); });
  QObject::connect(&checker, &UpdateChecker::busy_changed, check, [check](bool busy) { check->setEnabled(!busy); });

  QObject::connect(&checker, &UpdateChecker::available, indicator,
                   [window, indicator, &checker](const update::Release& release, bool manual) {
                     if (!manual) {
                       indicator->show_release(release);
                       return;
                     }
                     UpdateDialog* dialog = show_release_dialog(window, release, checker);
                     QObject::connect(dialog, &UpdateDialog::skipped, indicator, &QWidget::hide);
                   });
  QObject::connect(&checker, &UpdateChecker::up_to_date, window, [window, &checker](bool no_release) {
    show_over(window, UpdateDialog::up_to_date(checker.running(), no_release, window));
  });
  QObject::connect(&checker, &UpdateChecker::failed, window, [window](const QString& reason, bool manual) {
    if (manual) show_over(window, UpdateDialog::failure(reason, window));
  });
}

}  // namespace pychron::ui
