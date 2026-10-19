#pragma once

// The Publish window (data publishing spec, section 6.3): the store's
// releases, and what elctl publish does to them, without a terminal. Every
// store call runs on the bridge's worker (a store connection belongs to the
// thread that opened it); results come back here, and never to a window that
// was closed meanwhile. Dialogs are reached through hooks tests replace.

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <QMainWindow>
#include <QString>
#include <QStringList>

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/publish/release.hpp"
#pragma pop_macro("signals")

class QLabel;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableView;

namespace pychron::processing {
class StoreSource;
}

namespace pychron::ui {

class EntryBridge;

class PublishWindow : public QMainWindow {
  Q_OBJECT

 public:
  // The columns of the releases table.
  enum Column { Name, Version, State, Visibility, Members, Doi, Created, kColumns };

  PublishWindow(EntryBridge& bridge, processing::StoreSource& source, std::string store_url,
                std::function<QStringList()> selection, QWidget* parent = nullptr);
  ~PublishWindow() override;
  PublishWindow(const PublishWindow&) = delete;
  PublishWindow& operator=(const PublishWindow&) = delete;

  // Reads the releases (and embargoes) again; the current row is kept.
  void refresh();

  QTableView* releases() const noexcept { return releases_; }
  QLabel* status() const noexcept { return status_; }
  QProgressBar* progress() const noexcept { return progress_; }
  bool busy() const noexcept { return jobs_ > 0; }  // a job of this window is on the worker
  QString selected_release() const;                 // uuid of the current row; empty when none
  void select_release(const QString& uuid);

  // New release…: the selection's analyses and how many of them are under
  // embargo now; the spec the user filled in, or nullopt. Its analyses,
  // options and versions are the window's to set. Default: ReleaseDialog.
  using AskRelease = std::function<std::optional<publish::ReleaseSpec>(const QStringList& analyses, std::size_t embargoed)>;
  void set_ask_release(AskRelease ask) { ask_release_ = std::move(ask); }
  void new_release();

 Q_SIGNALS:
  void refreshed();

 private:
  void say(const QString& text);
  void update_buttons();

  EntryBridge& bridge_;
  processing::StoreSource& source_;
  std::string url_;
  std::function<QStringList()> selection_;
  QTableView* releases_;
  QStandardItemModel* model_;
  QLabel* status_;
  QProgressBar* progress_;
  QPushButton* refresh_button_;
  QPushButton* new_button_;
  AskRelease ask_release_;
  int jobs_ = 0;
};

}  // namespace pychron::ui
