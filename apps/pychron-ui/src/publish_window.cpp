#include "publish_window.hpp"

#include <set>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "entry_bridge.hpp"
#include "release_dialog.hpp"

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/embargo.hpp"
#include "pychron/publish/release.hpp"
#include "pychron/publish/workflow.hpp"
#include "pychron/reduction/arar_types.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

namespace ps = persistence;
namespace pb = publish;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// A release as the table shows it, with the count of analyses it pins.
struct ReleaseLine {
  ps::ReleaseRow row;
  std::size_t members = 0;
};

struct Listing {
  std::vector<ReleaseLine> releases;
};

// The stages build_release reports, in order (build.hpp).
const QStringList& build_stages() {
  static const QStringList stages = {QStringLiteral("select"), QStringLiteral("load"),    QStringLiteral("reduce"),
                                     QStringLiteral("tables"), QStringLiteral("files"),   QStringLiteral("figures"),
                                     QStringLiteral("parquet"), QStringLiteral("records"), QStringLiteral("manifest")};
  return stages;
}

std::optional<PublishWindow::BuildRequest> ask_build_dialog(QWidget* parent, const QString& release) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("Build %1").arg(release));
  auto* form = new QFormLayout(&dialog);
  auto* dir = new QLineEdit(QDir::homePath(), &dialog);
  auto* browse = new QPushButton(QObject::tr("Choose…"), &dialog);
  QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, dir] {
    const QString chosen = QFileDialog::getExistingDirectory(&dialog, QObject::tr("Build into"), dir->text());
    if (!chosen.isEmpty()) dir->setText(chosen);
  });
  auto* row = new QHBoxLayout;
  row->addWidget(dir, 1);
  row->addWidget(browse);
  form->addRow(QObject::tr("Directory"), row);
  auto* figures = new QCheckBox(QObject::tr("With figures (ideogram, spectra, isochron as SVG and PDF)"), &dialog);
  figures->setChecked(true);
  auto* private_build = new QCheckBox(QObject::tr("Private build (keeps embargoed rows; never hosted as public)"), &dialog);
  form->addRow(figures);
  form->addRow(private_build);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  form->addRow(buttons);
  if (dialog.exec() != QDialog::Accepted || dir->text().trimmed().isEmpty()) return std::nullopt;
  return PublishWindow::BuildRequest{dir->text().trimmed().toStdString(), figures->isChecked(), private_build->isChecked()};
}

// A build's stages, queued to the window through the bridge (which lives on
// the GUI thread); dropped once the window is gone.
class StageReporter : public pb::IProgress {
 public:
  StageReporter(QObject* bridge, std::function<void(const QString&)> on_stage)
      : bridge_(bridge), on_stage_(std::move(on_stage)) {}
  void stage(std::string_view name) override {
    QMetaObject::invokeMethod(bridge_, [f = on_stage_, n = QString::fromUtf8(name.data(), static_cast<int>(name.size()))] { f(n); },
                              Qt::QueuedConnection);
  }

 private:
  QObject* bridge_;
  std::function<void(const QString&)> on_stage_;
};

}  // namespace

PublishWindow::PublishWindow(EntryBridge& bridge, processing::StoreSource& source, std::string store_url,
                             std::function<QStringList()> selection, QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      source_(source),
      url_(std::move(store_url)),
      selection_(std::move(selection)),
      releases_(new QTableView(this)),
      model_(new QStandardItemModel(0, kColumns, this)),
      status_(new QLabel(this)),
      progress_(new QProgressBar(this)),
      refresh_button_(new QPushButton(tr("Refresh"), this)),
      new_button_(new QPushButton(tr("New release…"), this)),
      build_button_(new QPushButton(tr("Build…"), this)),
      verify_button_(new QPushButton(tr("Verify…"), this)),
      publish_button_(new QPushButton(tr("Publish…"), this)),
      withdraw_button_(new QPushButton(tr("Withdraw…"), this)),
      public_button_(new QPushButton(tr("Make public"), this)) {
  setWindowTitle(tr("Publish"));
  setObjectName(QStringLiteral("publish_window"));
  model_->setHorizontalHeaderLabels(
      {tr("Name"), tr("Version"), tr("State"), tr("Visibility"), tr("Members"), tr("DOI"), tr("Created")});
  releases_->setModel(model_);
  releases_->setSelectionBehavior(QAbstractItemView::SelectRows);
  releases_->setSelectionMode(QAbstractItemView::SingleSelection);
  releases_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  releases_->verticalHeader()->hide();
  releases_->horizontalHeader()->setStretchLastSection(true);
  connect(releases_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { update_buttons(); });

  auto* buttons = new QHBoxLayout;
  buttons->addWidget(new_button_);
  buttons->addWidget(build_button_);
  buttons->addWidget(verify_button_);
  buttons->addWidget(publish_button_);
  buttons->addWidget(withdraw_button_);
  buttons->addWidget(public_button_);
  buttons->addStretch();
  buttons->addWidget(refresh_button_);
  connect(new_button_, &QPushButton::clicked, this, &PublishWindow::new_release);
  connect(build_button_, &QPushButton::clicked, this, &PublishWindow::build);
  ask_build_ = [this](const QString& release) { return ask_build_dialog(this, release); };
  connect(verify_button_, &QPushButton::clicked, this, &PublishWindow::verify);
  connect(publish_button_, &QPushButton::clicked, this, &PublishWindow::publish);
  connect(withdraw_button_, &QPushButton::clicked, this, &PublishWindow::withdraw);
  connect(public_button_, &QPushButton::clicked, this, &PublishWindow::make_public);
  ask_directory_ = [this](const QString& suggested) {
    return QFileDialog::getExistingDirectory(this, tr("A built release directory"), suggested);
  };
  ask_publish_ = [this](const QString& release) -> std::optional<bool> {
    QMessageBox box(QMessageBox::Question, tr("Publish"),
                    tr("Publish %1? A published release no longer changes. It stays private unless made public.")
                        .arg(release),
                    QMessageBox::Cancel, this);
    QPushButton* as_private = box.addButton(tr("Publish as private"), QMessageBox::AcceptRole);
    QPushButton* as_public = box.addButton(tr("Publish as public"), QMessageBox::AcceptRole);
    box.setDefaultButton(as_private);
    box.exec();
    if (box.clickedButton() == as_private) return false;
    if (box.clickedButton() == as_public) return true;
    return std::nullopt;
  };
  ask_text_ = [this](const QString& question) -> std::optional<QString> {
    bool ok = false;
    QString text = QInputDialog::getText(this, tr("Withdraw"), question, QLineEdit::Normal, QString(), &ok);
    if (!ok) return std::nullopt;
    return text;
  };
  confirm_ = [this](const QString& question) {
    return QMessageBox::question(this, tr("Publish"), question) == QMessageBox::Yes;
  };
  report_ = [this](const QString& text) { QMessageBox::warning(this, tr("Verify"), text); };
  ask_release_ = [this](const QStringList& analyses, std::size_t embargoed) {
    QStringList names;
    for (int r = 0; r < model_->rowCount(); ++r) names << model_->item(r, Name)->text();
    names.removeDuplicates();
    return ask_release(this, static_cast<std::size_t>(analyses.size()), embargoed, names);
  };
  connect(refresh_button_, &QPushButton::clicked, this, &PublishWindow::refresh);

  progress_->setVisible(false);
  progress_->setTextVisible(true);
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->addLayout(buttons);
  layout->addWidget(releases_, 1);
  layout->addWidget(progress_);
  layout->addWidget(status_);
  setCentralWidget(central);
  resize(900, 500);
  update_buttons();
}

PublishWindow::~PublishWindow() = default;

void PublishWindow::say(const QString& text) { status_->setText(text); }

QString PublishWindow::selected_release() const {
  const QModelIndex current = releases_->currentIndex();
  if (!current.isValid()) return {};
  return model_->item(current.row(), Name)->data(Qt::UserRole).toString();
}

void PublishWindow::select_release(const QString& uuid) {
  for (int r = 0; r < model_->rowCount(); ++r)
    if (model_->item(r, Name)->data(Qt::UserRole).toString() == uuid) {
      releases_->setCurrentIndex(model_->index(r, Name));
      releases_->selectRow(r);
      return;
    }
}

void PublishWindow::update_buttons() {
  refresh_button_->setEnabled(true);
  const bool any = releases_->currentIndex().isValid();
  const QString state = current_text(State), visibility = current_text(Visibility);
  build_button_->setEnabled(any);  // any state: a published release is rebuilt to verify it
  verify_button_->setEnabled(any);
  publish_button_->setEnabled(state == QStringLiteral("draft"));
  withdraw_button_->setEnabled(state == QStringLiteral("published"));
  public_button_->setEnabled(state == QStringLiteral("published") && visibility == QStringLiteral("private"));
}

QString PublishWindow::current_text(int column) const {
  const QModelIndex current = releases_->currentIndex();
  if (!current.isValid()) return {};
  return model_->item(current.row(), column)->text();
}

void PublishWindow::change(const QString& done,
                           std::function<Result<void>(ps::IStore&, const ps::Actor&)> fn) {
  ++jobs_;
  bridge_.run<bool>(
      this,
      [fn = std::move(fn)](ps::IStore& store, const ps::Actor& actor) -> Result<bool> {
        if (auto r = fn(store, actor); !r) return fail(r.error());
        return true;
      },
      [this, done](Result<bool> r) {
        --jobs_;
        if (!r) {
          say(qs(r.error().what));
          return;
        }
        say(done);
        refresh();
      });
}

void PublishWindow::verify() {
  const QString uuid = selected_release();
  if (uuid.isEmpty()) {
    say(tr("Select a release first."));
    return;
  }
  const QString dir = ask_directory_ ? ask_directory_(last_build(uuid)) : QString();
  if (dir.isEmpty()) return;
  say(tr("Verifying %1 against the store…").arg(dir));
  ++jobs_;
  auto& source = source_;
  const std::string url = url_;
  bridge_.run<pb::StoreVerify>(
      this,
      [&source, url, dir](ps::IStore& store, const ps::Actor&) {
        return pb::verify_against_store(store, source, dir.toStdString(), url);
      },
      [this, dir](Result<pb::StoreVerify> checked) {
        --jobs_;
        if (!checked) {
          say(tr("Verify could not run: %1").arg(qs(checked.error().what)));
          return;
        }
        if (checked->ok()) {
          say(tr("verified %1: the files and a rebuild from the store agree").arg(dir));
          return;
        }
        QStringList lines;
        const auto add = [&lines](const pb::VerifyReport& r, const QString& against) {
          for (const auto& p : r.missing) lines << tr("%1: missing %2").arg(against, qs(p));
          for (const auto& p : r.changed) lines << tr("%1: changed %2").arg(against, qs(p));
          for (const auto& p : r.extra) lines << tr("%1: not in the manifest %2").arg(against, qs(p));
          if (r.reduction_version_differs) lines << tr("%1: %2").arg(against, qs(*r.reduction_version_differs));
        };
        add(checked->files, tr("files"));
        add(checked->rebuild, tr("rebuild"));
        say(tr("verify found %n difference(s) in %1", nullptr, static_cast<int>(lines.size())).arg(dir));
        if (report_) report_(lines.join(QLatin1Char('\n')));
      });
}

void PublishWindow::publish() {
  const QString uuid = selected_release();
  if (uuid.isEmpty()) {
    say(tr("Select a release first."));
    return;
  }
  const QString name = current_text(Name);
  const QString dir = ask_directory_ ? ask_directory_(last_build(uuid)) : QString();
  if (dir.isEmpty()) return;
  const auto make_public = ask_publish_ ? ask_publish_(name) : std::nullopt;
  if (!make_public) return;
  const bool pub = *make_public;
  change(tr("published %1 (%2)").arg(name, pub ? tr("public") : tr("private")),
         [uuid, dir, pub](ps::IStore& store, const ps::Actor& actor) -> Result<void> {
           auto row = pb::publish_built(store, actor, uuid.toStdString(), dir.toStdString(), pub);
           if (!row) return fail(row.error());
           return {};
         });
}

void PublishWindow::withdraw() {
  const QString uuid = selected_release();
  if (uuid.isEmpty()) {
    say(tr("Select a release first."));
    return;
  }
  const QString name = current_text(Name);
  const auto reason = ask_text_ ? ask_text_(tr("Why is %1 withdrawn? The reason is kept with it.").arg(name)) : std::nullopt;
  if (!reason) return;
  if (reason->trimmed().isEmpty()) {
    say(tr("A withdrawn release needs a reason."));
    return;
  }
  change(tr("withdrew %1").arg(name), [uuid, why = reason->trimmed().toStdString()](ps::IStore& store,
                                                                                    const ps::Actor& actor) {
    auto u = ps::Uuid::parse(uuid.toStdString());
    if (!u) return Result<void>(fail(ErrorKind::Config, "no release " + uuid.toStdString()));
    return pb::withdraw_release(store, actor, *u, why);
  });
}

void PublishWindow::make_public() {
  const QString uuid = selected_release();
  if (uuid.isEmpty()) {
    say(tr("Select a release first."));
    return;
  }
  const QString name = current_text(Name);
  if (!confirm_ || !confirm_(tr("Make %1 public? Anyone it is hosted for can then read it.").arg(name))) return;
  change(tr("%1 is public").arg(name), [uuid](ps::IStore& store, const ps::Actor& actor) {
    auto u = ps::Uuid::parse(uuid.toStdString());
    if (!u) return Result<void>(fail(ErrorKind::Config, "no release " + uuid.toStdString()));
    return pb::set_release_public(store, actor, *u);
  });
}

void PublishWindow::stage(const QString& name) {
  const qsizetype at = build_stages().indexOf(name);
  progress_->setVisible(true);
  progress_->setRange(0, static_cast<int>(build_stages().size()));
  if (at >= 0) progress_->setValue(static_cast<int>(at) + 1);
  progress_->setFormat(tr("%1 (%v of %m)").arg(name));
}

void PublishWindow::build() {
  const QString uuid = selected_release();
  if (uuid.isEmpty()) {
    say(tr("Select a release first."));
    return;
  }
  const QString name = model_->item(releases_->currentIndex().row(), Name)->text();
  auto request = ask_build_ ? ask_build_(name) : std::nullopt;
  if (!request) return;
  pb::BuildOptions options;
  options.out = request->out;
  options.with_figures = request->figures;
  options.private_build = request->private_build;
  options.store_url = url_;
  progress_->setVisible(true);
  progress_->setRange(0, static_cast<int>(build_stages().size()));
  progress_->setValue(0);
  say(tr("Building %1…").arg(name));
  ++jobs_;
  QPointer<PublishWindow> guard(this);
  auto reporter = std::make_shared<StageReporter>(&bridge_, [guard](const QString& s) {
    if (guard) guard->stage(s);
  });
  auto& source = source_;
  bridge_.run<pb::BuildResult>(
      this,
      [&source, options, uuid, reporter](ps::IStore& store, const ps::Actor&) -> Result<pb::BuildResult> {
        auto release = ps::Uuid::parse(uuid.toStdString());
        if (!release) return fail(ErrorKind::Config, "no release " + uuid.toStdString());
        return pb::build_release(store, source, *release, options, reporter.get());
      },
      [this, uuid](Result<pb::BuildResult> built) {
        --jobs_;
        if (!built) {
          progress_->setVisible(false);
          say(tr("The release was not built: %1").arg(qs(built.error().what)));
          return;
        }
        progress_->setValue(progress_->maximum());
        const QString dir = QString::fromStdString(built->dir.string());
        last_build_[uuid] = dir;
        QString text = tr("built %1: %2 analyses, %3 samples, %4 files")
                           .arg(dir)
                           .arg(built->manifest.pins.analyses.size())
                           .arg(built->manifest.pins.samples.size())
                           .arg(built->manifest.files.size());
        if (!built->parquet_written) text += tr(" (no Parquet tables: this program was built without Apache Arrow)");
        say(text);
        QStringList warnings;
        for (const auto& w : built->warnings) warnings << qs(w);
        status_->setToolTip(warnings.join(QLatin1Char('\n')));
      });
}

void PublishWindow::new_release() {
  const QStringList analyses = selection_ ? selection_() : QStringList();
  if (analyses.isEmpty()) {
    say(tr("Select analyses in the Data browser first: a release is made of them."));
    return;
  }
  std::vector<ps::Uuid> uuids;
  for (const QString& a : analyses)
    if (auto u = ps::Uuid::parse(a.toStdString())) uuids.push_back(*u);
  ++jobs_;
  bridge_.run<std::size_t>(
      this,
      [uuids](ps::IStore& store, const ps::Actor&) -> Result<std::size_t> {
        auto embargoed = store.embargoed_analyses(uuids, ps::UtcTime::now());
        if (!embargoed) return fail(embargoed.error());
        return embargoed->size();
      },
      [this, analyses](Result<std::size_t> embargoed) {
        --jobs_;
        if (!embargoed) {
          say(tr("The embargoes could not be read: %1").arg(qs(embargoed.error().what)));
          return;
        }
        auto spec = ask_release_ ? ask_release_(analyses, *embargoed) : std::nullopt;
        if (!spec) return;
        spec->analyses.clear();
        for (const QString& a : analyses) spec->analyses.push_back(a.toStdString());
        if (spec->options_json.empty()) spec->options_json = pb::release_options_json(pb::ReleaseOptions{});
        spec->reduction_version = std::string(reduction::kReductionVersion);
#ifdef PYCHRON_VERSION
        spec->software_version = std::string("pychron-cpp/") + PYCHRON_VERSION;
#else
        spec->software_version = "pychron-cpp";
#endif
        const std::string name = spec->name;
        ++jobs_;
        bridge_.run<pb::ReleaseCreated>(
            this,
            [spec = *spec](ps::IStore& store, const ps::Actor& actor) { return pb::create_release(store, actor, spec); },
            [this, name](Result<pb::ReleaseCreated> created) {
              --jobs_;
              if (!created) {
                say(tr("The release was not made: %1").arg(qs(created.error().what)));
                return;
              }
              const QString uuid = qs(created->release.str());
              say(tr("release %1 v%2: %3 members, %4 samples, %5 under embargo")
                      .arg(qs(name))
                      .arg(created->version)
                      .arg(created->members)
                      .arg(created->samples)
                      .arg(created->embargoed_analyses));
              connect(this, &PublishWindow::refreshed, this, [this, uuid] { select_release(uuid); },
                      Qt::SingleShotConnection);
              refresh();
            });
      });
}

void PublishWindow::refresh() {
  ++jobs_;
  const QString keep = selected_release();
  bridge_.run<Listing>(
      this,
      [](ps::IStore& store, const ps::Actor&) -> Result<Listing> {
        auto rows = store.releases({});
        if (!rows) return fail(rows.error());
        Listing out;
        for (auto& row : *rows) {
          auto heads = store.bookmark_heads(row.bookmark);
          if (!heads) return fail(heads.error());
          std::set<ps::Uuid> members;
          for (const auto& h : *heads)
            if (h.kind != ps::Kind::RefValue && h.kind != ps::Kind::InterpretedAge) members.insert(h.subject);
          out.releases.push_back({std::move(row), members.size()});
        }
        return out;
      },
      [this, keep](Result<Listing> listing) {
        --jobs_;
        if (!listing) {
          say(tr("The releases could not be read: %1").arg(qs(listing.error().what)));
          return;
        }
        model_->removeRows(0, model_->rowCount());
        for (const auto& line : listing->releases) {
          const ps::ReleaseRow& r = line.row;
          QList<QStandardItem*> items;
          items << new QStandardItem(qs(r.name)) << new QStandardItem(QString::number(r.version))
                << new QStandardItem(qs(std::string(ps::to_string(r.state))))
                << new QStandardItem(qs(std::string(ps::to_string(r.visibility))))
                << new QStandardItem(QString::number(line.members)) << new QStandardItem(qs(r.doi.value_or("")))
                << new QStandardItem(qs(r.created.iso()).left(10));
          items[Name]->setData(qs(r.uuid.str()), Qt::UserRole);
          items[Name]->setToolTip(qs(r.title));
          model_->appendRow(items);
        }
        if (!keep.isEmpty()) select_release(keep);
        update_buttons();
        Q_EMIT refreshed();
      });
}

}  // namespace pychron::ui
