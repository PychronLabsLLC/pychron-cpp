#pragma once

// Brand: the pychron mark, the splash screen, the About dialog and the
// application icon. Everything is painted from the theme at the size and
// pixel ratio it is shown at, so there are no image assets.
//
// The mark is the argon spectrum the program exists to measure: peaks at
// masses 36 to 40, drawn as a spectrometer draws them.

#include <chrono>

#include <QDialog>
#include <QElapsedTimer>
#include <QIcon>
#include <QPixmap>
#include <QSplashScreen>
#include <QString>

class QAction;
class QLabel;
class QMainWindow;
class QPainter;
class QPushButton;
class QRectF;

namespace pychron::ui {

// The release, from the CMake project version.
QString app_version();

// Version, build and runtime details, one "name: value" per line, for the
// About dialog and for pasting into a bug report.
QString build_info();

namespace brand {

// The argon peaks, filling `area` (baseline at its bottom). `labels` writes
// the masses under the peaks; `color` strokes and, faded, fills them.
void paint_peaks(QPainter& p, const QRectF& area, const QColor& color, bool labels);

// The splash and About banner art: background, grid, peaks and wordmark.
// `simulation` adds the SIMULATION badge.
QPixmap banner(QSize size, qreal dpr, bool simulation, bool compact);

// The application icon: the peaks on an ink tile.
QIcon app_icon();
// One size of it (the installers' icon files are rendered from these:
// pychron-ui --write-icons, tools/make_icons.py).
QPixmap icon_pixmap(int size);

// Gives `window` its icon and a Help menu with About pychron (the application
// menu on macOS), whose dialog is built on first use as the window's child.
// Returns the About action.
QAction* add_help_menu(QMainWindow* window);

}  // namespace brand

// Shown while main() loads the line, spectrometer and lab. status() repaints
// at once, so it is seen even though loading blocks the event loop.
// finish_after() keeps it up for a minimum time without blocking: a load that
// takes a few milliseconds would otherwise only flicker it. A click closes it.
class SplashScreen : public QSplashScreen {
  Q_OBJECT

 public:
  explicit SplashScreen(bool simulation);

  void status(const QString& text);
  // Closes once `window` is shown and the splash has been up for `minimum`.
  void finish_after(QWidget* window, std::chrono::milliseconds minimum);
  QString status_text() const { return message(); }
  bool simulation() const { return simulation_; }

 protected:
  void drawContents(QPainter* painter) override;

 private:
  bool simulation_;
  QElapsedTimer shown_;
};

class AboutDialog : public QDialog {
  Q_OBJECT

 public:
  explicit AboutDialog(QWidget* parent = nullptr);

  QPushButton* copy_button() const { return copy_; }
  QString text() const;  // everything shown below the banner, as plain text

 private:
  QLabel* banner_ = nullptr;
  QLabel* body_ = nullptr;
  QLabel* build_ = nullptr;
  QPushButton* copy_ = nullptr;
};

}  // namespace pychron::ui
