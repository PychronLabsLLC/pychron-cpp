#pragma once

// DockLayouts: where a main window's dock panels are. It puts them back as
// the window's own code places them (the factory layout) and keeps the layout
// the window was closed with, in the window's group of its QSettings
// (docs/superpowers/specs/2026-10-09-dock-arrangements-design.md).
//
// A child of the window it serves; MenuHub finds it there (of()) for the
// Window menu's Panels and Reset Layout. Every dock of the window needs an
// object name: Qt leaves an unnamed dock out of a saved layout.

#include <functional>

#include <QList>
#include <QObject>
#include <QString>

class QAction;
class QMainWindow;
class QSettings;
class QWidget;

namespace pychron::ui {

// The names, inside a window's group, of the keys its last layout is kept
// under.
struct DockLayoutKeys {
  QString state = QStringLiteral("state");
  QString geometry = QStringLiteral("geometry");
};

class DockLayouts : public QObject {
  Q_OBJECT

 public:
  using Keys = DockLayoutKeys;

  // `factory` puts every dock where the window's code wants it, whatever was
  // done to it since; the window has already called it once. `settings` is
  // the window's and outlives every call made here; null, nothing is read or
  // written.
  DockLayouts(QMainWindow* window, std::function<void()> factory, QSettings* settings, QString group, Keys keys = {});

  // The helper of `window`, or nullptr.
  static DockLayouts* of(const QWidget* window);

  // The factory layout. The window's size and position stay.
  void reset();
  // The layout and geometry save_last() wrote; the factory layout if what
  // was saved cannot be read; nothing if nothing was saved.
  void restore_last();
  void save_last();
  [[nodiscard]] bool can_save() const { return settings_ != nullptr; }
  // The show/hide action of each dock that can be closed, in the order the
  // docks were made.
  [[nodiscard]] QList<QAction*> panel_actions() const;
  QMainWindow* window() const { return window_; }

 private:
  QString key(const QString& name) const { return group_ + QLatin1Char('/') + name; }

  QMainWindow* window_;
  std::function<void()> factory_;
  QSettings* settings_;
  QString group_;
  Keys keys_;
};

}  // namespace pychron::ui
