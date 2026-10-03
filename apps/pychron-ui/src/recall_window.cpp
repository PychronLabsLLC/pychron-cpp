#include "recall_window.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>

#include "scene_view.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;
namespace r = pychron::reduction;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString num(double v, int sig = 7) { return QString::number(v, 'g', sig); }

QString percent(const pp::Value& v) {
  if (v.value == 0) return {};
  return QString::number(std::abs(v.error / v.value) * 100.0, 'f', 3);
}

QTableWidget* make_table(const QStringList& headers) {
  auto* t = new QTableWidget;
  t->setColumnCount(static_cast<int>(headers.size()));
  t->setHorizontalHeaderLabels(headers);
  t->verticalHeader()->setVisible(false);
  t->setEditTriggers(QAbstractItemView::NoEditTriggers);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  t->horizontalHeader()->setStretchLastSection(true);
  return t;
}

void set_cell(QTableWidget* t, int row, int col, const QString& text, const QString& tip = {}) {
  auto* item = new QTableWidgetItem(text);
  if (!tip.isEmpty()) item->setToolTip(tip);
  if (col > 0) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  t->setItem(row, col, item);
}

// Name | Value | ±1σ | % | Units
void fill_section(QTableWidget* t, const pp::RecallSection& s) {
  t->setRowCount(static_cast<int>(s.rows.size()));
  for (int i = 0; i < static_cast<int>(s.rows.size()); ++i) {
    const auto& r = s.rows[static_cast<std::size_t>(i)];
    set_cell(t, i, 0, qs(r.name), qs(r.note));
    if (r.value) {
      set_cell(t, i, 1, num(r.value->value));
      set_cell(t, i, 2, r.value->error != 0 ? num(r.value->error, 4) : QString());
      set_cell(t, i, 3, r.value->error != 0 ? percent(*r.value) : QString());
    } else {
      set_cell(t, i, 1, qs(r.text));
      set_cell(t, i, 2, {});
      set_cell(t, i, 3, {});
    }
    set_cell(t, i, 4, qs(r.units));
  }
  t->resizeColumnsToContents();
}

constexpr pp::Stage kStages[] = {pp::Stage::Intercept,       pp::Stage::Baseline,       pp::Stage::BaselineCorrected,
                                 pp::Stage::Blank,           pp::Stage::BlankCorrected, pp::Stage::IcFactor,
                                 pp::Stage::IcCorrected,     pp::Stage::DecayCorrected, pp::Stage::InterferenceCorrected};

const char* stage_title(pp::Stage s) {
  switch (s) {
    case pp::Stage::Intercept:
      return "I";
    case pp::Stage::Baseline:
      return "Bs";
    case pp::Stage::BaselineCorrected:
      return "I-Bs";
    case pp::Stage::Blank:
      return "Bk";
    case pp::Stage::BlankCorrected:
      return "I-Bs-Bk";
    case pp::Stage::IcFactor:
      return "IC";
    case pp::Stage::IcCorrected:
      return "×IC";
    case pp::Stage::DecayCorrected:
      return "Decay";
    case pp::Stage::InterferenceCorrected:
      return "IFC";
  }
  return "";
}

constexpr r::FitKind kFitKinds[] = {r::FitKind::Average, r::FitKind::Linear, r::FitKind::Parabolic, r::FitKind::Cubic,
                                    r::FitKind::Exponential};

}  // namespace

RecallWindow::RecallWindow(pp::IAnalysisSource& source, QWidget* parent) : QWidget(parent, Qt::Window), source_(source) {
  resize(900, 650);
  auto* layout = new QVBoxLayout(this);
  title_ = new QLabel;
  title_->setFont(style::title_font(title_->font()));
  title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(title_);
  tabs_ = new QTabWidget;
  layout->addWidget(tabs_);

  const QStringList value_headers{tr("Name"), tr("Value"), tr("±1σ"), tr("%"), tr("Units")};
  auto* summary = new QWidget;
  auto* sl = new QVBoxLayout(summary);
  computed_ = make_table(value_headers);
  ratios_ = make_table(value_headers);
  sl->addWidget(new QLabel(tr("Computed")));
  sl->addWidget(computed_, 3);
  sl->addWidget(new QLabel(tr("Corrected ratios")));
  sl->addWidget(ratios_, 2);
  tabs_->addTab(summary, tr("Summary"));

  auto* iso_page = new QWidget;
  auto* il = new QVBoxLayout(iso_page);
  stage_ = new QComboBox;
  stage_->addItem(tr("All stages"), -1);
  for (const auto s : kStages) stage_->addItem(QString::fromUtf8(stage_title(s)) + QStringLiteral("  (") + qs(std::string(pp::to_string(s))) + QStringLiteral(")"), static_cast<int>(s));
  il->addWidget(stage_);
  isotopes_ = make_table({});
  il->addWidget(isotopes_);
  connect(stage_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { fill_isotopes(); });
  tabs_->addTab(iso_page, tr("Isotopes"));

  auto* evo_page = new QWidget;
  auto* el = new QVBoxLayout(evo_page);
  kind_ = new QTabBar;
  kind_->addTab(tr("Signal"));
  kind_->addTab(tr("Baseline"));
  kind_->addTab(tr("Sniff"));
  el->addWidget(kind_);
  auto* evo_row = new QHBoxLayout;
  el->addLayout(evo_row, 1);
  evolutions_ = new SceneView;
  evo_row->addWidget(evolutions_, 1);

  fit_box_ = new QGroupBox(tr("Fit"));
  auto* fl = new QFormLayout(fit_box_);
  fit_isotope_ = new QComboBox;
  fit_kind_ = new QComboBox;
  for (const auto k : kFitKinds) fit_kind_->addItem(qs(std::string(r::to_string(k))), static_cast<int>(k));
  fit_error_ = new QComboBox;
  fit_error_->addItem(QStringLiteral("SEM"), static_cast<int>(r::ErrorType::Sem));
  fit_error_->addItem(QStringLiteral("SD"), static_cast<int>(r::ErrorType::Sd));
  fit_outliers_ = new QCheckBox(tr("Filter outliers"));
  fit_iterations_ = new QSpinBox;
  fit_iterations_->setRange(1, 10);
  fit_std_devs_ = new QDoubleSpinBox;
  fit_std_devs_->setRange(0.5, 10.0);
  fit_std_devs_->setSingleStep(0.5);
  fit_std_devs_->setDecimals(1);
  clear_excluded_ = new QPushButton(tr("Include all points"));
  fl->addRow(tr("Isotope"), fit_isotope_);
  fl->addRow(tr("Fit"), fit_kind_);
  fl->addRow(tr("Error"), fit_error_);
  fl->addRow(fit_outliers_);
  fl->addRow(tr("Iterations"), fit_iterations_);
  fl->addRow(tr("Std devs"), fit_std_devs_);
  fl->addRow(clear_excluded_);
  auto* hint = new QLabel(tr("Click a point (or drag a box) to leave it out of the fit or put it back."));
  hint->setWordWrap(true);
  fl->addRow(hint);
  edit_status_ = new QLabel;
  edit_status_->setWordWrap(true);
  edit_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  fl->addRow(edit_status_);
  auto* buttons = new QHBoxLayout;
  revert_ = new QPushButton(tr("Revert"));
  save_ = new QPushButton(tr("Save"));
  buttons->addWidget(revert_);
  buttons->addWidget(save_);
  fl->addRow(buttons);
  fit_box_->setMaximumWidth(260);
  evo_row->addWidget(fit_box_);

  connect(kind_, &QTabBar::currentChanged, this, [this] {
    fill_evolutions();
    fit_box_->setVisible(kind_->currentIndex() == 0);
  });
  connect(fit_isotope_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { fill_fit_editor(); });
  for (auto* combo : {fit_kind_, fit_error_})
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { fit_controls_changed(); });
  connect(fit_outliers_, &QCheckBox::toggled, this, [this] { fit_controls_changed(); });
  connect(fit_iterations_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { fit_controls_changed(); });
  connect(fit_std_devs_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { fit_controls_changed(); });
  connect(clear_excluded_, &QPushButton::clicked, this, [this] {
    const std::string key = fit_isotope_->currentText().toStdString();
    if (key.empty()) return;
    auto e = current_edit(key);
    e.user_excluded.clear();
    edits_[key] = e;
    recompute();
  });
  connect(revert_, &QPushButton::clicked, this, [this] { revert_edits(); });
  connect(save_, &QPushButton::clicked, this, [this] { save_edits(); });
  connect(evolutions_, &SceneView::point_clicked, this, [this](const QString& ref) { toggle_points({ref}); });
  connect(evolutions_, &SceneView::points_toggled, this, [this](const QStringList& refs) { toggle_points(refs); });
  tabs_->addTab(evo_page, tr("Evolutions"));

  budget_ = make_table({tr("Component"), tr("% of age variance"), QString()});
  tabs_->addTab(budget_, tr("Error budget"));
  identity_ = make_table(value_headers);
  tabs_->addTab(identity_, tr("Identity"));
  extraction_ = make_table(value_headers);
  tabs_->addTab(extraction_, tr("Extraction"));
  spectrometer_ = make_table(value_headers);
  tabs_->addTab(spectrometer_, tr("Spectrometer"));

  history_page_ = new QWidget;
  auto* hl = new QVBoxLayout(history_page_);
  auto* hrow = new QHBoxLayout;
  history_kind_ = new QComboBox;
  for (const auto k : pp::kRevisionKinds) history_kind_->addItem(qs(std::string(pp::title(k))), static_cast<int>(k));
  hrow->addWidget(new QLabel(tr("Revisions of")));
  hrow->addWidget(history_kind_);
  hrow->addStretch(1);
  hl->addLayout(hrow);
  revisions_ = make_table({tr("Seq"), tr("Date (UTC)"), tr("Author"), tr("Host"), tr("Change"), tr("Message")});
  revisions_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  hl->addWidget(revisions_, 2);
  history_note_ = new QLabel;
  history_note_->setWordWrap(true);
  hl->addWidget(history_note_);
  revision_content_ = make_table({});
  hl->addWidget(revision_content_, 3);
  tabs_->addTab(history_page_, tr("History"));
  connect(history_kind_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
    history_stale_ = true;
    fill_history();
  });
  connect(revisions_, &QTableWidget::itemSelectionChanged, this, [this] { show_revisions(); });
  connect(tabs_, &QTabWidget::currentChanged, this, [this] {
    if (tabs_->currentWidget() == history_page_) fill_history();
  });
}

bool RecallWindow::show_analysis(const QString& uuid) {
  uuid_ = uuid;
  edits_.clear();
  edited_.clear();
  history_stale_ = true;
  auto a = source_.load(uuid.toStdString());
  if (!a) {
    analysis_.reset();
    shown_.reset();
    title_->setText(tr("Cannot load %1: %2").arg(uuid, qs(a.error().what)));
    setWindowTitle(tr("Recall"));
    update_edit_state();
    return false;
  }
  analysis_ = *a;
  shown_ = analysis_;
  auto raw = source_.load_raw(uuid.toStdString());
  raw_ = raw ? std::move(*raw) : pp::RawData{};

  const QString keep = fit_isotope_->currentText();
  {
    QSignalBlocker block(fit_isotope_);
    fit_isotope_->clear();
    for (const auto& s : raw_.series)
      if (s.kind == pp::SeriesKind::Signal && analysis_->find_isotope(s.key)) fit_isotope_->addItem(qs(s.key));
    const int at = fit_isotope_->findText(keep);
    fit_isotope_->setCurrentIndex(at >= 0 ? at : 0);
  }
  edit_status_->clear();
  display();
  fill_fit_editor();
  if (tabs_->currentWidget() == history_page_) fill_history();
  return true;
}

void RecallWindow::display() {
  const auto reduced = pp::reduce_analysis(shown_, {});
  model_ = pp::make_recall_model(*reduced);

  setWindowTitle(tr("Recall — %1").arg(qs(shown_->runid)) + (edits_.empty() ? QString() : QStringLiteral(" *")));
  QStringList header;
  for (const auto& h : model_.header)
    if (!h.empty()) header << qs(h);
  title_->setText(qs(model_.title) + QStringLiteral("\n") + header.join(QStringLiteral("  ·  ")) +
                  (model_.reduction_note.empty() ? QString() : QStringLiteral("\n") + qs(model_.reduction_note)) +
                  (edits_.empty() ? QString() : QStringLiteral("\n") + tr("Unsaved fit edits")));
  fill_section(computed_, model_.computed);
  fill_section(ratios_, model_.ratios);
  fill_section(identity_, model_.identity);
  fill_section(extraction_, model_.extraction);
  fill_section(spectrometer_, model_.spectrometer);
  fill_isotopes();
  fill_evolutions();

  budget_->setRowCount(static_cast<int>(model_.error_budget.size()));
  for (int i = 0; i < static_cast<int>(model_.error_budget.size()); ++i) {
    const auto& c = model_.error_budget[static_cast<std::size_t>(i)];
    set_cell(budget_, i, 0, qs(c.name));
    set_cell(budget_, i, 1, QString::number(c.percent, 'f', 3));
    auto* bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setValue(static_cast<int>(std::lround(c.percent * 10)));
    bar->setTextVisible(false);
    budget_->setCellWidget(i, 2, bar);
  }
  budget_->resizeColumnsToContents();
  update_edit_state();
}

void RecallWindow::fill_isotopes() {
  const int stage = stage_->currentData().toInt();
  QStringList headers{tr("Isotope"), tr("Det."), tr("Fit"), tr("N")};
  std::vector<pp::Stage> shown;
  if (stage < 0) {
    shown.assign(std::begin(kStages), std::end(kStages));
    for (const auto s : shown) headers << QString::fromUtf8(stage_title(s)) << tr("±1σ");
  } else {
    shown.push_back(static_cast<pp::Stage>(stage));
    headers << tr("Value") << tr("±1σ") << tr("%");
  }
  headers << tr("Bk source");
  isotopes_->clear();
  isotopes_->setColumnCount(static_cast<int>(headers.size()));
  isotopes_->setHorizontalHeaderLabels(headers);
  isotopes_->setRowCount(static_cast<int>(model_.isotopes.size()));
  for (int r = 0; r < static_cast<int>(model_.isotopes.size()); ++r) {
    const auto& iso = model_.isotopes[static_cast<std::size_t>(r)];
    int c = 0;
    set_cell(isotopes_, r, c++, qs(iso.key));
    set_cell(isotopes_, r, c++, qs(iso.detector));
    set_cell(isotopes_, r, c++, qs(iso.fit) + (iso.baseline_fit.empty() ? QString() : QStringLiteral(" / ") + qs(iso.baseline_fit)));
    set_cell(isotopes_, r, c++, QString::number(iso.n));
    for (const auto s : shown) {
      auto it = iso.stages.find(s);
      if (it == iso.stages.end()) {
        set_cell(isotopes_, r, c++, {});
        set_cell(isotopes_, r, c++, {});
        if (stage >= 0) set_cell(isotopes_, r, c++, {});
        continue;
      }
      set_cell(isotopes_, r, c++, num(it->second.value));
      set_cell(isotopes_, r, c++, num(it->second.error, 4));
      if (stage >= 0) set_cell(isotopes_, r, c++, percent(it->second));
    }
    set_cell(isotopes_, r, c++, qs(iso.blank_source));
  }
  isotopes_->resizeColumnsToContents();
}

void RecallWindow::fill_evolutions() {
  if (!shown_) return;
  const pp::SeriesKind kinds[] = {pp::SeriesKind::Signal, pp::SeriesKind::Baseline, pp::SeriesKind::Sniff};
  const int i = std::clamp(kind_->currentIndex(), 0, 2);
  evolutions_->set_scene(std::make_shared<const pp::Scene>(pp::make_evolution_scene(*shown_, raw_, kinds[i])));
}

// ---------------------------------------------------------------- fit editing

pp::FitEdit RecallWindow::current_edit(const std::string& key) const {
  if (auto it = edits_.find(key); it != edits_.end()) return it->second;
  pp::FitEdit e;
  e.key = key;
  if (const pp::IsotopeData* iso = shown_ ? shown_->find_isotope(key) : nullptr) {
    e.fit = iso->fit.value_or(r::FitSpec{});
    e.user_excluded = iso->user_excluded;
  }
  if (e.fit.kind == r::FitKind::CustomPoly) e.fit.kind = r::FitKind::Linear;  // not editable here
  return e;
}

void RecallWindow::fill_fit_editor() {
  const std::string key = fit_isotope_->currentText().toStdString();
  const bool any = !key.empty() && shown_;
  for (QWidget* w : std::initializer_list<QWidget*>{fit_kind_, fit_error_, fit_outliers_, fit_iterations_, fit_std_devs_,
                                                     clear_excluded_})
    w->setEnabled(any);
  if (!any) {
    update_edit_state();
    return;
  }
  const pp::FitEdit e = current_edit(key);
  const QSignalBlocker b1(fit_kind_), b2(fit_error_), b3(fit_outliers_), b4(fit_iterations_), b5(fit_std_devs_);
  fit_kind_->setCurrentIndex(std::max(0, fit_kind_->findData(static_cast<int>(e.fit.kind))));
  fit_error_->setCurrentIndex(std::max(0, fit_error_->findData(static_cast<int>(e.fit.error))));
  fit_outliers_->setChecked(e.fit.outliers.enabled);
  fit_iterations_->setValue(e.fit.outliers.iterations);
  fit_std_devs_->setValue(e.fit.outliers.std_devs);
  fit_iterations_->setEnabled(e.fit.outliers.enabled);
  fit_std_devs_->setEnabled(e.fit.outliers.enabled);
  update_edit_state();
}

void RecallWindow::fit_controls_changed() {
  const std::string key = fit_isotope_->currentText().toStdString();
  if (key.empty() || !shown_) return;
  pp::FitEdit e = current_edit(key);
  e.fit.kind = static_cast<r::FitKind>(fit_kind_->currentData().toInt());
  e.fit.error = static_cast<r::ErrorType>(fit_error_->currentData().toInt());
  e.fit.outliers.enabled = fit_outliers_->isChecked();
  e.fit.outliers.iterations = fit_iterations_->value();
  e.fit.outliers.std_devs = fit_std_devs_->value();
  fit_iterations_->setEnabled(e.fit.outliers.enabled);
  fit_std_devs_->setEnabled(e.fit.outliers.enabled);
  edits_[key] = e;
  recompute();
}

void RecallWindow::toggle_points(const QStringList& refs) {
  if (!shown_ || kind_->currentIndex() != 0) return;
  std::string last;
  for (const auto& ref : refs) {
    const auto parsed = pp::parse_evolution_ref(ref.toStdString());
    if (!parsed || !shown_->find_isotope(parsed->first)) continue;
    pp::FitEdit e = current_edit(parsed->first);
    pp::toggle_index(e.user_excluded, parsed->second);
    edits_[parsed->first] = e;
    last = parsed->first;
  }
  if (last.empty()) return;
  {
    const QSignalBlocker block(fit_isotope_);
    fit_isotope_->setCurrentIndex(std::max(0, fit_isotope_->findText(qs(last))));
  }
  recompute();
  fill_fit_editor();
}

void RecallWindow::recompute() {
  if (!analysis_) return;
  // An edit equal to what was loaded is no edit.
  for (auto it = edits_.begin(); it != edits_.end();) {
    const pp::IsotopeData* iso = analysis_->find_isotope(it->first);
    const r::FitSpec stored = iso && iso->fit ? *iso->fit : r::FitSpec{};
    const bool same = iso && iso->fit && stored.kind == it->second.fit.kind && stored.error == it->second.fit.error &&
                      stored.outliers.enabled == it->second.fit.outliers.enabled &&
                      (!stored.outliers.enabled || (stored.outliers.iterations == it->second.fit.outliers.iterations &&
                                                    stored.outliers.std_devs == it->second.fit.outliers.std_devs)) &&
                      iso->user_excluded == it->second.user_excluded;
    it = same ? edits_.erase(it) : std::next(it);
  }
  if (edits_.empty()) {
    shown_ = analysis_;
    edited_.clear();
    edit_status_->clear();
    display();
    return;
  }
  std::vector<pp::FitEdit> list;
  for (const auto& [_, e] : edits_) list.push_back(e);
  auto result = pp::apply_fit_edits(*analysis_, raw_, list);
  if (!result) {
    edit_status_->setText(tr("Cannot refit: %1").arg(qs(result.error().what)));
    update_edit_state();
    return;
  }
  shown_ = result->analysis;
  edited_ = std::move(result->isotopes);
  edit_status_->clear();
  display();
}

void RecallWindow::update_edit_state() {
  pp::IRevisionSource* revisions = source_.revisions();
  const bool has_head = analysis_ && analysis_->heads.count("intercepts") > 0;
  const bool pending = !edits_.empty() && !edited_.empty();
  revert_->setEnabled(!edits_.empty());
  save_->setEnabled(pending && revisions && has_head);
  if (!revisions) {
    save_->setToolTip(tr("Saving needs a source that keeps revisions (pychron-ui --db)"));
  } else if (!has_head) {
    save_->setToolTip(tr("This analysis has no intercepts revision to build on"));
  } else {
    save_->setToolTip(tr("Save the edited fits as a new intercepts revision"));
  }
  if (pending && edit_status_->text().isEmpty()) {
    QStringList keys;
    for (const auto& e : edited_) keys << qs(e.key);
    edit_status_->setText(tr("Edited: %1 (not saved)").arg(keys.join(QStringLiteral(", "))));
  }
}

void RecallWindow::revert_edits() {
  edits_.clear();
  recompute();
  fill_fit_editor();
}

bool RecallWindow::save_edits() {
  pp::IRevisionSource* revisions = source_.revisions();
  if (!revisions || !analysis_ || edited_.empty()) return false;
  auto head = analysis_->heads.find("intercepts");
  if (head == analysis_->heads.end()) return false;
  const std::string message = pp::describe_fit_edits(*analysis_, edited_);
  auto outcome = revisions->save_intercepts(analysis_->uuid, head->second, edited_, message);
  if (!outcome) {
    edit_status_->setText(tr("Save failed: %1").arg(qs(outcome.error().what)));
    return false;
  }
  if (!outcome->saved) {
    edit_status_->setText(tr("Not saved: %1").arg(qs(outcome->conflict)));
    return false;
  }
  const QString revision = qs(outcome->revision);
  show_analysis(uuid_);
  edit_status_->setText(tr("Saved as revision %1").arg(revision.left(8)));
  return true;
}

void RecallWindow::closeEvent(QCloseEvent* event) {
  if (edits_.empty()) {
    event->accept();
    return;
  }
  const auto answer = QMessageBox::question(
      this, tr("Unsaved fit edits"), tr("Save the edited fits of %1?").arg(qs(analysis_->runid)),
      save_->isEnabled() ? QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel
                         : QMessageBox::Discard | QMessageBox::Cancel);
  if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !save_edits())) {
    event->ignore();
    return;
  }
  edits_.clear();
  event->accept();
}

// ---------------------------------------------------------------- history

void RecallWindow::fill_history() {
  if (!history_stale_) return;
  history_stale_ = false;
  history_.clear();
  revisions_->setRowCount(0);
  revision_content_->clear();
  revision_content_->setRowCount(0);
  revision_content_->setColumnCount(0);
  pp::IRevisionSource* revisions = source_.revisions();
  if (!revisions) {
    history_note_->setText(tr("History needs a source that keeps revisions (pychron-ui --db)."));
    return;
  }
  if (!analysis_) return;
  const auto kind = static_cast<pp::RevisionKind>(history_kind_->currentData().toInt());
  auto list = revisions->history(analysis_->uuid, kind);
  if (!list) {
    history_note_->setText(tr("Cannot read the history: %1").arg(qs(list.error().what)));
    return;
  }
  history_ = std::move(*list);
  const QSignalBlocker block(revisions_);
  revisions_->setRowCount(static_cast<int>(history_.size()));
  for (int i = 0; i < static_cast<int>(history_.size()); ++i) {
    const auto& h = history_[static_cast<std::size_t>(i)];
    set_cell(revisions_, i, 0, QString::number(h.seq) + (h.head ? QStringLiteral(" ●") : QString()),
             h.head ? tr("current (head)") : QString());
    const auto when = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(std::llround(h.created * 1000.0)), QTimeZone::utc());
    set_cell(revisions_, i, 1, when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    set_cell(revisions_, i, 2, qs(h.author));
    set_cell(revisions_, i, 3, qs(h.host));
    set_cell(revisions_, i, 4, qs(h.changeset_kind));
    set_cell(revisions_, i, 5, qs(h.message), qs(h.id));
    for (int c = 1; c < 6; ++c) revisions_->item(i, c)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  }
  revisions_->resizeColumnsToContents();
  history_note_->setText(history_.empty() ? tr("No revisions of this kind.")
                                          : tr("Select a revision to see it, or two to compare them."));
  if (!history_.empty()) revisions_->selectRow(0);
  show_revisions();
}

void RecallWindow::show_revisions() {
  pp::IRevisionSource* revisions = source_.revisions();
  if (!revisions) return;
  std::vector<int> rows;
  for (const auto& range : revisions_->selectedRanges())
    for (int r = range.topRow(); r <= range.bottomRow(); ++r) rows.push_back(r);
  std::sort(rows.begin(), rows.end());
  revision_content_->clear();
  revision_content_->setRowCount(0);
  revision_content_->setColumnCount(0);
  if (rows.empty() || rows.size() > 2) {
    if (rows.size() > 2) history_note_->setText(tr("Select one revision, or two to compare."));
    return;
  }
  auto table_of = [&](int row) { return revisions->revision_table(history_[static_cast<std::size_t>(row)].id); };
  if (rows.size() == 1) {
    auto t = table_of(rows[0]);
    if (!t) {
      history_note_->setText(tr("Cannot read the revision: %1").arg(qs(t.error().what)));
      return;
    }
    QStringList headers{tr("Key")};
    for (const auto& c : t->columns) headers << qs(c);
    revision_content_->setColumnCount(static_cast<int>(headers.size()));
    revision_content_->setHorizontalHeaderLabels(headers);
    revision_content_->setRowCount(static_cast<int>(t->rows.size()));
    for (int i = 0; i < static_cast<int>(t->rows.size()); ++i) {
      const auto& row = t->rows[static_cast<std::size_t>(i)];
      set_cell(revision_content_, i, 0, qs(row.key));
      for (int c = 0; c < static_cast<int>(row.cells.size()); ++c) set_cell(revision_content_, i, c + 1, qs(row.cells[static_cast<std::size_t>(c)]));
    }
    const auto& h = history_[static_cast<std::size_t>(rows[0])];
    history_note_->setText(tr("%1 by %2, %3").arg(qs(h.message.empty() ? h.changeset_kind : h.message), qs(h.author),
                                                    qs(h.id).left(8)));
    revision_content_->resizeColumnsToContents();
    return;
  }
  // The list is newest first: the lower row is the older revision.
  auto before = table_of(rows[1]);
  auto after = table_of(rows[0]);
  if (!before || !after) {
    history_note_->setText(tr("Cannot read the revisions"));
    return;
  }
  const auto diff = pp::diff_revisions(*before, *after);
  QStringList headers{tr("Key")};
  for (const auto& c : diff.columns) headers << qs(c);
  revision_content_->setColumnCount(static_cast<int>(headers.size()));
  revision_content_->setHorizontalHeaderLabels(headers);
  revision_content_->setRowCount(static_cast<int>(diff.rows.size()));
  for (int i = 0; i < static_cast<int>(diff.rows.size()); ++i) {
    const auto& row = diff.rows[static_cast<std::size_t>(i)];
    set_cell(revision_content_, i, 0, qs(row.key), qs(std::string(pp::to_string(row.state))));
    const QColor row_color = row.state == pp::DiffState::Added     ? theme().diff_added
                             : row.state == pp::DiffState::Removed ? theme().diff_removed
                                                                   : QColor();
    if (row_color.isValid()) revision_content_->item(i, 0)->setBackground(row_color);
    for (std::size_t c = 0; c < diff.columns.size(); ++c) {
      QString text;
      if (row.state == pp::DiffState::Removed) {
        text = qs(row.before[c]);
      } else if (row.changed[c] && row.state == pp::DiffState::Changed) {
        text = qs(row.before[c]) + QStringLiteral(" → ") + qs(row.after[c]);
      } else {
        text = qs(row.after[c]);
      }
      set_cell(revision_content_, i, static_cast<int>(c) + 1, text);
      auto* item = revision_content_->item(i, static_cast<int>(c) + 1);
      if (row_color.isValid()) {
        item->setBackground(row_color);
      } else if (row.changed[c]) {
        item->setBackground(theme().diff_changed);
      }
    }
  }
  revision_content_->resizeColumnsToContents();
  history_note_->setText(tr("%n row(s) differ between %1 and %2.", nullptr, diff.changed_rows())
                             .arg(QString::number(history_[static_cast<std::size_t>(rows[1])].seq),
                                  QString::number(history_[static_cast<std::size_t>(rows[0])].seq)));
}

}  // namespace pychron::ui
