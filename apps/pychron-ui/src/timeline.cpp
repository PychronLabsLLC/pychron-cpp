#include "timeline.hpp"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>
#include <ranges>
#include <tuple>
#include <utility>

#include "queue_table_model.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

using experiment::run::RunState;

constexpr int kRowHeight = 20;
constexpr int kLabelWidth = 56;
constexpr int kAxisHeight = 16;
constexpr double kZoomPerNotch = 1.25;
constexpr double kWheelNotch = 120;  // angleDelta of one notch
constexpr double kDragThreshold = 3;  // px a press may move and still be a click

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
  open_.clear();
  run_starts_.clear();
  longest_ = 0;
  ordered_ = true;
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

void TimelineModel::add(TimelineSegment s) {
  if (!segments_.empty() && s.start < segments_.back().start) ordered_ = false;
  open_.push_back(segments_.size());
  segments_.push_back(std::move(s));
}

// `which` returns the end for a segment it closes, nullopt to leave it open.
template <typename Pick>
void TimelineModel::close_open(Pick which) {
  std::erase_if(open_, [&](std::size_t i) {
    TimelineSegment& s = segments_[i];
    const std::optional<double> end = which(s);
    if (!end) return false;
    s.end = *end;
    longest_ = std::max(longest_, *end - s.start);
    return true;
  });
}

void TimelineModel::close_run(const std::string& run_id, double at) {
  close_open([&](const TimelineSegment& s) -> std::optional<double> {
    if (s.run_id != run_id) return std::nullopt;
    return std::max(s.start, at);
  });
}

std::vector<std::size_t> TimelineModel::visible(double t0, double t1) const {
  std::vector<std::size_t> out;
  const auto shows = [&](std::size_t i) {
    const TimelineSegment& s = segments_[i];
    return s.start <= t1 && drawn_end(s) >= t0;
  };
  if (!ordered_) {
    for (std::size_t i = 0; i < segments_.size(); ++i)
      if (shows(i)) out.push_back(i);
    return out;
  }
  // A closed segment that reaches t0 started no earlier than t0 - longest_.
  const auto first = std::ranges::lower_bound(segments_, t0 - longest_, {}, &TimelineSegment::start);
  const auto from = static_cast<std::size_t>(first - segments_.begin());
  for (std::size_t i : open_)
    if (i < from && shows(i)) out.push_back(i);
  for (std::size_t i = from; i < segments_.size() && segments_[i].start <= t1; ++i)
    if (shows(i)) out.push_back(i);
  return out;
}

void TimelineModel::on_run_started(const experiment::executor::RunStarted& e) {
  const double t = seconds(e.ts);
  // The queue's own waits (a delay before this run) are over.
  close_open([&](const TimelineSegment& s) -> std::optional<double> {
    if (s.lane != 0 || !s.run_id.empty()) return std::nullopt;
    return std::min(t, s.planned_end.value_or(t));
  });
  run_starts_.push_back(t);
  auto free = std::find(busy_.begin(), busy_.end(), false);
  int lane = 0;
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
    if (it->second >= 1 && std::cmp_less_equal(it->second, busy_.size())) busy_[static_cast<std::size_t>(it->second - 1)] = false;
    return;
  }
  if (e.to == RunState::Pending) return;
  add({it->second, e.run_id, names_[e.run_id], e.to, t, std::nullopt, std::nullopt});
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
  add(std::move(s));
}

void TimelineModel::on_queue_ended(TimePoint ts) {
  const double t = seconds(ts);
  close_open([&](const TimelineSegment& s) -> std::optional<double> {
    return std::max(s.start, s.planned_end ? std::min(*s.planned_end, t) : t);
  });
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

TimelineView::TimelineView(const TimelineModel& model, QWidget* parent)
    : QWidget(parent), model_(model), follow_(new QToolButton(this)) {
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  follow_->setText(tr("Follow"));
  follow_->setToolTip(tr("Back to the runs in progress"));
  follow_->setFixedHeight(kRowHeight - 2);
  follow_->setCursor(Qt::ArrowCursor);
  follow_->hide();
  connect(follow_, &QToolButton::clicked, this, [this] { set_mode(Mode::Follow); });
  for (int runs : {3, kFollowRuns, 12}) {
    auto* follow = new QAction(tr("Follow last %1 runs").arg(runs), this);
    connect(follow, &QAction::triggered, this, [this, runs] {
      set_follow_runs(runs);
      set_mode(Mode::Follow);
      emit follow_runs_changed(runs);
    });
    actions_ << follow;
  }
  auto* whole = new QAction(tr("Whole queue"), this);
  connect(whole, &QAction::triggered, this, [this] { set_mode(Mode::WholeQueue); });
  actions_ << whole;
}

QSize TimelineView::sizeHint() const {
  return {600, kAxisHeight + kRowHeight * (1 + std::max(1, model_.run_lanes())) + 4};
}
QSize TimelineView::minimumSizeHint() const { return {200, sizeHint().height()}; }

QRectF TimelineView::plot_area() const {
  return QRectF(kLabelWidth, 2, std::max(10, width() - kLabelWidth - 6), kRowHeight * (1 + std::max(1, model_.run_lanes())));
}

double TimelineView::full_end() const { return std::max(kMinSpan, model_.now()); }

void TimelineView::set_mode(Mode mode) {
  if (mode == Mode::Free && mode_ != Mode::Free) std::tie(t0_, t1_) = time_window();  // stays where it was
  apply_mode(mode);
}

void TimelineView::apply_mode(Mode mode) {
  mode_ = mode;
  follow_->setVisible(mode_ != Mode::Follow);
  update();
}

void TimelineView::set_follow_runs(int runs) {
  follow_runs_ = std::max(1, runs);
  update();
}

std::pair<double, double> TimelineView::time_window() const {
  switch (mode_) {
    case Mode::WholeQueue: return {0.0, std::max(1.0, model_.now())};
    case Mode::Free: return {t0_, t1_};
    case Mode::Follow: break;
  }
  // The queue's leading wait shows until enough runs have started.
  const auto& starts = model_.run_starts();
  const auto n = static_cast<std::size_t>(follow_runs_);
  const double start = starts.size() >= n ? starts[starts.size() - n] : 0.0;
  return {start, std::max(start + kMinSpan, model_.now())};
}

void TimelineView::set_time_window(double t0, double t1) {
  const double end = full_end();
  const double span = std::clamp(t1 - t0, kMinSpan, end);
  t0_ = std::clamp(t0, 0.0, end - span);
  t1_ = t0_ + span;
  apply_mode(Mode::Free);
}

// Empty for a segment the window does not show; one that crosses an edge of
// the window is cut there.
QRectF TimelineView::segment_rect(const TimelineSegment& s) const {
  const auto [t0, t1] = time_window();
  const double end = model_.drawn_end(s);
  if (s.start > t1 || end < t0) return {};
  const QRectF area = plot_area();
  const double scale = area.width() / (t1 - t0);
  const double x0 = std::max(area.left(), area.left() + (s.start - t0) * scale);
  const double x1 = std::min(area.right(), area.left() + (end - t0) * scale);
  return QRectF(x0, area.top() + s.lane * kRowHeight + 2, std::max(2.0, x1 - x0), kRowHeight - 4);
}

const TimelineSegment* TimelineView::segment_at(QPoint p) const {
  const auto [t0, t1] = time_window();
  const auto& segs = model_.segments();
  for (std::size_t i : std::views::reverse(model_.visible(t0, t1)))
    if (segment_rect(segs[i]).contains(p)) return &segs[i];
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
    p.fillRect(row, r % 2 == 0 ? theme().alt_base : theme().base);
    p.setPen(theme().muted_text);
    p.drawText(QRectF(0, row.top(), kLabelWidth - 4, kRowHeight), Qt::AlignRight | Qt::AlignVCenter,
               r == 0 ? tr("Waits") : tr("Lane %1").arg(r));
  }
  // Time axis: about five ticks.
  const auto [t0, t1] = time_window();
  const double span = t1 - t0;
  const double step_raw = span / 5;
  double step = 1;
  for (double s : {1.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0})
    if (s >= step_raw) {
      step = s;
      break;
    }
  if (step < step_raw) step = std::ceil(step_raw / 3600) * 3600;
  p.setPen(theme().grid);
  const double axis_y = area.bottom();
  // Counted, so the last tick does not depend on how the steps added up.
  const auto first_tick = static_cast<long long>(std::ceil((t0 - 1e-9) / step));
  const auto last_tick = static_cast<long long>(std::floor((t1 + 1e-9) / step));
  for (long long i = first_tick; i <= last_tick; ++i) {
    const double t = static_cast<double>(i) * step;
    const double x = area.left() + area.width() * (t - t0) / span;
    p.drawLine(QPointF(x, area.top()), QPointF(x, axis_y + 3));
    p.drawText(QRectF(x - 40, axis_y + 2, 80, kAxisHeight), Qt::AlignHCenter | Qt::AlignTop, clock_text(t));
  }
  // Segments: the window's only, whatever the queue's length.
  const auto shown = model_.visible(t0, t1);
  drawn_ = static_cast<int>(shown.size());
  for (std::size_t i : shown) {
    const TimelineSegment& s = model_.segments()[i];
    const QRectF r = segment_rect(s);
    QColor fill = s.state ? QueueTableModel::state_color(*s.state) : theme().neutral_fill;
    p.setPen(QPen(fill.darker(140), 1));
    p.setBrush(s.state ? QBrush(fill) : QBrush(fill, Qt::BDiagPattern));
    p.drawRoundedRect(r, 3, 3);
    QString text = s.state ? s.label + QStringLiteral(" ") + QString::fromLatin1(experiment::run::to_string(*s.state).data())
                           : s.label;
    const QFontMetrics fm(font());
    if (r.width() > 24) {
      p.setPen(theme().text);
      p.drawText(r.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                 fm.elidedText(text, Qt::ElideRight, static_cast<int>(r.width() - 6)));
    }
  }
  // Now, when the window holds it.
  if (model_.now() >= t0 && model_.now() <= t1) {
    const double xn = area.left() + area.width() * (model_.now() - t0) / span;
    p.setPen(QPen(theme().error, 1, Qt::DashLine));
    p.drawLine(QPointF(xn, area.top()), QPointF(xn, area.bottom()));
  }
}

// Ctrl+wheel zooms about the time under the cursor. A plain wheel is not
// ours: the pane the view sits in scrolls with it.
void TimelineView::wheelEvent(QWheelEvent* event) {
  const double notches = event->angleDelta().y() / kWheelNotch;
  if (!event->modifiers().testFlag(Qt::ControlModifier) || notches == 0) {
    event->ignore();
    return;
  }
  const auto [t0, t1] = time_window();
  const QRectF area = plot_area();
  const double at = std::clamp((event->position().x() - area.left()) / area.width(), 0.0, 1.0);
  const double span = std::clamp((t1 - t0) * std::pow(kZoomPerNotch, -notches), kMinSpan, full_end());
  const double start = t0 + at * (t1 - t0) - at * span;
  set_time_window(start, start + span);
  event->accept();
}

void TimelineView::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
  drag_from_ = event->position();
  std::tie(drag_t0_, drag_t1_) = time_window();
}

void TimelineView::mouseMoveEvent(QMouseEvent* event) {
  if (!drag_from_) return QWidget::mouseMoveEvent(event);
  const double dx = event->position().x() - drag_from_->x();
  if (mode_ != Mode::Free && std::abs(dx) < kDragThreshold) return;  // still a click
  const double dt = -dx / plot_area().width() * (drag_t1_ - drag_t0_);
  set_time_window(drag_t0_ + dt, drag_t1_ + dt);
}

void TimelineView::mouseReleaseEvent(QMouseEvent* event) {
  drag_from_.reset();
  QWidget::mouseReleaseEvent(event);
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return QWidget::mouseDoubleClickEvent(event);
  drag_from_.reset();
  set_mode(Mode::Follow);
}

void TimelineView::contextMenuEvent(QContextMenuEvent* event) {
  QMenu menu(this);
  menu.addActions(actions_);
  menu.exec(event->globalPos());
}

void TimelineView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  const QRectF area = plot_area();
  follow_->adjustSize();
  follow_->move(static_cast<int>(area.right()) - follow_->width() - 1, static_cast<int>(area.top()) + 1);
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
