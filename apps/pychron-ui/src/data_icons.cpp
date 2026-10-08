#include "data_icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include "theme.hpp"

namespace pychron::ui {

namespace {

void axes(QPainter& p) {
  p.drawLine(QPointF(2.5, 2), QPointF(2.5, 15.5));
  p.drawLine(QPointF(2.5, 15.5), QPointF(16.5, 15.5));
}

void dots(QPainter& p, std::initializer_list<QPointF> points, double radius = 1.2) {
  p.save();
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  for (const QPointF& point : points) p.drawEllipse(point, radius, radius);
  p.restore();
}

}  // namespace

QIcon data_icon(const QString& name) {
  // Line glyphs on an 18 pt square, drawn at 2x; a mask (template) icon.
  constexpr int kPoints = 18;
  constexpr int kScale = 2;
  QPixmap pixmap(kPoints * kScale, kPoints * kScale);
  pixmap.setDevicePixelRatio(kScale);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor ink = theme().text;
  const QPen line(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  const QPen thin(ink, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(line);
  p.setBrush(Qt::NoBrush);

  if (name == QLatin1String("time_series")) {
    // values through time, each with its error bar
    axes(p);
    p.setPen(thin);
    const std::initializer_list<QPointF> points{QPointF(5.5, 9), QPointF(8.5, 6), QPointF(11.5, 10.5), QPointF(14.5, 7)};
    for (const QPointF& at : points) p.drawLine(at - QPointF(0, 2.8), at + QPointF(0, 2.8));
    dots(p, points);
  } else if (name == QLatin1String("ideogram")) {
    // a probability density over the age axis
    QPainterPath bell(QPointF(1.5, 14.5));
    bell.cubicTo(QPointF(6.5, 14.5), QPointF(6.5, 3), QPointF(9, 3));
    bell.cubicTo(QPointF(11.5, 3), QPointF(11.5, 14.5), QPointF(16.5, 14.5));
    p.drawPath(bell);
    p.drawLine(QPointF(1.5, 16.2), QPointF(16.5, 16.2));
  } else if (name == QLatin1String("spectrum")) {
    // steps of released 39Ar, each a box as tall as its error, on a plateau
    axes(p);
    p.setPen(thin);
    for (const QRectF& step : {QRectF(3.5, 9.5, 2.5, 3.5), QRectF(6, 5.5, 3.5, 2.6), QRectF(9.5, 5, 4, 2.6),
                               QRectF(13.5, 3, 2.5, 3.4)}) {
      p.drawRect(step);
    }
  } else if (name == QLatin1String("inverse_isochron")) {
    // points on a line falling from the trapped to the radiogenic intercept
    axes(p);
    p.setPen(thin);
    p.drawLine(QPointF(2.5, 4), QPointF(15.5, 15.5));
    dots(p, {QPointF(5.4, 6.6), QPointF(8.6, 9.4), QPointF(12, 12.4)});
  } else if (name == QLatin1String("spectrum_isochron")) {
    // the two side by side: steps on the left, the falling line on the right
    p.setPen(thin);
    for (const QRectF& step : {QRectF(1.5, 9, 2, 3), QRectF(3.5, 6, 2.5, 2.4), QRectF(6, 5, 2, 2.4)}) p.drawRect(step);
    p.drawLine(QPointF(1.5, 15.5), QPointF(8, 15.5));
    p.setPen(line);
    p.drawLine(QPointF(10.5, 3), QPointF(10.5, 15.5));
    p.drawLine(QPointF(10.5, 15.5), QPointF(16.5, 15.5));
    p.setPen(thin);
    p.drawLine(QPointF(10.5, 5), QPointF(16, 14.5));
    dots(p, {QPointF(12.2, 8), QPointF(14.2, 11.4)}, 1.0);
  } else if (name == QLatin1String("isotope_evolution_fit")) {
    // a signal decaying through its points, fitted back to time zero
    axes(p);
    QPainterPath fit(QPointF(2.5, 4));
    fit.quadTo(QPointF(8, 11.5), QPointF(16, 12.2));
    p.setPen(thin);
    p.drawPath(fit);
    dots(p, {QPointF(6.2, 7.6), QPointF(9.6, 10.4), QPointF(13.4, 11.6)});
    p.setPen(line);
    p.setBrush(Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawEllipse(QPointF(2.5, 4), 1.7, 1.7);  // the intercept
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  } else if (name == QLatin1String("blank_fit")) {
    // references scattered about the level fitted through them
    axes(p);
    p.setPen(QPen(ink, 1.0, Qt::DashLine, Qt::FlatCap));
    p.drawLine(QPointF(4, 10.5), QPointF(16.5, 10.5));
    dots(p, {QPointF(5.5, 11.8), QPointF(8.5, 9.2), QPointF(11.5, 11.4), QPointF(14.5, 9.6)});
  } else if (name == QLatin1String("icfactor_fit")) {
    // two detectors reading the same beam differently
    p.drawRoundedRect(QRectF(2.5, 4, 4.5, 11.5), 1.2, 1.2);
    p.drawRoundedRect(QRectF(11, 8, 4.5, 7.5), 1.2, 1.2);
    p.setPen(thin);
    p.drawLine(QPointF(8.2, 3.2), QPointF(9.8, 6.2));
    p.drawLine(QPointF(9.8, 3.2), QPointF(8.2, 6.2));
  } else if (name == QLatin1String("recall")) {
    // one analysis, opened: a sheet and its lines
    p.drawRoundedRect(QRectF(3.5, 2, 11, 14), 1.5, 1.5);
    p.setPen(thin);
    for (const double y : {5.8, 9.0, 12.2}) p.drawLine(QPointF(6, y), QPointF(12, y));
  } else if (name == QLatin1String("export")) {
    // an arrow up out of a tray
    QPainterPath tray(QPointF(3, 10.5));
    tray.lineTo(3, 15.5);
    tray.lineTo(15, 15.5);
    tray.lineTo(15, 10.5);
    p.drawPath(tray);
    p.drawLine(QPointF(9, 11.5), QPointF(9, 2.5));
    QPainterPath head(QPointF(5.6, 5.8));
    head.lineTo(9, 2.5);
    head.lineTo(12.4, 5.8);
    p.drawPath(head);
  } else {
    return {};
  }
  p.end();
  QIcon icon(pixmap);
  icon.setIsMask(true);
  return icon;
}

}  // namespace pychron::ui
