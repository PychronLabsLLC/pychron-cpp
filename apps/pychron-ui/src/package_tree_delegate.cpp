#include "package_tree_delegate.hpp"

#include <algorithm>

#include <QPainter>
#include <QTreeView>

#include "theme.hpp"

namespace pychron::ui {

namespace {

constexpr int kRowHeight = 28;
constexpr int kMargin = 4;    // between the highlight and the edge of the view
constexpr int kChevron = 22;  // the width kept for a package's chevron
constexpr int kIndent = 14;   // a level under its package
constexpr int kGap = 8;

QFont smaller(QFont font) {
  font.setPointSizeF(font.pointSizeF() * 0.86);
  return font;
}

}  // namespace

void PackageTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
  const Theme& t = theme();
  const bool level = index.data(package_tree::IsLevelRole).toBool();
  const bool selected = option.state.testFlag(QStyle::State_Selected);
  const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
  const QRect row = option.rect.adjusted(kMargin, 1, -kMargin, -1);

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  if (selected || hovered) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(selected ? t.accent_soft : t.accent_wash);
    painter->drawRoundedRect(row, 6, 6);
  }

  if (!level && index.model()->hasChildren(index)) {
    const auto* view = qobject_cast<const QTreeView*>(option.widget);
    const bool open = view != nullptr && view->isExpanded(index);
    const QPointF c(row.left() + kChevron / 2.0 + 1, row.center().y() + 0.5);
    painter->setPen(QPen(selected ? t.accent_strong : t.muted_text, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPolyline(open ? QPolygonF({c + QPointF(-4, -2), c + QPointF(0, 2), c + QPointF(4, -2)})
                               : QPolygonF({c + QPointF(-2, -4), c + QPointF(2, 0), c + QPointF(-2, 4)}));
  }

  int left = row.left() + kChevron + (level ? kIndent : 0);
  int right = row.right() - kGap;

  // The right side first: the name gives way to it.
  const QString detail = index.data(package_tree::DetailRole).toString();
  if (!detail.isEmpty()) {
    const QFont font = smaller(option.font);
    const QFontMetrics fm(font);
    const QString text = fm.elidedText(detail, Qt::ElideRight, row.width() / 2);
    const int width = fm.horizontalAdvance(text);
    painter->setFont(font);
    if (level) {
      painter->setPen(selected ? t.accent_strong : t.muted_text);
      painter->drawText(QRect(right - width, row.top(), width, row.height()), Qt::AlignVCenter | Qt::AlignRight, text);
      right -= width + kGap;
    } else {
      const QRect pill(right - width - 12, row.center().y() - fm.height() / 2 - 1, width + 12, fm.height() + 3);
      painter->setPen(Qt::NoPen);
      painter->setBrush(selected ? t.base : t.border);
      painter->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
      painter->setPen(selected ? t.accent_strong : t.muted_text);
      painter->drawText(pill, Qt::AlignCenter, text);
      right = pill.left() - kGap;
    }
  }

  const QString tag = index.data(package_tree::TagRole).toString();
  const bool warning = index.data(package_tree::WarningRole).toBool();
  const QFont tag_font = smaller(option.font);
  const int tag_width = tag.isEmpty() ? 0 : QFontMetrics(tag_font).horizontalAdvance(tag) + kGap;
  const int dot_width = warning ? 6 + kGap : 0;

  QFont name_font = option.font;
  if (!level) name_font.setWeight(QFont::DemiBold);
  const QFontMetrics name_fm(name_font);
  const QString name =
      name_fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, std::max(0, right - left - tag_width - dot_width));
  painter->setFont(name_font);
  painter->setPen(selected ? t.accent_strong : t.text);
  painter->drawText(QRect(left, row.top(), right - left, row.height()), Qt::AlignVCenter | Qt::AlignLeft, name);
  left += name_fm.horizontalAdvance(name) + kGap;

  if (!tag.isEmpty() && left + tag_width - kGap <= right) {
    painter->setFont(tag_font);
    painter->setPen(selected ? t.accent_strong : t.faint_text);
    painter->drawText(QRect(left, row.top(), right - left, row.height()), Qt::AlignVCenter | Qt::AlignLeft, tag);
    left += tag_width;
  }
  if (warning && left + 6 <= right) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(t.warning);
    painter->drawEllipse(QRectF(left, row.center().y() - 2.5, 6, 6));
  }
  painter->restore();
}

QSize PackageTreeDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
  const QSize base = QStyledItemDelegate::sizeHint(option, index);
  return {base.width() + kChevron + kIndent + 2 * kMargin, std::max(kRowHeight, option.fontMetrics.height() + 10)};
}

}  // namespace pychron::ui
