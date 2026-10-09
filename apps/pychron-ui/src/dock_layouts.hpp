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
#include <QStringList>

#include "pychron/core/error.hpp"

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
  // The show/hide action of each dock that can be closed, by title.
  [[nodiscard]] QList<QAction*> panel_actions() const;
  QMainWindow* window() const { return window_; }

  // ---- named arrangements: a layout and the window's geometry, kept under
  // <group>/arrangements/<name>. Names are told apart ignoring case.

  // `raw` trimmed, or why it cannot name an arrangement (Config, code
  // "bad_name"): 1 to 64 characters, no slash, backslash or control character.
  [[nodiscard]] static Result<QString> valid_name(const QString& raw);
  // Sorted, ignoring case.
  [[nodiscard]] QStringList names() const;
  [[nodiscard]] bool contains(const QString& name) const;
  // Keeps the layout the window has now, in place of any of that name.
  // Config "bad_name" or "no_settings"; Io "not_saved" when the settings
  // could not be written (nothing is then kept).
  [[nodiscard]] Result<void> save_as(const QString& name);
  // Config "unknown_name" (the layout stays as it is) or "bad_layout" (what
  // was saved cannot be read: the factory layout, and applyFailed).
  [[nodiscard]] Result<void> apply(const QString& name);
  void remove(const QString& name);

 signals:
  // "arrangement “<name>” not applied: <why>"
  void applyFailed(const QString& message);

 private:
  QString key(const QString& name) const { return group_ + QLatin1Char('/') + name; }
  // The name as it was saved, or empty.
  QString stored_name(const QString& name) const;

  QMainWindow* window_;
  std::function<void()> factory_;
  QSettings* settings_;
  QString group_;
  Keys keys_;
};

}  // namespace pychron::ui
