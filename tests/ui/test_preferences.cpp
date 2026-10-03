// File > Preferences…: the saved values (defaults for anything missing or
// invalid), the dialog's pages and buttons, fonts reaching the application and
// the script editor, the data browser's page size, and both main windows
// saving and applying what the dialog holds. The spectrometer's large-move
// threshold goes to its window when one is open, else to its saved settings.
// Settings live in temp ini files.

#include <memory>

#include <QApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "code_editor.hpp"
#include "data_browser_window.hpp"
#include "data_main_window.hpp"
#include "main_window.hpp"
#include "preferences.hpp"
#include "preferences_dialog.hpp"
#include "pychron/processing/source.hpp"
#include "spectrometer_bridge.hpp"
#include "spectrometer_fixture.hpp"
#include "spectrometer_window.hpp"
#include "theme.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
namespace pp = pychron::processing;
using pychron::ui::DataBrowserWindow;
using pychron::ui::Preferences;
using pychron::ui::PreferencesDialog;
using pychron::ui::SpectrometerWindow;

namespace {

std::unique_ptr<pp::MemorySource> make_source(int n) {
  auto src = std::make_unique<pp::MemorySource>();
  for (int i = 0; i < n; ++i) {
    auto a = std::make_shared<pp::Analysis>();
    a->identifier = "air";
    a->uuid = "uuid-" + std::to_string(i);
    a->aliquot = i + 1;
    a->runid = pp::make_runid(a->identifier, a->aliquot, -1);
    a->analysis_type = "air";
    a->timestamp = 1'700'000'000.0 + 3600.0 * i;
    src->add(a);
  }
  return src;
}

void click(QDialogButtonBox* box, QDialogButtonBox::StandardButton which) { box->button(which)->click(); }

}  // namespace

class TestPreferences : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir dir_;
  int file_ = 0;
  QString path_;

  std::unique_ptr<QSettings> settings() const { return std::make_unique<QSettings>(path_, QSettings::IniFormat); }
  PreferencesDialog::SettingsFactory factory() const {
    return [path = path_] { return std::make_unique<QSettings>(path, QSettings::IniFormat); };
  }

 private slots:
  void initTestCase() { ui::style::apply(*qobject_cast<QApplication*>(QCoreApplication::instance())); }

  void init() { path_ = dir_.filePath(QStringLiteral("settings-%1.ini").arg(file_++)); }

  void cleanup() { ui::apply_application_preferences(Preferences{}); }

  void nothing_saved_reads_as_the_defaults() {
    const Preferences p = ui::load_preferences(*settings());
    QCOMPARE(p, Preferences{});
    QCOMPARE(p.font_pt, 0);
    QCOMPARE(p.code_font_pt, 0);
    QCOMPARE(p.browser_page_size, Preferences::kDefaultPageSize);
  }

  void saved_values_round_trip() {
    Preferences p;
    p.font_pt = 14;
    p.code_font_pt = 11;
    p.browser_page_size = 500;
    ui::save_preferences(*settings(), p);
    QCOMPARE(ui::load_preferences(*settings()), p);
  }

  void invalid_saved_values_read_as_their_defaults() {
    {
      auto s = settings();
      s->setValue(QStringLiteral("preferences/font_pt"), QStringLiteral("huge"));
      s->setValue(QStringLiteral("preferences/code_font_pt"), 200);
      s->setValue(QStringLiteral("preferences/browser_page_size"), -5);
    }
    QCOMPARE(ui::load_preferences(*settings()), Preferences{});
  }

  void dialog_shows_the_values_and_restores_defaults() {
    Preferences p;
    p.font_pt = 13;
    p.browser_page_size = 300;
    PreferencesDialog d({p, std::nullopt}, {});
    QCOMPARE(d.pages()->count(), 2);  // no spectrometer page without one
    QVERIFY(d.confirm_move() == nullptr);
    QCOMPARE(d.font_size()->value(), 13);
    QCOMPARE(d.code_font_size()->text(), QStringLiteral("Default"));
    QCOMPARE(d.page_size()->value(), 300);
    QCOMPARE(d.values().preferences, p);
    QVERIFY(!d.values().confirm_move_amu);

    d.pages()->setCurrentRow(1);
    QCOMPARE(d.stack()->currentIndex(), 1);

    click(d.buttons(), QDialogButtonBox::RestoreDefaults);
    QCOMPARE(d.values().preferences, Preferences{});
    QCOMPARE(d.font_size()->text(), QStringLiteral("Default"));
  }

  void spectrometer_page_holds_the_move_threshold() {
    PreferencesDialog d({Preferences{}, 2.5}, {});
    QCOMPARE(d.pages()->count(), 3);
    QCOMPARE(d.pages()->item(2)->text(), QStringLiteral("Spectrometer"));
    QVERIFY(d.confirm_move() != nullptr);
    QCOMPARE(d.confirm_move()->value(), 2.5);
    d.confirm_move()->setValue(0.0);
    QCOMPARE(d.confirm_move()->text(), QStringLiteral("Never ask"));
    QCOMPARE(*d.values().confirm_move_amu, 0.0);
    click(d.buttons(), QDialogButtonBox::RestoreDefaults);
    QCOMPARE(*d.values().confirm_move_amu, SpectrometerWindow::kDefaultConfirmMoveAmu);
  }

  void apply_and_ok_hand_over_the_values_and_cancel_does_not() {
    std::vector<PreferencesDialog::Values> applied;
    auto apply = [&](const PreferencesDialog::Values& v) { applied.push_back(v); };
    {
      PreferencesDialog d({Preferences{}, std::nullopt}, apply);
      d.page_size()->setValue(100);
      click(d.buttons(), QDialogButtonBox::Apply);
      QCOMPARE(applied.size(), std::size_t{1});
      QCOMPARE(applied.back().preferences.browser_page_size, 100);
      QVERIFY(d.result() != QDialog::Accepted);
      d.font_size()->setValue(15);
      click(d.buttons(), QDialogButtonBox::Ok);
      QCOMPARE(applied.size(), std::size_t{2});
      QCOMPARE(applied.back().preferences.font_pt, 15);
      QCOMPARE(d.result(), static_cast<int>(QDialog::Accepted));
    }
    {
      PreferencesDialog d({Preferences{}, std::nullopt}, apply);
      d.page_size()->setValue(1000);
      click(d.buttons(), QDialogButtonBox::RestoreDefaults);
      click(d.buttons(), QDialogButtonBox::Cancel);
      QCOMPARE(applied.size(), std::size_t{2});
    }
  }

  void fonts_reach_the_application_and_the_script_editor() {
    const double platform = QApplication::font().pointSizeF();
    ui::CodeEditor editor;
    QCOMPARE(editor.font().pointSizeF(), platform);  // code follows the interface

    Preferences p;
    p.font_pt = 17;
    ui::apply_application_preferences(p);
    QCOMPARE(QApplication::font().pointSize(), 17);
    QCOMPARE(editor.font().pointSize(), 17);

    p.code_font_pt = 8;
    ui::apply_application_preferences(p);
    QCOMPARE(QApplication::font().pointSize(), 17);
    QCOMPARE(editor.font().pointSize(), 8);
    QCOMPARE(editor.tabStopDistance(), editor.fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4.0);
    ui::CodeEditor later;
    QCOMPARE(later.font().pointSize(), 8);

    ui::apply_application_preferences(Preferences{});
    QCOMPARE(QApplication::font().pointSizeF(), platform);
    QCOMPARE(editor.font().pointSizeF(), platform);
  }

  void the_browser_pages_by_the_preference() {
    auto src = make_source(260);
    DataBrowserWindow w(*src);
    QCOMPARE(w.model()->rowCount(), Preferences::kDefaultPageSize);
    w.set_page_size(50);
    QCOMPARE(w.page_size(), 50);
    QCOMPARE(w.query().limit, 50);
    QCOMPARE(w.model()->rowCount(), 50);  // reloaded
    QTest::mouseClick(w.load_more_button(), Qt::LeftButton);
    QCOMPARE(w.model()->rowCount(), 100);
  }

  void main_window_saves_and_applies_the_preferences() {
    auto line = ui::test::make_example_line();
    auto src = make_source(260);
    QTemporaryDir presets_dir;
    pp::PresetStore presets(presets_dir.path().toStdString());
    ui::MainWindow window(*line);
    window.set_data(src.get(), &presets);
    window.set_preferences_settings(factory());
    QCOMPARE(window.preferences_action()->menuRole(), QAction::PreferencesRole);
    QCOMPARE(window.preferences_action()->shortcut(), QKeySequence(QKeySequence::Preferences));

    window.data_action()->trigger();
    QCOMPARE(window.data_window()->model()->rowCount(), Preferences::kDefaultPageSize);

    window.preferences_action()->trigger();
    PreferencesDialog* d = window.open_preferences();  // the open one again
    QVERIFY(d != nullptr);
    QCOMPARE(window.findChildren<PreferencesDialog*>().size(), 1);
    QCOMPARE(d->pages()->count(), 2);  // no spectrometer
    d->page_size()->setValue(120);
    click(d->buttons(), QDialogButtonBox::Ok);

    QCOMPARE(ui::load_preferences(*settings()).browser_page_size, 120);
    QCOMPARE(window.data_window()->model()->rowCount(), 120);
  }

  void the_move_threshold_goes_to_the_spectrometer() {
    auto line = ui::test::make_example_line();
    auto sim = ui::test::make_sim_spectrometer();
    auto bridge = std::make_unique<ui::SpectrometerBridge>(*sim->spec, *sim->scan, sim->bus);
    const QString key = QStringLiteral("spectrometer_window/%1/confirm_move_amu").arg(bridge->name());
    {
      ui::MainWindow window(*line);
      window.set_preferences_settings(factory());
      window.set_spectrometer(bridge.get(), true, factory());

      // No spectrometer window yet: straight to its saved settings.
      PreferencesDialog* d = window.open_preferences();
      QCOMPARE(d->pages()->count(), 3);
      QCOMPARE(d->confirm_move()->value(), SpectrometerWindow::kDefaultConfirmMoveAmu);
      d->confirm_move()->setValue(2.5);
      click(d->buttons(), QDialogButtonBox::Ok);
      QVERIFY(window.spectrometer_window() == nullptr);  // not opened for it
      QCOMPARE(settings()->value(key).toDouble(), 2.5);

      // The window reads it, and a change reaches the open window at once.
      window.spectrometer_action()->trigger();
      QVERIFY(window.spectrometer_window() != nullptr);
      QCOMPARE(window.spectrometer_window()->confirm_move_amu(), 2.5);
      d = window.open_preferences();
      QCOMPARE(d->confirm_move()->value(), 2.5);
      d->confirm_move()->setValue(0.0);
      click(d->buttons(), QDialogButtonBox::Apply);
      QCOMPARE(window.spectrometer_window()->confirm_move_amu(), 0.0);
      QCOMPARE(settings()->value(key).toDouble(), 0.0);
      click(d->buttons(), QDialogButtonBox::Cancel);
      window.set_spectrometer(nullptr, false);
    }
    bridge.reset();
  }

  void spectrometer_threshold_settings_are_validated() {
    auto s = settings();
    QCOMPARE(SpectrometerWindow::saved_confirm_move_amu(*s, QStringLiteral("x")),
             SpectrometerWindow::kDefaultConfirmMoveAmu);
    SpectrometerWindow::save_confirm_move_amu(*s, QStringLiteral("x"), -1.0);  // ignored
    QVERIFY(!s->contains(QStringLiteral("spectrometer_window/x/confirm_move_amu")));
    SpectrometerWindow::save_confirm_move_amu(*s, QStringLiteral("x"), 1.5);
    QCOMPARE(SpectrometerWindow::saved_confirm_move_amu(*s, QStringLiteral("x")), 1.5);
    s->setValue(QStringLiteral("spectrometer_window/x/confirm_move_amu"), QStringLiteral("lots"));
    QCOMPARE(SpectrometerWindow::saved_confirm_move_amu(*s, QStringLiteral("x")),
             SpectrometerWindow::kDefaultConfirmMoveAmu);
  }

  void data_reduction_window_has_preferences_too() {
    auto src = make_source(260);
    QTemporaryDir presets_dir;
    pp::PresetStore presets(presets_dir.path().toStdString());
    ui::DataMainWindow window(*src, presets, QStringLiteral("data-reduction"));
    window.set_preferences_settings(factory());
    QVERIFY(window.preferences_action()->isVisible());
    QCOMPARE(window.browser()->model()->rowCount(), Preferences::kDefaultPageSize);
    PreferencesDialog* d = window.open_preferences();
    QCOMPARE(d->pages()->count(), 2);
    d->page_size()->setValue(60);
    click(d->buttons(), QDialogButtonBox::Ok);
    QCOMPARE(ui::load_preferences(*settings()).browser_page_size, 60);
    QCOMPARE(window.browser()->model()->rowCount(), 60);
  }
};

QTEST_MAIN(TestPreferences)
#include "test_preferences.moc"
