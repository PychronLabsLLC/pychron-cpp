#include "timeline.hpp"

#include <algorithm>
#include <cmath>

#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>

#include "queue_table_model.hpp"

namespace pychron::ui {

namespace {

using experiment::run::RunState;

constexpr int kRowHeight = 20;
constexpr int kLabelWidth = 56;
constexpr int kAxisHeight = 16;

bool final_state(RunState s) {
  return s == RunState::Success || s == RunState::Failed || s == RunState::Cancelled || s == RunState::Aborted;
}

QString clock_text(double seconds) {
  const auto total = static_cast<long long>(std::llround(std::max(0.0, seconds)));
  return QStringLiteral("%1:%2:%3")
      .arg(total / 3600)
      .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

// ---- model -----------------------------------------------------------------

void TimelineModel::clear() {
  origin_.reset();
  now_ = 0;
  segments_.clear();
  lanes_.clear();
  names_.clear();
  busy_.clear();
}

double TimelineModel::seconds(TimePoint ts) {
  if (!origin_) origin_ = ts;
  const double s = std::chrono::duration<double>(ts - *origin_).count();
  now_ = std::max(now_, s);
  return s;
}

std::optional<int> TimelineModel::lane_of(const std::string& run_id) const {
  auto it = lanes_.find(run_id);
  if (it == lanes_.end()) return std::nullopt;
  return it->second;
}

void TimelineModel::close_run(const std::string& run_id, double at) {
  for (auto& s : segments_)
    if (s.run_id == run_id && !s.end) s.end = std::max(s.start, at);
}

void TimelineModel::on_run_started(const experiment::executor::RunStarted& e) {
  const double t = seconds(e.ts);
  // The queue's own waits (a delay before this run) are over.
  for (auto& s : segments_)
    if (s.lane == 0 && s.run_id.empty() && !s.end) s.end = std::min(t, s.planned_end.value_or(t));
  auto free = std::find(busy_.begin(), busy_.end(), false);
  int lane;
  if (free == busy_.end()) {
    busy_.push_back(true);
    lane = static_cast<int>(busy_.size());
  } else {
    *free = true;
    lane = static_cast<int>(free - busy_.begin()) + 1;
  }
  lanes_[e.run_id] = lane;
  names_[e.run_id] = QString::fromStdString(e.identifier);
}

void TimelineModel::on_state(const experiment::run::RunStateChanged& e) {
  auto it = lanes_.find(e.run_id);
  if (it == lanes_.end()) return;
  const double t = seconds(e.ts);
  close_run(e.run_id, t);
  if (final_state(e.to)) {
    if (it->second >= 1 && it->second <= static_cast<int>(busy_.size())) busy_[static_cast<std::size_t>(it->second - 1)] = false;
    return;
  }
  if (e.to == RunState::Pending) return;
  segments_.push_back({it->second, e.run_id, names_[e.run_id], e.to, t, std::nullopt, std::nullopt});
}

void TimelineModel::on_waiting(const experiment::executor::ExecutorWaiting& e) {
  const double t = seconds(e.ts);
  TimelineSegment s;
  s.lane = 0;
  s.run_id = e.run_id;
  const QString who = e.run_id.empty() ? QString() : names_[e.run_id] + QStringLiteral(": ");
  s.label = who + QString::fromStdString(e.reason);
  s.start = t;
  if (e.duration > experiment::Duration::zero()) s.planned_end = t + e.duration.count();
  segments_.push_back(std::move(s));
}

void TimelineModel::on_queue_ended(TimePoint ts) {
  const double t = seconds(ts);
  for (auto& s : segments_)
    if (!s.end) s.end = std::max(s.start, s.planned_end ? std::min(*s.planned_end, t) : t);
  std::fill(busy_.begin(), busy_.end(), false);
}

void TimelineModel::advance(TimePoint now) {
  if (origin_) seconds(now);
}

double TimelineModel::drawn_end(const TimelineSegment& s) const {
  if (s.end) return *s.end;
  if (s.planned_end) return std::min(*s.planned_end, std::max(now_, s.start));
  return std::max(now_, s.start);
}

// ---- view ------------------------------------------------------------------

TimelineView::TimelineView(const TimelineModel& model, QWidget* parent) : QWidget(parent), model_(model) {
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize TimelineView::sizeHint() const {
  return {600, kAxisHeight + kRowHeight * (1 + std::max(1, model_.run_lanes())) + 4};
}
QSize TimelineView::minimumSizeHint() const { return {200, sizeHint().height()}; }

QRectF TimelineView::plot_area() const {
  return QRectF(kLabelWidth, 2, std::max(10, width() - kLabelWidth - 6), kRowHeight * (1 + std::max(1, model_.run_lanes())));
}

double TimelineView::span() const { return std::max(1.0, model_.now()); }

QRectF TimelineView::segment_rect(const TimelineSegment& s) const {
  const QRectF area = plot_area();
  const double x0 = area.left() + area.width() * s.start / span();
  const double x1 = area.left() + area.width() * model_.drawn_end(s) / span();
  return QRectF(x0, area.top() + s.lane * kRowHeight + 2, std::max(2.0, x1 - x0), kRowHeight - 4);
}

const TimelineSegment* TimelineView::segment_at(QPoint p) const {
  const auto& segs = model_.segments();
  for (auto it = segs.rbegin(); it != segs.rend(); ++it)
    if (segment_rect(*it).contains(p)) return &*it;
  return nullptr;
}

void TimelineView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF area = plot_area();
  const int rows = 1 + std::max(1, model_.run_lanes());
  // Lane backgrounds and names.
  for (int r = 0; r < rows; ++r) {
    const QRectF row(area.left(), area.top() + r * kRowHeight, area.width(), kRowHeight);
    p.fillRect(row, r % 2 == 0 ? QColor(0xf7, 0xf7, 0xf7) : QColor(0xff, 0xff, 0xff));
    p.setPen(QColor(0x55, 0x55, 0x55));
    p.drawText(QRectF(0, row.top(), kLabelWidth - 4, kRowHeight), Qt::AlignRight | Qt::AlignVCenter,
               r == 0 ? tr("Waits") : tr("Lane %1").arg(r));
  }
  // Time axis: about five ticks.
  const double step_raw = span() / 5;
  double step = 1;
  for (double s : {1.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0})
    if (s >= step_raw) {
      step = s;
      break;
    }
  if (step < step_raw) step = std::ceil(step_raw / 3600) * 3600;
  p.setPen(QColor(0xbb, 0xbb, 0xbb));
  const double axis_y = area.bottom();
  for (double t = 0; t <= span() + 1e-9; t += step) {
    const double x = area.left() + area.width() * t / span();
    p.drawLine(QPointF(x, area.top()), QPointF(x, axis_y + 3));
    p.drawText(QRectF(x - 40, axis_y + 2, 80, kAxisHeight), Qt::AlignHCenter | Qt::AlignTop, clock_text(t));
  }
  // Segments.
  for (const auto& s : model_.segments()) {
    const QRectF r = segment_rect(s);
    QColor fill = s.state ? QueueTableModel::state_color(*s.state) : QColor(0xdd, 0xdd, 0xdd);
    p.setPen(QPen(fill.darker(140), 1));
    p.setBrush(s.state ? QBrush(fill) : QBrush(fill, Qt::BDiagPattern));
    p.drawRoundedRect(r, 3, 3);
    QString text = s.state ? s.label + QStringLiteral(" ") + QString::fromLatin1(experiment::run::to_string(*s.state).data())
                           : s.label;
    const QFontMetrics fm(font());
    if (r.width() > 24) {
      p.setPen(QColor(0x22, 0x22, 0x22));
      p.drawText(r.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                 fm.elidedText(text, Qt::ElideRight, static_cast<int>(r.width() - 6)));
    }
  }
  // Now.
  const double xn = area.left() + area.width() * model_.now() / span();
  p.setPen(QPen(QColor(0xd3, 0x2f, 0x2f), 1, Qt::DashLine));
  p.drawLine(QPointF(xn, area.top()), QPointF(xn, area.bottom()));
}

bool TimelineView::event(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    auto* help = static_cast<QHelpEvent*>(event);
    if (const TimelineSegment* s = segment_at(help->pos())) {
      const double end = model_.drawn_end(*s);
      QString text = s->label;
      if (s->state) text += QStringLiteral(" · ") + QString::fromLatin1(experiment::run::to_string(*s->state).data());
      text += QStringLiteral("\n%1 – %2 (%3 s)%4")
                  .arg(clock_text(s->start), clock_text(end))
                  .arg(end - s->start, 0, 'f', 1)
                  .arg(s->end ? QString() : tr(", ongoing"));
      QToolTip::showText(help->globalPos(), text, this);
    } else {
      QToolTip::hideText();
    }
    return true;
  }
  return QWidget::event(event);
}

}  // namespace pychron::ui
