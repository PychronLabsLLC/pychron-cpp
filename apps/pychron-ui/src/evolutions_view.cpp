#include "evolutions_view.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include <qcustomplot.h>

#include "strip_chart_model.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

using experiment::collect::SeriesKey;
using experiment::collect::SeriesKind;

constexpr std::size_t kPaletteSize = 8;  // StripChartModel::palette_color cycles 8
constexpr SeriesKind kKinds[] = {SeriesKind::Signal, SeriesKind::Baseline, SeriesKind::Sniff};

QString series_name(const SeriesKey& key) {
  return key.isotope.empty() ? QString::fromStdString(key.detector)
                             : QStringLiteral("%1 %2").arg(QString::fromStdString(key.isotope),
                                                          QString::fromStdString(key.detector));
}

}  // namespace

EvolutionsView::EvolutionsView(std::map<std::string, QColor> colors, QWidget* parent)
    : QWidget(parent),
      colors_(std::move(colors)),
      tabs_(new QTabBar),
      focus_box_(new QComboBox),
      title_(new QLabel),
      plot_(new QCustomPlot),
      peaks_(new QListWidget),
      intercepts_(new QListWidget) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  for (auto k : kKinds) {
    QString name = QString::fromLatin1(experiment::collect::to_string(k).data());
    if (!name.isEmpty()) name[0] = name[0].toUpper();
    tabs_->addTab(name);
  }
  auto* top = new QHBoxLayout;
  top->addWidget(tabs_, 1);
  top->addWidget(focus_box_);
  layout->addLayout(top);
  layout->addWidget(title_);
  layout->addWidget(plot_, 1);
  layout->addWidget(new QLabel(tr("Intercepts")));
  intercepts_->setMaximumHeight(90);
  layout->addWidget(intercepts_);
  auto* peaks_label = new QLabel(tr("Peak centers"));
  layout->addWidget(peaks_label);
  peaks_->setMaximumHeight(80);
  layout->addWidget(peaks_);

  plot_->setMinimumHeight(200);
  plot_->axisRect()->setBackground(QBrush(theme().plot_bg));
  plot_->xAxis->setLabel(tr("Time (s)"));
  plot_->yAxis->setLabel(tr("Intensity"));
  plot_->legend->setVisible(false);  // shown once there are graphs
  plot_->setPlottingHint(QCP::phFastPolylines, true);

  connect(focus_box_, &QComboBox::activated, this, [this](int i) {
    if (i <= 0) {
      set_focus(std::nullopt);
      return;
    }
    const QStringList parts = focus_box_->itemData(i).toStringList();
    set_focus(SeriesKey{parts.value(0).toStdString(), parts.value(1).toStdString(), kind_});
  });
  connect(tabs_, &QTabBar::currentChanged, this, [this](int i) {
    if (i >= 0 && std::cmp_less(i, std::size(kKinds))) set_kind(kKinds[i]);
  });
  refresh_.setInterval(kRefreshMs);
  connect(&refresh_, &QTimer::timeout, this, [this] {
    if (dirty_) refresh();
  });
  refresh_.start();
  title_->setText(tr("No run"));
}

void EvolutionsView::clear() {
  lines_.clear();
  fill_focus();  // the focus stays chosen for the next run's series
  peaks_->clear();
  block_.clear();
  rebuild();
  refresh();
}

void EvolutionsView::on_run_started(const experiment::executor::RunStarted& e) {
  run_ = QString::fromStdString(e.identifier);
  clear();
  title_->setText(run_);
}

void EvolutionsView::on_series(const std::vector<experiment::collect::SeriesUpdated>& batch) {
  bool new_line = false;
  for (const auto& u : batch) {
    for (const auto& [key, value] : u.values) {
      auto it = lines_.find(key);
      if (it == lines_.end()) {
        Line line;
        // Detector colour, unless another series of this kind already has it
        // (peak hopping puts several isotopes on one detector).
        std::set<QRgb> used;
        for (const auto& [k, l] : lines_)
          if (k.kind == key.kind) used.insert(l.color.rgb());
        auto c = colors_.find(key.detector);
        line.color = c != colors_.end() && !used.contains(c->second.rgb()) ? c->second : QColor();
        if (!line.color.isValid()) {
          line.color = StripChartModel::palette_color(used.size());  // every palette colour taken: cycle
          for (std::size_t i = 0; i < kPaletteSize; ++i) {
            if (!used.contains(StripChartModel::palette_color(i).rgb())) {
              line.color = StripChartModel::palette_color(i);
              break;
            }
          }
        }
        it = lines_.emplace(key, std::move(line)).first;
        new_line |= key.kind == kind_;
      }
      it->second.t.push_back(u.t);
      it->second.v.push_back(value);
    }
    block_ = QStringLiteral("%1 %2/%3").arg(QString::fromStdString(u.label)).arg(u.count).arg(u.target);
  }
  title_->setText(block_.isEmpty() ? run_ : QStringLiteral("%1 — %2").arg(run_, block_));
  if (new_line) {
    fill_focus();
    rebuild();
  }
  dirty_ = true;
}

void EvolutionsView::on_peak_center(const jobs::PeakCenterDone& e) {
  const auto& r = e.result;
  QString text = QStringLiteral("%1 on %2: ").arg(QString::fromStdString(r.isotope), QString::fromStdString(r.detector));
  if (r.ok && r.center) {
    text += tr("center %1").arg(*r.center, 0, 'f', 6);
    if (r.table_updated) text += tr(", table updated");
  } else {
    text += tr("failed: %1").arg(QString::fromStdString(r.message));
  }
  peaks_->addItem(text);
}

void EvolutionsView::on_fits(const std::vector<experiment::collect::FitsUpdated>& batch) {
  for (const auto& u : batch)
    for (const auto& f : u.fits) {
      auto it = lines_.find(f.key);
      if (it == lines_.end()) continue;  // a fit arrives after its points
      it->second.fit = f.fit;
      it->second.time_zero = u.time_zero;
    }
  dirty_ = true;
}

void EvolutionsView::set_focus(std::optional<SeriesKey> key) {
  focus_ = std::move(key);
  fill_focus();
  rebuild();
  refresh();
}

void EvolutionsView::fill_focus() {
  const QSignalBlocker block(focus_box_);
  focus_box_->clear();
  focus_box_->addItem(tr("All series"));
  for (const auto& [key, line] : lines_)
    if (key.kind == kind_)
      focus_box_->addItem(series_name(key), QStringList{QString::fromStdString(key.isotope), QString::fromStdString(key.detector)});
  int current = 0;
  if (focus_)
    for (int i = 1; i < focus_box_->count(); ++i) {
      const QStringList parts = focus_box_->itemData(i).toStringList();
      if (parts.value(0).toStdString() == focus_->isotope && parts.value(1).toStdString() == focus_->detector) current = i;
    }
  focus_box_->setCurrentIndex(current);
}

QStringList EvolutionsView::focus_choices() const {
  QStringList out;
  for (int i = 0; i < focus_box_->count(); ++i) out.append(focus_box_->itemText(i));
  return out;
}

void EvolutionsView::set_kind(SeriesKind kind) {
  if (kind == kind_) return;
  kind_ = kind;
  if (focus_) focus_->kind = kind;  // the same isotope on the same detector, if it has this kind
  for (int i = 0; std::cmp_less(i, std::size(kKinds)); ++i)
    if (kKinds[i] == kind && tabs_->currentIndex() != i) tabs_->setCurrentIndex(i);
  rebuild();
  refresh();
}

void EvolutionsView::rebuild() {
  plot_->clearGraphs();
  graphs_.clear();
  for (const auto& [key, line] : lines_) {
    if (key.kind != kind_ || (focus_ && !(key == *focus_))) continue;
    Graphs g{key};
    g.points = plot_->addGraph();
    g.points->setName(series_name(key));
    g.points->setPen(QPen(line.color, 1.5));
    g.points->setLineStyle(QCPGraph::lsNone);
    // Every point is clickable, and QCustomPlot 2.1.1's scatter thinning
    // divides by a zero pixel span when a column's points are off-range.
    g.points->setAdaptiveSampling(false);
    g.points->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, line.color, line.color, 4));
    g.curve = plot_->addGraph();
    g.curve->setPen(QPen(line.color, 1.2));
    g.curve->removeFromLegend();
    g.intercept = plot_->addGraph();
    g.intercept->setLineStyle(QCPGraph::lsNone);
    g.intercept->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDiamond, QPen(line.color.darker(150), 1.5),
                                                 QBrush(line.color), 9));
    g.intercept->removeFromLegend();
    graphs_.push_back(g);
  }
  excluded_ = nullptr;
  if (!graphs_.empty()) {
    excluded_ = plot_->addGraph();
    excluded_->setLineStyle(QCPGraph::lsNone);
    excluded_->setAdaptiveSampling(false);
    excluded_->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCross, theme().muted_text, 9));
    excluded_->removeFromLegend();
  }
  plot_->legend->setVisible(!graphs_.empty());  // an empty legend draws a stray box
  dirty_ = true;
}

void EvolutionsView::refresh() {
  constexpr int kCurveSamples = 48;
  dirty_ = false;
  QVector<double> ex, ey;
  intercepts_->clear();
  for (auto& g : graphs_) {
    const Line& line = lines_.at(g.key);
    g.points->setData(line.t, line.v, true);
    QVector<double> cx, cy, ix, iy;
    if (line.fit && !line.t.isEmpty()) {
      // From time zero (where the intercept is) to the last point.
      const double x0 = std::min(line.time_zero, line.t.front());
      const double x1 = line.t.back();
      for (int i = 0; i <= kCurveSamples; ++i) {
        const double x = x0 + (x1 - x0) * i / kCurveSamples;
        const double y = reduction::predict(*line.fit, x - line.time_zero);
        if (std::isfinite(y)) {
          cx.push_back(x);
          cy.push_back(y);
        }
      }
      ix.push_back(line.time_zero);
      iy.push_back(line.fit->value);
      for (auto idx : line.fit->filtered_idx)
        if (std::cmp_less(idx, line.t.size())) {
          ex.push_back(line.t[static_cast<qsizetype>(idx)]);
          ey.push_back(line.v[static_cast<qsizetype>(idx)]);
        }
      const double rel = line.fit->value != 0 ? 100.0 * line.fit->error / std::abs(line.fit->value) : 0.0;
      auto* item = new QListWidgetItem(QStringLiteral("%1  %2  %3 ± %4  (%5%)")
                                           .arg(series_name(g.key), QString::fromLatin1(reduction::to_string(line.fit->kind).data()))
                                           .arg(line.fit->value, 0, 'g', 7)
                                           .arg(line.fit->error, 0, 'g', 3)
                                           .arg(rel, 0, 'f', 3),
                                       intercepts_);
      item->setForeground(line.color.darker(130));
      item->setToolTip(tr("%1 point(s) in the fit, %2 excluded").arg(line.fit->n_used).arg(line.fit->filtered_idx.size()));
    }
    g.curve->setData(cx, cy, true);
    g.intercept->setData(ix, iy, true);
  }
  if (excluded_) excluded_->setData(ex, ey);
  plot_->rescaleAxes();
  // A margin so the intercept marker at time zero and the extreme points are not clipped.
  for (QCPAxis* axis : {plot_->xAxis, plot_->yAxis}) {
    const QCPRange r = axis->range();
    const double pad = r.size() > 0 ? r.size() * 0.04 : 1.0;
    axis->setRange(r.lower - pad, r.upper + pad);
  }
  plot_->replot(QCustomPlot::rpQueuedReplot);
}

int EvolutionsView::graph_count() const { return static_cast<int>(graphs_.size()); }

int EvolutionsView::fit_curve_count() const {
  int n = 0;
  for (const auto& g : graphs_) n += g.curve->dataCount() > 0 ? 1 : 0;
  return n;
}

std::optional<reduction::Intercept> EvolutionsView::fit_of(const SeriesKey& key) const {
  auto it = lines_.find(key);
  if (it == lines_.end()) return std::nullopt;
  return it->second.fit;
}

QStringList EvolutionsView::intercept_lines() const {
  QStringList out;
  for (int i = 0; i < intercepts_->count(); ++i) out.append(intercepts_->item(i)->text());
  return out;
}

std::size_t EvolutionsView::point_count(SeriesKind kind) const {
  std::size_t n = 0;
  for (const auto& [key, line] : lines_)
    if (key.kind == kind) n += static_cast<std::size_t>(line.t.size());
  return n;
}

QString EvolutionsView::title_text() const { return title_->text(); }

QStringList EvolutionsView::peak_centers() const {
  QStringList out;
  for (int i = 0; i < peaks_->count(); ++i) out.append(peaks_->item(i)->text());
  return out;
}

QColor EvolutionsView::series_color(const SeriesKey& key) const {
  auto it = lines_.find(key);
  return it == lines_.end() ? QColor() : it->second.color;
}

}  // namespace pychron::ui
