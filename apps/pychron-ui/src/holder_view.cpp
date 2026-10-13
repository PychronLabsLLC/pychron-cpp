#include "holder_view.hpp"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>

#include "theme.hpp"

namespace pychron::ui {

namespace ps = persistence;

namespace {

struct Frame {
  double scale = 1, cx = 0, cy = 0;  // holder units -> widget
  QPointF origin;
  QPointF map(double x, double y) const { return {origin.x() + (x - cx) * scale, origin.y() - (y - cy) * scale}; }
};

Frame frame_for(const QRectF& rect, const ps::HolderValue& holder) {
  double minx = 0, maxx = 0, miny = 0, maxy = 0;
  bool first = true;
  const double r0 = holder.radius.value_or(0.05);
  for (const auto& h : holder.holes) {
    const double r = h.radius.value_or(r0);
    if (first) {
      minx = h.x - r, maxx = h.x + r, miny = h.y - r, maxy = h.y + r;
      first = false;
    }
    minx = std::min(minx, h.x - r), maxx = std::max(maxx, h.x + r);
    miny = std::min(miny, h.y - r), maxy = std::max(maxy, h.y + r);
  }
  Frame f;
  const double w = std::max(maxx - minx, 1e-9), hgt = std::max(maxy - miny, 1e-9);
  f.scale = 0.9 * std::min(rect.width() / w, rect.height() / hgt);
  f.cx = (minx + maxx) / 2;
  f.cy = (miny + maxy) / 2;
  f.origin = rect.center();
  return f;
}

}  // namespace

HolderView::HolderView(QWidget* parent) : QWidget(parent) { setMinimumSize(200, 200); }

void HolderView::set_holder(std::optional<ps::HolderValue> holder) {
  holder_ = std::move(holder);
  update();
}
void HolderView::set_fill(std::map<int, std::string> projects) {
  projects_ = std::move(projects);
  update();
}
void HolderView::set_selected(std::set<int> positions) {
  selected_ = std::move(positions);
  update();
}

void HolderView::paint_holder(QPainter& p, const QRectF& rect, const ps::HolderValue& holder,
                              const std::map<int, std::string>& projects, const std::set<int>& selected) {
  if (holder.holes.empty()) return;
  const Frame f = frame_for(rect, holder);
  const double r0 = holder.radius.value_or(0.05);
  // One colour per project, in name order, from the theme's series.
  std::map<std::string, QColor> colours;
  for (const auto& [pos, project] : projects)
    if (!project.empty()) colours.emplace(project, QColor());
  std::size_t i = 0;
  for (auto& [project, colour] : colours) colour = theme().series[i++ % theme().series.size()];
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  QFont font = p.font();
  for (const auto& h : holder.holes) {
    const int position = h.ordinal + 1;
    const double r = h.radius.value_or(r0) * f.scale;
    const QPointF c = f.map(h.x, h.y);
    const auto filled = projects.find(position);
    p.setBrush(filled != projects.end() && !filled->second.empty() ? QBrush(colours.at(filled->second))
                                                                    : QBrush(Qt::NoBrush));
    p.setPen(QPen(selected.contains(position) ? theme().accent : theme().strong_border, selected.contains(position) ? 3 : 1));
    p.drawEllipse(c, r, r);
    font.setPixelSize(std::max(6, static_cast<int>(r * 0.9)));
    p.setFont(font);
    p.setPen(theme().text);
    p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter,
               holder.has_hole_numbers ? QString::fromStdString(h.hole_id) : QString::number(position));
  }
  p.restore();
}

int HolderView::position_at(const QPointF& point) const {
  if (!holder_) return 0;
  const Frame f = frame_for(QRectF(rect()), *holder_);
  const double r0 = holder_->radius.value_or(0.05);
  for (const auto& h : holder_->holes) {
    const QPointF c = f.map(h.x, h.y);
    const double r = h.radius.value_or(r0) * f.scale;
    if (std::hypot(point.x() - c.x(), point.y() - c.y()) <= r) return h.ordinal + 1;
  }
  return 0;
}

void HolderView::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme().base);
  if (!holder_) {
    p.setPen(theme().muted_text);
    p.drawText(rect(), Qt::AlignCenter, tr("No holder"));
    return;
  }
  paint_holder(p, QRectF(rect()), *holder_, projects_, selected_);
  if (drag_from_) {
    p.setPen(QPen(theme().accent, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(*drag_from_, drag_to_).normalized());
  }
}

void HolderView::mousePressEvent(QMouseEvent* event) {
  drag_from_ = event->position();
  drag_to_ = event->position();
}

void HolderView::mouseMoveEvent(QMouseEvent* event) {
  if (!drag_from_) return;
  drag_to_ = event->position();
  update();
}

void HolderView::mouseReleaseEvent(QMouseEvent* event) {
  if (!drag_from_ || !holder_) return;
  const QRectF band = QRectF(*drag_from_, event->position()).normalized();
  const bool add = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
  std::set<int> picked;
  if (band.width() < 4 && band.height() < 4) {
    if (const int p = position_at(event->position())) picked.insert(p);
  } else {
    const Frame f = frame_for(QRectF(rect()), *holder_);
    for (const auto& h : holder_->holes)
      if (band.contains(f.map(h.x, h.y))) picked.insert(h.ordinal + 1);
  }
  drag_from_.reset();
  if (add) {
    for (int p : picked)
      if (!selected_.insert(p).second) selected_.erase(p);
  } else {
    selected_ = picked;
  }
  update();
  Q_EMIT selection_changed(selected_);
}

}  // namespace pychron::ui
