#pragma once

// The Publish window (data publishing spec, section 6.3): the store's
// releases, and what elctl publish does to them, without a terminal. Every
// store call runs on the bridge's worker (a store connection belongs to the
// thread that opened it); results come back here, and never to a window that
// was closed meanwhile. Dialogs are reached through hooks tests replace.

#include <functional>
#include <map>
#include <string>

#include <QMainWindow>
#include <QString>
#include <QStringList>

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
  int jobs_ = 0;
};

}  // namespace pychron::ui
