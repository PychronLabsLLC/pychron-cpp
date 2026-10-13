#include "menu_hub.hpp"

#include "dock_layouts.hpp"
#include "shortcuts.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QGuiApplication>
#include <QDialog>
#include <QEvent>
#include <QInputDialog>
#include <QLayout>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QLineEdit>
#include <QStatusBar>
#include <QSplashScreen>
#include <QWindow>

namespace pychron::ui {

namespace {

constexpr std::array<MenuHub::Menu, MenuHub::kMenus> kOrder{
    MenuHub::Menu::File,    MenuHub::Menu::Queue,  MenuHub::Menu::Rows,   MenuHub::Menu::Executor,
    MenuHub::Menu::Scripts, MenuHub::Menu::View,   MenuHub::Menu::Entry,  MenuHub::Menu::Fit,
    MenuHub::Menu::Window,  MenuHub::Menu::Help};

std::size_t slot(MenuHub::Menu menu) { return static_cast<std::size_t>(menu); }

// The menus that are submenus of Experiment (adjacent in kOrder).
bool under_experiment(MenuHub::Menu menu) {
  using Menu = MenuHub::Menu;
  return menu == Menu::Queue || menu == Menu::Rows || menu == Menu::Executor || menu == Menu::Scripts;
}

constexpr std::array<MenuHub::FileRole, MenuHub::kFileRoles> kFileRoleOrder{MenuHub::FileRole::Save,
                                                                           MenuHub::FileRole::SaveAs};
constexpr std::array<MenuHub::FileList, MenuHub::kFileLists> kFileListOrder{MenuHub::FileList::New,
                                                                           MenuHub::FileList::Open};

// "Save Queue" for a window that saves queues, "Save" for one that saves nothing.
QString file_text(MenuHub::FileRole role, const QString& noun) {
  const bool plain = noun.isEmpty();
  if (role == MenuHub::FileRole::Save) return plain ? MenuHub::tr("&Save") : MenuHub::tr("&Save %1").arg(noun);
  return plain ? MenuHub::tr("Save &As…") : MenuHub::tr("Save %1 &As…").arg(noun);
}

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
  // One bar for all only where the platform shows a global one: a parentless
  // QMenuBar is the native macOS bar, and nothing at all under another
  // platform plugin (offscreen, in tests), where its shortcuts never fire.
  if (QGuiApplication::platformName() == QLatin1String("cocoa") &&
      !QCoreApplication::testAttribute(Qt::AA_DontUseNativeMenuBar))
    return Bars::Shared;
#endif
  return Bars::PerWindow;
}

MenuHub& MenuHub::reset(Bars bars) {
  QPointer<MenuHub>& hub = the_hub();
  delete hub.data();
  hub = new MenuHub(bars, QCoreApplication::instance());
  return *hub;
}

MenuHub::MenuHub(Bars bars, QObject* parent) : QObject(parent), mode_(bars) {
  QCoreApplication::instance()->installEventFilter(this);
  connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
    if (QWidget* active = QApplication::activeWindow(); active == nullptr || active->windowType() != Qt::Popup)
      under_popup_ = active;
    update_gates();
    refresh_windows();
  });
  // The Window menu's own actions, shared by every bar. Created before any
  // bar is: make_bar() fills the menu from them.
  minimize_ = new QAction(tr("Minimize"), this);
  minimize_->setShortcut(key(Shortcut::MinimizeWindow));
  connect(minimize_, &QAction::triggered, this, [this] {
    if (QWidget* w = current_window()) w->showMinimized();
  });
  zoom_ = new QAction(tr("Zoom"), this);
  connect(zoom_, &QAction::triggered, this, [this] {
    QWidget* w = current_window();
    if (w == nullptr) return;
    if (w->isMaximized()) w->showNormal();
    else w->showMaximized();
  });
  // File's own, shared by every bar. New and Open carry a submenu of what
  // windows gave them (contribute_file).
  for (const FileList list : kFileListOrder) {
    const auto at = static_cast<std::size_t>(list);
    auto* action = new QAction(list == FileList::New ? tr("&New") : tr("&Open"), this);
    file_list_menus_[at] = new QMenu;
    action->setMenu(file_list_menus_[at]);
    action->setEnabled(false);
    file_lists_[at] = action;
  }
  // Save and Save As each do what the window in front gave for it, looked up
  // as it runs (it can be reached, from the palette or a menu left open,
  // after the window it was enabled for left the front). Save As has no key:
  // the platform's is View > Spectrometer's.
  for (const FileRole role : kFileRoleOrder) {
    auto* action = new QAction(file_text(role, QString()), this);
    if (role == FileRole::Save) action->setShortcut(key(Shortcut::FileSave));
    action->setEnabled(false);
    connect(action, &QAction::triggered, this, [this, role] {
      if (const FileTarget* target = file_target(role); target != nullptr && target->action->isEnabled())
        target->action->trigger();
    });
    file_[static_cast<std::size_t>(role)] = action;
  }
  for (const Menu menu : {Menu::Queue, Menu::Rows, Menu::Executor, Menu::Scripts}) {
    auto* hint = new QAction(tr("Open View > Experiment to use this menu"), this);
    hint->setEnabled(false);
    placeholders_[slot(menu)] = hint;
  }
  bring_all_ = new QAction(tr("Bring All to Front"), this);
  connect(bring_all_, &QAction::triggered, this, [this] {
    QWidget* front = current_window();
    for (const Entry& e : windows_)
      if (e.window != nullptr && e.window != front && !e.window->isMinimized()) e.window->raise();
    if (front != nullptr) front->raise();
  });
  // The dock layout of the window in front: its panels, its named
  // arrangements, and back to the layout it was installed with. The two
  // submenus are filled as they open, from whichever window that is then.
  panels_ = new QAction(tr("Panels"), this);
  panels_menu_ = new QMenu;
  panels_->setMenu(panels_menu_);
  connect(panels_menu_, &QMenu::aboutToShow, this, &MenuHub::fill_panels);
  arrangements_ = new QAction(tr("Arrangements"), this);
  arrangements_menu_ = new QMenu;
  arrangements_->setMenu(arrangements_menu_);
  delete_menu_ = new QMenu(tr("Delete"), arrangements_menu_);
  connect(arrangements_menu_, &QMenu::aboutToShow, this, &MenuHub::fill_arrangements);
  reset_layout_ = new QAction(tr("Reset Layout"), this);
  connect(reset_layout_, &QAction::triggered, this, [this] {
    if (DockLayouts* layouts = front_layouts()) layouts->reset();
  });
  save_arrangement_ = new QAction(tr("Save Arrangement As…"), this);
  connect(save_arrangement_, &QAction::triggered, this, &MenuHub::save_arrangement);
  fill_arrangements();  // Save Arrangement As… is in its menu from the start
  if (mode_ == Bars::Shared) {
    // No parent: on macOS the bar of every window that has none of its own.
    shared_ = new QMenuBar(nullptr);
    bars_.push_back(make_bar(shared_));
  }
}

MenuHub::~MenuHub() {
  // At application exit the platform is already gone, and with it any use in
  // tidying the bar; a hub replaced by reset() deletes its own.
  if (QCoreApplication::closingDown()) return;
  delete shared_.data();
  delete panels_menu_.data();
  delete arrangements_menu_.data();  // and Delete, its child
  for (const auto& menu : file_list_menus_) delete menu.data();
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
    case Menu::View:
      return tr("&View");
    case Menu::Entry:
      return tr("E&ntry");
    case Menu::Fit:
      return tr("F&it");
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
  for (const Menu menu : kOrder) {
    if (!under_experiment(menu)) {
      b.menus[slot(menu)] = bar->addMenu(title(menu));
      continue;
    }
    if (b.experiment == nullptr) b.experiment = bar->addMenu(tr("&Experiment"));  // where the first of them comes
    b.menus[slot(menu)] = b.experiment->addMenu(title(menu));
  }
  rebuild(b);
  return b;
}

QMenu* MenuHub::experiment_menu(const QMenuBar* bar) const {
  const auto it = std::find_if(bars_.begin(), bars_.end(), [&](const Bar& b) { return b.bar == bar; });
  return it == bars_.end() ? nullptr : it->experiment.data();
}

QList<QMenu*> MenuHub::menus(const QMenuBar* bar) const {
  QList<QMenu*> out;
  for (const Bar& b : bars_) {
    if (b.bar != bar) continue;
    for (const auto& m : b.menus) out.append(m.data());
  }
  return out;
}

void MenuHub::set_arrangement_asks(ArrangementAsks asks) { asks_ = std::move(asks); }

// The layout helper of the window in front, or nullptr: read by every layout
// command as it runs, since a command can be reached (the palette, a menu
// left open) after the window it was enabled for has gone from the front.
DockLayouts* MenuHub::front_layouts() const { return DockLayouts::of(current_window()); }

void MenuHub::fill_panels() {
  panels_menu_->clear();  // the actions are the docks' own and stay theirs
  if (const DockLayouts* layouts = front_layouts()) panels_menu_->addActions(layouts->panel_actions());
}

void MenuHub::fill_arrangements() {
  arrangements_menu_->clear();  // deletes the entries made here, which it owns
  delete_menu_->clear();
  const DockLayouts* layouts = front_layouts();
  const QStringList names = layouts != nullptr ? layouts->names() : QStringList();
  for (const QString& name : names) {
    QString shown = name;
    shown.replace(QLatin1Char('&'), QStringLiteral("&&"));  // a name is not a mnemonic
    connect(arrangements_menu_->addAction(shown), &QAction::triggered, this, [this, name] { apply_arrangement(name); });
    connect(delete_menu_->addAction(shown), &QAction::triggered, this, [this, name] {
      if (DockLayouts* front = front_layouts()) front->remove(name);
    });
  }
  if (!names.isEmpty()) arrangements_menu_->addSeparator();
  arrangements_menu_->addAction(save_arrangement_);
  arrangements_menu_->addAction(delete_menu_->menuAction());
  delete_menu_->menuAction()->setVisible(!names.isEmpty());
}

void MenuHub::apply_arrangement(const QString& name) {
  DockLayouts* layouts = front_layouts();
  if (layouts == nullptr) return;
  const Result<void> applied = layouts->apply(name);
  if (applied) return;
  constexpr int kShownMs = 5000;
  layouts->window()->statusBar()->showMessage(
      tr("arrangement “%1” not applied: %2").arg(name, QString::fromStdString(applied.error().what)), kShownMs);
}

// Asks for a name until one is saved or the user gives up.
void MenuHub::save_arrangement() {
  DockLayouts* layouts = front_layouts();
  if (layouts == nullptr || !layouts->can_save()) return;
  const QPointer<QWidget> over = layouts->window();
  const QString title = tr("Save Arrangement");
  const auto ask_name = [&]() -> std::optional<QString> {
    if (asks_.name) return asks_.name(over);
    bool ok = false;
    const QString typed = QInputDialog::getText(over, title, tr("Name:"), QLineEdit::Normal, QString(), &ok);
    return ok ? std::optional<QString>(typed) : std::nullopt;
  };
  const auto ask_replace = [&](const QString& name) {
    if (asks_.replace) return asks_.replace(over, name);
    return QMessageBox::question(over, title, tr("Replace arrangement “%1”?").arg(name)) == QMessageBox::Yes;
  };
  const auto refuse = [&](const std::string& why) {
    const QString text = QString::fromStdString(why);
    if (asks_.refuse) {
      asks_.refuse(over, text);
    } else {
      QMessageBox::warning(over, title, text);
    }
  };
  while (true) {
    const std::optional<QString> typed = ask_name();
    // A dialog ran the event loop: the window may have closed under it.
    if (!typed || over == nullptr || DockLayouts::of(over) != layouts) return;
    const Result<QString> name = DockLayouts::valid_name(*typed);
    if (!name) {
      refuse(name.error().what);
      continue;
    }
    if (const QString there = layouts->stored(*name); !there.isEmpty() && !ask_replace(there)) continue;
    if (over == nullptr || DockLayouts::of(over) != layouts) return;
    if (const Result<void> saved = layouts->save_as(*name); !saved) refuse(saved.error().what);
    return;
  }
}

QList<MenuHub::Command> MenuHub::commands() const {
  QList<Command> out;
  for (const Menu menu : kOrder) {
    if (menu == Menu::File) {
      for (const FileList list : kFileListOrder)
        for (QAction* a : file_entries(list)) out.append({a, menu});
      for (QAction* a : file_) out.append({a, menu});
    }
    for (const Group& g : groups_) {
      if (g.menu != menu || g.owner == nullptr) continue;
      for (const auto& a : g.actions)
        if (a != nullptr) out.append({a.data(), menu});
    }
    if (menu == Menu::Window) {  // the hub's own that are worth finding by name
      out.append({reset_layout_, menu});
      out.append({save_arrangement_, menu});
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

void MenuHub::set_file_action(QWidget* owner, FileRole role, QAction* action, const QString& noun) {
  std::erase_if(file_targets_, [&](const FileTarget& t) { return t.owner == owner && t.role == role; });
  file_targets_.push_back({owner, role, action, noun});
  // Its enabled state is the item's; a queued call, since an action that is
  // being destroyed says so before the pointer to it is cleared.
  connect(action, &QAction::changed, this, &MenuHub::update_file_actions, Qt::UniqueConnection);
  const auto later = [this] { QMetaObject::invokeMethod(this, &MenuHub::update_file_actions, Qt::QueuedConnection); };
  connect(action, &QObject::destroyed, this, later);
  connect(owner, &QObject::destroyed, this, later);
  update_file_actions();
}

void MenuHub::contribute_file(QWidget* owner, FileList list, const QList<FileEntry>& entries) {
  for (const FileEntry& e : entries) file_items_.push_back({owner, list, e.action, e.home});
  connect(owner, &QObject::destroyed, this, &MenuHub::schedule_rebuild, Qt::UniqueConnection);
  fill_file_lists();
  update_file_actions();
}

QList<QAction*> MenuHub::file_entries(FileList list) const {
  QList<QAction*> out;
  for (const FileItem& item : file_items_)
    if (item.list == list && item.owner != nullptr && item.action != nullptr) out.append(item.action.data());
  return out;
}

// The submenus of File > New and File > Open, from the entries that are left.
void MenuHub::fill_file_lists() {
  std::erase_if(file_items_, [](const FileItem& item) { return item.owner == nullptr || item.action == nullptr; });
  for (const FileList list : kFileListOrder) {
    QMenu* menu = file_list_menus_[static_cast<std::size_t>(list)];
    const QList<QAction*> want = file_entries(list);
    const QList<QAction*> have = menu->actions();
    if (std::equal(have.begin(), have.end(), want.begin(), want.end())) continue;
    menu->clear();  // the entries are their windows' and stay theirs
    menu->addActions(want);
  }
}

const MenuHub::FileTarget* MenuHub::file_target(FileRole role) const {
  const QWidget* active = active_window();
  if (active == nullptr) return nullptr;
  const auto it = std::find_if(file_targets_.begin(), file_targets_.end(), [&](const FileTarget& t) {
    return t.role == role && t.owner != nullptr && t.action != nullptr && t.owner->window() == active;
  });
  return it == file_targets_.end() ? nullptr : &*it;
}

void MenuHub::update_file_actions() {
  std::erase_if(file_targets_, [](const FileTarget& t) { return t.owner == nullptr || t.action == nullptr; });
  for (const FileRole role : kFileRoleOrder) {
    const FileTarget* target = file_target(role);
    QAction* item = file_action(role);
    item->setText(file_text(role, target != nullptr ? target->noun : QString()));
    item->setEnabled(target != nullptr && target->action->isEnabled());
  }
  // New's and Open's key is on the entry whose window is in front, and on
  // none when no entry's is: a key that is on no enabled action is the
  // window's own to use.
  const QWidget* active = active_window();
  for (const FileList list : kFileListOrder) {
    const QKeySequence keys = key(list == FileList::New ? Shortcut::FileNew : Shortcut::FileOpen);
    bool given = false;
    bool any = false;
    for (const FileItem& item : file_items_) {
      if (item.list != list || item.owner == nullptr || item.action == nullptr) continue;
      any = true;
      const bool here = !given && active != nullptr && item.home && item.home(active);
      given = given || here;
      if (const QKeySequence want = here ? keys : QKeySequence(); item.action->shortcut() != want)
        item.action->setShortcut(want);
    }
    file_list_action(list)->setEnabled(any);
  }
}

bool MenuHub::eventFilter(QObject* watched, QEvent* event) {
  if (!watched->isWidgetType()) return false;
  auto* w = static_cast<QWidget*>(watched);
  switch (event->type()) {
    case QEvent::Show:
      if (mode_ == Bars::PerWindow && w->isWindow() && takes_bar(w)) install(w);
      // Listed in the order they are shown: noted now, not at the rebuild.
      if (w->isWindow() && takes_bar(w)) add_window(w);
      [[fallthrough]];
    case QEvent::Hide:
    case QEvent::WindowTitleChange:
    case QEvent::WindowStateChange:
      // The Window menu lists the open windows: keep up with them.
      if (w->isWindow() && takes_bar(w)) schedule_rebuild();
      break;
    default:
      break;
  }
  return false;
}

QWidget* MenuHub::active_window() const {
  QWidget* active = QApplication::activeWindow();
  if (active != nullptr && active->windowType() == Qt::Popup) return under_popup_;
  return active;
}

QWidget* MenuHub::current_window() const {
  QWidget* active = active_window();
  return active != nullptr && takes_bar(active) ? active : nullptr;
}

QList<QAction*> MenuHub::window_actions() const {
  QList<QAction*> out;
  for (const Entry& e : windows_)
    if (e.window != nullptr && e.action != nullptr) out.append(e.action.data());
  return out;
}

void MenuHub::add_window(QWidget* w) {
  if (std::any_of(windows_.begin(), windows_.end(), [&](const Entry& e) { return e.window == w; })) return;
  auto* action = new QAction(this);
  action->setCheckable(true);
  QPointer<QWidget> window(w);
  connect(action, &QAction::triggered, this, [this, window] {
    if (window == nullptr) return;
    if (window->isMinimized()) window->showNormal();
    window->raise();
    window->activateWindow();
    refresh_windows();  // the tick follows the window, not the click
  });
  windows_.push_back({window, action});
}

// Brings windows_ in step with the open windows: one action each, kept while
// its window stays open (so the menus are not rebuilt for a change of title
// or of the window in front).
void MenuHub::refresh_windows() {
  std::erase_if(windows_, [](const Entry& e) {
    const bool gone = e.window == nullptr || !e.window->isVisible();
    if (gone) delete e.action.data();
    return gone;
  });
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (w->isVisible() && takes_bar(w)) add_window(w);  // any shown before the hub was watching
  }
  const QWidget* front = current_window();
  for (const Entry& e : windows_) {
    e.action->setText(e.window->windowTitle().isEmpty() ? QCoreApplication::applicationName() : e.window->windowTitle());
    e.action->setChecked(e.window == front);
  }
  minimize_->setEnabled(front != nullptr);
  zoom_->setEnabled(front != nullptr);
  bring_all_->setEnabled(!windows_.empty());
  const DockLayouts* layouts = front_layouts();
  panels_->setEnabled(layouts != nullptr);
  arrangements_->setEnabled(layouts != nullptr);
  reset_layout_->setEnabled(layouts != nullptr);
  save_arrangement_->setEnabled(layouts != nullptr && layouts->can_save());
}

QList<QAction*> MenuHub::window_menu() const {
  QList<QAction*> want{minimize_, zoom_, nullptr, bring_all_, nullptr, panels_, arrangements_, reset_layout_};
  const QList<QAction*> open = window_actions();
  if (!open.isEmpty()) {
    want.append(nullptr);
    want.append(open);
  }
  return want;
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
  fill_file_lists();
  update_file_actions();
  refresh_windows();
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
    if (menu == Menu::Window) want = window_menu();  // the hub's own, first
    if (menu == Menu::File) {
      want = QList<QAction*>(file_lists_.begin(), file_lists_.end());
      want.append(QList<QAction*>(file_.begin(), file_.end()));
    }
    for (const Group& g : groups_) {
      if (g.menu != menu || g.owner == nullptr) continue;
      QList<QAction*> live;
      for (const auto& a : g.actions)
        if (a != nullptr) live.append(a);
      if (live.isEmpty()) continue;
      if (!want.isEmpty()) want.append(nullptr);
      want.append(live);
    }
    // An experiment menu with nothing in it yet keeps its place in the bar.
    if (want.isEmpty() && placeholders_[slot(menu)] != nullptr) want.append(placeholders_[slot(menu)]);
    sync_menu(m, want);
    m->menuAction()->setVisible(!want.isEmpty());
  }
}

void MenuHub::update_gates() {
  const QWidget* active = active_window();
  for (const Gate& g : gates_) {
    if (g.group == nullptr) continue;
    g.group->setEnabled(g.owner != nullptr && g.owner->window() == active);
  }
  update_file_actions();
}

}  // namespace pychron::ui
