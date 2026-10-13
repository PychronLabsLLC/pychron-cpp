#include "flow_layout.hpp"

#include <algorithm>

#include <QWidget>

namespace pychron::ui {

FlowLayout::FlowLayout(QWidget* parent, int h_spacing, int v_spacing)
    : QLayout(parent), h_spacing_(h_spacing), v_spacing_(v_spacing) {
  setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
  // Named in full: a destructor does not dispatch to a derived class.
  while (QLayoutItem* item = FlowLayout::takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

int FlowLayout::count() const { return static_cast<int>(items_.size()); }

QLayoutItem* FlowLayout::itemAt(int index) const { return items_.value(index); }

QLayoutItem* FlowLayout::takeAt(int index) {
  return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

int FlowLayout::heightForWidth(int width) const { return arrange(QRect(0, 0, width, 0), true); }

void FlowLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  arrange(rect, false);
}

// Asks for no more than it needs: the width it is given decides the rows.
QSize FlowLayout::sizeHint() const { return minimumSize(); }

// No narrower than its widest item.
QSize FlowLayout::minimumSize() const {
  QSize size;
  for (const QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
  const QMargins m = contentsMargins();
  return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::arrange(const QRect& rect, bool dry) const {
  const QMargins m = contentsMargins();
  const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
  int x = area.x(), y = area.y(), row_height = 0;
  for (QLayoutItem* item : items_) {
    if (item->widget() && item->widget()->isHidden()) continue;
    const QSize s = item->sizeHint();
    if (x > area.x() && x + s.width() > area.right() + 1) {
      x = area.x();
      y += row_height + v_spacing_;
      row_height = 0;
    }
    if (!dry) item->setGeometry(QRect(QPoint(x, y), s));
    x += s.width() + h_spacing_;
    row_height = std::max(row_height, s.height());
  }
  return y + row_height - rect.y() + m.bottom();
}

}  // namespace pychron::ui
