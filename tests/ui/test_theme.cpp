// The theme: the application style sheet colours what the helpers mark, the
// palette is light whatever the desktop says, and no widget source names a
// colour by value.

#include <QVBoxLayout>
#include <QStyleOptionButton>
#include <QStyle>
#include <QSpinBox>
#include <QListWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QtTest/QtTest>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QToolTip>

#include "health_bar.hpp"
#include "theme.hpp"

using pychron::ui::HealthBar;
using pychron::ui::theme;
namespace style = pychron::ui::style;

namespace {

QImage render(QWidget& w) {
  w.ensurePolished();
  w.resize(w.sizeHint().expandedTo(QSize(120, 30)));
  return w.grab().toImage();
}

QColor text_color(QWidget& w) {
  w.ensurePolished();
  return w.palette().color(w.foregroundRole());
}

}  // namespace

class TestTheme : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() { style::apply(*qobject_cast<QApplication*>(QCoreApplication::instance())); }

  void palette_is_the_light_theme() {
    const QPalette p = QApplication::palette();
    QCOMPARE(p.color(QPalette::Window), theme().window);
    QCOMPARE(p.color(QPalette::Base), theme().base);
    QCOMPARE(p.color(QPalette::Text), theme().text);
    QCOMPARE(p.color(QPalette::Highlight), theme().accent);
    QVERIFY(p.color(QPalette::Window).lightness() > 200);
    QVERIFY(p.color(QPalette::Text).lightness() < 80);
    QCOMPARE(p.color(QPalette::Disabled, QPalette::Text), theme().faint_text);
  }

  void tone_colours_a_label_and_changes_after_it_is_shown() {
    QLabel label(QStringLiteral("status"));
    QCOMPARE(text_color(label), theme().text);
    style::set_tone(&label, style::Tone::Error);
    QCOMPARE(text_color(label), theme().error_text);
    style::set_tone(&label, style::Tone::Muted);
    QCOMPARE(text_color(label), theme().muted_text);
    style::set_tone(&label, style::Tone::Accent);
    QCOMPARE(text_color(label), theme().accent);
    style::set_tone(&label, style::Tone::Warning);
    QCOMPARE(text_color(label), theme().warning_text);
    style::set_tone(&label, style::Tone::Normal);
    QCOMPARE(text_color(label), theme().text);
  }

  void banner_frame_and_banner_label() {
    QFrame frame;
    auto* row = new QHBoxLayout(&frame);
    auto* label = new QLabel(QStringLiteral("spectrometer lost"));
    row->addWidget(label);
    style::make_banner(&frame);
    const QImage image = render(frame);
    QCOMPARE(image.pixelColor(1, 1), theme().error_bg);
    QCOMPARE(text_color(*label), theme().on_error_bg);

    QLabel alone(QStringLiteral("row 3: no such plan"));
    style::make_banner(&alone);
    QCOMPARE(render(alone).pixelColor(1, 1), theme().error_bg);
    QCOMPARE(text_color(alone), theme().on_error_bg);
  }

  void invalid_draws_a_border_and_clears() {
    QLineEdit edit;
    style::set_invalid(&edit, true);
    QImage image = render(edit);
    QCOMPARE(image.pixelColor(0, image.height() / 2), theme().error);
    style::set_invalid(&edit, false);
    image = render(edit);
    QVERIFY(image.pixelColor(0, image.height() / 2) != theme().error);
  }

  // A ticked box is a filled square of the accent with a light tick, so a
  // column of them reads at a glance; an empty one is the page's white.
  void a_ticked_box_is_filled_with_the_accent() {
    QCheckBox box(QStringLiteral("Autoscroll"));
    render(box);
    QStyleOptionButton opt;
    opt.initFrom(&box);
    const QRect mark = box.style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, &box);
    QVERIFY(mark.width() >= 14 && mark.height() >= 14);
    const QPoint fill(mark.center().x(), mark.top() + 2);  // inside the square, clear of the tick

    QCOMPARE(render(box).pixelColor(fill), theme().base);
    box.setChecked(true);
    const QImage ticked = render(box);
    QCOMPARE(ticked.pixelColor(fill), theme().accent);
    bool tick = false;
    for (int y = mark.top() + 2; y < mark.bottom() - 1 && !tick; ++y) {
      // Scaled and smoothed onto the fill, the tick is never the pure white it is drawn in.
      for (int x = mark.left() + 2; x < mark.right() - 1 && !tick; ++x) {
        tick = ticked.pixelColor(x, y).lightness() > theme().accent.lightness() + 80;
      }
    }
    QVERIFY2(tick, "no tick drawn on the fill");

    box.setEnabled(false);
    QVERIFY(render(box).pixelColor(fill) != theme().accent);  // a box that cannot be changed does not shout
  }

  // The same mark in a list or a table: a column of Save boxes is boxes of this look too.
  void a_ticked_item_has_the_same_mark() {
    QListWidget list;
    auto* item = new QListWidgetItem(QStringLiteral("Ar40"), &list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    list.resize(160, 60);
    const auto has_accent = [&] {
      const QImage image = render(list);
      const QRect row = list.visualItemRect(item).translated(list.viewport()->pos());
      for (int y = row.top(); y <= row.bottom(); ++y) {
        for (int x = row.left(); x < row.left() + 24; ++x) {
          if (image.pixelColor(x, y) == theme().accent) return true;
        }
      }
      return false;
    };
    QVERIFY(!has_accent());
    item->setCheckState(Qt::Checked);
    QVERIFY(has_accent());
  }

  // A field is a hairline until it is the one being typed in; then a ring of
  // the accent, two pixels, which is seen across the room. Its size does not
  // change, so nothing beside it moves.
  void an_input_is_a_hairline_at_rest_and_an_accent_ring_with_the_focus() {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* edit = new QLineEdit(QStringLiteral("66001"));
    auto* other = new QLineEdit;
    layout->addWidget(edit);
    layout->addWidget(other);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    other->setFocus();
    host.activateWindow();
    QTRY_VERIFY(other->hasFocus());

    const QSize at_rest = edit->size();
    QImage image = edit->grab().toImage();
    const int mid = image.height() / 2;
    QCOMPARE(image.pixelColor(0, mid), theme().border);
    QCOMPARE(image.pixelColor(1, mid), theme().base);

    edit->setFocus();
    QTRY_VERIFY(edit->hasFocus());
    image = edit->grab().toImage();
    QCOMPARE(image.pixelColor(0, mid), theme().accent);
    QCOMPARE(image.pixelColor(1, mid), theme().accent);
    QCOMPARE(image.pixelColor(2, mid), theme().base);
    QCOMPARE(edit->size(), at_rest);
    QCOMPARE(edit->sizeHint(), other->sizeHint());
  }

  // The other fields are fields too.
  void spin_and_combo_boxes_take_the_same_ring() {
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* spin = new QSpinBox;
    auto* combo = new QComboBox;
    combo->addItem(QStringLiteral("unknown"));
    auto* other = new QLineEdit;
    for (QWidget* w : {static_cast<QWidget*>(spin), static_cast<QWidget*>(combo), static_cast<QWidget*>(other)}) {
      layout->addWidget(w);
    }
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    host.activateWindow();
    for (QWidget* w : {static_cast<QWidget*>(spin), static_cast<QWidget*>(combo)}) {
      other->setFocus();
      QTRY_VERIFY(other->hasFocus());
      const QSize at_rest = w->size();
      QCOMPARE(w->grab().toImage().pixelColor(0, w->height() / 2), theme().border);
      w->setFocus();
      QTRY_VERIFY(w->hasFocus());
      const QImage image = w->grab().toImage();
      QCOMPARE(image.pixelColor(0, w->height() / 2), theme().accent);
      QCOMPARE(image.pixelColor(1, w->height() / 2), theme().accent);
      QCOMPARE(w->size(), at_rest);
    }
  }

  void chip_background_follows_the_level() {
    QLabel chip(QStringLiteral("laser"));
    for (const auto level : {style::Level::Ok, style::Level::Warning, style::Level::Error, style::Level::Unknown}) {
      style::set_chip(&chip, level);
      const QImage image = render(chip);
      QCOMPARE(image.pixelColor(image.width() / 2, 1), style::level_color(level));
    }
    QCOMPARE(style::level_color(style::Level::Ok), theme().ok);
    QCOMPARE(style::level_color(style::Level::Warning), theme().warning);
    QCOMPARE(style::level_color(style::Level::Error), theme().error);
    QCOMPARE(style::level_color(style::Level::Unknown), theme().inactive);
  }

  void health_bar_chips_use_the_status_levels() {
    QCOMPARE(HealthBar::level_of(HealthBar::Status::Ok), style::Level::Ok);
    QCOMPARE(HealthBar::level_of(HealthBar::Status::Degraded), style::Level::Warning);
    QCOMPARE(HealthBar::level_of(HealthBar::Status::Down), style::Level::Error);
    QCOMPARE(HealthBar::level_of(HealthBar::Status::Unknown), style::Level::Unknown);

    HealthBar bar;
    bar.seed({"laser"});
    pychron::TransportHealth health;
    health.transport = "laser";
    health.connected = false;
    bar.update_health(health);
    QLabel* chip = bar.chip("laser");
    QVERIFY(chip != nullptr);
    const QImage image = render(*chip);
    QCOMPARE(image.pixelColor(image.width() / 2, 1), theme().error);
  }

  // A tooltip is a light surface with a hairline, like a menu: not the menu
  // bar's ink.
  void tooltip_is_a_light_surface_with_a_hairline() {
    QLabel host(QStringLiteral("host"));
    host.resize(200, 100);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    QToolTip::showText(host.mapToGlobal(QPoint(20, 20)), QStringLiteral("Outer Pipette 2"), &host);
    QWidget* tip = nullptr;
    for (QWidget* w : QApplication::allWidgets()) {
      if (w->inherits("QTipLabel")) tip = w;
    }
    QVERIFY(tip);
    const QImage image = tip->grab().toImage();
    QCOMPARE(image.pixelColor(0, image.height() / 2), theme().strong_border);
    QCOMPARE(image.pixelColor(3, image.height() / 2), theme().base);
    QCOMPARE(text_color(*tip), theme().text);
    QToolTip::hideText();
  }

  void tip_text_sets_the_first_of_several_lines_heavier() {
    QCOMPARE(style::tip_text(QStringLiteral("Outer Pipette 2")), QStringLiteral("Outer Pipette 2"));
    const QString tip = style::tip_text(QStringLiteral("62410-01D  excluded\nAge <Ma> 28.2\n12.4% gas"));
    QVERIFY(Qt::mightBeRichText(tip));
    QVERIFY(tip.contains(QStringLiteral("font-weight:600\">62410-01D  excluded</span>")));
    QVERIFY(tip.contains(QStringLiteral("Age &lt;Ma&gt; 28.2<br>12.4% gas")));
  }

  void fonts() {
    QFont base;
    base.setPointSizeF(10);
    const QFont title = style::title_font(base);
    QVERIFY(title.bold());
    QCOMPARE(title.pointSizeF(), 13.0);
    QCOMPARE(style::mono_font().styleHint(), QFont::Monospace);
  }

  // Only installed families are named: a missing one makes Qt build its alias
  // table on macOS ("Populating font family aliases took ... ms").
  void fonts_name_only_installed_families() {
    const QStringList installed = QFontDatabase::families();
    const auto check = [&](const QFont& f, const char* what) {
      for (const QString& family : f.families())
        QVERIFY2(installed.contains(family, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("%1 names %2, which is not installed").arg(QLatin1String(what), family)));
    };
    check(QApplication::font(), "the application font");
    check(style::mono_font(), "the code font");
  }

  // Colours are named in theme.cpp and nowhere else.
  void no_widget_source_names_a_colour_by_value() {
    const QRegularExpression literal(QStringLiteral(
        "QColor\\(\\s*(0x|[0-9])|QRgb\\s*[,>][^;]*0x|\"[^\"\\n]*#[0-9a-fA-F]{3,8}\\b|"
        "\\bQt::(black|white|red|green|blue|cyan|magenta|yellow|gray|(dark|light)[A-Z][a-z]+)\\b"));
    const QDir dir(QStringLiteral(PYCHRON_UI_SOURCE_DIR));
    const QStringList files = dir.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.hpp")}, QDir::Files);
    QVERIFY(files.size() > 20);
    QStringList found;
    for (const QString& name : files) {
      if (name == QStringLiteral("theme.cpp")) continue;
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
      int number = 0;
      while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        ++number;
        if (literal.match(line).hasMatch()) found << QStringLiteral("%1:%2: %3").arg(name).arg(number).arg(line.trimmed());
      }
    }
    QVERIFY2(found.isEmpty(), qPrintable(found.join(QLatin1Char('\n'))));
  }
};

QTEST_MAIN(TestTheme)
#include "test_theme.moc"
