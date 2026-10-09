// DockLayouts: a window's factory layout, its last one and its named
// arrangements, on a bare QMainWindow with three docks.

#include <functional>
#include <memory>
#include <string>

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "dock_layouts.hpp"

using pychron::ui::DockLayouts;

namespace {

// A (left), B (right), C (right, behind B, not closable); with `extra`, D
// (bottom) as well.
struct Window {
  explicit Window(QSettings* settings, bool extra = false, DockLayouts::Keys keys = {}) {
    w.setCentralWidget(new QLabel(QStringLiteral("center")));
    a = dock(QStringLiteral("A"));
    b = dock(QStringLiteral("B"));
    c = dock(QStringLiteral("C"));
    c->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    if (extra) d = dock(QStringLiteral("D"));
    const std::function<void()> factory = [this] {
      for (QDockWidget* each : {a, b, c, d})
        if (each != nullptr) each->setFloating(false);
      w.addDockWidget(Qt::LeftDockWidgetArea, a);
      w.addDockWidget(Qt::RightDockWidgetArea, b);
      w.addDockWidget(Qt::RightDockWidgetArea, c);
      if (d != nullptr) w.addDockWidget(Qt::BottomDockWidgetArea, d);
      w.tabifyDockWidget(b, c);
      for (QDockWidget* each : {a, b, c, d})
        if (each != nullptr) each->show();
      b->raise();
    };
    factory();
    layouts = new DockLayouts(&w, factory, settings, QStringLiteral("win"), std::move(keys));
    w.resize(800, 600);
  }
  Q_DISABLE_COPY_MOVE(Window)
  ~Window() = default;

  QDockWidget* dock(const QString& name) {
    auto* made = new QDockWidget(name, &w);
    made->setObjectName(name);
    made->setWidget(new QLabel(name));
    return made;
  }

  void show() {
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
  }

  // Every dock docked, shown (B and C share tabs: one of them is in front),
  // in the place the factory gives it.
  void verify_factory() const {
    for (QDockWidget* each : {a, b, c, d}) {
      if (each == nullptr) continue;
      QVERIFY2(!each->isHidden(), qPrintable(each->objectName()));
      QVERIFY2(!each->isFloating(), qPrintable(each->objectName()));
    }
    QVERIFY(a->isVisible());
    QVERIFY(b->isVisible());
    QCOMPARE(w.dockWidgetArea(a), Qt::LeftDockWidgetArea);
    QCOMPARE(w.dockWidgetArea(b), Qt::RightDockWidgetArea);
    QCOMPARE(w.dockWidgetArea(c), Qt::RightDockWidgetArea);
    QVERIFY(w.tabifiedDockWidgets(b).contains(c));
  }

  QMainWindow w;
  QDockWidget* a = nullptr;
  QDockWidget* b = nullptr;
  QDockWidget* c = nullptr;
  QDockWidget* d = nullptr;
  DockLayouts* layouts = nullptr;
};

QStringList texts(const QList<QAction*>& actions) {
  QStringList out;
  for (const QAction* action : actions) out.append(action->text());
  return out;
}

}  // namespace

class TestDockLayouts : public QObject {
  Q_OBJECT

 private:
  static std::unique_ptr<QSettings> ini(const QTemporaryDir& dir) {
    return std::make_unique<QSettings>(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
  }

 private slots:
  void resetRestoresFactoryAfterEverythingMoved() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->close();
    win.b->setFloating(true);
    win.w.addDockWidget(Qt::BottomDockWidgetArea, win.c);
    QVERIFY(!win.a->isVisible());
    QVERIFY(win.b->isFloating());
    QCOMPARE(win.w.dockWidgetArea(win.c), Qt::BottomDockWidgetArea);

    win.layouts->reset();
    win.verify_factory();
  }

  void resetDocksPanelsThatFloatTogetherOrAreHiddenAndFloating() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    win.a->setFloating(true);
    win.b->setFloating(true);
    win.b->hide();

    win.layouts->reset();
    win.verify_factory();
    QCOMPARE(win.w.findChildren<QDockWidget*>().size(), 3);
  }

  void lastLayoutSurvivesASecondWindow() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    {
      Window first(settings.get());
      first.show();
      first.a->close();
      first.b->setFloating(true);
      first.layouts->save_last();
    }
    Window second(settings.get());
    second.layouts->restore_last();
    second.show();
    QVERIFY(!second.a->isVisible());
    QVERIFY(second.b->isFloating());
    QVERIFY(second.c->isVisible());
  }

  void garbageLastLayoutGivesFactoryWithoutError() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    settings->setValue(QStringLiteral("win/state"), QByteArray("not a layout"));
    Window win(settings.get());
    win.show();
    win.a->close();
    win.layouts->restore_last();
    win.verify_factory();
  }

  void customKeysAreUsed() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get(), false, DockLayouts::Keys{QStringLiteral("dock_state"), QStringLiteral("geometry")});
    win.show();
    win.layouts->save_last();
    QVERIFY(settings->contains(QStringLiteral("win/dock_state")));
    QVERIFY(settings->contains(QStringLiteral("win/geometry")));
    QVERIFY(!settings->contains(QStringLiteral("win/state")));
  }

  void withoutSettingsNothingIsWritten() {
    QTemporaryDir dir;
    Window win(nullptr);
    win.show();
    QVERIFY(!win.layouts->can_save());
    win.a->close();
    win.layouts->save_last();
    win.layouts->restore_last();
    QVERIFY(!win.a->isVisible());  // nothing saved, nothing restored
    win.layouts->reset();
    win.verify_factory();
    QVERIFY(QDir(dir.path()).isEmpty());
  }

  void withSettingsItCanSave() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    const Window win(settings.get());
    QVERIFY(win.layouts->can_save());
    QCOMPARE(win.layouts->window(), &win.w);
  }

  void panelActionsAreTheClosableDocks() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    Window win(settings.get());
    win.show();
    const QList<QAction*> panels = win.layouts->panel_actions();
    QCOMPARE(texts(panels), QStringList({QStringLiteral("A"), QStringLiteral("B")}));  // C is not closable
    panels[0]->trigger();
    QVERIFY(!win.a->isVisible());
    panels[0]->trigger();
    QVERIFY(win.a->isVisible());
  }

  void ofFindsTheHelperOrNull() {
    QTemporaryDir dir;
    const auto settings = ini(dir);
    const Window win(settings.get());
    QCOMPARE(DockLayouts::of(&win.w), win.layouts);
    const QMainWindow bare;
    QVERIFY(DockLayouts::of(&bare) == nullptr);
    QVERIFY(DockLayouts::of(nullptr) == nullptr);
  }
};

QTEST_MAIN(TestDockLayouts)
#include "test_dock_layouts.moc"
