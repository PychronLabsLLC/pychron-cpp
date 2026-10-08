// File > Preferences…: the saved values (defaults for anything missing or
// invalid), the dialog's pages and buttons, fonts reaching the application and
// the script editor, the data browser's page size, and both main windows
// saving and applying what the dialog holds. The spectrometer's large-move
// threshold goes to its window when one is open, else to its saved settings.
// Settings live in temp ini files.
//
// With a line, two more pages: Logging and Metrics. Theirs are not QSettings
// but the line's local override file (extraction_line.local.toml), written
// here beside a config in a temp folder.

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
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
#include "line_settings.hpp"
#include "logging_page.hpp"
#include "main_window.hpp"
#include "metrics_page.hpp"
#include "preferences.hpp"
#include "preferences_dialog.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/core/log_hub.hpp"
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

namespace fs = std::filesystem;
using pychron::ui::LineSettings;

void write_file(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary);
  out << text;
}
std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// A shared config that says something about logging and metrics, so a
// preference can be told from what it overrides.
const char* const kSharedConfig =
    "[system]\nname = \"t\"\n\n[logging]\ndefault_level = \"warn\"\nmax_files = 3\n\n[logging.levels]\nscheduler = \"debug\"\n\n"
    "[metrics]\nport = 9500\n";

LogLevel level_of(const config::LoggingConfig& l, const std::string& pattern) {
  for (const auto& [p, level] : l.levels) {
    if (p == pattern) return level;
  }
  return LogLevel::Info;
}

}  // namespace

class TestPreferences : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir dir_;
  int file_ = 0;
  QString path_;
  fs::path config_dir_, main_file_, local_file_;

  LineSettings line_settings() const {
    auto s = ui::load_line_settings(main_file_);
    if (!s) qFatal("the test's config did not load");
    return *s;
  }

  std::unique_ptr<QSettings> settings() const { return std::make_unique<QSettings>(path_, QSettings::IniFormat); }
  PreferencesDialog::SettingsFactory factory() const {
    return [path = path_] { return std::make_unique<QSettings>(path, QSettings::IniFormat); };
  }

 private slots:
  void initTestCase() { ui::style::apply(*qobject_cast<QApplication*>(QCoreApplication::instance())); }

  void init() {
    path_ = dir_.filePath(QStringLiteral("settings-%1.ini").arg(file_++));
    // A config folder of this test's own.
    config_dir_ = fs::path(dir_.path().toStdString()) / ("line-" + std::to_string(file_));
    fs::create_directories(config_dir_);
    main_file_ = config_dir_ / "extraction_line.toml";
    local_file_ = config_dir_ / "extraction_line.local.toml";
    write_file(main_file_, kSharedConfig);
  }

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

  // ---- Logging and Metrics: the line's local override file ----------------------

  void line_pages_are_there_only_with_a_line() {
    PreferencesDialog without({Preferences{}, std::nullopt, std::nullopt}, {});
    QCOMPARE(without.pages()->count(), 2);
    QVERIFY(without.logging_page() == nullptr);
    QVERIFY(without.metrics_page() == nullptr);

    PreferencesDialog with({Preferences{}, std::nullopt, line_settings()}, {});
    QCOMPARE(with.pages()->count(), 4);
    QCOMPARE(with.pages()->item(2)->text(), QStringLiteral("Logging"));
    QCOMPARE(with.pages()->item(3)->text(), QStringLiteral("Metrics"));
    QVERIFY(with.logging_page() != nullptr);
    QVERIFY(with.metrics_page() != nullptr);
  }

  void line_settings_load_what_is_in_force_and_what_is_shared() {
    write_file(local_file_, "[logging]\ndefault_level = \"trace\"\n\n[metrics]\nenabled = true\n");
    const LineSettings s = line_settings();
    QCOMPARE(s.logging.default_level, LogLevel::Trace);
    QCOMPARE(s.shared_logging.default_level, LogLevel::Warn);
    QCOMPARE(s.logging.max_files, std::int64_t{3});
    QVERIFY(s.metrics.enabled);
    QVERIFY(!s.shared_metrics.enabled);
    QCOMPARE(s.metrics.port, std::int64_t{9500});
  }

  void a_config_that_cannot_be_read_has_no_line_settings() {
    QVERIFY(!ui::load_line_settings(config_dir_ / "nothing.toml").has_value());
  }

  void logging_page_shows_the_values_and_gives_them_back() {
    PreferencesDialog d({Preferences{}, std::nullopt, line_settings()}, {});
    ui::LoggingPage* page = d.logging_page();
    QCOMPARE(page->default_level()->currentText(), QStringLiteral("warn"));
    QCOMPARE(page->levels()->rowCount(), 1);
    QCOMPARE(page->levels()->item(0, 0)->text(), QStringLiteral("scheduler"));
    QCOMPARE(page->level_at(0)->currentText(), QStringLiteral("debug"));
    QCOMPARE(page->max_files()->value(), 3);
    QCOMPARE(page->max_size()->value(), 10);
    QVERIFY(page->folder()->text().isEmpty());
    QVERIFY(!page->echo()->isChecked());

    page->default_level()->setCurrentText(QStringLiteral("debug"));
    page->folder()->setText(QStringLiteral("/tmp/pychron-logs"));
    page->max_size()->setValue(25);
    page->echo()->setChecked(true);
    const config::LoggingConfig got = d.values().line->logging;
    QCOMPARE(got.default_level, LogLevel::Debug);
    QCOMPARE(got.dir, fs::path("/tmp/pychron-logs"));
    QCOMPARE(got.max_size_mb, std::int64_t{25});
    QCOMPARE(got.max_files, std::int64_t{3});
    QVERIFY(got.echo_stderr);
    QCOMPARE(got.levels.size(), std::size_t{1});
  }

  void logger_levels_are_added_edited_and_removed() {
    PreferencesDialog d({Preferences{}, std::nullopt, line_settings()}, {});
    ui::LoggingPage* page = d.logging_page();
    QVERIFY(!page->remove_level()->isEnabled());  // nothing selected

    page->add_level()->click();
    QCOMPARE(page->levels()->rowCount(), 2);
    page->levels()->item(1, 0)->setText(QStringLiteral("  *.wire "));
    page->level_at(1)->setCurrentText(QStringLiteral("trace"));
    page->add_level()->click();  // left empty: not a level
    config::LoggingConfig got = d.values().line->logging;
    QCOMPARE(got.levels.size(), std::size_t{2});
    QCOMPARE(level_of(got, "*.wire"), LogLevel::Trace);
    QCOMPARE(level_of(got, "scheduler"), LogLevel::Debug);

    page->levels()->selectRow(0);
    QVERIFY(page->remove_level()->isEnabled());
    page->remove_level()->click();
    got = d.values().line->logging;
    QCOMPARE(got.levels.size(), std::size_t{1});
    QCOMPARE(got.levels[0].first, std::string("*.wire"));
  }

  void a_pattern_given_twice_keeps_the_later_level() {
    PreferencesDialog d({Preferences{}, std::nullopt, line_settings()}, {});
    ui::LoggingPage* page = d.logging_page();
    page->add_level()->click();
    page->levels()->item(1, 0)->setText(QStringLiteral("scheduler"));
    page->level_at(1)->setCurrentText(QStringLiteral("error"));
    const config::LoggingConfig got = d.values().line->logging;
    QCOMPARE(got.levels.size(), std::size_t{1});
    QCOMPARE(level_of(got, "scheduler"), LogLevel::Error);
  }

  void metrics_page_shows_the_values_and_gives_them_back() {
    LineSettings s = line_settings();
    s.metrics_status = QStringLiteral("Listening on 0.0.0.0:9500");
    PreferencesDialog d({Preferences{}, std::nullopt, s}, {});
    ui::MetricsPage* page = d.metrics_page();
    QVERIFY(!page->enabled()->isChecked());
    QCOMPARE(page->port()->value(), 9500);
    QCOMPARE(page->where()->currentIndex(), int(ui::MetricsPage::AllAddresses));
    QVERIFY(!page->address()->isEnabled());
    QVERIFY(page->status()->text().contains(QStringLiteral("Listening on 0.0.0.0:9500")));

    page->enabled()->setChecked(true);
    page->where()->setCurrentIndex(int(ui::MetricsPage::ThisComputer));
    page->port()->setValue(9464);
    config::MetricsConfig got = d.values().line->metrics;
    QVERIFY(got.enabled);
    QCOMPARE(got.bind, std::string("127.0.0.1"));
    QCOMPARE(got.port, std::int64_t{9464});

    page->where()->setCurrentIndex(int(ui::MetricsPage::OneAddress));
    QVERIFY(page->address()->isEnabled());
    page->address()->setText(QStringLiteral(" 192.168.1.20 "));
    QCOMPARE(d.values().line->metrics.bind, std::string("192.168.1.20"));
  }

  void metrics_page_reads_an_address_of_its_own_as_one_address() {
    LineSettings s = line_settings();
    s.metrics.bind = "192.168.1.20";
    PreferencesDialog d({Preferences{}, std::nullopt, s}, {});
    QCOMPARE(d.metrics_page()->where()->currentIndex(), int(ui::MetricsPage::OneAddress));
    QCOMPARE(d.metrics_page()->address()->text(), QStringLiteral("192.168.1.20"));
    s.metrics.bind = "127.0.0.1";
    d.set_values({Preferences{}, std::nullopt, s});
    QCOMPARE(d.metrics_page()->where()->currentIndex(), int(ui::MetricsPage::ThisComputer));
  }

  void an_address_that_is_not_one_is_refused_and_nothing_is_saved() {
    int saved = 0, applied = 0;
    PreferencesDialog d({Preferences{}, std::nullopt, line_settings()}, [&](const PreferencesDialog::Values&) { ++applied; });
    d.set_line_save([&](const LineSettings&) {
      ++saved;
      return QString();
    });
    d.show();
    d.metrics_page()->where()->setCurrentIndex(int(ui::MetricsPage::OneAddress));
    d.metrics_page()->address()->setText(QStringLiteral("labpc.local"));
    click(d.buttons(), QDialogButtonBox::Ok);
    QVERIFY(d.isVisible());
    QVERIFY(d.problem()->isVisibleTo(&d));
    QVERIFY(d.problem()->text().contains(QStringLiteral("labpc.local")));
    QCOMPARE(d.stack()->currentWidget(), static_cast<QWidget*>(d.metrics_page()));  // taken to the field
    QCOMPARE(saved, 0);
    QCOMPARE(applied, 0);

    d.metrics_page()->address()->setText(QStringLiteral("10.0.0.5"));
    click(d.buttons(), QDialogButtonBox::Ok);
    QVERIFY(!d.isVisible());
    QCOMPARE(saved, 1);
    QCOMPARE(applied, 1);
  }

  void a_file_that_cannot_be_written_is_said_and_the_dialog_stays() {
    int applied = 0;
    PreferencesDialog d({Preferences{}, std::nullopt, line_settings()}, [&](const PreferencesDialog::Values&) { ++applied; });
    bool fail = true;
    d.set_line_save([&](const LineSettings&) {
      return fail ? QStringLiteral("extraction_line.local.toml: cannot be written") : QString();
    });
    d.show();
    click(d.buttons(), QDialogButtonBox::Apply);
    QVERIFY(d.problem()->isVisibleTo(&d));
    QVERIFY(d.problem()->text().contains(QStringLiteral("cannot be written")));
    QCOMPARE(applied, 0);  // nothing half applied
    click(d.buttons(), QDialogButtonBox::Ok);
    QVERIFY(d.isVisible());

    fail = false;
    click(d.buttons(), QDialogButtonBox::Apply);
    QVERIFY(!d.problem()->isVisibleTo(&d));  // the complaint goes when its cause does
    QCOMPARE(applied, 1);
  }

  void restore_defaults_goes_back_to_the_shared_file() {
    write_file(local_file_, "[logging]\ndefault_level = \"trace\"\n\n[logging.levels]\n\n[metrics]\nenabled = true\nbind = \"127.0.0.1\"\n");
    PreferencesDialog d({Preferences{}, std::nullopt, line_settings()}, {});
    QCOMPARE(d.logging_page()->default_level()->currentText(), QStringLiteral("trace"));
    QCOMPARE(d.logging_page()->levels()->rowCount(), 0);
    QVERIFY(d.metrics_page()->enabled()->isChecked());

    click(d.buttons(), QDialogButtonBox::RestoreDefaults);
    QCOMPARE(d.logging_page()->default_level()->currentText(), QStringLiteral("warn"));
    QCOMPARE(d.logging_page()->levels()->rowCount(), 1);
    QVERIFY(!d.metrics_page()->enabled()->isChecked());
    QCOMPARE(d.metrics_page()->port()->value(), 9500);
    QCOMPARE(d.metrics_page()->where()->currentIndex(), int(ui::MetricsPage::AllAddresses));
  }

  void saving_writes_only_what_differs_from_the_shared_file() {
    write_file(local_file_, "# this computer's port\n[transports.valve_bus]\nport = \"COM4\"\n");
    // (the transport is not in this test's shared config: only the text is at stake here)
    LineSettings s;
    s.shared_logging.default_level = LogLevel::Warn;
    s.shared_logging.max_files = 3;
    s.shared_logging.levels = {{"scheduler", LogLevel::Debug}};
    s.shared_metrics.port = 9500;
    s.logging = s.shared_logging;
    s.metrics = s.shared_metrics;
    s.logging.default_level = LogLevel::Debug;
    s.metrics.enabled = true;
    QCOMPARE(ui::save_line_settings(main_file_, s), QString());
    QCOMPARE(QString::fromStdString(read_file(local_file_)),
             QStringLiteral("# this computer's port\n[transports.valve_bus]\nport = \"COM4\"\n\n"
                            "[logging]\ndefault_level = \"debug\"\n\n[metrics]\nenabled = true\n"));

    // Back to what the shared file says: the tables go, the rest stays.
    s.logging = s.shared_logging;
    s.metrics = s.shared_metrics;
    QCOMPARE(ui::save_line_settings(main_file_, s), QString());
    QCOMPARE(QString::fromStdString(read_file(local_file_)),
             QStringLiteral("# this computer's port\n[transports.valve_bus]\nport = \"COM4\"\n"));
  }

  void saved_line_settings_are_what_the_line_loads_next() {
    LineSettings s = line_settings();
    s.logging.default_level = LogLevel::Error;
    s.logging.levels = {{"*.wire", LogLevel::Trace}};
    s.logging.dir = "/tmp/pychron \"logs\"";
    s.metrics.enabled = true;
    s.metrics.bind = "127.0.0.1";
    QCOMPARE(ui::save_line_settings(main_file_, s), QString());
    const auto cfg = config::load_system_config(main_file_);
    QVERIFY2(cfg.has_value(), cfg ? "" : cfg.error().what.c_str());
    QCOMPARE(cfg->logging.default_level, LogLevel::Error);
    QCOMPARE(cfg->logging.levels.size(), std::size_t{1});
    QCOMPARE(cfg->logging.dir, fs::path("/tmp/pychron \"logs\""));
    QCOMPARE(cfg->logging.max_files, std::int64_t{3});
    QVERIFY(cfg->metrics.enabled);
    QCOMPARE(cfg->metrics.bind, std::string("127.0.0.1"));
    QCOMPARE(cfg->metrics.port, std::int64_t{9500});
  }

  void a_local_file_that_is_not_toml_is_reported_and_kept() {
    write_file(local_file_, "[transports.valve_bus\n");
    LineSettings s;
    s.metrics.enabled = true;
    const QString problem = ui::save_line_settings(main_file_, s);
    QVERIFY(problem.contains(QStringLiteral("extraction_line.local.toml")));
    QCOMPARE(read_file(local_file_), std::string("[transports.valve_bus\n"));
  }

  void main_window_has_no_line_pages_until_it_is_told_the_file() {
    auto line = ui::test::make_example_line();
    ui::MainWindow window(*line);
    window.set_preferences_settings(factory());
    PreferencesDialog* d = window.open_preferences();
    QCOMPARE(d->pages()->count(), 2);
    d->close();
  }

  void main_window_writes_the_local_file_and_changes_levels_at_once() {
    auto line = ui::test::make_example_line();
    const std::shared_ptr<LogHub> hub = line->log_hub();
    QVERIFY(hub != nullptr);
    Logger scheduler = hub->logger("scheduler");
    Logger other = hub->logger("other");
    QVERIFY(!other.enabled(LogLevel::Debug));

    ui::MainWindow window(*line);
    window.set_preferences_settings(factory());
    window.set_line_config_file(main_file_);
    window.set_metrics_status(QStringLiteral("Off"));
    PreferencesDialog* d = window.open_preferences();
    QCOMPARE(d->pages()->count(), 4);
    QVERIFY(d->metrics_page()->status()->text().contains(QStringLiteral("Off")));

    d->logging_page()->default_level()->setCurrentText(QStringLiteral("debug"));
    d->logging_page()->level_at(0)->setCurrentText(QStringLiteral("error"));  // scheduler
    d->metrics_page()->enabled()->setChecked(true);
    click(d->buttons(), QDialogButtonBox::Apply);
    QVERIFY(!d->problem()->isVisibleTo(d));

    // In the file, for the next start and for elctl...
    const auto cfg = config::load_system_config(main_file_);
    QVERIFY2(cfg.has_value(), cfg ? "" : cfg.error().what.c_str());
    QCOMPARE(cfg->logging.default_level, LogLevel::Debug);
    QCOMPARE(level_of(cfg->logging, "scheduler"), LogLevel::Error);
    QVERIFY(cfg->metrics.enabled);
    // ...and the levels in force now.
    QVERIFY(other.enabled(LogLevel::Debug));
    QVERIFY(!scheduler.enabled(LogLevel::Warn));

    // Opened again, the dialog shows what was saved.
    click(d->buttons(), QDialogButtonBox::Cancel);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    PreferencesDialog* again = window.open_preferences();
    QCOMPARE(again->logging_page()->default_level()->currentText(), QStringLiteral("debug"));
    QVERIFY(again->metrics_page()->enabled()->isChecked());

    // Restore Defaults, then OK: the local file is gone and the shared levels are back.
    click(again->buttons(), QDialogButtonBox::RestoreDefaults);
    click(again->buttons(), QDialogButtonBox::Ok);
    QVERIFY(!fs::exists(local_file_));
    QVERIFY(!other.enabled(LogLevel::Debug));
    QVERIFY(scheduler.enabled(LogLevel::Debug));
  }


  // A level set for the session from the log panel is not a preference, and
  // an Apply that is about something else must not take it away.
  void applying_something_else_leaves_the_sessions_levels_alone() {
    auto line = ui::test::make_example_line();
    const std::shared_ptr<LogHub> hub = line->log_hub();
    QVERIFY(hub != nullptr);
    hub->set_level("transport", LogLevel::Trace);  // as "Set logger level…" does
    Logger transport = hub->logger("transport");
    QVERIFY(transport.enabled(LogLevel::Trace));

    ui::MainWindow window(*line);
    window.set_preferences_settings(factory());
    window.set_line_config_file(main_file_);
    PreferencesDialog* d = window.open_preferences();
    d->font_size()->setValue(14);
    d->metrics_page()->port()->setValue(9600);
    click(d->buttons(), QDialogButtonBox::Apply);
    QVERIFY(!d->problem()->isVisibleTo(d));
    QVERIFY(transport.enabled(LogLevel::Trace));

    // A change to the levels is the user saying what they are to be.
    d->logging_page()->default_level()->setCurrentText(QStringLiteral("error"));
    click(d->buttons(), QDialogButtonBox::Ok);
    QVERIFY(!transport.enabled(LogLevel::Trace));
  }
};

QTEST_MAIN(TestPreferences)
#include "test_preferences.moc"
