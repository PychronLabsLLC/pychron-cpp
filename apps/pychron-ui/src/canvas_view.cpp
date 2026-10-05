#include "canvas_view.hpp"
#include "theme.hpp"

#include <QDateTime>
#include <QLocale>

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

QColor CanvasView::isolated_color() { return theme().neutral_fill; }

QColor CanvasView::source_color(canvas::SourceKind kind, int ordinal) {
  const auto& c = theme().sources;
  switch (kind) {
    case canvas::SourceKind::Pump: return c.pump;
    case canvas::SourceKind::Pipette: return c.pipette;
    case canvas::SourceKind::Laser: return c.laser;
    case canvas::SourceKind::Tank: return c.tanks[static_cast<std::size_t>(ordinal < 0 ? 0 : ordinal) % c.tanks.size()];
    case canvas::SourceKind::Spectrometer: return c.spectrometer;
    case canvas::SourceKind::Getter: return c.getter;
    case canvas::SourceKind::None: break;
  }
  return isolated_color();
}

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
  connect(&bridge_, &CoreBridge::actuationFailed, this, [this](const ActuationFailed&) { apply_state(); });
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
  sources_ = canvas::sources(c);
  const QPointF origin = to_qpoint(c.canvas.origin);
  scene_.setSceneRect(origin.x(), origin.y(), c.canvas.size.width, c.canvas.size.height);

  for (const auto& v : c.valves) {
    auto* item = new ValveItem(v.name, v.kind);
    item->setPos(to_qpoint(v.pos));
    item->set_label(QString::fromStdString(v.label()));
    item->set_on_click([this](const std::string& name) { on_click(name); });
    item->set_on_lock_request([this](const std::string& name, bool locked) { request_lock(name, locked); });
    item->set_on_hover([this, item, name = v.name] {
      item->set_details(valve_details(bridge_.state(), name, QDateTime::currentDateTime()));
    });
    scene_.addItem(item);
    valves_[v.name] = item;
    positions_[v.name] = item->pos();
  }
  const QColor stage_color = color_or(c, "stage", isolated_color());
  for (const auto& s : c.stages) {
    const std::string& label = s.display_name ? *s.display_name : s.name;
    auto* item = new StageItem(s.name, QString::fromStdString(label), s.size, stage_color);
    item->set_symbol(s.symbol);
    item->setPos(to_qpoint(s.pos));
    scene_.addItem(item);
    stages_[s.name] = item;
    positions_[s.name] = item->pos();
    boxes_[s.name] = QRectF(item->pos().x() - s.size.width / 2, item->pos().y() - s.size.height / 2, s.size.width,
                            s.size.height);
  }
  const QColor pipette_color = color_or(c, "pipette", isolated_color());
  for (const auto& p : c.pipettes) {
    const std::string& label = p.display_name ? *p.display_name : p.vlabel.empty() ? p.name : p.vlabel;
    auto* item = new StageItem(p.name, QString::fromStdString(label), p.size, pipette_color);
    item->setPos(to_qpoint(p.pos));
    scene_.addItem(item);
    stages_[p.name] = item;
    positions_[p.name] = item->pos();
    boxes_[p.name] = QRectF(item->pos().x() - p.size.width / 2, item->pos().y() - p.size.height / 2, p.size.width,
                            p.size.height);
  }
  for (const auto& g : c.gauges) {
    auto* item = new GaugeLabelItem(g.name);
    const auto& defined = bridge_.config().gauges;
    item->set_wired(std::any_of(defined.begin(), defined.end(), [&](const auto& d) { return d.name == g.name; }));
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
    QPointF a;
    QPointF b;
    if (!position(conn.start, a) || !position(conn.end, b)) {
      continue;
    }
    a += to_qpoint(conn.start_offset);
    b += to_qpoint(conn.end_offset);
    if (conn.orientation == canvas::Orientation::Auto) {
      add_pipe({a, b}, width, {conn.start, conn.end});
      continue;
    }
    add_pipe(oriented(a, conn.start, b, conn.end, conn.orientation == canvas::Orientation::Vertical), width,
             {conn.start, conn.end});
  }
  for (const auto& e : c.elbows) {
    QPointF a;
    QPointF b;
    if (!position(e.start, a) || !position(e.end, b)) {
      continue;
    }
    a += to_qpoint(e.start_offset);
    b += to_qpoint(e.end_offset);
    if (a.x() == b.x() || a.y() == b.y()) {
      add_pipe({a, b}, width, {e.start, e.end});  // lined up: nothing to turn
      continue;
    }
    const bool left = e.corner == canvas::Corner::UpperLeft || e.corner == canvas::Corner::LowerLeft;
    const bool upper = e.corner == canvas::Corner::UpperLeft || e.corner == canvas::Corner::UpperRight;
    QPointF corner(left ? std::min(a.x(), b.x()) : std::max(a.x(), b.x()),
                   upper ? std::min(a.y(), b.y()) : std::max(a.y(), b.y()));
    // The ends sit on two opposite corners of their bounding box and the
    // elbow turns at one of the other two. Asked for a corner an end is on,
    // turn as legacy pychron does: straight up or down from the start.
    if (corner == a || corner == b) {
      corner = QPointF(a.x(), b.y());
    }
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

std::vector<QPointF> CanvasView::oriented(QPointF a, const std::string& a_name, QPointF b, const std::string& b_name,
                                          bool vertical) const {
  auto box = [this](const std::string& name) -> const QRectF* {
    auto it = boxes_.find(name);
    return it == boxes_.end() ? nullptr : &it->second;
  };
  if (vertical) {
    if (const QRectF* r = box(b_name); r && a.x() >= r->left() && a.x() <= r->right()) return {a, QPointF(a.x(), b.y())};
    if (const QRectF* r = box(a_name); r && b.x() >= r->left() && b.x() <= r->right()) return {QPointF(b.x(), a.y()), b};
    if (a.x() == b.x()) return {a, b};
    return {a, QPointF(a.x(), b.y()), b};
  }
  if (const QRectF* r = box(b_name); r && a.y() >= r->top() && a.y() <= r->bottom()) return {a, QPointF(b.x(), a.y())};
  if (const QRectF* r = box(a_name); r && b.y() >= r->top() && b.y() <= r->bottom()) return {QPointF(a.x(), b.y()), b};
  if (a.y() == b.y()) return {a, b};
  return {a, QPointF(b.x(), a.y()), b};
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
  scene_.addItem(item->outline());
  // Break the border of a volume the pipe runs into: the first point belongs
  // to the first endpoint, the last to the second. Only volumes: valves keep
  // a whole border, as in legacy pychron.
  const auto& names = item->endpoints();
  if (points.size() >= 2 && names.size() >= 2) {
    const std::vector<QPointF> reversed(points.rbegin(), points.rend());
    const std::array<std::pair<const std::vector<QPointF>*, const std::string*>, 2> ends{
        {{&reversed, &names[0]}, {&points, &names[1]}}};
    for (const auto& [run, name] : ends) {
      auto box = boxes_.find(*name);
      if (box == boxes_.end()) continue;
      // Positions are whole pixels, so an end meant for the edge can stop a
      // fraction short of it: look for the crossing a border's width out,
      // then step back in to the edge.
      const double reach = ConnectionItem::kBorderWidth;
      if (auto entry = box_entry(*run, box->second.adjusted(-reach, -reach, reach, reach))) {
        entry->edge += entry->inward * reach;
        scene_.addItem(item->add_gap(*entry, StageItem::border_inset(box->second, *entry, width)));
      }
    }
  }
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

namespace {

// "40 s", "13 min", "2 h 13 min", "17 d 4 h": two units at most.
QString span(std::chrono::seconds d) {
  const qint64 s = std::max<qint64>(0, d.count());
  if (s < 60) return CanvasView::tr("%1 s").arg(s);
  if (s < 3600) return CanvasView::tr("%1 min").arg(s / 60);
  if (s < 86400) return CanvasView::tr("%1 h %2 min").arg(s / 3600).arg(s % 3600 / 60);
  return CanvasView::tr("%1 d %2 h").arg(s / 86400).arg(s % 86400 / 3600);
}

// The time alone today, with the date before it otherwise.
QString moment(systems::WallTime t, const QDateTime& now) {
  const QDateTime at = QDateTime::fromSecsSinceEpoch(t.time_since_epoch().count());
  return QLocale().toString(at, at.date() == now.date() ? QStringLiteral("HH:mm:ss") : QStringLiteral("d MMM yyyy HH:mm"));
}

}  // namespace

// The lines of a valve's tooltip under its name: what it is, the state it is
// in and since when, and its history (kept between runs).
QStringList CanvasView::valve_details(const CoreBridge::State& state, const std::string& name, const QDateTime& now) {
  QStringList lines;
  const auto info = state.switches.find(name);
  if (info != state.switches.end() && !info->second.description.empty())
    lines += QString::fromStdString(info->second.description);

  const auto at = state.valves.find(name);
  const ValveState valve = at == state.valves.end() ? ValveState::Unknown : at->second;
  QString line;
  switch (valve) {
    case ValveState::Open: line = tr("Open"); break;
    case ValveState::Closed: line = tr("Closed"); break;
    default: line = tr("State unknown"); break;
  }
  if (info == state.switches.end()) return lines << line;
  const systems::SwitchStats& h = info->second.stats;
  const systems::WallTime wall{std::chrono::seconds(now.toSecsSinceEpoch())};
  if (h.since && valve != ValveState::Unknown)
    line += tr(" since %1 (%2)").arg(moment(*h.since, now), span(wall - *h.since));
  if (info->second.locked) line += tr(", locked");
  if (!info->second.owner.empty()) line += tr(", owned by %1").arg(QString::fromStdString(info->second.owner));
  lines += line;

  if (h.opens == 0 && h.closes == 0 && h.failures == 0) return lines << tr("Never actuated");
  if (h.last_actuation) lines += tr("Last actuated %1").arg(moment(*h.last_actuation, now));
  lines += tr("Opened %L1, closed %L2, failed %L3").arg(h.opens).arg(h.closes).arg(h.failures);
  // A spell still running counts up to now.
  std::chrono::seconds open = h.open_time;
  if (valve == ValveState::Open && h.since && wall > *h.since) open += wall - *h.since;
  lines += tr("Time open %1").arg(span(open));
  return lines;
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
    item->set_details(valve_details(state, name, QDateTime::currentDateTime()));
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
  // A region takes the colour of the source connected to it with the highest
  // precedence (legacy pychron's rule): a pump over a tank, over a pipette or
  // a laser, over a spectrometer, over a getter. Each tank has a colour of
  // its own, so what is open to a tank shows whose gas it is. It depends on
  // nothing but who is connected, so a region never changes colour because
  // another one did. A region with no source stays neutral; a source alone
  // wears its own colour. Open valves and pipes take the colour of what they
  // join.
  std::map<std::string, QColor> colors;
  for (const auto& region : regions) {
    const canvas::Source* source = canvas::dominant(sources_, region.volumes);
    if (source == nullptr) continue;
    // A pipette holds its tank's gas: it is drawn as the tank is, unless
    // the file gives it a colour of its own.
    const canvas::Source* shown = source;
    if (!source->color && !source->tank.empty()) {
      if (auto tank = sources_.find(source->tank); tank != sources_.end()) shown = &tank->second;
    }
    QColor color = shown->color ? QColor(QString::fromStdString(*shown->color))
                                : source_color(shown->kind, shown->ordinal);
    if (!color.isValid()) color = source_color(shown->kind, shown->ordinal);
    for (const auto& volume : region.volumes) colors[volume] = color;
    for (const auto& valve : region.valves) colors[valve] = color;
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
        color = it->second;  // the volume's own fill: through the gap in its border they read as one
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
