#include "recall_window.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "scene_view.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

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

}  // namespace

RecallWindow::RecallWindow(pp::IAnalysisSource& source, QWidget* parent) : QWidget(parent, Qt::Window), source_(source) {
  resize(900, 650);
  auto* layout = new QVBoxLayout(this);
  title_ = new QLabel;
  QFont f = title_->font();
  f.setPointSizeF(f.pointSizeF() * 1.3);
  f.setBold(true);
  title_->setFont(f);
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
  evolutions_ = new SceneView;
  el->addWidget(evolutions_, 1);
  connect(kind_, &QTabBar::currentChanged, this, [this] { fill_evolutions(); });
  tabs_->addTab(evo_page, tr("Evolutions"));

  budget_ = make_table({tr("Component"), tr("% of age variance"), QString()});
  tabs_->addTab(budget_, tr("Error budget"));
  identity_ = make_table(value_headers);
  tabs_->addTab(identity_, tr("Identity"));
  extraction_ = make_table(value_headers);
  tabs_->addTab(extraction_, tr("Extraction"));
  spectrometer_ = make_table(value_headers);
  tabs_->addTab(spectrometer_, tr("Spectrometer"));
}

bool RecallWindow::show_analysis(const QString& uuid) {
  uuid_ = uuid;
  auto a = source_.load(uuid.toStdString());
  if (!a) {
    title_->setText(tr("Cannot load %1: %2").arg(uuid, qs(a.error().what)));
    setWindowTitle(tr("Recall"));
    return false;
  }
  analysis_ = *a;
  auto raw = source_.load_raw(uuid.toStdString());
  raw_ = raw ? std::move(*raw) : pp::RawData{};
  const auto reduced = pp::reduce_analysis(analysis_, {});
  model_ = pp::make_recall_model(*reduced);

  setWindowTitle(tr("Recall — %1").arg(qs(analysis_->runid)));
  QStringList header;
  for (const auto& h : model_.header)
    if (!h.empty()) header << qs(h);
  title_->setText(qs(model_.title) + QStringLiteral("\n") + header.join(QStringLiteral("  ·  ")) +
                  (model_.reduction_note.empty() ? QString() : QStringLiteral("\n") + qs(model_.reduction_note)));
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
  return true;
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
  if (!analysis_) return;
  const pp::SeriesKind kinds[] = {pp::SeriesKind::Signal, pp::SeriesKind::Baseline, pp::SeriesKind::Sniff};
  const int i = std::clamp(kind_->currentIndex(), 0, 2);
  evolutions_->set_scene(std::make_shared<const pp::Scene>(pp::make_evolution_scene(*analysis_, raw_, kinds[i])));
}

}  // namespace pychron::ui
