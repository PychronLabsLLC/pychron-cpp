#include "level_sheet_pdf.hpp"

#include <set>

#include <QDateTime>
#include <QFile>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

#include "holder_view.hpp"

namespace pychron::ui {

namespace ps = persistence;

namespace {

QString local(const ps::UtcTime& t) {
  const QDateTime dt = QDateTime::fromString(QString::fromStdString(t.iso()), Qt::ISODateWithMs);
  return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// Writes rows of cells in columns of the given widths, starting at `y`; a new
// page when the next row would not fit. Returns the y after the last row.
class Table {
 public:
  Table(QPdfWriter& writer, QPainter& p, std::vector<double> widths, std::vector<QString> header, int* pages)
      : writer_(writer), p_(p), widths_(std::move(widths)), header_(std::move(header)), pages_(pages) {}

  double row_height() const { return p_.fontMetrics().height() * 1.5; }

  double start(double y) {
    y_ = y;
    put(header_, true);
    return y_;
  }

  void put(const std::vector<QString>& cells, bool bold = false) {
    const double bottom = writer_.height() - row_height();
    if (y_ + row_height() > bottom) {
      writer_.newPage();
      ++*pages_;
      y_ = 0;
      if (!bold) put(header_, true);
    }
    QFont f = p_.font();
    f.setBold(bold);
    p_.setFont(f);
    double x = 0;
    for (std::size_t i = 0; i < cells.size() && i < widths_.size(); ++i) {
      const double w = widths_[i] * writer_.width();
      p_.drawText(QRectF(x, y_, w, row_height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                  cells[i]);
      x += w;
    }
    p_.drawLine(QPointF(0, y_ + row_height()), QPointF(writer_.width(), y_ + row_height()));
    y_ += row_height();
    f.setBold(false);
    p_.setFont(f);
  }

  double y() const { return y_; }

 private:
  QPdfWriter& writer_;
  QPainter& p_;
  std::vector<double> widths_;
  std::vector<QString> header_;
  int* pages_;
  double y_ = 0;
};

}  // namespace

Result<int> write_package_pdf(const QString& path, const PackageSheets& s) {
  {
    QFile probe(path);
    if (!probe.open(QIODevice::WriteOnly)) return fail(ErrorKind::Io, "cannot write " + path.toStdString());
  }
  QPdfWriter writer(path);
  writer.setPageSize(QPageSize(QPageSize::Letter));
  writer.setResolution(150);
  writer.setTitle(QString::fromStdString(s.package.name));
  writer.setCreator(QStringLiteral("pychron-ui"));
  QPainter p(&writer);
  if (!p.isActive()) return fail(ErrorKind::Io, "cannot write " + path.toStdString());
  QFont font = p.font();
  font.setPointSizeF(9);
  p.setFont(font);
  int pages = 1;
  const double line = p.fontMetrics().height() * 1.4;

  // Summary.
  QFont title = font;
  title.setPointSizeF(16);
  title.setBold(true);
  p.setFont(title);
  p.drawText(QPointF(0, line * 1.5), QStringLiteral("%1 (%2)").arg(QString::fromStdString(s.package.name),
                                                                    QString::fromStdString(s.package.kind)));
  p.setFont(font);
  double y = line * 3;
  if (s.chronology && !s.chronology->doses.empty()) {
    double hours = 0;
    for (const auto& d : s.chronology->doses) hours += static_cast<double>(d.end.micros - d.start.micros) / 3.6e9;
    p.drawText(QPointF(0, y), QObject::tr("Chronology: %1 doses, %2 h").arg(s.chronology->doses.size()).arg(hours, 0, 'f', 2));
    y += line * 0.5;
    Table doses(writer, p, {0.1, 0.3, 0.3}, {QObject::tr("Power"), QObject::tr("Start"), QObject::tr("End")}, &pages);
    doses.start(y);
    for (const auto& d : s.chronology->doses) doses.put({QString::number(d.power), local(d.start), local(d.end)});
    y = doses.y() + line;
  }
  Table levels(writer, p, {0.1, 0.25, 0.15, 0.5}, {QObject::tr("Level"), QObject::tr("Holder"), QObject::tr("Positions"), QObject::tr("Projects")}, &pages);
  levels.start(y);
  for (const auto& l : s.levels) {
    std::set<std::string> projects;
    int filled = 0;
    for (const auto& r : l.positions)
      if (r.sample) {
        ++filled;
        projects.insert(r.project);
      }
    QStringList names;
    for (const auto& n : projects) names << QString::fromStdString(n);
    levels.put({QString::fromStdString(l.level.name), QString::fromStdString(l.level.holder_name.value_or("")),
                QString::number(filled), names.join(QStringLiteral(", "))});
  }

  // One page (or more) per level.
  for (const auto& l : s.levels) {
    writer.newPage();
    ++pages;
    p.setFont(title);
    p.drawText(QPointF(0, line * 1.5), QStringLiteral("%1 %2").arg(QString::fromStdString(s.package.name),
                                                                   QString::fromStdString(l.level.name)));
    p.setFont(font);
    double top = line * 2.5;
    std::map<int, std::string> fill;
    for (const auto& r : l.positions)
      if (r.sample) fill[r.position] = r.project;
    std::optional<ps::HolderValue> holder;
    if (l.level.holder)
      if (auto it = s.holders.find(*l.level.holder); it != s.holders.end()) holder = it->second;
    int holes = 0;
    if (holder) {
      const double size = writer.width() * 0.45;
      HolderView::paint_holder(p, QRectF((writer.width() - size) / 2, top, size, size), *holder, fill, {});
      top += size + line;
      holes = static_cast<int>(holder->holes.size());
    }
    Table table(writer, p, {0.04, 0.06, 0.12, 0.18, 0.15, 0.18, 0.12, 0.15},
                {QString(), QObject::tr("Pos."), QObject::tr("Identifier"), QObject::tr("Sample"),
                 QObject::tr("Material"), QObject::tr("Project"), QObject::tr("PI"), QObject::tr("Note")},
                &pages);
    table.start(top);
    std::map<int, const ps::PositionRow*> by_position;
    for (const auto& r : l.positions) by_position[r.position] = &r;
    int last = holes;
    if (!by_position.empty()) last = std::max(last, by_position.rbegin()->first);
    for (int position = 1; position <= last; ++position) {
      const auto it = by_position.find(position);
      const ps::PositionRow* r = it == by_position.end() ? nullptr : it->second;
      table.put({QStringLiteral("☐"), QString::number(position),
                 r && r->identifier ? QString::fromStdString(*r->identifier) : QString(),
                 r ? QString::fromStdString(r->sample_name) : QString(),
                 r ? QString::fromStdString(r->material) : QString(), r ? QString::fromStdString(r->project) : QString(),
                 r ? QString::fromStdString(r->principal_investigator) : QString(),
                 r && r->note ? QString::fromStdString(*r->note) : QString()});
    }
  }
  p.end();
  return pages;
}

}  // namespace pychron::ui
