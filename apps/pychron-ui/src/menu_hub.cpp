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

QPointer<MenuHub>& the_hub() {
  static QPointer<MenuHub> hub;
  return hub;
}

}  // namespace

MenuHub& MenuHub::instance() {
  QPointer<MenuHub>& hub = the_hub();
  if (hub == nullptr) hub = new MenuHub(platform_bars(), QCoreApplication::instance());  // goes with the application
  return *hub;
}

MenuHub::Bars MenuHub::platform_bars() {
#ifdef Q_OS_MACOS
  return Bars::Shared;
#else
  return Bars::PerWindow;
#endif
}

MenuHub& MenuHub::reset(Bars bars) {
  QPointer<MenuHub>& hub = the_hub();
  delete hub.data();
  hub = new MenuHub(bars, QCoreApplication::instance());
  return *hub;
}

MenuHub::MenuHub(Bars bars, QObject* parent) : QObject(parent), mode_(bars) {
  QCoreApplication::instance()->installEventFilter(this);
  connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] { update_gates(); });
  if (mode_ == Bars::Shared) {
    // No parent: on macOS the bar of every window that has none of its own.
    shared_ = new QMenuBar(nullptr);
    bars_.push_back(make_bar(shared_));
  }
}

MenuHub::~MenuHub() {
  // At application exit the platform is already gone, and with it any use in
  // tidying the bar; a hub replaced by reset() deletes its own.
  if (!QCoreApplication::closingDown()) delete shared_.data();
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
  if (QMenuBar* bar = bar_for(window)) return bar;
  if (mode_ == Bars::Shared || !takes_bar(window)) return nullptr;

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
  bars_.push_back(make_bar(bar));
  return bar;
}

QMenuBar* MenuHub::bar_for(const QWidget* window) const {
  if (window == nullptr) return nullptr;
  if (mode_ == Bars::Shared) return takes_bar(window) ? shared_.data() : nullptr;
  for (const Bar& b : bars_)
    if (b.bar != nullptr && b.bar->parentWidget() == window) return b.bar;
  return nullptr;
}

MenuHub::Bar MenuHub::make_bar(QMenuBar* bar) {
  Bar b;
  b.bar = bar;
  for (const Menu menu : kOrder) b.menus[slot(menu)] = bar->addMenu(title(menu));
  rebuild(b);
  return b;
}

QList<QMenu*> MenuHub::menus(const QMenuBar* bar) const {
  QList<QMenu*> out;
  for (const Bar& b : bars_) {
    if (b.bar != bar) continue;
    for (const auto& m : b.menus) out.append(m.data());
  }
  return out;
}

QList<MenuHub::Command> MenuHub::commands() const {
  QList<Command> out;
  for (const Menu menu : kOrder) {
    for (const Group& g : groups_) {
      if (g.menu != menu || g.owner == nullptr) continue;
      for (const auto& a : g.actions)
        if (a != nullptr) out.append({a.data(), menu});
    }
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
    if (mode_ == Bars::PerWindow && w->isWindow() && takes_bar(w)) install(w);
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

namespace {

// Brings `m` to `want` (nullptr: a separator) without taking out an action
// that stays: on macOS a Preferences, Quit or About action taken out of a menu
// hides the application menu's item until the bar is next switched.
void sync_menu(QMenu* m, const QList<QAction*>& want) {
  QList<QAction*> separators;  // the menu's own, reused in order
  for (QAction* a : m->actions())
    if (a->isSeparator() && a->parent() == m) separators.append(a);
  QList<QAction*> target;
  qsizetype used = 0;
  for (QAction* a : want) {
    if (a != nullptr) {
      target.append(a);
    } else if (used < separators.size()) {
      target.append(separators.at(used++));
    } else {
      auto* separator = new QAction(m);
      separator->setSeparator(true);
      target.append(separator);
    }
  }
  // Not QList ==: Qt 6.4 memcmps two empty lists' null data (UBSan).
  const QList<QAction*> current = m->actions();
  if (std::equal(current.begin(), current.end(), target.begin(), target.end())) return;
  for (QAction* a : current) {
    if (target.contains(a)) continue;
    m->removeAction(a);
    if (a->isSeparator() && a->parent() == m) delete a;
  }
  for (qsizetype i = 0; i < target.size(); ++i) {
    const QList<QAction*> now = m->actions();
    if (i < now.size() && now.at(i) == target.at(i)) continue;
    m->insertAction(i < now.size() ? now.at(i) : nullptr, target.at(i));  // moves it if already there
  }
}

}  // namespace

void MenuHub::rebuild(Bar& b) {
  for (const Menu menu : kOrder) {
    QMenu* m = b.menus[slot(menu)];
    if (m == nullptr) continue;
    QList<QAction*> want;  // the actions belong to their windows
    for (const Group& g : groups_) {
      if (g.menu != menu || g.owner == nullptr) continue;
      QList<QAction*> live;
      for (const auto& a : g.actions)
        if (a != nullptr) live.append(a);
      if (live.isEmpty()) continue;
      if (!want.isEmpty()) want.append(nullptr);
      want.append(live);
    }
    sync_menu(m, want);
    m->menuAction()->setVisible(!want.isEmpty());
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
