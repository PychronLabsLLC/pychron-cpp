// The unified menu bar: every window shows the same menus in the same order,
// app actions work from any window, a window's own actions only while it is
// active (so one shortcut can mean different things in different windows),
// and empty menus are hidden everywhere.

#include <QtTest/QtTest>

#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "main_window.hpp"
#include "menu_hub.hpp"
#include "ui_fixture.hpp"

using pychron::ui::MenuHub;
using Menu = MenuHub::Menu;
using Scope = MenuHub::Scope;

namespace {

QMenuBar* bar_of(QWidget& w) { return MenuHub::instance().bar_for(&w); }

// Counts `action` being taken out of the menus it is in.
struct RemovalWatch : QObject {
  QAction* action;
  int removed = 0;
  explicit RemovalWatch(QAction* a) : action(a) {}
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::ActionRemoved && static_cast<QActionEvent*>(e)->action() == action) ++removed;
    return false;
  }
};

// The titles of the menus `w` shows, without mnemonics.
QStringList shown(QWidget& w) {
  QStringList out;
  QMenuBar* bar = bar_of(w);
  if (bar == nullptr) return out;
  for (QMenu* m : MenuHub::instance().menus(bar))
    if (m->menuAction()->isVisible()) out << m->title().remove(QLatin1Char('&'));
  return out;
}

QStringList texts(QMenu* menu) {
  QStringList out;
  for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
  return out;
}

QMenu* menu_of(QWidget& w, Menu menu) { return MenuHub::instance().menus(bar_of(w)).at(static_cast<int>(menu)); }

bool activate(QWidget& w) {
  w.activateWindow();
  return QTest::qWaitForWindowActive(&w);
}

// A plain window with a layout, like the recall and data browser windows.
struct PlainWindow : QWidget {
  PlainWindow() { new QVBoxLayout(this); }
};

}  // namespace

class TestMenuHub : public QObject {
  Q_OBJECT

  QTemporaryDir tmp_;

 private slots:
  void init() { QCoreApplication::processEvents(); }  // the last test's windows leave the menus
  void cleanup() {
    QCoreApplication::processEvents();
    if (MenuHub::instance().bars() != MenuHub::platform_bars()) MenuHub::reset(MenuHub::platform_bars());
  }

  void every_window_shows_the_same_menus() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    QMainWindow figure;  // a secondary window that contributes nothing
    PlainWindow recall;
    QDialog dialog;
    new QVBoxLayout(&dialog);
    for (QWidget* w : {static_cast<QWidget*>(&main), static_cast<QWidget*>(&figure), static_cast<QWidget*>(&recall),
                       static_cast<QWidget*>(&dialog)})
      w->show();

    const QStringList expected{QStringLiteral("File"), QStringLiteral("Window"), QStringLiteral("Help")};
    QCOMPARE(shown(main), expected);
    QCOMPARE(shown(figure), expected);
    QCOMPARE(shown(recall), expected);
    QVERIFY(bar_of(dialog) == nullptr);  // dialogs keep no bar
    // One bar on macOS, a copy per window elsewhere.
    QCOMPARE(bar_of(figure) == bar_of(main), MenuHub::platform_bars() == MenuHub::Bars::Shared);

    // The same actions, not copies: File > Preferences in the figure window
    // is the main window's.
    QVERIFY(menu_of(figure, Menu::File)->actions().contains(main.preferences_action()));
    QVERIFY(menu_of(recall, Menu::Window)->actions().contains(main.spectrometer_action()));
    QCOMPARE(texts(menu_of(figure, Menu::Window)).first(), QStringLiteral("Extraction Line"));
    QVERIFY(menu_of(figure, Menu::Help)->actions().contains(main.about_action()));
  }

  void shared_one_bar_serves_every_window() {
    MenuHub& hub = MenuHub::reset(MenuHub::Bars::Shared);
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow main(*line);
    QMainWindow figure;
    PlainWindow recall;
    QDialog dialog;
    new QVBoxLayout(&dialog);
    for (QWidget* w : {static_cast<QWidget*>(&main), static_cast<QWidget*>(&figure), static_cast<QWidget*>(&recall),
                       static_cast<QWidget*>(&dialog)})
      w->show();

    QMenuBar* bar = hub.bar_for(&main);
    QVERIFY(bar != nullptr);
    QVERIFY(bar->parentWidget() == nullptr);  // Qt's global bar on macOS
    QCOMPARE(hub.bar_for(&figure), bar);
    QCOMPARE(hub.bar_for(&recall), bar);
    QVERIFY(hub.bar_for(&dialog) == nullptr);
    // No window has a bar of its own.
    QVERIFY(main.menuWidget() == nullptr);
    QVERIFY(figure.menuWidget() == nullptr);
    QVERIFY(recall.layout()->menuBar() == nullptr);
    QCOMPARE(shown(main), (QStringList{QStringLiteral("File"), QStringLiteral("Window"), QStringLiteral("Help")}));
    QVERIFY(menu_of(main, Menu::File)->actions().contains(main.preferences_action()));
    QVERIFY(menu_of(main, Menu::Help)->actions().contains(main.about_action()));

    // Another window's menus come and go in the same bar.
    auto* owner = new QMainWindow;
    auto* save = new QAction(QStringLiteral("Save"), owner);
    hub.contribute(owner, Menu::Queue, {save}, Scope::Window);
    QVERIFY(shown(figure).contains(QStringLiteral("Queue")));
    delete owner;
    QTRY_VERIFY(!shown(figure).contains(QStringLiteral("Queue")));
  }

  // On macOS taking Preferences, Quit or About out of a menu hides its item in
  // the application menu, so a change elsewhere must leave them in place.
  void actions_that_stay_are_never_taken_out() {
    for (const auto bars : {MenuHub::Bars::PerWindow, MenuHub::Bars::Shared}) {
      MenuHub& hub = MenuHub::reset(bars);
      auto line = pychron::ui::test::make_example_line();
      pychron::ui::MainWindow main(*line);
      QMainWindow figure;
      main.show();
      figure.show();
      RemovalWatch watch(main.preferences_action());
      for (QMenu* m : hub.menus(hub.bar_for(&figure))) m->installEventFilter(&watch);
      for (QMenu* m : hub.menus(hub.bar_for(&main))) m->installEventFilter(&watch);

      auto* owner = new QMainWindow;
      auto* first = new QAction(QStringLiteral("first"), owner);
      auto* last = new QAction(QStringLiteral("last"), owner);
      hub.contribute(owner, Menu::File, {first}, Scope::App);  // after Preferences' group
      hub.contribute(owner, Menu::Queue, {last}, Scope::Window);
      QCOMPARE(texts(menu_of(figure, Menu::File)).last(), QStringLiteral("first"));
      delete owner;
      QTRY_VERIFY(!shown(figure).contains(QStringLiteral("Queue")));  // the deferred rebuild has run
      QVERIFY(!texts(menu_of(figure, Menu::File)).contains(QStringLiteral("first")));

      QCOMPARE(watch.removed, 0);
      QVERIFY(menu_of(figure, Menu::File)->actions().contains(main.preferences_action()));
      QVERIFY(menu_of(main, Menu::File)->actions().contains(main.preferences_action()));
      QVERIFY(!menu_of(figure, Menu::File)->actions().last()->isSeparator());  // no stray separator
      QCoreApplication::processEvents();
      if (bars != MenuHub::platform_bars()) MenuHub::reset(MenuHub::platform_bars());
    }
  }

  void a_menu_appears_with_its_actions_and_goes_with_its_window() {
    QMainWindow a;
    a.show();
    QVERIFY(!shown(a).contains(QStringLiteral("Queue")));
    auto* owner = new QMainWindow;
    auto* save = new QAction(QStringLiteral("Save"), owner);
    MenuHub::instance().contribute(owner, Menu::Queue, {save}, Scope::Window);
    QVERIFY(shown(a).contains(QStringLiteral("Queue")));  // at once, in a window that was already up
    QCOMPARE(texts(menu_of(a, Menu::Queue)), QStringList{QStringLiteral("Save")});
    delete owner;
    QTRY_VERIFY(!shown(a).contains(QStringLiteral("Queue")));
  }

  void groups_are_separated_in_contribution_order() {
    QMainWindow a;
    a.show();
    QMainWindow owner;
    auto* x = new QAction(QStringLiteral("x"), &owner);
    auto* y = new QAction(QStringLiteral("y"), &owner);
    auto* z = new QAction(QStringLiteral("z"), &owner);
    MenuHub::instance().contribute(&owner, Menu::Scripts, {x, y}, Scope::App);
    MenuHub::instance().contribute(&owner, Menu::Scripts, {z}, Scope::App);
    QCOMPARE(texts(menu_of(a, Menu::Scripts)),
             (QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("|"), QStringLiteral("z")}));
  }

  void window_actions_follow_the_active_window() {
    QMainWindow a;
    QMainWindow b;
    auto* save_a = new QAction(QStringLiteral("Save queue"), &a);
    auto* save_b = new QAction(QStringLiteral("Save script"), &b);
    auto* app = new QAction(QStringLiteral("Everywhere"), &a);
    save_a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    save_b->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));  // the same key, in the same bar
    app->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J));
    int saved_a = 0, saved_b = 0, everywhere = 0;
    connect(save_a, &QAction::triggered, this, [&] { ++saved_a; });
    connect(save_b, &QAction::triggered, this, [&] { ++saved_b; });
    connect(app, &QAction::triggered, this, [&] { ++everywhere; });
    MenuHub::instance().contribute(&a, Menu::Queue, {save_a}, Scope::Window);
    MenuHub::instance().contribute(&b, Menu::Scripts, {save_b}, Scope::Window);
    MenuHub::instance().contribute(&a, Menu::Window, {app}, Scope::App);
    a.show();
    b.show();

    if (!activate(a)) QSKIP("this platform does not activate windows");
    QVERIFY(save_a->isEnabled());
    QVERIFY(!save_b->isEnabled());
    QTest::keyClick(&a, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(saved_a, 1);
    QCOMPARE(saved_b, 0);

    QVERIFY(activate(b));
    QVERIFY(!save_a->isEnabled());
    QVERIFY(save_b->isEnabled());
    QTest::keyClick(&b, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(saved_a, 1);
    QCOMPARE(saved_b, 1);
    // The menu still shows both, with their shortcuts.
    QCOMPARE(save_a->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_S));

    // An app action works from a window that is not its owner's.
    QTest::keyClick(&b, Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(everywhere, 1);
  }

  void a_window_still_decides_its_own_actions_enabled_state() {
    QMainWindow a;
    QMainWindow other;
    auto* save = new QAction(QStringLiteral("Save"), &a);
    MenuHub::instance().contribute(&a, Menu::Queue, {save}, Scope::Window);
    a.show();
    other.show();
    if (!activate(a)) QSKIP("this platform does not activate windows");
    save->setEnabled(false);  // nothing to save
    QVERIFY(activate(other));
    QVERIFY(activate(a));
    QVERIFY(!save->isEnabled());  // becoming active does not override it
    save->setEnabled(true);
    QVERIFY(save->isEnabled());
    QVERIFY(activate(other));
    save->setEnabled(true);  // and inactive wins over the window's own say-so
    QVERIFY(!save->isEnabled());
  }

  void the_experiment_window_fills_the_queue_rows_executor_and_scripts_menus() {
    pychron::ui::test::SimLab sim;
    pychron::ui::ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::ExperimentWindow window(
        bridge, true, std::make_unique<QSettings>(tmp_.filePath(QStringLiteral("s.ini")), QSettings::IniFormat));
    window.show();
    QCOMPARE(shown(window), (QStringList{QStringLiteral("Queue"), QStringLiteral("Rows"), QStringLiteral("Executor"),
                                         QStringLiteral("Scripts")}));
    QVERIFY(texts(menu_of(window, Menu::Queue)).contains(QStringLiteral("&Save")));
    QVERIFY(texts(menu_of(window, Menu::Executor)).contains(QStringLiteral("Start")));
    QVERIFY(texts(menu_of(window, Menu::Scripts)).contains(QStringLiteral("Script &Editor...")));
    // And a window with nothing of its own shows them too.
    QMainWindow figure;
    figure.show();
    QCOMPARE(shown(figure), shown(window));
  }
};

QTEST_MAIN(TestMenuHub)
#include "test_menu_hub.moc"
