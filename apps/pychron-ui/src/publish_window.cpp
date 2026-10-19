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

// Qt's `signals` keyword macro would rewrite persistence::CollectionRoots::signals.
#pragma push_macro("signals")
#undef signals
#include "pychron/processing/store_source.hpp"
#include "pychron/publish/embargo.hpp"
#include "pychron/publish/release.hpp"
#pragma pop_macro("signals")

namespace pychron::ui {

namespace ps = persistence;

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
      refresh_button_(new QPushButton(tr("Refresh"), this)) {
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
  buttons->addWidget(refresh_button_);
  buttons->addStretch();
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
