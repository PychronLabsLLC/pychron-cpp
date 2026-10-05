// The theme: the application style sheet colours what the helpers mark, the
// palette is light whatever the desktop says, and no widget source names a
// colour by value.

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
