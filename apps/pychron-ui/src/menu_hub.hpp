#pragma once

// MenuHub: one menu bar for the whole application. Every top-level window
// shows the same menus in the same order (File, Queue, Rows, Executor,
// Scripts, Window, Help); on macOS that makes the global bar the same
// whichever window is in front.
//
// Windows keep owning their actions and contribute them here. App actions
// (Preferences, Window > Spectrometer, About) work from every window.
// Window actions (Save queue, Delete rows, Start) are enabled only while
// their window is active, so the same shortcut can mean Save queue in the
// experiment window and Save script in the script editor; the action's own
// enabled state still applies on top.
//
// Bars are installed when a window is first shown: every QMainWindow, and
// any other top-level widget with a layout. Dialogs, popups and the splash
// do not get one; nor does a window with the "pychron_no_menubar" property.
// A menu with nothing in it is hidden, in every window alike.

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
  enum class Menu { File, Queue, Rows, Executor, Scripts, Window, Help };
  enum class Scope {
    App,     // works from every window
    Window,  // enabled only while `owner`'s window is active
  };
  static constexpr std::size_t kMenus = 7;

  // The application's hub (created on first use; needs a QApplication).
  static MenuHub& instance();

  // Adds `actions` to `menu` as one group, separated from the other groups,
  // in contribution order, for as long as `owner` lives. `owner` is the
  // window the actions belong to (its top-level window decides Window scope).
  void contribute(QWidget* owner, Menu menu, const QList<QAction*>& actions, Scope scope);

  // Gives `window` the unified bar now, if it is a window that takes one and
  // has none yet (normally done when it is first shown). Returns the bar.
  QMenuBar* install(QWidget* window);

  static QString title(Menu menu);
  // The menus `bar` shows, in order (hidden ones included); empty if `bar`
  // is not one of the hub's.
  QList<QMenu*> menus(const QMenuBar* bar) const;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  explicit MenuHub(QObject* parent);

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
  void rebuild();
  void rebuild(Bar& bar);
  void schedule_rebuild();
  void update_gates();

  std::vector<Group> groups_;
  std::vector<Bar> bars_;
  std::vector<Gate> gates_;
  bool rebuild_pending_ = false;
};

}  // namespace pychron::ui
