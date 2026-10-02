#include "evolutions_view.hpp"

#include <set>
#include <utility>

#include <QLabel>
#include <QListWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include <qcustomplot.h>

#include "strip_chart_model.hpp"

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
      title_(new QLabel),
      plot_(new QCustomPlot),
      peaks_(new QListWidget) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  for (auto k : kKinds) {
    QString name = QString::fromLatin1(experiment::collect::to_string(k).data());
    if (!name.isEmpty()) name[0] = name[0].toUpper();
    tabs_->addTab(name);
  }
  layout->addWidget(tabs_);
  layout->addWidget(title_);
  layout->addWidget(plot_, 1);
  auto* peaks_label = new QLabel(tr("Peak centers"));
  layout->addWidget(peaks_label);
  peaks_->setMaximumHeight(80);
  layout->addWidget(peaks_);

  plot_->setMinimumHeight(200);
  plot_->axisRect()->setBackground(QBrush(QColor(0xfa, 0xfa, 0xd2)));
  plot_->xAxis->setLabel(tr("Time (s)"));
  plot_->yAxis->setLabel(tr("Intensity"));
  plot_->legend->setVisible(false);  // shown once there are graphs
  plot_->setPlottingHint(QCP::phFastPolylines, true);

  connect(tabs_, &QTabBar::currentChanged, this, [this](int i) {
    if (i >= 0 && i < static_cast<int>(std::size(kKinds))) set_kind(kKinds[i]);
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
  if (new_line) rebuild();
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

void EvolutionsView::set_kind(SeriesKind kind) {
  if (kind == kind_) return;
  kind_ = kind;
  for (int i = 0; i < static_cast<int>(std::size(kKinds)); ++i)
    if (kKinds[i] == kind && tabs_->currentIndex() != i) tabs_->setCurrentIndex(i);
  rebuild();
  refresh();
}

void EvolutionsView::rebuild() {
  plot_->clearGraphs();
  graphs_.clear();
  for (const auto& [key, line] : lines_) {
    if (key.kind != kind_) continue;
    QCPGraph* g = plot_->addGraph();
    g->setName(series_name(key));
    g->setPen(QPen(line.color, 1.5));
    g->setLineStyle(QCPGraph::lsNone);
    g->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, line.color, line.color, 4));
    graphs_.emplace_back(key, g);
  }
  plot_->legend->setVisible(!graphs_.empty());  // an empty legend draws a stray box
  dirty_ = true;
}

void EvolutionsView::refresh() {
  dirty_ = false;
  for (auto& [key, graph] : graphs_) {
    const Line& line = lines_.at(key);
    graph->setData(line.t, line.v, true);
  }
  plot_->rescaleAxes();
  plot_->replot(QCustomPlot::rpQueuedReplot);
}

int EvolutionsView::graph_count() const { return plot_->graphCount(); }

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
