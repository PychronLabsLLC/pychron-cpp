#include "publish_window.hpp"

#include <set>
#include <utility>
#include <vector>

#include <QDateTime>
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
      new_button_(new QPushButton(tr("New release…"), this)) {
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
  buttons->addStretch();
  buttons->addWidget(refresh_button_);
  connect(new_button_, &QPushButton::clicked, this, &PublishWindow::new_release);
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

void PublishWindow::update_buttons() { refresh_button_->setEnabled(true); }

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
