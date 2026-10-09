#include "dock_layouts.hpp"

#include <utility>

#include <QAction>
#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>

namespace pychron::ui {

namespace {

QList<QDockWidget*> docks_of(const QMainWindow* window) {
  return window->findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly);
}

}  // namespace

DockLayouts::DockLayouts(QMainWindow* window, std::function<void()> factory, QSettings* settings, QString group, Keys keys)
    : QObject(window),
      window_(window),
      factory_(std::move(factory)),
      settings_(settings),
      group_(std::move(group)),
      keys_(std::move(keys)) {
  // Qt leaves a dock with no object name out of a saved layout, silently.
  for (const QDockWidget* dock : docks_of(window_)) Q_ASSERT(!dock->objectName().isEmpty());
}

DockLayouts* DockLayouts::of(const QWidget* window) {
  return window == nullptr ? nullptr : window->findChild<DockLayouts*>(QString(), Qt::FindDirectChildrenOnly);
}

void DockLayouts::reset() { factory_(); }

void DockLayouts::restore_last() {
  if (settings_ == nullptr) return;
  const QByteArray state = settings_->value(key(keys_.state)).toByteArray();
  if (state.isEmpty()) return;
  if (const QByteArray geometry = settings_->value(key(keys_.geometry)).toByteArray(); !geometry.isEmpty()) {
    window_->restoreGeometry(geometry);
  }
  // What was saved is untrusted: a layout Qt will not take leaves the
  // window as its own code built it.
  if (!window_->restoreState(state)) reset();
}

void DockLayouts::save_last() {
  if (settings_ == nullptr) return;
  settings_->setValue(key(keys_.geometry), window_->saveGeometry());
  settings_->setValue(key(keys_.state), window_->saveState());
}

QList<QAction*> DockLayouts::panel_actions() const {
  QList<QAction*> out;
  for (QDockWidget* dock : docks_of(window_)) {
    if (dock->features().testFlag(QDockWidget::DockWidgetClosable)) out.append(dock->toggleViewAction());
  }
  return out;
}

}  // namespace pychron::ui
