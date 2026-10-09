#pragma once

// Draws the rows of the Packages window's tree as a sidebar list: a rounded
// hover and selection, a chevron on a package, and on the right the count of
// a package or the holder of a level. The tree has no indentation and no
// branch decoration of its own; the delegate draws both, so the highlight
// spans the row.

#include <QStyledItemDelegate>

namespace pychron::ui {

namespace package_tree {

// What a row of the tree carries besides its name (Qt::DisplayRole).
enum Role {
  UuidRole = Qt::UserRole,
  IsLevelRole,
  DetailRole,   // QString on the right: "analyzed/positions" of a package, the holder of a level
  TagRole,      // QString after the name: the kind, when it is not an irradiation
  WarningRole,  // bool: a dot after the name (an irradiation without a chronology)
};

}  // namespace package_tree

class PackageTreeDelegate : public QStyledItemDelegate {
  Q_OBJECT

 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

}  // namespace pychron::ui
