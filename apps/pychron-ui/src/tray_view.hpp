#pragma once

// TrayView (laser window design, section 6): a sample tray drawn to scale in
// its own frame, x to the right and y up, as the tray map gives it. With the
// tray's calibration the stage is drawn where it is over the tray; the holes
// the calibration was taken at are ringed, the holes a camera has found are
// dotted, and the hole the stage was last sent to is filled.
//
// It only draws and says what was clicked: going there is the window's.

#include <optional>
#include <vector>

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "pychron/laser/calibration.hpp"
#include "pychron/laser/tray_map.hpp"

namespace pychron::ui {

class TrayView : public QWidget {
  Q_OBJECT

 public:
  explicit TrayView(QWidget* parent = nullptr);

  // The map is copied; null: no tray.
  void set_tray(const laser::TrayMap* map);
  QString tray() const { return name_; }
  // The tray's place on the stage; without it the stage is not drawn.
  void set_transform(std::optional<laser::Transform> transform);
  void set_stage(std::optional<laser::StageXY> stage);
  void set_calibration_holes(const QStringList& holes);
  void set_corrected_holes(const QStringList& holes);
  void set_current_hole(const QString& hole);

  // Widget pixels. A null point for a hole the tray lacks.
  QPointF hole_center(const QString& id) const;
  double hole_radius(const QString& id) const;  // 0 for a hole the tray lacks
  // The hole under a widget point; empty between holes.
  QString hole_at(const QPointF& point) const;
  // Where the stage is drawn; nullopt when it is not.
  std::optional<QPointF> stage_point() const;

  QSize sizeHint() const override { return {420, 420}; }
  QSize minimumSizeHint() const override { return {200, 200}; }

 signals:
  void holeClicked(const QString& id);
  void holeMenu(const QString& id, const QPoint& global);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;

 private:
  struct Fit {
    double scale = 1;  // pixels per mm
    QPointF origin;    // widget pixel of the tray frame's (0, 0)
  };
  Fit fit() const;
  QPointF to_widget(double x, double y) const;
  const laser::Hole* find(const QString& id) const;

  QString name_;
  laser::HoleShape shape_ = laser::HoleShape::Circle;
  std::vector<laser::Hole> holes_;
  std::optional<laser::Transform> transform_;
  std::optional<laser::StageXY> stage_;
  QStringList calibration_;
  QStringList corrected_;
  QString current_;
};

}  // namespace pychron::ui
