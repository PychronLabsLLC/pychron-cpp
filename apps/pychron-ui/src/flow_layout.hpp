#pragma once

// FlowLayout: items left to right, wrapping onto further rows as the width
// runs out, so every item is on screen at any width. Its height follows its
// width.

#include <QLayout>
#include <QList>

namespace pychron::ui {

class FlowLayout : public QLayout {
 public:
  explicit FlowLayout(QWidget* parent = nullptr, int h_spacing = 4, int v_spacing = 4);
  ~FlowLayout() override;

  void addItem(QLayoutItem* item) override;
  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override;
  void setGeometry(const QRect& rect) override;
  QSize sizeHint() const override;
  QSize minimumSize() const override;

 private:
  // Places the items in `rect` (unless `dry`); returns the height used.
  int arrange(const QRect& rect, bool dry) const;

  QList<QLayoutItem*> items_;
  int h_spacing_;
  int v_spacing_;
};

}  // namespace pychron::ui
