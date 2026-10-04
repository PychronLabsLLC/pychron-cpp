#pragma once

// MenuHub: one menu bar for the whole application. Every top-level window
// shows the same menus in the same order (File, Queue, Rows, Executor,
// Scripts, View, Entry, Window, Help). Entry is there only when a store is.
//
// View holds what windows contribute to it: the actions that open the
// application's main views. Window is the hub's own, the usual one: Minimize,
// Zoom, Bring All to Front, then every open window, the one in front ticked;
// choosing one brings it forward.
//
// On macOS there is literally one bar: a parentless QMenuBar, which Qt makes
// the global menu bar for every window (Bars::Shared). Per-window bars there
// would each merge Preferences, Quit and About into the one application menu,
// and Qt hides that shared item whenever any of those copies goes. Elsewhere
// each window shows its own copy of the bar (Bars::PerWindow).
//
// Windows keep owning their actions and contribute them here. App actions
// (Preferences, View > Spectrometer, About) work from every window.
// Window actions (Save queue, Delete rows, Start) are enabled only while
// their window is active, so the same shortcut can mean Save queue in the
// experiment window and Save script in the script editor; the action's own
// enabled state still applies on top.
//
// Per-window bars are installed when a window is first shown: every
// QMainWindow, and any other top-level widget with a layout. Dialogs, popups
// and the splash do not get one; nor does a window with the
// "pychron_no_menubar" property. Menus are updated in place: an action that
// stays is never taken out and put back.
//
// The bar is the same from launch: Queue, Rows, Executor and Scripts, which
// the experiment window fills, are there before it is, each holding one
// greyed line saying where its commands come from. File, View and Help are
// hidden only when nothing at all has contributed to them (no main window).

#include <array>
#include <vector>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QAction;
class QActionGroup;
class QMenu;
class QMenuBar;
class QWidget;

namespace pychron::ui {

class MenuHub : public QObject {
  Q_OBJECT

 public:
  // Entry comes last so the slots of the others keep their values; kOrder
  // places it between View and Window.
  enum class Menu { File, Queue, Rows, Executor, Scripts, View, Window, Help, Entry };
  enum class Scope {
    App,     // works from every window
    Window,  // enabled only while `owner`'s window is active
  };
  enum class Bars {
    PerWindow,  // each window shows its own copy of the bar
    Shared,     // one parentless bar for every window (macOS)
  };
  static constexpr std::size_t kMenus = 9;

  // The application's hub (created on first use; needs a QApplication).
  static MenuHub& instance();
  // The platform's way: Shared on macOS, PerWindow elsewhere.
  static Bars platform_bars();
  // Replaces the hub with a fresh one using `bars` (tests; no window may
  // have contributed to or be showing the old one's menus).
  static MenuHub& reset(Bars bars);

  Bars bars() const { return mode_; }

  // Adds `actions` to `menu` as one group, separated from the other groups,
  // in contribution order, for as long as `owner` lives. `owner` is the
  // window the actions belong to (its top-level window decides Window scope).
  void contribute(QWidget* owner, Menu menu, const QList<QAction*>& actions, Scope scope);

  // Gives `window` the unified bar now, if it is a window that takes one and
  // has none yet (normally done when it is first shown). Returns the bar it
  // shows: its own, or the shared one.
  QMenuBar* install(QWidget* window);
  // The bar `window` shows the menus in, or nullptr (a dialog, or a window
  // not shown yet with Bars::PerWindow).
  QMenuBar* bar_for(const QWidget* window) const;

  // Every contributed action, in menu order then contribution order (for the
  // command palette); hidden and disabled ones included.
  struct Command {
    QAction* action;
    Menu menu;
  };
  QList<Command> commands() const;

  static QString title(Menu menu);

  // The greyed line an experiment menu (Queue, Rows, Executor, Scripts) holds
  // while nothing has contributed to it; nullptr for the other menus.
  QAction* placeholder(Menu menu) const { return placeholders_[static_cast<std::size_t>(menu)]; }

  // The Window menu's own actions (the same in every bar).
  QAction* minimize_action() const { return minimize_; }
  QAction* zoom_action() const { return zoom_; }
  QAction* bring_all_action() const { return bring_all_; }
  // One per open window that takes a bar, in the order they were first
  // shown: its title, ticked when it is the active one.
  QList<QAction*> window_actions() const;
  // The menus `bar` shows, in order (hidden ones included); empty if `bar`
  // is not one of the hub's.
  QList<QMenu*> menus(const QMenuBar* bar) const;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  MenuHub(Bars bars, QObject* parent);
  ~MenuHub() override;

  struct Group {
    QPointer<QWidget> owner;
    Menu menu;
    QList<QPointer<QAction>> actions;
  };
  struct Bar {
    QPointer<QMenuBar> bar;
    std::array<QPointer<QMenu>, kMenus> menus;
  };
  struct Gate {
    QPointer<QWidget> owner;
    QPointer<QActionGroup> group;
  };

  static bool takes_bar(const QWidget* window);
  Bar make_bar(QMenuBar* bar);
  void rebuild();
  void rebuild(Bar& bar);
  void schedule_rebuild();
  void update_gates();
  // The window Minimize and Zoom act on: the active one, if it takes a bar.
  QWidget* current_window() const;
  void add_window(QWidget* window);
  void refresh_windows();
  QList<QAction*> window_menu() const;  // nullptr: a separator

  Bars mode_;
  QPointer<QMenuBar> shared_;
  std::vector<Group> groups_;
  std::vector<Bar> bars_;
  std::vector<Gate> gates_;
  struct Entry {
    QPointer<QWidget> window;
    QPointer<QAction> action;
  };
  std::vector<Entry> windows_;
  std::array<QAction*, kMenus> placeholders_{};
  QAction* minimize_ = nullptr;
  QAction* zoom_ = nullptr;
  QAction* bring_all_ = nullptr;
  bool rebuild_pending_ = false;
};

}  // namespace pychron::ui
