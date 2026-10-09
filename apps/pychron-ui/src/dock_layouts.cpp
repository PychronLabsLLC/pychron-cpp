#include "dock_layouts.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <QAction>
#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>

namespace pychron::ui {

namespace {

constexpr qsizetype kMaxNameLength = 64;
const QString kArrangements = QStringLiteral("arrangements");

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
  // By title: the order Qt keeps a window's docks in changes whenever one is
  // brought to the front of its tabs.
  QList<QDockWidget*> closable;
  for (QDockWidget* dock : docks_of(window_)) {
    if (dock->features().testFlag(QDockWidget::DockWidgetClosable)) closable.append(dock);
  }
  std::ranges::sort(closable, [](const QDockWidget* a, const QDockWidget* b) {
    return a->windowTitle().compare(b->windowTitle(), Qt::CaseInsensitive) < 0;
  });
  QList<QAction*> out;
  for (QDockWidget* dock : closable) out.append(dock->toggleViewAction());
  return out;
}

Result<QString> DockLayouts::valid_name(const QString& raw) {
  const auto bad = [](std::string why) {
    return fail(Error{.kind = ErrorKind::Config, .what = std::move(why), .device = {}, .code = "bad_name"});
  };
  QString name = raw.trimmed();
  if (name.isEmpty()) return bad("a name is needed");
  if (name.size() > kMaxNameLength) return bad("a name is at most 64 characters");
  if (name.contains(QLatin1Char('/'))) return bad("a name cannot contain “/”");
  if (name.contains(QLatin1Char('\\'))) return bad("a name cannot contain “\\”");
  if (std::ranges::any_of(name, [](QChar c) { return c.category() == QChar::Other_Control; })) {
    return bad("a name cannot contain a control character");
  }
  return name;
}

QStringList DockLayouts::names() const {
  if (settings_ == nullptr) return {};
  settings_->beginGroup(key(kArrangements));
  QStringList found = settings_->childGroups();
  settings_->endGroup();
  found.sort(Qt::CaseInsensitive);
  return found;
}

QString DockLayouts::stored(const QString& name) const {
  for (const QString& each : names()) {
    if (each.compare(name, Qt::CaseInsensitive) == 0) return each;
  }
  return {};
}

bool DockLayouts::contains(const QString& name) const { return !stored(name).isEmpty(); }

Result<void> DockLayouts::save_as(const QString& name) {
  const Result<QString> valid = valid_name(name);
  if (!valid) return fail(valid.error());
  if (settings_ == nullptr) {
    return fail(Error{.kind = ErrorKind::Config, .what = "this window keeps no settings", .device = {}, .code = "no_settings"});
  }
  const QString root = key(kArrangements) + QLatin1Char('/');
  const QString state_key = QStringLiteral("/state");
  const QString geometry_key = QStringLiteral("/geometry");
  // One of the same name, however it is capitalised, goes; it is put back if
  // the new one cannot be kept.
  const QString old = stored(*valid);
  const QByteArray old_state = old.isEmpty() ? QByteArray() : settings_->value(root + old + state_key).toByteArray();
  const QByteArray old_geometry = old.isEmpty() ? QByteArray() : settings_->value(root + old + geometry_key).toByteArray();
  if (!old.isEmpty()) settings_->remove(root + old);

  const QString at = root + *valid;
  settings_->setValue(at + geometry_key, window_->saveGeometry());
  settings_->setValue(at + state_key, window_->saveState());
  // Written now, so that one that cannot be kept is refused now and not
  // found missing at the next start. Asked of a second QSettings on the same
  // store, which shares what is still unwritten: the window's own keeps the
  // first error of the session for good, and would fail every save after it.
  settings_->sync();
  QSettings probe(settings_->fileName(), settings_->format());
  probe.sync();
  if (probe.status() != QSettings::NoError) {
    settings_->remove(at);
    if (!old.isEmpty()) {
      settings_->setValue(root + old + geometry_key, old_geometry);
      settings_->setValue(root + old + state_key, old_state);
    }
    return fail(Error{.kind = ErrorKind::Io, .what = "the settings could not be written", .device = {}, .code = "not_saved"});
  }
  return {};
}

Result<void> DockLayouts::apply(const QString& name) {
  const QString saved_as = stored(name);
  if (saved_as.isEmpty()) {
    return fail(Error{.kind = ErrorKind::Config,
                      .what = "no arrangement “" + name.toStdString() + "”",
                      .device = {},
                      .code = "unknown_name"});
  }
  const QString at = key(kArrangements) + QLatin1Char('/') + saved_as;
  if (const QByteArray geometry = settings_->value(at + QStringLiteral("/geometry")).toByteArray(); !geometry.isEmpty()) {
    window_->restoreGeometry(geometry);  // refused: the layout is still worth having
  }
  const QByteArray state = settings_->value(at + QStringLiteral("/state")).toByteArray();
  if (state.isEmpty() || !window_->restoreState(state)) {
    reset();
    const std::string why = "what was saved cannot be read";
    emit applyFailed(QStringLiteral("arrangement “%1” not applied: %2").arg(saved_as, QString::fromStdString(why)));
    return fail(Error{.kind = ErrorKind::Config, .what = why, .device = {}, .code = "bad_layout"});
  }
  return {};
}

void DockLayouts::remove(const QString& name) {
  const QString saved_as = stored(name);
  if (!saved_as.isEmpty()) settings_->remove(key(kArrangements) + QLatin1Char('/') + saved_as);
}

}  // namespace pychron::ui
