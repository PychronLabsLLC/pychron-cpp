#include "data_browser_window.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "data_icons.hpp"
#include "shortcuts.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

const std::pair<pp::Facet, const char*> kFacets[] = {
    {pp::Facet::AnalysisType, "Analysis type"},
    {pp::Facet::MassSpectrometer, "Spectrometer"},
    {pp::Facet::Sample, "Sample"},
    {pp::Facet::Identifier, "Identifier"},
};

}  // namespace

DataBrowserWindow::DataBrowserWindow(pp::IAnalysisSource& source, QWidget* parent)
    : QWidget(parent, Qt::Window), source_(source), model_(new AnalysisTableModel(this)) {
  setWindowTitle(tr("Data — %1").arg(qs(source.name())));
  resize(1100, 650);

  auto* filters = new QWidget;
  auto* fl = new QVBoxLayout(filters);
  fl->setContentsMargins(0, 0, 0, 0);
  search_ = new QLineEdit;
  search_->setPlaceholderText(tr("Run id, identifier or sample"));
  search_->setClearButtonEnabled(true);
  fl->addWidget(search_);
  dates_ = new QComboBox;
  dates_->addItem(tr("Any date"), 0.0);
  dates_->addItem(tr("Last 24 hours"), 24.0);
  dates_->addItem(tr("Last week"), 24.0 * 7);
  dates_->addItem(tr("Last month"), 24.0 * 31);
  dates_->addItem(tr("Last year"), 24.0 * 366);
  fl->addWidget(dates_);
  exclude_invalid_ = new QCheckBox(tr("Hide invalid"));
  exclude_invalid_->setChecked(true);
  fl->addWidget(exclude_invalid_);
  for (const auto& [facet, label] : kFacets) {
    auto* box = new QGroupBox(tr(label));
    auto* bl = new QVBoxLayout(box);
    auto* list = new QListWidget;
    list->setMaximumHeight(130);
    bl->addWidget(list);
    fl->addWidget(box);
    facets_[facet] = list;
    connect(list, &QListWidget::itemChanged, this, [this] {
      if (!updating_) reload();
    });
  }
  auto* refresh_button = new QPushButton(tr("Rescan"));
  fl->addWidget(refresh_button);
  fl->addStretch();

  table_ = new QTableView;
  table_->setModel(model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->verticalHeader()->setVisible(false);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setColumnHidden(AnalysisTableModel::Project, true);
  table_->setColumnHidden(AnalysisTableModel::Irradiation, true);
  table_->setColumnHidden(AnalysisTableModel::Uuid, true);
  table_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(table_->horizontalHeader(), &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    QMenu menu(this);
    for (int c = 0; c < AnalysisTableModel::ColumnCount; ++c) {
      QAction* a = menu.addAction(model_->headerData(c, Qt::Horizontal).toString());
      a->setCheckable(true);
      a->setChecked(!table_->isColumnHidden(c));
      connect(a, &QAction::toggled, this, [this, c](bool on) { table_->setColumnHidden(c, !on); });
    }
    menu.exec(table_->horizontalHeader()->mapToGlobal(pos));
  });

  auto* right = new QWidget;
  auto* rl = new QVBoxLayout(right);
  rl->setContentsMargins(0, 0, 0, 0);
  rl->addWidget(table_);
  auto* bar = new QHBoxLayout;
  status_ = new QLabel;
  more_ = new QPushButton(tr("Load more"));
  bar->addWidget(status_, 1);
  bar->addWidget(more_);
  rl->addLayout(bar);

  // What can be done with the selection (or with all that is shown).
  toolbar_ = new QToolBar(tr("Data"));
  toolbar_->setObjectName(QStringLiteral("data_toolbar"));
  toolbar_->setIconSize(QSize(18, 18));
  toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  auto plot = [this](const char* kind, const QString& label, const QString& tip) {
    const QString k = QString::fromLatin1(kind);
    QAction* a = toolbar_->addAction(data_icon(k), label, this, [this, k] { request_figure(k); });
    a->setData(k);
    a->setToolTip(tip);
  };
  plot("time_series", tr("Time series"), tr("Time series of the selected analyses"));
  plot("ideogram", tr("Ideogram"), tr("Ideogram of the selected analyses"));
  plot("spectrum", tr("Age spectrum"), tr("Age spectrum of the selected analyses"));
  plot("inverse_isochron", tr("Inverse isochron"), tr("Inverse isochron of the selected analyses"));
  plot("spectrum_isochron", tr("Spectrum + isochron"),
       tr("Age spectrum and inverse isochron of the selected analyses, side by side"));
  toolbar_->addSeparator();
  plot("isotope_evolution_fit", tr("Isotope evolutions..."), tr("Refit the selected analyses' isotope evolutions"));
  plot("blank_fit", tr("Blanks..."), tr("Blanks: fit the selected unknowns' values from reference analyses"));
  plot("icfactor_fit", tr("IC factors..."), tr("IC factors: fit the selected unknowns' values from reference analyses"));
  toolbar_->addSeparator();
  recall_ = toolbar_->addAction(data_icon(QStringLiteral("recall")), tr("Recall"), this, &DataBrowserWindow::recall_current);
  recall_->setToolTip(tr("Recall: open the current analysis"));
  export_ = toolbar_->addAction(data_icon(QStringLiteral("export")), tr("Export"), this, &DataBrowserWindow::request_export);
  export_->setToolTip(tr("Export: write the selected analyses (or all shown) as a 40Ar/39Ar data report after "
                         "Schaen et al. (2021), CSV or JSON"));
  ask_export_path = [this](const QString& suggested) {
    return QFileDialog::getSaveFileName(this, tr("Export data report"), suggested,
                                        tr("CSV report (*.csv);;JSON report (*.json)"));
  };

  auto* split = new QSplitter;
  split->addWidget(filters);
  split->addWidget(right);
  split->setStretchFactor(1, 1);
  split->setSizes({240, 860});
  auto* layout = new QVBoxLayout(this);
  layout->setMenuBar(toolbar_);  // above the margins, the width of the window
  layout->addWidget(split);

  connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
  connect(dates_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { reload(); });
  connect(exclude_invalid_, &QCheckBox::toggled, this, [this] { reload(); });
  connect(refresh_button, &QPushButton::clicked, this, &DataBrowserWindow::refresh);
  connect(more_, &QPushButton::clicked, this, &DataBrowserWindow::load_more);
  connect(table_, &QTableView::activated, this, [this](const QModelIndex&) { recall_current(); });
  auto* next = new QShortcut(key(Shortcut::RecallNext), this);
  auto* prev = new QShortcut(key(Shortcut::RecallPrevious), this);
  connect(next, &QShortcut::activated, this, [this] { recall_step(1); });
  connect(prev, &QShortcut::activated, this, [this] { recall_step(-1); });

  refresh();
}

QListWidget* DataBrowserWindow::facet_list(pp::Facet f) const {
  auto it = facets_.find(f);
  return it == facets_.end() ? nullptr : it->second;
}

pp::BrowseQuery DataBrowserWindow::query() const {
  pp::BrowseQuery q;
  q.text = search_->text().trimmed().toStdString();
  const double hours = dates_->currentData().toDouble();
  if (hours > 0) q.last_hours = hours;
  q.exclude_tags = exclude_invalid_->isChecked() ? std::vector<std::string>{"invalid"} : std::vector<std::string>{};
  q.limit = page_size_;
  for (const auto& [facet, list] : facets_) {
    std::vector<std::string> checked;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked) checked.push_back(list->item(i)->text().toStdString());
    switch (facet) {
      case pp::Facet::AnalysisType:
        q.analysis_types = checked;
        break;
      case pp::Facet::MassSpectrometer:
        q.mass_spectrometers = checked;
        break;
      case pp::Facet::Sample:
        q.samples = checked;
        break;
      case pp::Facet::Identifier:
        q.identifiers = checked;
        break;
      default:
        break;
    }
  }
  return q;
}

void DataBrowserWindow::set_page_size(int rows) {
  rows = std::max(1, rows);
  if (rows == page_size_) return;
  page_size_ = rows;
  reload();
}

void DataBrowserWindow::refresh() {
  if (auto ok = source_.refresh(); !ok) status_->setText(tr("Rescan failed: %1").arg(qs(ok.error().what)));
  reload();
}

void DataBrowserWindow::update_facets() {
  updating_ = true;
  const auto q = query();
  for (const auto& [facet, list] : facets_) {
    QStringList checked;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked) checked << list->item(i)->text();
    auto values = source_.facet(facet, q);
    list->clear();
    if (!values) continue;
    QStringList all;
    for (const auto& v : *values) all << qs(v);
    for (const auto& c : checked)
      if (!all.contains(c)) all << c;  // keep a checked value visible even when nothing matches
    for (const auto& v : all) {
      auto* item = new QListWidgetItem(v, list);
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      item->setCheckState(checked.contains(v) ? Qt::Checked : Qt::Unchecked);
    }
  }
  updating_ = false;
}

void DataBrowserWindow::reload() {
  if (updating_) return;
  update_facets();
  auto page = source_.browse(query());
  if (!page) {
    model_->set_rows({});
    status_->setText(tr("Error: %1").arg(qs(page.error().what)));
    more_->setEnabled(false);
    return;
  }
  next_ = page->next;
  total_ = page->total;
  model_->set_rows(std::move(page->rows));
  more_->setEnabled(next_.has_value());
  status_->setText(total_ ? tr("%1 of %2 analyses").arg(model_->rowCount()).arg(*total_)
                          : tr("%1 analyses").arg(model_->rowCount()));
  table_->resizeColumnsToContents();
}

void DataBrowserWindow::load_more() {
  if (!next_) return;
  auto q = query();
  q.after = next_;
  auto page = source_.browse(q);
  if (!page) {
    status_->setText(tr("Error: %1").arg(qs(page.error().what)));
    return;
  }
  next_ = page->next;
  model_->append_rows(std::move(page->rows));
  more_->setEnabled(next_.has_value());
  status_->setText(total_ ? tr("%1 of %2 analyses").arg(model_->rowCount()).arg(*total_)
                          : tr("%1 analyses").arg(model_->rowCount()));
}

QStringList DataBrowserWindow::selected_uuids() const {
  QStringList out;
  QList<int> rows;
  for (const auto& idx : table_->selectionModel()->selectedRows()) rows << idx.row();
  std::sort(rows.begin(), rows.end());
  for (int r : rows) out << qs(model_->row(r).uuid);
  return out;
}

void DataBrowserWindow::select_rows(const QList<int>& rows) {
  table_->clearSelection();
  for (int r : rows)
    table_->selectionModel()->select(model_->index(r, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (!rows.isEmpty())
    table_->selectionModel()->setCurrentIndex(model_->index(rows.front(), 0), QItemSelectionModel::NoUpdate);
}

QAction* DataBrowserWindow::plot_action(const QString& kind) const {
  for (QAction* a : toolbar_->actions())
    if (!kind.isEmpty() && a->data().toString() == kind) return a;
  return nullptr;
}

void DataBrowserWindow::recall_current() {
  const QModelIndex idx = table_->currentIndex();
  if (!idx.isValid()) return;
  emit recall_requested(qs(model_->row(idx.row()).uuid));
}

void DataBrowserWindow::recall_step(int delta) {
  if (model_->rowCount() == 0) return;
  const int cur = table_->currentIndex().isValid() ? table_->currentIndex().row() : -1;
  const int next = std::clamp(cur + delta, 0, model_->rowCount() - 1);
  select_rows({next});
  recall_current();
}

void DataBrowserWindow::request_figure(const QString& kind) {
  QStringList ids = selected_uuids();
  if (ids.isEmpty())
    for (const auto& r : model_->rows()) ids << qs(r.uuid);
  if (!ids.isEmpty()) emit figure_requested(kind, ids);
}

void DataBrowserWindow::show_message(const QString& text) { status_->setText(text); }

void DataBrowserWindow::request_export() {
  QStringList ids = selected_uuids();
  if (ids.isEmpty())
    for (int r = 0; r < model_->rowCount(); ++r) ids << qs(model_->row(r).uuid);
  if (ids.isEmpty()) {
    status_->setText(tr("Nothing to export"));
    return;
  }
  // Named after the one sample shown, else "analyses".
  std::string sample;
  bool one_sample = true;
  for (int r = 0; r < model_->rowCount() && one_sample; ++r) {
    const auto& row = model_->row(r);
    if (ids.contains(qs(row.uuid))) {
      if (sample.empty()) sample = row.sample;
      one_sample = row.sample == sample;
    }
  }
  const QString stem = one_sample && !sample.empty() ? qs(sample) : QStringLiteral("analyses");
  const QString path = ask_export_path ? ask_export_path(stem + QStringLiteral("-report.csv")) : QString();
  if (path.isEmpty()) return;
  emit export_requested(path, ids);
}

}  // namespace pychron::ui
