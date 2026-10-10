#include "data_browser_window.hpp"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QTimeZone>
#include <QToolBar>
#include <QVBoxLayout>

#include "data_icons.hpp"
#include "shortcuts.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

const std::pair<pp::Facet, const char*> kFacets[] = {
    {pp::Facet::AnalysisType, "Analysis type"},
    {pp::Facet::MassSpectrometer, "Spectrometer"},
    {pp::Facet::Sample, "Sample"},
    {pp::Facet::Identifier, "Identifier"},
    {pp::Facet::Irradiation, "Irradiation"},
    {pp::Facet::Level, "Level"},
};

// The date drop-down's entry for a range typed in, among hours counted back.
constexpr double kRangeChoice = -1.0;

QString color_by_label(ColorBy by) {
  switch (by) {
    case ColorBy::AnalysisType:
      return DataBrowserWindow::tr("Analysis type");
    case ColorBy::Tag:
      return DataBrowserWindow::tr("Tag");
    case ColorBy::Spectrometer:
      return DataBrowserWindow::tr("Spectrometer");
    case ColorBy::IrradiationLevel:
      return DataBrowserWindow::tr("Irradiation level");
    case ColorBy::None:
      return DataBrowserWindow::tr("None");
  }
  return {};
}

QDateTimeEdit* utc_edit() {
  auto* edit = new QDateTimeEdit;
  edit->setTimeZone(QTimeZone::utc());
  edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd hh:mm"));
  edit->setCalendarPopup(true);
  edit->setDateTime(QDateTime(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::utc()));
  return edit;
}

}  // namespace

DataBrowserWindow::DataBrowserWindow(pp::IAnalysisSource& source, QWidget* parent)
    : QWidget(parent, Qt::Window),
      source_(source),
      model_(new AnalysisTableModel(this)),
      type_colors_(default_type_colors(theme())) {
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
  dates_->addItem(tr("Range…"), kRangeChoice);
  fl->addWidget(dates_);
  range_ = new QWidget;
  auto* range_form = new QGridLayout(range_);
  range_form->setContentsMargins(0, 0, 0, 0);
  from_on_ = new QCheckBox(tr("From (UTC)"));
  to_on_ = new QCheckBox(tr("To (UTC)"));
  from_ = utc_edit();
  to_ = utc_edit();
  range_form->addWidget(from_on_, 0, 0);
  range_form->addWidget(from_, 0, 1);
  range_form->addWidget(to_on_, 1, 0);
  range_form->addWidget(to_, 1, 1);
  range_form->setColumnStretch(1, 1);
  range_->setVisible(false);
  fl->addWidget(range_);
  exclude_invalid_ = new QCheckBox(tr("Hide invalid"));
  exclude_invalid_->setChecked(true);
  fl->addWidget(exclude_invalid_);
  color_by_ = new QComboBox;
  for (const ColorBy by : kColorByChoices) color_by_->addItem(color_by_label(by), to_text(by));
  auto* color_row = new QHBoxLayout;
  color_row->addWidget(new QLabel(tr("Colour by")));
  color_row->addWidget(color_by_, 1);
  fl->addLayout(color_row);
  for (const auto& [facet, label] : kFacets) {
    auto* box = new FacetBox(tr(label));
    fl->addWidget(box);
    facets_[facet] = box;
    connect(box, &FacetBox::changed, this, [this] { reload(); });
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

  // Six lists are taller than the window.
  auto* filter_scroll = new QScrollArea;
  filter_scroll->setWidget(filters);
  filter_scroll->setWidgetResizable(true);
  filter_scroll->setFrameShape(QFrame::NoFrame);
  filter_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* split = new QSplitter;
  split->addWidget(filter_scroll);
  split->addWidget(right);
  split->setStretchFactor(1, 1);
  split->setSizes({260, 840});
  auto* layout = new QVBoxLayout(this);
  layout->setMenuBar(toolbar_);  // above the margins, the width of the window
  layout->addWidget(split);

  connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
  connect(dates_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { date_preset_changed(); });
  // One query for a date typed in, not one for each key.
  for (QDateTimeEdit* edit : {from_, to_}) connect(edit, &QDateTimeEdit::editingFinished, this, [this] { reload(); });
  for (QCheckBox* on : {from_on_, to_on_}) connect(on, &QCheckBox::toggled, this, [this] { reload(); });
  connect(color_by_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
    set_color_by(color_by_from_text(color_by_->currentData().toString()));
  });
  // The model lays its rows out again: the separators' spans go with them.
  connect(model_, &QAbstractItemModel::modelReset, this, [this] { span_breaks(0); });
  connect(model_, &QAbstractItemModel::rowsInserted, this,
          [this](const QModelIndex&, int first, int) { span_breaks(first); });
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
  const FacetBox* box = facet_box(f);
  return box != nullptr ? box->list() : nullptr;
}

FacetBox* DataBrowserWindow::facet_box(pp::Facet f) const {
  auto it = facets_.find(f);
  return it == facets_.end() ? nullptr : it->second;
}

bool DataBrowserWindow::range_chosen() const { return dates_->currentData().toDouble() == kRangeChoice; }

bool DataBrowserWindow::range_is_backwards() const {
  return range_chosen() && from_on_->isChecked() && to_on_->isChecked() && from_->dateTime() > to_->dateTime();
}

pp::BrowseQuery DataBrowserWindow::query() const {
  pp::BrowseQuery q;
  q.text = search_->text().trimmed().toStdString();
  if (range_chosen()) {
    if (from_on_->isChecked()) q.from = static_cast<double>(from_->dateTime().toSecsSinceEpoch());
    // The field shows minutes; the bound is inclusive: the whole of the minute named.
    if (to_on_->isChecked()) q.to = static_cast<double>(to_->dateTime().toSecsSinceEpoch() + 59);
  } else if (const double hours = dates_->currentData().toDouble(); hours > 0) {
    q.last_hours = hours;
  }
  q.exclude_tags = exclude_invalid_->isChecked() ? std::vector<std::string>{"invalid"} : std::vector<std::string>{};
  q.limit = page_size_;
  for (const auto& [facet, box] : facets_) {
    std::vector<std::string> checked;
    for (const auto& value : box->checked()) checked.push_back(value.toStdString());
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
      case pp::Facet::Irradiation:
        q.irradiations = checked;
        break;
      case pp::Facet::Level:
        q.levels = checked;
        break;
      default:
        break;
    }
  }
  return q;
}

void DataBrowserWindow::date_preset_changed() {
  range_->setVisible(range_chosen());
  if (range_chosen() && !range_seeded_) {
    // The first time, the range is that of the rows shown.
    range_seeded_ = true;
    if (!model_->rows().empty()) {
      const QSignalBlocker block_from(from_on_);
      const QSignalBlocker block_to(to_on_);
      const auto minute = [](double t) { return static_cast<qint64>(std::floor(t / 60.0)) * 60; };
      from_->setDateTime(QDateTime::fromSecsSinceEpoch(minute(model_->rows().back().timestamp), QTimeZone::utc()));
      to_->setDateTime(QDateTime::fromSecsSinceEpoch(minute(model_->rows().front().timestamp), QTimeZone::utc()));
      from_on_->setChecked(true);
      to_on_->setChecked(true);
    }
  }
  reload();
}

void DataBrowserWindow::set_gap_hours(double hours) { model_->set_gap_threshold(hours * 3600.0); }

void DataBrowserWindow::set_type_colors(const TypeColors& colors) {
  type_colors_ = colors;
  model_->set_coloring(model_->color_by(), type_colors_);
}

void DataBrowserWindow::set_color_by(ColorBy by) {
  const int at = color_by_->findData(to_text(by));
  if (at >= 0 && at != color_by_->currentIndex()) {
    color_by_->setCurrentIndex(at);  // comes back here
    return;
  }
  if (by == model_->color_by()) return;
  model_->set_coloring(by, type_colors_);
  emit color_by_changed(by);
}

void DataBrowserWindow::span_breaks(int first) {
  if (first == 0) table_->clearSpans();
  for (const int row : model_->break_rows())
    if (row >= first) table_->setSpan(row, 0, 1, AnalysisTableModel::ColumnCount);
}

void DataBrowserWindow::show_count() {
  status_->setText(total_ ? tr("%1 of %2 analyses").arg(model_->analysis_count()).arg(*total_)
                          : tr("%1 analyses").arg(model_->analysis_count()));
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
  const auto q = query();
  for (const auto& [facet, box] : facets_) {
    auto values = source_.facet(facet, q);
    QStringList all;
    if (values)
      for (const auto& v : *values) all << qs(v);
    box->set_values(all);
  }
}

void DataBrowserWindow::reload() {
  if (range_is_backwards()) {
    status_->setText(tr("From is after To"));
    return;
  }
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
  show_count();
  table_->resizeColumnsToContents();
}

void DataBrowserWindow::load_more() {
  if (!next_ || range_is_backwards()) return;
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
  show_count();
}

QStringList DataBrowserWindow::selected_uuids() const {
  QStringList out;
  QList<int> rows;
  for (const auto& idx : table_->selectionModel()->selectedRows()) rows << idx.row();
  std::sort(rows.begin(), rows.end());
  for (int r : rows)
    if (const auto* analysis = model_->analysis_at(r)) out << qs(analysis->uuid);
  return out;
}

void DataBrowserWindow::select_rows(const QList<int>& rows) {
  table_->clearSelection();
  QList<int> shown;
  for (int r : rows)
    if (const int row = r < 0 ? -1 : model_->display_row_of(static_cast<std::size_t>(r)); row >= 0) shown << row;
  for (int row : shown)
    table_->selectionModel()->select(model_->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (!shown.isEmpty())
    table_->selectionModel()->setCurrentIndex(model_->index(shown.front(), 0), QItemSelectionModel::NoUpdate);
}

QAction* DataBrowserWindow::plot_action(const QString& kind) const {
  for (QAction* a : toolbar_->actions())
    if (!kind.isEmpty() && a->data().toString() == kind) return a;
  return nullptr;
}

void DataBrowserWindow::recall_current() {
  const auto* analysis = model_->analysis_at(table_->currentIndex().row());
  if (analysis != nullptr) emit recall_requested(qs(analysis->uuid));
}

void DataBrowserWindow::recall_step(int delta) {
  const int count = model_->analysis_count();
  if (count == 0) return;
  // Among the analyses: a separator is never the current row.
  const auto* current = model_->analysis_at(table_->currentIndex().row());
  const int cur = current != nullptr ? static_cast<int>(current - model_->rows().data()) : -1;
  select_rows({std::clamp(cur + delta, 0, count - 1)});
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
    for (const auto& r : model_->rows()) ids << qs(r.uuid);
  if (ids.isEmpty()) {
    status_->setText(tr("Nothing to export"));
    return;
  }
  // Named after the one sample shown, else "analyses".
  std::string sample;
  bool one_sample = true;
  for (const auto& row : model_->rows()) {
    if (!one_sample) break;
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
