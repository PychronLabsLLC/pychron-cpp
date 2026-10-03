// Brand: the splash shows load progress and a simulation badge, the About
// dialog says what is running, and Help > About opens it from the main window.

#include <QtTest/QtTest>

#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QPushButton>
#include <QTemporaryDir>

#include "brand.hpp"
#include "data_main_window.hpp"
#include "main_window.hpp"
#include "theme.hpp"
#include "ui_fixture.hpp"

using pychron::ui::AboutDialog;
using pychron::ui::SplashScreen;
using pychron::ui::theme;
namespace brand = pychron::ui::brand;
namespace style = pychron::ui::style;

namespace {

// How many pixels of `image` are within a few steps of `color`.
int count_near(const QImage& image, const QColor& color) {
  int n = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      if (std::abs(c.red() - color.red()) < 8 && std::abs(c.green() - color.green()) < 8 &&
          std::abs(c.blue() - color.blue()) < 8)
        ++n;
    }
  }
  return n;
}

}  // namespace

class TestBrand : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() { style::apply(*qobject_cast<QApplication*>(QCoreApplication::instance())); }

  void the_version_comes_from_the_build() {
    QVERIFY(!pychron::ui::app_version().isEmpty());
    const QString info = pychron::ui::build_info();
    QVERIFY(info.startsWith(QStringLiteral("pychron: ") + pychron::ui::app_version()));
    QVERIFY(info.contains(QStringLiteral("Qt: ") + QString::fromLatin1(qVersion())));
    QVERIFY(info.contains(QStringLiteral("compiler: ")));
  }

  void the_banner_draws_the_peaks_in_the_signal_colour() {
    const QImage plain = brand::banner(QSize(600, 340), 1.0, false, false).toImage();
    QCOMPARE(plain.size(), QSize(600, 340));
    QVERIFY(count_near(plain, theme().signal) > 200);  // the trace and the rule
    QCOMPARE(count_near(plain, theme().warning), 0);
    // At twice the pixel ratio it is drawn at twice the pixels, not scaled up.
    const QPixmap sharp = brand::banner(QSize(600, 340), 2.0, false, false);
    QCOMPARE(sharp.size(), QSize(1200, 680));
    QCOMPARE(sharp.devicePixelRatio(), 2.0);
  }

  void simulation_adds_the_badge() {
    const QImage sim = brand::banner(QSize(600, 340), 1.0, true, false).toImage();
    QVERIFY(count_near(sim, theme().warning) > 100);
  }

  void the_splash_shows_each_step() {
    SplashScreen splash(true);
    QVERIFY(splash.simulation());
    splash.show();
    splash.status(QStringLiteral("Loading the extraction line: extraction_line.toml"));
    QCOMPARE(splash.status_text(), QStringLiteral("Loading the extraction line: extraction_line.toml"));
    const QImage before = splash.grab().toImage();
    splash.status(QStringLiteral("Loading the lab"));
    QCOMPARE(splash.status_text(), QStringLiteral("Loading the lab"));
    QVERIFY(splash.grab().toImage() != before);  // the new line is painted
  }

  void a_quick_load_keeps_the_splash_up_for_the_minimum() {
    QWidget window;
    window.show();
    SplashScreen splash(false);
    splash.show();
    splash.finish_after(&window, std::chrono::milliseconds(300));
    QVERIFY(splash.isVisible());  // the event loop keeps running meanwhile
    QTest::qWait(100);
    QVERIFY(splash.isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!splash.isVisible(), 3000);

    SplashScreen late(false);
    late.show();
    QTest::qWait(60);
    late.finish_after(&window, std::chrono::milliseconds(10));  // already up long enough
    QTRY_VERIFY_WITH_TIMEOUT(!late.isVisible(), 3000);
  }

  void the_icon_has_every_size() {
    const QIcon icon = brand::app_icon();
    QVERIFY(!icon.isNull());
    for (const int s : {16, 32, 256}) QCOMPARE(icon.pixmap(QSize(s, s), 1.0).size(), QSize(s, s));
  }

  void about_says_what_is_running_and_copies_the_build_info() {
    AboutDialog about;
    about.show();
    const QString text = about.text();
    QVERIFY(text.contains(QStringLiteral("GNU General Public License v3")));
    QVERIFY(text.contains(QStringLiteral("In development.")));
    QVERIFY(text.contains(pychron::ui::build_info()));
    QApplication::clipboard()->clear();
    QTest::mouseClick(about.copy_button(), Qt::LeftButton);
    if (QApplication::clipboard()->supportsSelection() || !QApplication::clipboard()->text().isEmpty())
      QCOMPARE(QApplication::clipboard()->text(), pychron::ui::build_info());
    QCOMPARE(about.copy_button()->text(), QStringLiteral("Copied"));
  }

  void help_about_opens_one_dialog() {
    auto line = pychron::ui::test::make_example_line();
    pychron::ui::MainWindow window(*line);
    QVERIFY(!window.windowIcon().isNull());
    QVERIFY(window.about_dialog() == nullptr);
    QCOMPARE(window.about_action()->menuRole(), QAction::AboutRole);
    window.about_action()->trigger();
    AboutDialog* first = window.about_dialog();
    QVERIFY(first != nullptr);
    QVERIFY(first->isVisible());
    first->close();
    window.about_action()->trigger();
    QCOMPARE(window.about_dialog(), first);  // reopened, not rebuilt
    QVERIFY(first->isVisible());
  }

  void a_data_reduction_install_has_help_about_too() {
    QTemporaryDir dir;
    pychron::processing::MemorySource source;
    pychron::processing::PresetStore presets(dir.path().toStdString());
    pychron::ui::DataMainWindow window(source, presets, QStringLiteral("reduction"));
    QVERIFY(!window.windowIcon().isNull());
    QCOMPARE(window.about_action()->menuRole(), QAction::AboutRole);
    window.about_action()->trigger();
    auto* about = window.findChild<AboutDialog*>();
    QVERIFY(about != nullptr);
    QVERIFY(about->isVisible());
  }
};

QTEST_MAIN(TestBrand)
#include "test_brand.moc"
