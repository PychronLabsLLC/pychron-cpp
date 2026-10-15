#include "evolutions_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QTabBar>
#include <QTableWidget>
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

std::vector<EvolutionPanel> evolution_panels(const std::vector<SeriesKey>& arrived, SeriesKind kind) {
  std::vector<EvolutionPanel> out;
  for (const auto& key : arrived) {
    if (key.kind != kind) continue;
    const QString title = kind == SeriesKind::Sniff ? QString::fromStdString(key.isotope) : series_name(key);
    out.push_back({key, title});
  }
  return out;
}

EvolutionsView::EvolutionsView(std::map<std::string, QColor> colors, QWidget* parent)
    : QWidget(parent),
      colors_(std::move(colors)),
      tabs_(new QTabBar),
      title_(new QLabel),
      plot_(new QCustomPlot),
      margins_(new QCPMarginGroup(plot_)),
      scroll_(new QScrollArea),
      peaks_(new QListWidget),
      intercepts_(new QTableWidget) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  for (auto k : kKinds) {
    QString name = QString::fromLatin1(experiment::collect::to_string(k).data());
    if (!name.isEmpty()) name[0] = name[0].toUpper();
    tabs_->addTab(name);
  }
  layout->addWidget(tabs_);
  layout->addWidget(title_);
  // The panels scroll; they never scroll sideways.
  scroll_->setWidget(plot_);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setMinimumHeight(200);
  layout->addWidget(scroll_, 1);
  layout->addWidget(new QLabel(tr("Intercepts")));
  intercepts_->setColumnCount(7);
  intercepts_->setHorizontalHeaderLabels(
      {tr("Isotope"), tr("Detector"), tr("Fit"), tr("Intercept"), tr("± 1σ"), tr("%"), tr("Points")});
  intercepts_->verticalHeader()->setVisible(false);
  intercepts_->verticalHeader()->setDefaultSectionSize(intercepts_->fontMetrics().height() + 6);
  intercepts_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  intercepts_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  intercepts_->setSelectionBehavior(QAbstractItemView::SelectRows);
  intercepts_->setSelectionMode(QAbstractItemView::SingleSelection);
  intercepts_->setMaximumHeight(150);
  layout->addWidget(intercepts_);
  auto* peaks_label = new QLabel(tr("Peak centers"));
  layout->addWidget(peaks_label);
  peaks_->setMaximumHeight(80);
  layout->addWidget(peaks_);

  plot_->setPlottingHint(QCP::phFastPolylines, true);
  rebuild();  // no panels yet: the default axis rect goes
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
  arrived_.clear();
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
        arrived_.push_back(key);
        new_line |= key.kind == kind_;
      }
      it->second.t.push_back(u.t);
      it->second.v.push_back(value);
    }
    block_ = QStringLiteral("%1 %2/%3").arg(QString::fromStdString(u.label)).arg(u.count).arg(u.target);
  }
  title_->setText(block_.isEmpty() ? run_ : QStringLiteral("%1 — %2").arg(run_, block_));
  if (new_line) {
    rebuild();
    refresh();  // the new rects at their data at once, not at QCustomPlot's default range
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

void EvolutionsView::set_kind(SeriesKind kind) {
  if (kind == kind_) return;
  kind_ = kind;
  for (int i = 0; std::cmp_less(i, std::size(kKinds)); ++i)
    if (kKinds[i] == kind && tabs_->currentIndex() != i) tabs_->setCurrentIndex(i);
  rebuild();
  refresh();
}

void EvolutionsView::rebuild() {
  // Graphs first: they hang on the axes the layout's rects own.
  plot_->clearGraphs();
  plot_->plotLayout()->clear();
  graphs_.clear();
  const auto panels = evolution_panels(arrived_, kind_);
  for (std::size_t i = 0; i < panels.size(); ++i) {
    const Line& line = lines_.at(panels[i].key);
    auto* rect = new QCPAxisRect(plot_);  // owned by the layout
    plot_->plotLayout()->addElement(static_cast<int>(i), 0, rect);
    rect->setBackground(QBrush(theme().plot_bg));
    rect->setMarginGroup(QCP::msLeft | QCP::msRight, margins_);
    QCPAxis* x = rect->axis(QCPAxis::atBottom);
    QCPAxis* y = rect->axis(QCPAxis::atLeft);
    y->setLabel(panels[i].title);
    // One time axis: labelled under the bottom panel only.
    const bool bottom = i + 1 == panels.size();
    x->setTickLabels(bottom);
    x->setLabel(bottom ? tr("Time (s)") : QString());
    Graphs g{panels[i].key, panels[i].title, rect};
    g.points = plot_->addGraph(x, y);
    g.points->setPen(QPen(line.color, 1.5));
    g.points->setLineStyle(QCPGraph::lsNone);
    // Every point is clickable, and QCustomPlot 2.1.1's scatter thinning
    // divides by a zero pixel span when a column's points are off-range.
    g.points->setAdaptiveSampling(false);
    g.points->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCircle, line.color, line.color, 4));
    g.curve = plot_->addGraph(x, y);
    g.curve->setPen(QPen(line.color, 1.2));
    g.intercept = plot_->addGraph(x, y);
    g.intercept->setLineStyle(QCPGraph::lsNone);
    g.intercept->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDiamond, QPen(line.color.darker(150), 1.5),
                                                 QBrush(line.color), 9));
    g.excluded = plot_->addGraph(x, y);
    g.excluded->setLineStyle(QCPGraph::lsNone);
    g.excluded->setAdaptiveSampling(false);
    g.excluded->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssCross, theme().muted_text, 9));
    graphs_.push_back(g);
  }
  constexpr int kTimeAxisHeight = 40;  // the bottom panel's tick labels and title
  plot_->setMinimumHeight(static_cast<int>(panels.size()) * kPanelMinHeight + kTimeAxisHeight);
  dirty_ = true;
}

void EvolutionsView::refresh() {
  constexpr int kCurveSamples = 48;
  dirty_ = false;
  int row = 0;
  for (auto& g : graphs_) {
    QVector<double> ex, ey;
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
      const std::array<QString, 7> cells = {QString::fromStdString(g.key.isotope),
                               QString::fromStdString(g.key.detector),
                               QString::fromLatin1(reduction::to_string(line.fit->kind).data()),
                               QString::number(line.fit->value, 'g', 7),
                               QString::number(line.fit->error, 'g', 3),
                               QString::number(rel, 'f', 3),
                               QString::number(line.fit->n_used)};
      if (intercepts_->rowCount() <= row) intercepts_->setRowCount(row + 1);
      for (int c = 0; std::cmp_less(c, cells.size()); ++c) {
        // The row's items are kept from one refresh to the next, so a selection stays.
        auto* item = intercepts_->item(row, c);
        if (!item) {
          item = new QTableWidgetItem;
          if (c >= 3) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
          intercepts_->setItem(row, c, item);
        }
        item->setText(cells[static_cast<std::size_t>(c)]);
        item->setForeground(c < 2 ? QBrush(line.color.darker(130)) : QBrush());
        item->setToolTip(tr("%1 point(s) in the fit, %2 excluded").arg(line.fit->n_used).arg(line.fit->filtered_idx.size()));
      }
      ++row;
    }
    g.curve->setData(cx, cy, true);
    g.intercept->setData(ix, iy, true);
    g.excluded->setData(ex, ey);
  }
  intercepts_->setRowCount(row);
  // A margin so the intercept marker at time zero and the extreme points are not clipped.
  const auto padded = [](const QCPRange& r) {
    const double pad = r.size() > 0 ? r.size() * 0.04 : 1.0;
    return QCPRange(r.lower - pad, r.upper + pad);
  };
  // Each panel's y is its own; x is one range over every panel.
  std::optional<QCPRange> x;
  for (const auto& g : graphs_) {
    QCPAxis* y = g.rect->axis(QCPAxis::atLeft);
    y->rescale();
    y->setRange(padded(y->range()));
    QCPAxis* t = g.rect->axis(QCPAxis::atBottom);
    t->rescale();
    if (x) {
      x->expand(t->range());
    } else {
      x = t->range();
    }
  }
  if (x)
    for (const auto& g : graphs_) g.rect->axis(QCPAxis::atBottom)->setRange(padded(*x));
  // The bottom row also holds the time axis: it gets that much more height, so
  // its plotting area is as tall as the others'. Measured from the last layout;
  // a change asks for another refresh until it settles.
  if (graphs_.size() > 1) {
    const QCPAxisRect* top = graphs_.front().rect;
    const QCPAxisRect* bottom = graphs_.back().rect;
    const int outer = top->outerRect().height();
    const int extra = (bottom->margins().top() + bottom->margins().bottom()) -
                      (top->margins().top() + top->margins().bottom());
    if (outer > 0 && extra > 0) {
      const double factor = static_cast<double>(outer + extra) / outer;
      auto* layout = plot_->plotLayout();
      const int last = static_cast<int>(graphs_.size()) - 1;
      if (std::abs(layout->rowStretchFactors().value(last, 1.0) - factor) > 0.01) {
        layout->setRowStretchFactor(last, factor);
        dirty_ = true;
      }
    }
  }
  plot_->replot(QCustomPlot::rpQueuedReplot);
}

int EvolutionsView::graph_count() const { return static_cast<int>(graphs_.size()); }

QStringList EvolutionsView::panel_titles() const {
  QStringList out;
  for (const auto& g : graphs_) out << g.title;
  return out;
}

std::pair<double, double> EvolutionsView::panel_x_range(int i) const {
  if (i < 0 || std::cmp_greater_equal(i, graphs_.size())) return {0, 0};
  const QCPRange r = graphs_[static_cast<std::size_t>(i)].rect->axis(QCPAxis::atBottom)->range();
  return {r.lower, r.upper};
}

std::pair<double, double> EvolutionsView::panel_y_range(int i) const {
  if (i < 0 || std::cmp_greater_equal(i, graphs_.size())) return {0, 0};
  const QCPRange r = graphs_[static_cast<std::size_t>(i)].rect->axis(QCPAxis::atLeft)->range();
  return {r.lower, r.upper};
}

int EvolutionsView::panel_plot_height(int i) const {
  if (i < 0 || std::cmp_greater_equal(i, graphs_.size())) return 0;
  return graphs_[static_cast<std::size_t>(i)].rect->height();
}

bool EvolutionsView::panel_has_time_labels(int i) const {
  if (i < 0 || std::cmp_greater_equal(i, graphs_.size())) return false;
  const QCPAxis* x = graphs_[static_cast<std::size_t>(i)].rect->axis(QCPAxis::atBottom);
  return x->tickLabels() && !x->label().isEmpty();
}

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

QStringList EvolutionsView::intercept_columns() const {
  QStringList out;
  for (int c = 0; c < intercepts_->columnCount(); ++c) out.append(intercepts_->horizontalHeaderItem(c)->text());
  return out;
}

QList<QStringList> EvolutionsView::intercept_rows() const {
  QList<QStringList> out;
  for (int r = 0; r < intercepts_->rowCount(); ++r) {
    QStringList cells;
    for (int c = 0; c < intercepts_->columnCount(); ++c) cells.append(intercepts_->item(r, c)->text());
    out.append(cells);
  }
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
