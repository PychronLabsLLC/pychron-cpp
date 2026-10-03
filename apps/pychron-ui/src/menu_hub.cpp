#include "menu_hub.hpp"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QLayout>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSplashScreen>
#include <QWindow>

namespace pychron::ui {

namespace {

constexpr std::array<MenuHub::Menu, MenuHub::kMenus> kOrder{
    MenuHub::Menu::File,    MenuHub::Menu::Queue,  MenuHub::Menu::Rows, MenuHub::Menu::Executor,
    MenuHub::Menu::Scripts, MenuHub::Menu::Window, MenuHub::Menu::Help};

std::size_t slot(MenuHub::Menu menu) { return static_cast<std::size_t>(menu); }

}  // namespace

MenuHub& MenuHub::instance() {
  static QPointer<MenuHub> hub;
  if (hub == nullptr) hub = new MenuHub(QCoreApplication::instance());  // goes with the application
  return *hub;
}

MenuHub::MenuHub(QObject* parent) : QObject(parent) {
  QCoreApplication::instance()->installEventFilter(this);
  connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] { update_gates(); });
}

QString MenuHub::title(Menu menu) {
  switch (menu) {
    case Menu::File:
      return tr("&File");
    case Menu::Queue:
      return tr("&Queue");
    case Menu::Rows:
      return tr("&Rows");
    case Menu::Executor:
      return tr("&Executor");
    case Menu::Scripts:
      return tr("S&cripts");
    case Menu::Window:
      return tr("&Window");
    case Menu::Help:
      break;
  }
  return tr("&Help");
}

bool MenuHub::takes_bar(const QWidget* window) {
  if (!window->isWindow() || window->windowType() != Qt::Window) return false;  // dialogs, tools, popups, splash
  if (window->property("pychron_no_menubar").toBool()) return false;
  if (qobject_cast<const QDialog*>(window) != nullptr || qobject_cast<const QSplashScreen*>(window) != nullptr ||
      qobject_cast<const QMenu*>(window) != nullptr)
    return false;
  if (qobject_cast<const QMainWindow*>(window) != nullptr) return true;
  return window->layout() != nullptr;
}

QMenuBar* MenuHub::install(QWidget* window) {
  for (const Bar& b : bars_)
    if (b.bar != nullptr && b.bar->parentWidget() == window) return b.bar;
  if (!takes_bar(window)) return nullptr;

  auto* bar = new QMenuBar(window);
  if (auto* main = qobject_cast<QMainWindow*>(window)) {
    main->setMenuBar(bar);
  } else {
    if (window->layout()->menuBar() != nullptr) {
      delete bar;
      return nullptr;  // the window has a bar of its own
    }
    window->layout()->setMenuBar(bar);
  }
  Bar b;
  b.bar = bar;
  for (const Menu menu : kOrder) b.menus[slot(menu)] = bar->addMenu(title(menu));
  rebuild(b);
  bars_.push_back(b);
  return bar;
}

QList<QMenu*> MenuHub::menus(const QMenuBar* bar) const {
  QList<QMenu*> out;
  for (const Bar& b : bars_) {
    if (b.bar != bar) continue;
    for (const auto& m : b.menus) out.append(m.data());
  }
  return out;
}

void MenuHub::contribute(QWidget* owner, Menu menu, const QList<QAction*>& actions, Scope scope) {
  Group g{owner, menu, {}};
  for (QAction* a : actions) g.actions.append(a);
  groups_.push_back(std::move(g));

  if (scope == Scope::Window) {
    auto gate = std::find_if(gates_.begin(), gates_.end(), [&](const Gate& x) { return x.owner == owner; });
    if (gate == gates_.end()) {
      auto* group = new QActionGroup(owner);
      group->setExclusionPolicy(QActionGroup::ExclusionPolicy::None);
      gates_.push_back({owner, group});
      gate = std::prev(gates_.end());
    }
    for (QAction* a : actions) gate->group->addAction(a);
  }
  // Its menus go when it does (deferred: its actions are being destroyed).
  connect(owner, &QObject::destroyed, this, &MenuHub::schedule_rebuild, Qt::UniqueConnection);
  rebuild();
  update_gates();
}

bool MenuHub::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Show && watched->isWidgetType()) {
    auto* w = static_cast<QWidget*>(watched);
    if (w->isWindow() && takes_bar(w)) install(w);
  }
  return false;
}

void MenuHub::schedule_rebuild() {
  if (rebuild_pending_) return;
  rebuild_pending_ = true;
  QMetaObject::invokeMethod(
      this,
      [this] {
        rebuild_pending_ = false;
        rebuild();
      },
      Qt::QueuedConnection);
}

void MenuHub::rebuild() {
  std::erase_if(groups_, [](const Group& g) { return g.owner == nullptr; });
  std::erase_if(bars_, [](const Bar& b) { return b.bar == nullptr; });
  std::erase_if(gates_, [](const Gate& g) { return g.owner == nullptr || g.group == nullptr; });
  for (Bar& b : bars_) rebuild(b);
}

void MenuHub::rebuild(Bar& b) {
  for (const Menu menu : kOrder) {
    QMenu* m = b.menus[slot(menu)];
    if (m == nullptr) continue;
    m->clear();  // the actions belong to their windows; only separators go
    bool any = false;
    for (const Group& g : groups_) {
      if (g.menu != menu || g.owner == nullptr) continue;
      QList<QAction*> live;
      for (const auto& a : g.actions)
        if (a != nullptr) live.append(a);
      if (live.isEmpty()) continue;
      if (any) m->addSeparator();
      m->addActions(live);
      any = true;
    }
    m->menuAction()->setVisible(any);
  }
}

void MenuHub::update_gates() {
  const QWidget* active = QApplication::activeWindow();
  for (const Gate& g : gates_) {
    if (g.group == nullptr) continue;
    g.group->setEnabled(g.owner != nullptr && g.owner->window() == active);
  }
}

}  // namespace pychron::ui
