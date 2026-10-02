#include "canvas_view.hpp"

#include <algorithm>
#include <array>

#include <QMessageBox>

#include "pychron/systems/network_graph.hpp"

namespace pychron::ui {

namespace {

QColor color_or(const canvas::Canvas& c, const std::string& key, QColor fallback) {
  if (auto it = c.colors.find(key); it != c.colors.end()) {
    QColor parsed(QString::fromStdString(it->second));
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return fallback;
}

// Region fill colours, cycled by region index.
constexpr std::array<QRgb, 6> kRegionPalette = {0x8fd3ff, 0xffd27f, 0xb6e3a8, 0xe3b6e0, 0xffb3a7, 0xc9c3ff};

// Projection of `p` onto the segment a-b, clamped to it.
QPointF project(QPointF p, QPointF a, QPointF b) {
  const QPointF ab = b - a;
  const double len2 = QPointF::dotProduct(ab, ab);
  if (len2 == 0) {
    return a;
  }
  const double t = std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0);
  return a + t * ab;
}

}  // namespace

QColor CanvasView::isolated_color() { return QColor(0xdd, 0xdd, 0xdd); }

CanvasView::CanvasView(CoreBridge& bridge, QWidget* parent) : QGraphicsView(parent), bridge_(bridge) {
  setScene(&scene_);
  setRenderHint(QPainter::Antialiasing);
  if (const canvas::Canvas* c = bridge_.canvas()) {
    build(*c);
    open_valve_color_ = c->canvas.open_valve_color;
  }

  connect(&bridge_, &CoreBridge::snapshot, this, [this](const Snapshot&) { apply_state(); });
  connect(&bridge_, &CoreBridge::valveChanged, this, [this](const ValveChanged&) { apply_state(); });
  connect(&bridge_, &CoreBridge::lockChanged, this, [this](const QString&, bool) { apply_state(); });
  confirm_unlock_ = [this](const QString& name) {
    return QMessageBox::question(this, tr("Unlock valve"),
                                 tr("Unlock %1? It will accept open and close commands again.").arg(name),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
  };
  connect(&bridge_, &CoreBridge::pressureSample, this, &CanvasView::on_pressure);
  connect(&bridge_, &CoreBridge::alarm, this, &CanvasView::on_alarm);
  connect(&bridge_, &CoreBridge::actuationStarted, this, [this](const QString& name) {
    if (ValveItem* v = valve(name.toStdString())) {
      v->set_pending(true);
    }
  });
  connect(&bridge_, &CoreBridge::actuationFinished, this, &CanvasView::on_finished);
  apply_state();
}

void CanvasView::build(const canvas::Canvas& c) {
  const QPointF origin = to_qpoint(c.canvas.origin);
  scene_.setSceneRect(origin.x(), origin.y(), c.canvas.size.width, c.canvas.size.height);

  for (const auto& v : c.valves) {
    auto* item = new ValveItem(v.name, v.kind);
    item->setPos(to_qpoint(v.pos));
    item->set_on_click([this](const std::string& name) { on_click(name); });
    item->set_on_lock_request([this](const std::string& name, bool locked) { request_lock(name, locked); });
    scene_.addItem(item);
    valves_[v.name] = item;
    positions_[v.name] = item->pos();
  }
  const QColor stage_color = color_or(c, "stage", isolated_color());
  for (const auto& s : c.stages) {
    const std::string& label = s.display_name.empty() ? s.name : s.display_name;
    auto* item = new StageItem(s.name, QString::fromStdString(label), s.size, stage_color);
    item->setPos(to_qpoint(s.pos));
    scene_.addItem(item);
    stages_[s.name] = item;
    positions_[s.name] = item->pos();
  }
  const QColor pipette_color = color_or(c, "pipette", isolated_color());
  for (const auto& p : c.pipettes) {
    const std::string& label = p.vlabel.empty() ? p.name : p.vlabel;
    auto* item = new StageItem(p.name, QString::fromStdString(label), p.size, pipette_color);
    item->setPos(to_qpoint(p.pos));
    scene_.addItem(item);
    stages_[p.name] = item;
    positions_[p.name] = item->pos();
  }
  for (const auto& g : c.gauges) {
    auto* item = new GaugeLabelItem(g.name);
    item->setPos(to_qpoint(g.pos));
    scene_.addItem(item);
    gauges_[g.name] = item;
    positions_[g.name] = item->pos();
  }
  for (const auto& l : c.labels) {
    auto* item = new LabelItem(QString::fromStdString(l.text), QString::fromStdString(l.font));
    item->setPos(to_qpoint(l.pos));
    scene_.addItem(item);
  }

  const auto width = static_cast<double>(c.canvas.connection_width);
  for (const auto& conn : c.connections) {
    add_path({conn.start, conn.end}, width);
  }
  for (const auto& e : c.elbows) {
    QPointF a;
    QPointF b;
    if (!position(e.start, a) || !position(e.end, b)) {
      continue;
    }
    const bool left = e.corner == canvas::Corner::UpperLeft || e.corner == canvas::Corner::LowerLeft;
    const bool upper = e.corner == canvas::Corner::UpperLeft || e.corner == canvas::Corner::UpperRight;
    const QPointF corner(left ? std::min(a.x(), b.x()) : std::max(a.x(), b.x()),
                         upper ? std::min(a.y(), b.y()) : std::max(a.y(), b.y()));
    add_pipe({a, corner, b}, width, {e.start, e.end});
  }
  for (const auto& t : c.tees) {
    QPointF l;
    QPointF r;
    QPointF m;
    if (!position(t.left, l) || !position(t.right, r) || !position(t.mid, m)) {
      continue;
    }
    add_pipe({l, r}, width, {t.left, t.right, t.mid});
    add_pipe({m, project(m, l, r)}, width, {t.mid, t.left, t.right});
    --connections_;  // a tee counts once
  }
  for (const auto& x : c.crosses) {
    add_path({x.left, x.right}, width);
    add_path({x.top, x.bottom}, width);
  }
}

void CanvasView::add_path(const std::vector<std::string>& names, double width) {
  std::vector<QPointF> points;
  for (const auto& name : names) {
    QPointF p;
    if (!position(name, p)) {
      return;
    }
    points.push_back(p);
  }
  add_pipe(points, width, names);
}

ConnectionItem* CanvasView::add_pipe(const std::vector<QPointF>& points, double width,
                                     std::vector<std::string> endpoints) {
  auto* item = new ConnectionItem(points, width, std::move(endpoints));
  scene_.addItem(item);
  pipes_.push_back(item);
  ++connections_;
  return item;
}

bool CanvasView::position(const std::string& name, QPointF& out) const {
  auto it = positions_.find(name);
  if (it == positions_.end()) {
    return false;
  }
  out = it->second;
  return true;
}

ValveItem* CanvasView::valve(const std::string& name) const {
  auto it = valves_.find(name);
  return it == valves_.end() ? nullptr : it->second;
}

StageItem* CanvasView::stage(const std::string& name) const {
  auto it = stages_.find(name);
  return it == stages_.end() ? nullptr : it->second;
}

GaugeLabelItem* CanvasView::gauge(const std::string& name) const {
  auto it = gauges_.find(name);
  return it == gauges_.end() ? nullptr : it->second;
}

void CanvasView::on_click(const std::string& name) {
  const ValveItem* item = valve(name);
  if (!item || bridge_.pending(name)) {
    return;
  }
  const auto op = item->state() == ValveState::Open ? systems::SwitchOp::Close : systems::SwitchOp::Open;
  bridge_.actuate(QString::fromStdString(name), op);
}

bool CanvasView::request_lock(const std::string& name, bool locked) {
  ValveItem* item = valve(name);
  if (!item || item->kind() == canvas::ValveKind::Manual) {
    return false;
  }
  const QString qname = QString::fromStdString(name);
  if (!locked && !confirm_unlock_(qname)) {
    return false;
  }
  if (auto result = bridge_.set_locked(qname, locked); !result) {
    item->flash(QString::fromStdString(result.error().what));
    return false;
  }
  return true;
}

void CanvasView::apply_state() {
  const auto& state = bridge_.state();
  for (auto& [name, item] : valves_) {
    if (auto it = state.valves.find(name); it != state.valves.end()) {
      item->set_state(it->second);
    }
    if (auto it = state.switches.find(name); it != state.switches.end()) {
      item->set_locked(it->second.locked);
    }
    item->set_pending(bridge_.pending(name));
  }
  for (auto& [name, item] : gauges_) {
    if (auto it = state.pressures.find(name); it != state.pressures.end()) {
      auto units = state.units.find(name);
      item->set_value(it->second, units == state.units.end() ? std::string{} : units->second);
    }
  }
  apply_regions();
}

void CanvasView::set_open_valve_color(canvas::OpenValveColor mode) {
  open_valve_color_ = mode;
  apply_regions();
}

void CanvasView::apply_regions() {
  const systems::NetworkGraph* network = bridge_.network();
  if (!network) {
    for (auto& [name, item] : valves_) {
      item->set_inherited_color(std::nullopt);
    }
    return;
  }
  const auto regions = network->connected_volumes(bridge_.state().valves);
  // Volumes and the open valves joining them take the region colour; pipes
  // inherit it from whichever element they touch.
  std::map<std::string, QColor> colors;
  std::size_t shared = 0;
  for (const auto& region : regions) {
    if (region.volumes.size() < 2) {
      continue;
    }
    const QColor color(kRegionPalette[shared++ % kRegionPalette.size()]);
    for (const auto& volume : region.volumes) {
      colors[volume] = color;
    }
    for (const auto& valve : region.valves) {
      colors[valve] = color;
    }
  }
  // Open valves joining a shared region wear its colour when asked to; every
  // other valve keeps its state colour.
  for (auto& [name, item] : valves_) {
    auto it = colors.find(name);
    const bool inherit = open_valve_color_ == canvas::OpenValveColor::Inherit && it != colors.end();
    item->set_inherited_color(inherit ? std::optional<QColor>(it->second) : std::nullopt);
  }
  for (auto& [name, item] : stages_) {
    auto it = colors.find(name);
    item->set_region_color(it == colors.end() ? isolated_color() : it->second);
  }
  for (ConnectionItem* pipe : pipes_) {
    QColor color = ConnectionItem::default_color();
    for (const auto& endpoint : pipe->endpoints()) {
      if (auto it = colors.find(endpoint); it != colors.end()) {
        color = it->second.darker(120);  // a shade deeper than the volume fill so pipes read as pipes
        break;
      }
    }
    pipe->set_region_color(color);
  }
}

void CanvasView::on_pressure(const PressureSample& sample) {
  GaugeLabelItem* item = gauge(sample.gauge);
  if (!item) {
    return;
  }
  item->set_value(sample.value, sample.units);
  if (!item->in_alarm()) {
    return;
  }
  // Clear the alarm colour once a reading is back inside the configured limits.
  for (const auto& g : bridge_.config().gauges) {
    if (g.name == sample.gauge) {
      const bool high = g.alarm_high && sample.value > *g.alarm_high;
      const bool low = g.alarm_low && sample.value < *g.alarm_low;
      item->set_alarm(high || low);
    }
  }
}

void CanvasView::on_alarm(const Alarm& alarm) {
  if (GaugeLabelItem* item = gauge(alarm.source)) {
    item->set_alarm(true);
  }
}

void CanvasView::on_finished(const QString& name, const Result<void>& result) {
  ValveItem* item = valve(name.toStdString());
  if (!item) {
    return;
  }
  item->set_pending(bridge_.pending(item->name()));
  if (!result) {
    item->flash(QString::fromStdString(result.error().what));
  }
}

}  // namespace pychron::ui
