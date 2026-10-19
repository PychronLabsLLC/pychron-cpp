#pragma once

// The Publish window (data publishing spec, section 6.3): the store's
// releases, and what elctl publish does to them, without a terminal. Every
// store call runs on the bridge's worker (a store connection belongs to the
// thread that opened it); results come back here, and never to a window that
// was closed meanwhile. Dialogs are reached through hooks tests replace.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <QMainWindow>
#include <QDate>
#include <QMap>
#include <QString>
#include <QStringList>

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/persistence/publish.hpp"
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

  // Build…: where, and how, the current release is built; nullopt cancels.
  // Default: a dialog with a directory and the With figures and Private
  // build options.
  struct BuildRequest {
    std::filesystem::path out;
    bool figures = false;
    bool private_build = false;
  };
  using AskBuild = std::function<std::optional<BuildRequest>(const QString& release)>;
  void set_ask_build(AskBuild ask) { ask_build_ = std::move(ask); }
  void build();
  // The directory the release was last built into from this window; empty when none.
  QString last_build(const QString& release_uuid) const { return last_build_.value(release_uuid); }

  // Verify… and Publish… ask for a built directory (suggested: the last
  // build from this window); empty cancels. Default: a directory chooser.
  using AskDirectory = std::function<QString(const QString& suggested)>;
  void set_ask_directory(AskDirectory ask) { ask_directory_ = std::move(ask); }
  // Publish…: true public, false private, nullopt cancels. Default: a dialog.
  using AskPublish = std::function<std::optional<bool>(const QString& release)>;
  void set_ask_publish(AskPublish ask) { ask_publish_ = std::move(ask); }
  // Withdraw…: the reason (required); nullopt cancels. Default: an input dialog.
  using AskText = std::function<std::optional<QString>(const QString& question)>;
  void set_ask_text(AskText ask) { ask_text_ = std::move(ask); }
  // Make public: yes or no. Default: a question box.
  using Confirm = std::function<bool(const QString& question)>;
  void set_confirm(Confirm confirm) { confirm_ = std::move(confirm); }
  // What Verify found, one difference a line. Default: a message box.
  using Report = std::function<void(const QString& text)>;
  void set_report(Report report) { report_ = std::move(report); }

  void verify();
  void publish();
  void withdraw();
  void make_public();

  // The Embargoes dock: the live embargoes (Scope, Until, Reason, Set), set
  // and lift. A new one asks for its scope (project, sample, analysis), the
  // scope's name, an optional end and a reason; default: a dialog.
  enum EmbargoColumn { EmbargoScope, EmbargoUntil, EmbargoReason, EmbargoSet, kEmbargoColumns };
  struct EmbargoForm {
    persistence::EmbargoScopeType type = persistence::EmbargoScopeType::Sample;
    QString name;
    std::optional<QDate> until;
    QString reason;
  };
  using AskEmbargo = std::function<std::optional<EmbargoForm>()>;
  void set_ask_embargo(AskEmbargo ask) { ask_embargo_ = std::move(ask); }
  void set_embargo();
  void lift_embargo();  // the current row of the dock
  QTableView* embargoes() const noexcept { return embargoes_; }

  QPushButton* publish_button() const noexcept { return publish_button_; }
  QPushButton* withdraw_button() const noexcept { return withdraw_button_; }
  QPushButton* public_button() const noexcept { return public_button_; }

 Q_SIGNALS:
  void refreshed();

 private:
  void say(const QString& text);
  void update_buttons();
  void stage(const QString& name);  // a build's progress, on the GUI thread
  QString current_text(int column) const;  // of the current row; empty when none
  // A state change of the current release on the worker, said and refreshed.
  void change(const QString& done, std::function<Result<void>(persistence::IStore&, const persistence::Actor&)> fn);

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
  QPushButton* build_button_;
  QPushButton* verify_button_;
  QPushButton* publish_button_;
  QPushButton* withdraw_button_;
  QPushButton* public_button_;
  AskRelease ask_release_;
  AskBuild ask_build_;
  AskDirectory ask_directory_;
  AskPublish ask_publish_;
  AskText ask_text_;
  Confirm confirm_;
  Report report_;
  AskEmbargo ask_embargo_;
  QTableView* embargoes_;
  QStandardItemModel* embargo_model_;
  QMap<QString, QString> last_build_;  // release uuid -> directory
  int jobs_ = 0;
};

}  // namespace pychron::ui
