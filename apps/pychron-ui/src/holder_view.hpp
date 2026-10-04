#pragma once

// A holder drawn hole by hole (entry spec, section 9.3): filled holes are
// coloured by project, selected holes outlined. A click picks a hole; with
// Shift or Ctrl it adds to the selection; a drag picks every hole in the band.

#include <map>
#include <optional>
#include <set>

#include <QWidget>

#include "entry_headers.hpp"

namespace pychron::ui {

class HolderView : public QWidget {
  Q_OBJECT

 public:
  explicit HolderView(QWidget* parent = nullptr);

  void set_holder(std::optional<persistence::HolderValue> holder);
  // Position (1-based hole) -> project name, for the fill colour; "" for empty holes.
  void set_fill(std::map<int, std::string> projects);
  void set_selected(std::set<int> positions);
  const std::set<int>& selected() const noexcept { return selected_; }
  // The hole at a widget point; 0 for none.
  int position_at(const QPointF& point) const;
  // Draws the holder into `rect` of `painter` (the PDF uses this).
  static void paint_holder(QPainter& painter, const QRectF& rect, const persistence::HolderValue& holder,
                           const std::map<int, std::string>& projects, const std::set<int>& selected);

  QSize sizeHint() const override { return {320, 320}; }

 Q_SIGNALS:
  void selection_changed(const std::set<int>& positions);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  std::optional<persistence::HolderValue> holder_;
  std::map<int, std::string> projects_;
  std::set<int> selected_;
  std::optional<QPointF> drag_from_;
  QPointF drag_to_;
};

}  // namespace pychron::ui
