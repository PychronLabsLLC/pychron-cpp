#pragma once

// SceneView (data browsing and visualization design, section 8.2): draws a
// processing::Scene with QCustomPlot. One axis rect per panel, stacked top to
// bottom with linked x axes; graphs in a grid. The view computes nothing: it
// maps scene layers to QCustomPlot objects and maps clicks back to analyses.
//
//   click a point          point_clicked(uuid)
//   shift-drag a rectangle points_toggled(uuids inside)
//   hover                  the point's tooltip
//   wheel / drag           zoom / pan (x shared within a graph)
//   double-click           reset the view to the scene's limits
//   right-click            reset view, save PNG/PDF, copy image

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QList>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "pychron/processing/scene.hpp"

class QCustomPlot;
class QCPAxisRect;
class QCPItemRect;
class QRubberBand;

namespace pychron::ui {

class SceneView : public QWidget {
  Q_OBJECT

 public:
  explicit SceneView(QWidget* parent = nullptr);

  void set_scene(processing::ScenePtr scene);
  const processing::ScenePtr& scene() const noexcept { return scene_; }

  // Puts every axis back to the scene's limits (or the data's).
  void reset_view();
  bool save_png(const QString& path, int width = 0, int height = 0);
  bool save_pdf(const QString& path);

  // For tests.
  QCustomPlot* plot() const noexcept { return plot_; }
  int panel_count() const noexcept { return static_cast<int>(rects_.size()); }
  // Widget position of the first point of `uuid` in `panel` (row-major over
  // graphs then panels); nullopt when not drawn.
  std::optional<QPoint> point_position(const std::string& uuid, int panel = 0) const;
  // Analyses whose points lie inside the widget rectangle.
  QStringList points_in(const QRect& rect) const;
  QString tooltip_at(const QPoint& pos) const;
  QStringList texts(int panel) const;  // annotation lines
  // The shaded spans of `panel` as drawn, in widget pixels.
  QList<QRectF> span_rects(int panel) const;

 signals:
  void point_clicked(const QString& uuid);
  void points_toggled(const QStringList& uuids);
  void recall_requested(const QString& uuid);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct HitPoint {
    double x, y;
    std::string uuid;
    std::string tooltip;
  };
  struct HitBox {  // a spectrum step: a click anywhere inside selects it
    double x0, x1, y0, y1;
    HitPoint point;  // the box center
  };
  struct RectInfo {
    QCPAxisRect* rect = nullptr;
    std::vector<HitPoint> points;
    std::vector<HitBox> boxes;
    std::vector<QString> texts;
    std::vector<QCPItemRect*> spans;
    std::optional<double> x_min, x_max, y_min, y_max;  // scene limits
    bool log = false;
  };

  void rebuild();
  // The point drawn nearest `pos`, the topmost of several at one place. With
  // `analyses_only`, of the points that name an analysis: what a click or the
  // context menu acts on, so a mean drawn over an analysis does not hide it.
  const HitPoint* hit(const QPoint& pos, const RectInfo** where = nullptr, bool analyses_only = false) const;
  void show_context_menu(const QPoint& pos);

  processing::ScenePtr scene_;
  QCustomPlot* plot_;
  QRubberBand* band_ = nullptr;
  QPoint press_pos_;
  bool pressed_ = false;
  bool selecting_ = false;
  std::vector<RectInfo> rects_;
};

}  // namespace pychron::ui
