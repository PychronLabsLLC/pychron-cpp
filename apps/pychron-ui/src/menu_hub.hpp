#pragma once

// MenuHub: one menu bar for the whole application. Every top-level window
// shows the same menus in the same order (File, Experiment, View, Entry, Fit,
// Window, Help). Entry and Fit are there only when a store is. Experiment
// holds Queue, Rows, Executor and Scripts as submenus: they are menus like
// the others (Menu, contribute, menus()), only one level down.
//
// View holds what windows contribute to it: the actions that open the
// application's main views. Window is the hub's own, the usual one: Minimize,
// Zoom, Bring All to Front; then Panels, Arrangements and Reset Layout, for
// the dock panels of the window in front (dock_layouts.hpp); then every open
// window, the one in front ticked; choosing one brings it forward.
//
// On macOS there is literally one bar: a parentless QMenuBar, which Qt makes
// the global menu bar for every window (Bars::Shared). Per-window bars there
// would each merge Preferences, Quit and About into the one application menu,
// and Qt hides that shared item whenever any of those copies goes. Elsewhere
// each window shows its own copy of the bar (Bars::PerWindow).
//
// Windows keep owning their actions and contribute them here. App actions
// (Preferences, View > Spectrometer, About) work from every window.
// Window actions (Delete rows, Start, Close Tab) are enabled only while
// their window is active, so the same shortcut can mean one thing in the
// experiment window and another in the script editor; the action's own
// enabled state still applies on top.
//
// File begins with the hub's own New, Open, Save and Save As, once, whatever
// windows there are. New and Open are submenus of what there is to start or
// open (New Queue, Open Script…): entries given with contribute_file, which
// work from every window and bring up the window they act in. Save and Save
// As each stand for the action the window in front gave (set_file_action):
// Save saves the queue in the experiment window and the script in the script
// editor, says which ("Save Queue"), and is greyed in a window that gave
// none. The keys are the hub's: Save's on Save, and New's and Open's on the
// entry whose window is in front (Open Queue in the experiment window), on
// none elsewhere.
//
// Per-window bars are installed when a window is first shown: every
// QMainWindow, and any other top-level widget with a layout. Dialogs, popups
// and the splash do not get one; nor does a window with the
// "pychron_no_menubar" property. Menus are updated in place: an action that
// stays is never taken out and put back.
//
// The bar is the same from launch: Experiment's Queue, Rows, Executor and
// Scripts, which the experiment window fills, are there before it is, each
// holding one greyed line saying where its commands come from. File, View and Help are
// hidden only when nothing at all has contributed to them (no main window).

#include <array>
#include <functional>
#include <optional>
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

class DockLayouts;

// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor): one instance, destroyed only by itself
class MenuHub : public QObject {
  Q_OBJECT

 public:
  // Entry and Fit come last so the slots of the others keep their values;
  // kOrder places them between View and Window.
  enum class Menu { File, Queue, Rows, Executor, Scripts, View, Window, Help, Entry, Fit };
  enum class Scope {
    App,     // works from every window
    Window,  // enabled only while `owner`'s window is active
  };
  enum class Bars {
    PerWindow,  // each window shows its own copy of the bar
    Shared,     // one parentless bar for every window (macOS)
  };
  static constexpr std::size_t kMenus = 10;
  enum class FileRole { Save, SaveAs };
  static constexpr std::size_t kFileRoles = 2;
  enum class FileList { New, Open };
  static constexpr std::size_t kFileLists = 2;
  // An entry of File > New or File > Open. `home` says whether `window` is
  // the one the entry acts in: while that window is in front the entry has
  // the list's key.
  struct FileEntry {
    QAction* action = nullptr;
    std::function<bool(const QWidget* window)> home;
  };

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

  // `action` is what File > `role` does while `owner`'s window is in front,
  // for as long as both live; `noun` names what it acts on ("Queue"). The
  // action is put in no menu and must have no shortcut. Given again for the
  // same owner and role, it replaces the one before.
  void set_file_action(QWidget* owner, FileRole role, QAction* action, const QString& noun);
  // The hub's own File item for `role` (the same in every bar).
  QAction* file_action(FileRole role) const { return file_[static_cast<std::size_t>(role)]; }

  // Adds `entries` to File > New or File > Open, in contribution order, for
  // as long as `owner` lives. They work from every window (their own enabled
  // state is all that greys them) and must have no shortcut.
  void contribute_file(QWidget* owner, FileList list, const QList<FileEntry>& entries);
  // File > New and File > Open themselves (the same in every bar): each
  // carries the submenu, and is greyed while it has no entry.
  QAction* file_list_action(FileList list) const { return file_lists_[static_cast<std::size_t>(list)]; }
  // The entries of a list, as its submenu shows them.
  QList<QAction*> file_entries(FileList list) const;

  // Gives `window` the unified bar now, if it is a window that takes one and
  // has none yet (normally done when it is first shown). Returns the bar it
  // shows: its own, or the shared one.
  QMenuBar* install(QWidget* window);
  // The bar `window` shows the menus in, or nullptr (a dialog, or a window
  // not shown yet with Bars::PerWindow).
  QMenuBar* bar_for(const QWidget* window) const;

  // The active window, for the commands that are live: a popup (the command
  // palette, a menu) that a platform hands activation to when it is shown
  // belongs to the window that was active under it.
  QWidget* active_window() const;
  // The window Minimize and Zoom act on: the active one, if it takes a bar.
  QWidget* current_window() const;

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
  // Window > Panels, Arrangements and Reset Layout act on the dock layout of
  // the window in front (dock_layouts.hpp) and are greyed when it has none.
  // Panels and Arrangements carry a submenu, filled when it is about to show.
  QAction* panels_action() const { return panels_; }
  QAction* arrangements_action() const { return arrangements_; }
  QAction* reset_layout_action() const { return reset_layout_; }
  // Arrangements > Save Arrangement As…: greyed for a window that keeps no settings.
  QAction* save_arrangement_action() const { return save_arrangement_; }
  // What Save Arrangement As… asks, over the window in front. An empty
  // function is the dialog it replaces.
  struct ArrangementAsks {
    std::function<std::optional<QString>(QWidget* over)> name;             // nullopt: cancelled
    std::function<bool(QWidget* over, const QString& name)> replace;       // one of that name is there
    std::function<void(QWidget* over, const QString& why)> refuse;         // the name will not do, or was not saved
  };
  void set_arrangement_asks(ArrangementAsks asks);

  // One per open window that takes a bar, in the order they were first
  // shown: its title, ticked when it is the active one.
  QList<QAction*> window_actions() const;
  // The menus of `bar`, indexed by Menu (hidden ones included; the order they
  // are shown in is the bar's own); empty if `bar` is not one of the hub's.
  // Queue, Rows, Executor and Scripts are submenus of experiment_menu(bar).
  QList<QMenu*> menus(const QMenuBar* bar) const;
  // `bar`'s Experiment menu, or nullptr if `bar` is not one of the hub's.
  QMenu* experiment_menu(const QMenuBar* bar) const;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  MenuHub(Bars bars, QObject* parent);
  ~MenuHub() override;

  struct Group {
    QPointer<QWidget> owner;
    Menu menu{};
    QList<QPointer<QAction>> actions;
  };
  struct Bar {
    QPointer<QMenuBar> bar;
    QPointer<QMenu> experiment;  // holds Queue, Rows, Executor and Scripts
    std::array<QPointer<QMenu>, kMenus> menus;
  };
  struct Gate {
    QPointer<QWidget> owner;
    QPointer<QActionGroup> group;
  };
  struct FileItem {
    QPointer<QWidget> owner;
    FileList list{};
    QPointer<QAction> action;
    std::function<bool(const QWidget*)> home;
  };
  struct FileTarget {
    QPointer<QWidget> owner;
    FileRole role{};
    QPointer<QAction> action;
    QString noun;
  };

  static bool takes_bar(const QWidget* window);
  Bar make_bar(QMenuBar* bar);
  void rebuild();
  void rebuild(Bar& bar);
  void schedule_rebuild();
  void update_gates();
  const FileTarget* file_target(FileRole role) const;  // of the window in front, or nullptr
  void update_file_actions();
  void fill_file_lists();
  void add_window(QWidget* window);
  void refresh_windows();
  QList<QAction*> window_menu() const;  // nullptr: a separator
  DockLayouts* front_layouts() const;
  void fill_panels();
  void fill_arrangements();
  void apply_arrangement(const QString& name);
  void save_arrangement();

  Bars mode_;
  QPointer<QMenuBar> shared_;
  QPointer<QWidget> under_popup_;  // the active window before a popup took activation
  std::vector<Group> groups_;
  std::vector<Bar> bars_;
  std::vector<Gate> gates_;
  std::vector<FileTarget> file_targets_;
  std::array<QAction*, kFileRoles> file_{};
  std::vector<FileItem> file_items_;
  std::array<QAction*, kFileLists> file_lists_{};
  std::array<QPointer<QMenu>, kFileLists> file_list_menus_;
  struct Entry {
    QPointer<QWidget> window;
    QPointer<QAction> action;
  };
  std::vector<Entry> windows_;
  std::array<QAction*, kMenus> placeholders_{};
  QAction* minimize_ = nullptr;
  QAction* zoom_ = nullptr;
  QAction* bring_all_ = nullptr;
  QAction* panels_ = nullptr;
  QAction* arrangements_ = nullptr;
  QAction* reset_layout_ = nullptr;
  QAction* save_arrangement_ = nullptr;
  ArrangementAsks asks_;
  QPointer<QMenu> panels_menu_;
  QPointer<QMenu> arrangements_menu_;
  QPointer<QMenu> delete_menu_;
  bool rebuild_pending_ = false;
};

}  // namespace pychron::ui
