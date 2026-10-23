#include "data_browser_window.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <unordered_set>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QTimeZone>
#include <QToolBar>
#include <QVBoxLayout>

#include "data_icons.hpp"
#include "error_report.hpp"
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
// Add all asks first above this many analyses.
constexpr std::size_t kAddAllConfirmAbove = 1000;

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
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  edit->setTimeZone(QTimeZone::utc());
#else
  edit->setTimeSpec(Qt::UTC);
#endif
  edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd hh:mm"));
  edit->setCalendarPopup(true);
  edit->setDateTime(QDateTime(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::utc()));
  return edit;
}

QDoubleSpinBox* age_edit() {
  auto* edit = new QDoubleSpinBox;
  edit->setRange(-10000.0, 10000.0);
  edit->setDecimals(4);
  edit->setSuffix(QStringLiteral(" Ma"));
  edit->setKeyboardTracking(false);
  return edit;
}

}  // namespace

DataBrowserWindow::DataBrowserWindow(pp::IAnalysisSource& source, QWidget* parent)
    : QWidget(parent, Qt::Window),
      source_(source),
      model_(new AnalysisTableModel(this)),
      selected_model_(new AnalysisTableModel(this)),
      type_colors_(default_type_colors(theme())) {
  setWindowTitle(tr("Data — %1").arg(qs(source.name())));
  resize(1400, 650);

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
  auto* age_form = new QGridLayout;
  age_min_on_ = new QCheckBox(tr("Age from"));
  age_max_on_ = new QCheckBox(tr("to"));
  age_min_ = age_edit();
  age_max_ = age_edit();
  age_min_on_->setToolTip(tr("Analyses whose cached age is at least this"));
  age_max_on_->setToolTip(tr("Analyses whose cached age is at most this"));
  age_form->addWidget(age_min_on_, 0, 0);
  age_form->addWidget(age_min_, 0, 1);
  age_form->addWidget(age_max_on_, 1, 0);
  age_form->addWidget(age_max_, 1, 1);
  age_form->setColumnStretch(1, 1);
  fl->addLayout(age_form);
  exclude_invalid_ = new QCheckBox(tr("Hide invalid"));
  exclude_invalid_->setChecked(true);
  fl->addWidget(exclude_invalid_);
  color_by_ = new QComboBox;
  for (const ColorBy by : kColorByChoices) color_by_->addItem(color_by_label(by), to_text(by));
  auto* color_row = new QHBoxLayout;
  color_row->addWidget(new QLabel(tr("Color by")));
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
  set_up_table(table_, model_);
  selected_table_ = new QTableView;
  set_up_table(selected_table_, selected_model_);

  // Filtered: what the query matches.
  auto* right = new QWidget;
  auto* rl = new QVBoxLayout(right);
  rl->setContentsMargins(0, 0, 0, 0);
  unavailable_ = new QLabel;
  unavailable_->setWordWrap(true);
  unavailable_->setTextFormat(Qt::PlainText);
  unavailable_->setTextInteractionFlags(Qt::TextSelectableByMouse);  // it may hold a command to copy
  style::make_banner(unavailable_);
  unavailable_->hide();
  rl->addWidget(unavailable_);
  rl->addWidget(table_);
  auto* bar = new QHBoxLayout;
  status_ = new QLabel;
  more_ = new QPushButton(tr("Load more"));
  bar->addWidget(status_, 1);
  bar->addWidget(more_);
  rl->addLayout(bar);

  // Between the two lists, what moves analyses from one to the other.
  auto* strip = new QWidget;
  auto* sl = new QVBoxLayout(strip);
  sl->setContentsMargins(0, 0, 0, 0);
  add_ = new QPushButton(tr("Add →"));
  add_->setToolTip(tr("Add the highlighted analyses to the selected ones (Enter)"));
  add_all_ = new QPushButton(tr("Add all"));
  add_all_->setToolTip(tr("Add every analysis the filters match, not only those loaded"));
  remove_ = new QPushButton(tr("← Remove"));
  remove_->setToolTip(tr("Take the highlighted analyses out of the selected ones (Delete)"));
  clear_ = new QPushButton(tr("Clear"));
  clear_->setToolTip(tr("Take every analysis out of the selected ones"));
  for (QPushButton* b : {add_, add_all_, remove_, clear_}) sl->addWidget(b);
  sl->addStretch();

  // Selected: analyses kept across queries.
  auto* chosen = new QWidget;
  auto* cl = new QVBoxLayout(chosen);
  cl->setContentsMargins(0, 0, 0, 0);
  selected_count_ = new QLabel;
  cl->addWidget(selected_count_);
  cl->addWidget(selected_table_);

  auto* lists = new QSplitter(Qt::Horizontal);
  lists->addWidget(right);
  lists->addWidget(strip);
  lists->addWidget(chosen);
  lists->setStretchFactor(0, 1);
  lists->setStretchFactor(2, 1);
  lists->setCollapsible(1, false);

  // What can be done with Selected.
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
  export_->setToolTip(tr("Export: write the selected analyses (right list) as a 40Ar/39Ar data report after "
                         "Schaen et al. (2021), CSV or JSON"));
  publish_ = toolbar_->addAction(data_icon(QStringLiteral("publish")), tr("Publish"), this, [this] {
    if (const QStringList ids = selection_uuids(); !ids.isEmpty()) emit publish_requested(ids);
  });
  set_actions_enabled(false);
  ask_export_path = [this](const QString& suggested) {
    return QFileDialog::getSaveFileName(this, tr("Export data report"), suggested,
                                        tr("CSV report (*.csv);;JSON report (*.json)"));
  };

  confirm_add_all = [this](std::size_t count) {
    return QMessageBox::question(this, tr("Add all"),
                                 tr("Add all %n analyses to Selected?", nullptr, static_cast<int>(count))) ==
           QMessageBox::Yes;
  };

  // Six lists are taller than the window.
  auto* filter_scroll = new QScrollArea;
  filter_scroll->setWidget(filters);
  filter_scroll->setWidgetResizable(true);
  filter_scroll->setFrameShape(QFrame::NoFrame);
  filter_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* split = new QSplitter;
  split->addWidget(filter_scroll);
  split->addWidget(lists);
  split->setStretchFactor(1, 1);
  split->setSizes({260, 1140});
  auto* layout = new QVBoxLayout(this);
  layout->setMenuBar(toolbar_);  // above the margins, the width of the window
  layout->addWidget(split);

  connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
  connect(dates_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { date_preset_changed(); });
  // One query for a date typed in, not one for each key.
  for (QDateTimeEdit* edit : {from_, to_}) connect(edit, &QDateTimeEdit::editingFinished, this, [this] { reload(); });
  for (QCheckBox* on : {from_on_, to_on_}) connect(on, &QCheckBox::toggled, this, [this] { reload(); });
  for (QDoubleSpinBox* edit : {age_min_, age_max_})
    connect(edit, &QDoubleSpinBox::editingFinished, this, [this] { reload(); });
  for (QCheckBox* on : {age_min_on_, age_max_on_}) connect(on, &QCheckBox::toggled, this, [this] { reload(); });
  connect(color_by_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
    set_color_by(color_by_from_text(color_by_->currentData().toString()));
  });
  connect(exclude_invalid_, &QCheckBox::toggled, this, [this] { reload(); });
  connect(refresh_button, &QPushButton::clicked, this, &DataBrowserWindow::refresh);
  connect(more_, &QPushButton::clicked, this, &DataBrowserWindow::load_more);
  connect(add_, &QPushButton::clicked, this, &DataBrowserWindow::add_highlighted);
  connect(add_all_, &QPushButton::clicked, this, &DataBrowserWindow::add_all);
  connect(remove_, &QPushButton::clicked, this, &DataBrowserWindow::remove_highlighted);
  connect(clear_, &QPushButton::clicked, this, &DataBrowserWindow::clear_selected);
  // Not activated: that comes on Enter too, which adds.
  for (QTableView* view : {table_, selected_table_}) {
    connect(view, &QTableView::doubleClicked, this, [this, view](const QModelIndex&) { recall_in(view); });
    view->installEventFilter(this);
  }
  auto* next = new QShortcut(key(Shortcut::RecallNext), this);
  auto* prev = new QShortcut(key(Shortcut::RecallPrevious), this);
  connect(next, &QShortcut::activated, this, [this] { recall_step(1); });
  connect(prev, &QShortcut::activated, this, [this] { recall_step(-1); });

  sync_selected();
  refresh();
}

void DataBrowserWindow::set_up_table(QTableView* view, AnalysisTableModel* model) {
  view->setModel(model);
  view->setSelectionBehavior(QAbstractItemView::SelectRows);
  view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view->verticalHeader()->setVisible(false);
  view->horizontalHeader()->setStretchLastSection(true);
  view->setColumnHidden(AnalysisTableModel::Project, true);
  view->setColumnHidden(AnalysisTableModel::Uuid, true);
  view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(view->horizontalHeader(), &QWidget::customContextMenuRequested, this, [this, view, model](const QPoint& pos) {
    QMenu menu(this);
    for (int c = 0; c < AnalysisTableModel::ColumnCount; ++c) {
      QAction* a = menu.addAction(model->headerData(c, Qt::Horizontal).toString());
      a->setCheckable(true);
      a->setChecked(!view->isColumnHidden(c));
      connect(a, &QAction::toggled, this, [view, c](bool on) { view->setColumnHidden(c, !on); });
    }
    menu.exec(view->horizontalHeader()->mapToGlobal(pos));
  });
  // The model lays its rows out again: the separators' spans go with them.
  connect(model, &QAbstractItemModel::modelReset, this, [this, view] { span_breaks(view, 0); });
  connect(model, &QAbstractItemModel::rowsInserted, this,
          [this, view](const QModelIndex&, int first, int) { span_breaks(view, first); });
}

AnalysisTableModel* DataBrowserWindow::model_of(const QTableView* view) const {
  return view == selected_table_ ? selected_model_ : model_;
}

QTableView* DataBrowserWindow::focused_table() const { return selected_table_->hasFocus() ? selected_table_ : table_; }

bool DataBrowserWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() != QEvent::KeyPress || (watched != table_ && watched != selected_table_))
    return QWidget::eventFilter(watched, event);
  const auto* key = static_cast<QKeyEvent*>(event);
  // Enter on the keypad comes with the keypad modifier; any other modifier is someone else's key.
  if ((key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier) return QWidget::eventFilter(watched, event);
  auto* view = static_cast<QTableView*>(watched);
  switch (key->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (view != table_) break;
      add_highlighted();
      return true;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      if (view != selected_table_) break;
      remove_highlighted();
      return true;
    case Qt::Key_Space:
      recall_in(view);
      return true;
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
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

bool DataBrowserWindow::age_bound_set() const { return age_min_on_->isChecked() || age_max_on_->isChecked(); }

bool DataBrowserWindow::age_is_backwards() const {
  return age_min_on_->isChecked() && age_max_on_->isChecked() && age_min_->value() > age_max_->value();
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
  if (age_min_on_->isChecked()) q.age_min = age_min_->value();
  if (age_max_on_->isChecked()) q.age_max = age_max_->value();
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

void DataBrowserWindow::set_gap_hours(double hours) {
  for (AnalysisTableModel* m : {model_, selected_model_}) m->set_gap_threshold(hours * 3600.0);
}

void DataBrowserWindow::set_type_colors(const TypeColors& colors) {
  type_colors_ = colors;
  for (AnalysisTableModel* m : {model_, selected_model_}) m->set_coloring(m->color_by(), type_colors_);
}

void DataBrowserWindow::set_color_by(ColorBy by) {
  const int at = color_by_->findData(to_text(by));
  if (at >= 0 && at != color_by_->currentIndex()) {
    color_by_->setCurrentIndex(at);  // comes back here
    return;
  }
  if (by == model_->color_by()) return;
  for (AnalysisTableModel* m : {model_, selected_model_}) m->set_coloring(by, type_colors_);
  emit color_by_changed(by);
}

void DataBrowserWindow::span_breaks(QTableView* view, int first) {
  if (first == 0) view->clearSpans();
  for (const int row : model_of(view)->break_rows())
    if (row >= first) view->setSpan(row, 0, 1, AnalysisTableModel::ColumnCount);
}

void DataBrowserWindow::show_count() {
  if (age_bound_set()) {
    status_->setText(total_ ? tr("%1 of %2 analyses with a cached age in range").arg(model_->analysis_count()).arg(*total_)
                            : tr("%1 analyses with a cached age in range").arg(model_->analysis_count()));
    return;
  }
  status_->setText(total_ ? tr("%1 of %2 analyses").arg(model_->analysis_count()).arg(*total_)
                          : tr("%1 analyses").arg(model_->analysis_count()));
}

void DataBrowserWindow::set_page_size(int rows) {
  rows = std::max(1, rows);
  if (rows == page_size_) return;
  page_size_ = rows;
  reload();
}

DataBrowserWindow::Place DataBrowserWindow::place() const {
  const auto* current = model_->analysis_at(table_->currentIndex().row());
  return {highlighted_uuids(), current != nullptr ? current->uuid : std::string{}, table_->verticalScrollBar()->value(),
          model_->analysis_count()};
}

bool DataBrowserWindow::select_again(const Place& was) {
  // A load resets the table: what was selected is found again by uuid.
  const QSet<QString> wanted(was.selected.begin(), was.selected.end());
  QList<int> rows;
  int current_row = -1;
  const auto& listed = model_->rows();
  for (std::size_t i = 0; i < listed.size(); ++i) {
    if (wanted.contains(qs(listed[i].uuid))) rows << static_cast<int>(i);
    if (!was.current.empty() && listed[i].uuid == was.current) current_row = model_->display_row_of(i);
  }
  select_rows(rows);
  if (current_row >= 0)
    table_->selectionModel()->setCurrentIndex(model_->index(current_row, 0), QItemSelectionModel::NoUpdate);
  return rows.size() == was.selected.size();
}

void DataBrowserWindow::refresh() {
  const Place was = place();
  if (auto ok = source_.refresh(); !ok) {
    log_shown("data", tr("Rescan"), ok.error());
    status_->setText(tr("Rescan failed: %1").arg(qs(ok.error().what)));
  }
  reload();
  if (select_again(was)) table_->verticalScrollBar()->setValue(was.scroll);
  refetch_selected();  // last: its status is the newer news
}

void DataBrowserWindow::refresh_if_changed() {
  if (!isVisible()) {
    stale_ = true;  // showEvent
    return;
  }
  refresh_changed();
}

void DataBrowserWindow::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (stale_) refresh_changed();
}

void DataBrowserWindow::refresh_changed() {
  stale_ = false;
  if (!source_.refresh()) return;
  if (source_.generation() != loaded_generation_ && !query_is_backwards()) reload_changed();
  // Selected does not depend on the filters: it follows the store whatever
  // became of Filtered (a backwards range, a failed browse).
  if (source_.generation() != selected_generation_) refetch_selected();
}

void DataBrowserWindow::reload_changed() {
  const Place was = place();
  update_facets();
  // Everything that was loaded, in one query: "Load more" goes on from its end.
  auto q = query();
  q.limit = std::max(page_size_, was.analyses);
  auto page = source_.browse(q);
  if (!page) {
    log_shown("data", tr("Browse"), page.error());
    status_->setText(tr("Error: %1").arg(qs(page.error().what)));
    return;  // what was shown stays
  }
  // What is new at the top pushed as many rows off the end, and one of them
  // may be the row being read: they are fetched after it, so the table only grows.
  std::unordered_set<std::string> had;
  for (const auto& row : model_->rows()) had.insert(row.uuid);
  const auto fresh = std::count_if(page->rows.begin(), page->rows.end(),
                                   [&had](const pp::AnalysisSummary& row) { return !had.contains(row.uuid); });
  if (fresh > 0 && !had.empty() && page->next) {
    q.after = page->next;
    q.limit = static_cast<int>(fresh);
    if (auto pushed = source_.browse(q)) {
      std::move(pushed->rows.begin(), pushed->rows.end(), std::back_inserter(page->rows));
      page->next = pushed->next;
    }
  }
  loaded_generation_ = source_.generation();
  next_ = page->next;
  total_ = page->total;
  model_->set_rows(std::move(page->rows));
  more_->setEnabled(next_.has_value());
  show_count();
  // The column widths are the user's by now: not resized, as reload() does.
  select_again(was);
  // With nothing selected there is no row to stay with: the place is kept
  // unless rows went away from under it.
  if (!was.selected.isEmpty() || model_->analysis_count() >= was.analyses) {
    table_->verticalScrollBar()->setValue(was.scroll);
  }
}

void DataBrowserWindow::refetch_selected() {
  const auto generation = source_.generation();
  if (selection_.empty()) {
    selected_generation_ = generation;
    return;
  }
  // Selected is what the user kept: looked up by uuid, whatever the filters say.
  pp::BrowseQuery q;
  q.uuids = selection_.uuids();
  q.exclude_tags.clear();  // the default hides invalid runs; Selected keeps what it holds
  q.limit = static_cast<int>(q.uuids.size());
  auto page = source_.browse(q);
  if (!page) {
    log_shown("data", tr("Browse"), page.error());
    status_->setText(tr("Error: %1").arg(qs(page.error().what)));
    return;  // what was held stays
  }
  // sync_selected() resets the table: the highlight is found again by uuid.
  QSet<QString> highlighted;
  for (const auto& idx : selected_table_->selectionModel()->selectedRows())
    if (const auto* a = selected_model_->analysis_at(idx.row())) highlighted.insert(qs(a->uuid));
  const auto* current = selected_model_->analysis_at(selected_table_->currentIndex().row());
  const std::string current_uuid = current != nullptr ? current->uuid : std::string{};
  const int scroll = selected_table_->verticalScrollBar()->value();
  selected_generation_ = generation;
  const std::size_t dropped = selection_.replace_from(std::move(page->rows));
  sync_selected();
  QList<int> rows;
  int current_row = -1;
  const auto& listed = selected_model_->rows();
  for (std::size_t i = 0; i < listed.size(); ++i) {
    if (highlighted.contains(qs(listed[i].uuid))) rows << static_cast<int>(i);
    if (!current_uuid.empty() && listed[i].uuid == current_uuid) current_row = static_cast<int>(i);
  }
  select_in(selected_table_, rows);
  if (current_row >= 0)
    selected_table_->selectionModel()->setCurrentIndex(
        selected_model_->index(selected_model_->display_row_of(static_cast<std::size_t>(current_row)), 0),
        QItemSelectionModel::NoUpdate);
  selected_table_->verticalScrollBar()->setValue(scroll);
  if (dropped > 0) status_->setText(tr("%n selected analyses no longer in store", nullptr, static_cast<int>(dropped)));
}

void DataBrowserWindow::set_publish_available(bool available) {
  publish_available_ = available;
  update_publish();
}

void DataBrowserWindow::update_publish() {
  publish_->setToolTip(publish_available_ ? tr("Publish…") : tr("Publishing needs a database"));
  publish_->setEnabled(publish_available_ && !selection_.empty());
}

void DataBrowserWindow::set_unavailable(const QString& reason) {
  unavailable_->setText(reason);
  unavailable_->setVisible(!reason.isEmpty());
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
  if (age_is_backwards()) {
    status_->setText(tr("Age from is above to"));
    return;
  }
  update_facets();
  auto page = source_.browse(query());
  if (!page) {
    model_->set_rows({});
    log_shown("data", tr("Browse"), page.error());
    status_->setText(tr("Error: %1").arg(qs(page.error().what)));
    more_->setEnabled(false);
    return;
  }
  loaded_generation_ = source_.generation();
  next_ = page->next;
  total_ = page->total;
  model_->set_rows(std::move(page->rows));
  more_->setEnabled(next_.has_value());
  show_count();
  table_->resizeColumnsToContents();
}

void DataBrowserWindow::load_more() {
  if (!next_ || query_is_backwards()) return;
  auto q = query();
  q.after = next_;
  auto page = source_.browse(q);
  if (!page) {
    log_shown("data", tr("Browse"), page.error());
    status_->setText(tr("Error: %1").arg(qs(page.error().what)));
    return;
  }
  next_ = page->next;
  model_->append_rows(std::move(page->rows));
  more_->setEnabled(next_.has_value());
  show_count();
}

QStringList DataBrowserWindow::highlighted_uuids() const {
  QStringList out;
  QList<int> rows;
  for (const auto& idx : table_->selectionModel()->selectedRows()) rows << idx.row();
  std::sort(rows.begin(), rows.end());
  for (int r : rows)
    if (const auto* analysis = model_->analysis_at(r)) out << qs(analysis->uuid);
  return out;
}

QStringList DataBrowserWindow::selection_uuids() const {
  QStringList out;
  for (const auto& uuid : selection_.uuids()) out << qs(uuid);
  return out;
}

void DataBrowserWindow::add_highlighted() {
  std::vector<pp::AnalysisSummary> rows;
  for (const auto& idx : table_->selectionModel()->selectedRows())
    if (const auto* analysis = model_->analysis_at(idx.row())) rows.push_back(*analysis);
  add_to_selected(std::move(rows));
  // Filtered may be older than the store (a backwards range, a failed browse):
  // what was copied is no newer than that load, so the next refresh looks again.
  selected_generation_ = std::min(selected_generation_, loaded_generation_);
}

void DataBrowserWindow::add_to_selected(std::vector<pp::AnalysisSummary> rows) {
  const std::size_t asked = rows.size();
  const std::size_t added = selection_.add(std::move(rows));
  sync_selected();
  QString text = tr("Added %n", nullptr, static_cast<int>(added));
  if (const std::size_t held = asked - added; held > 0) text += tr(" (%1 already selected)").arg(static_cast<qulonglong>(held));
  status_->setText(text);
}

void DataBrowserWindow::add_all() {
  // The rows come fresh from the source, so Selected's generation stays.
  if (range_is_backwards()) {
    status_->setText(tr("From is after To"));
    return;
  }
  if (age_is_backwards()) {
    status_->setText(tr("Age from is above to"));
    return;
  }
  // An unknown total (nullopt) is not asked about.
  if (total_ && *total_ > kAddAllConfirmAbove && confirm_add_all && !confirm_add_all(*total_)) return;
  pp::BrowseQuery q = query();
  q.limit = page_size_;
  std::vector<pp::AnalysisSummary> rows;
  std::optional<QString> failure;
  for (;;) {
    auto page = source_.browse(q);
    if (!page) {
      failure = qs(page.error().what);
      break;
    }
    rows.insert(rows.end(), page->rows.begin(), page->rows.end());
    if (!page->next) break;
    q.after = page->next;
  }
  add_to_selected(std::move(rows));
  if (failure) {
    log_shown("data", tr("Add all"), *failure);
    status_->setText(tr("Error: %1").arg(*failure));
  }
}

void DataBrowserWindow::remove_highlighted() {
  std::vector<std::string> uuids;
  for (const auto& idx : selected_table_->selectionModel()->selectedRows())
    if (const auto* analysis = selected_model_->analysis_at(idx.row())) uuids.push_back(analysis->uuid);
  if (selection_.remove(uuids) > 0) sync_selected();
}

void DataBrowserWindow::clear_selected() {
  selection_.clear();
  sync_selected();
}

void DataBrowserWindow::set_actions_enabled(bool on) {
  for (QAction* a : toolbar_->actions())
    if (!a->data().toString().isEmpty()) a->setEnabled(on);
  export_->setEnabled(on);
  update_publish();
}

void DataBrowserWindow::sync_selected() {
  const bool was_empty = selected_model_->analysis_count() == 0;
  selected_model_->set_rows(selection_.rows());
  if (was_empty && !selection_.empty()) selected_table_->resizeColumnsToContents();
  std::unordered_set<std::string> marked;
  for (const auto& row : selection_.rows()) marked.insert(row.uuid);
  model_->set_marked(std::move(marked));
  selected_count_->setText(tr("%n selected", nullptr, static_cast<int>(selection_.size())));
  set_actions_enabled(!selection_.empty());
}

void DataBrowserWindow::select_in(QTableView* view, const QList<int>& rows) {
  AnalysisTableModel* m = model_of(view);
  view->clearSelection();
  QList<int> shown;
  for (int r : rows)
    if (const int row = r < 0 ? -1 : m->display_row_of(static_cast<std::size_t>(r)); row >= 0) shown << row;
  for (int row : shown)
    view->selectionModel()->select(m->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (!shown.isEmpty())
    view->selectionModel()->setCurrentIndex(m->index(shown.front(), 0), QItemSelectionModel::NoUpdate);
}

void DataBrowserWindow::select_rows(const QList<int>& rows) { select_in(table_, rows); }

void DataBrowserWindow::select_selected_rows(const QList<int>& rows) { select_in(selected_table_, rows); }

QAction* DataBrowserWindow::plot_action(const QString& kind) const {
  for (QAction* a : toolbar_->actions())
    if (!kind.isEmpty() && a->data().toString() == kind) return a;
  return nullptr;
}

void DataBrowserWindow::recall_current() { recall_in(focused_table()); }

void DataBrowserWindow::recall_in(const QTableView* view) {
  const auto* analysis = model_of(view)->analysis_at(view->currentIndex().row());
  if (analysis != nullptr) emit recall_requested(qs(analysis->uuid));
}

void DataBrowserWindow::recall_step(int delta) { recall_step_in(focused_table(), delta); }

void DataBrowserWindow::recall_step_in(QTableView* view, int delta) {
  const AnalysisTableModel* m = model_of(view);
  const int count = m->analysis_count();
  if (count == 0) return;
  // Among the analyses: a separator is never the current row.
  const auto* current = m->analysis_at(view->currentIndex().row());
  const int cur = current != nullptr ? static_cast<int>(current - m->rows().data()) : -1;
  select_in(view, {std::clamp(cur + delta, 0, count - 1)});
  recall_in(view);
}

void DataBrowserWindow::request_figure(const QString& kind) {
  if (const QStringList ids = selection_uuids(); !ids.isEmpty()) emit figure_requested(kind, ids);
}

void DataBrowserWindow::show_message(const QString& text) { status_->setText(text); }

void DataBrowserWindow::request_export() {
  const QStringList ids = selection_uuids();
  if (ids.isEmpty()) {
    status_->setText(tr("Nothing to export"));
    return;
  }
  // Named after the one sample selected, else "analyses".
  std::string sample;
  bool one_sample = true;
  for (const auto& row : selection_.rows()) {
    if (!one_sample) break;
    if (sample.empty()) sample = row.sample;
    one_sample = row.sample == sample;
  }
  const QString stem = one_sample && !sample.empty() ? qs(sample) : QStringLiteral("analyses");
  const QString path = ask_export_path ? ask_export_path(stem + QStringLiteral("-report.csv")) : QString();
  if (path.isEmpty()) return;
  emit export_requested(path, ids);
}

}  // namespace pychron::ui
