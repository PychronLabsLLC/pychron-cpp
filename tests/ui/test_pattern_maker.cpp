// PatternMakerWindow (laser window design, section 6): a pattern made on
// screen is the file a queue names.

#include <filesystem>
#include <fstream>
#include <random>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QtTest/QtTest>

#include "pattern_maker_window.hpp"
#include "pychron/laser/pattern.hpp"

using namespace pychron;
using namespace pychron::ui;
namespace fs = std::filesystem;

class PatternMakerTest : public QObject {
  Q_OBJECT

  template <class W>
  W* the(const char* name) {
    W* w = window_->findChild<W*>(QString::fromLatin1(name));
    if (w == nullptr) qFatal("no widget named %s", name);
    return w;
  }
  bool has(const char* name) { return window_->findChild<QWidget*>(QString::fromLatin1(name)) != nullptr; }
  void number(const char* name, double value) { the<QDoubleSpinBox>(name)->setValue(value); }
  void whole(const char* name, int value) { the<QSpinBox>(name)->setValue(value); }
  void kind(const char* name) {
    auto* combo = the<QComboBox>("kind");
    const int index = combo->findText(QString::fromLatin1(name));
    QVERIFY2(index >= 0, name);
    combo->setCurrentIndex(index);
  }
  QString text(const char* name) { return the<QLabel>(name)->text(); }

 private slots:
  void init() {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("pychron-ui-patterns-" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir_);
    std::ofstream(dir_ / "hexagon.toml") << "kind = \"polygon\"\nradius = 0.75\nnsides = 6\nvelocity = 1.5\niterations = 2\n";
    std::ofstream(dir_ / "follow.toml") << "kind = \"dragonfly\"\nduration = 12\nspiral = \"square\"\n";
    library_ = laser::PatternLibrary::load(dir_);
    window_ = std::make_unique<PatternMakerWindow>(library_, dir_);
    window_->show();
  }
  void cleanup() {
    window_.reset();
    fs::remove_all(dir_);
  }

  void starts_with_a_new_polygon() {
    QCOMPARE(the<QComboBox>("kind")->currentText(), QStringLiteral("polygon"));
    QCOMPARE(the<QComboBox>("kind")->count(), 10);
    QVERIFY(the<QLineEdit>("name")->text().isEmpty());
    QCOMPARE(window_->pattern(), laser::Pattern::defaults(laser::PatternKind::Polygon));
    // nothing to save until it has a name
    QVERIFY(!the<QPushButton>("save")->isEnabled());
    QVERIFY2(text("problem").contains(QStringLiteral("name")), qPrintable(text("problem")));
  }

  void shows_only_the_kinds_fields() {
    for (const char* name : {"field_velocity", "field_iterations", "field_radius", "field_nsides", "field_rotation"}) {
      QVERIFY2(has(name), name);
    }
    QVERIFY(!has("field_length"));
    QVERIFY(!has("field_seed"));
    kind("raster");
    for (const char* name : {"field_velocity", "field_iterations", "field_length", "field_offset", "field_rotation",
                             "field_dx", "field_single_pass"}) {
      QVERIFY2(has(name), name);
    }
    QVERIFY(!has("field_radius"));
    QVERIFY(!has("field_nsides"));
    // a kind starts from legacy's defaults for it
    QCOMPARE(the<QDoubleSpinBox>("field_length")->value(), 15.0);
    QVERIFY(the<QCheckBox>("field_single_pass")->isChecked());
    kind("random");
    QVERIFY(has("field_seed"));
    QVERIFY(has("field_seed_fixed"));
  }

  void the_preview_follows_the_fields() {
    the<QLineEdit>("name")->setText(QStringLiteral("square"));
    whole("field_nsides", 4);
    number("field_radius", 1);
    number("field_velocity", 2);
    // out to the first corner, round the four sides, and back: 1 + 4 sqrt(2) + 1
    QVERIFY2(text("summary").contains(QStringLiteral("7.66 mm")), qPrintable(text("summary")));
    QVERIFY2(text("summary").contains(QStringLiteral("3.8 s")), qPrintable(text("summary")));
    QCOMPARE(window_->preview_points(), 6);
    whole("field_iterations", 2);
    QCOMPARE(window_->preview_points(), 11);
    QVERIFY(text("problem").isEmpty());
    QVERIFY(the<QPushButton>("save")->isEnabled());
    window_->grab();  // it paints
  }

  void a_value_that_cannot_run_is_said_and_save_is_off() {
    the<QLineEdit>("name")->setText(QStringLiteral("flat"));
    QVERIFY(the<QPushButton>("save")->isEnabled());
    number("field_radius", 0);
    QVERIFY2(text("problem").contains(QStringLiteral("radius")), qPrintable(text("problem")));
    QVERIFY(!the<QPushButton>("save")->isEnabled());
    number("field_radius", 0.5);
    QVERIFY(text("problem").isEmpty());
    // too many points over its iterations
    whole("field_nsides", 200);
    whole("field_iterations", 200);
    QVERIFY2(text("problem").contains(QStringLiteral("points")), qPrintable(text("problem")));
    QVERIFY(!the<QPushButton>("save")->isEnabled());
  }

  void a_name_that_cannot_be_a_file_is_refused() {
    the<QLineEdit>("name")->setText(QStringLiteral("../up"));
    QVERIFY(!the<QPushButton>("save")->isEnabled());
    QVERIFY2(text("problem").contains(QStringLiteral("name")), qPrintable(text("problem")));
  }

  void save_writes_the_file_and_the_library_has_it() {
    QSignalSpy saved(window_.get(), &PatternMakerWindow::saved);
    the<QLineEdit>("name")->setText(QStringLiteral("square"));
    whole("field_nsides", 4);
    number("field_radius", 1.25);
    number("field_rotation", 45);
    the<QPushButton>("save")->click();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved[0][0].toString(), QStringLiteral("square"));
    QVERIFY(fs::exists(dir_ / "square.toml"));
    const auto loaded = laser::Pattern::load(dir_ / "square.toml");
    QVERIFY2(loaded.has_value(), loaded ? "" : loaded.error().what.c_str());
    QCOMPARE(loaded->nsides, 4);
    QCOMPARE(loaded->radius, 1.25);
    QCOMPARE(loaded->rotation, 45.0);
    const auto held = library_.find("square");
    QVERIFY(held != nullptr);
    QCOMPARE(*held, *loaded);
    // and it is offered to open
    QVERIFY(the<QComboBox>("open_list")->findText(QStringLiteral("square")) >= 0);
    QVERIFY2(text("problem").isEmpty(), qPrintable(text("problem")));
    QVERIFY2(text("summary").contains(QStringLiteral("saved")), qPrintable(text("summary")));
  }

  void opens_an_existing_pattern_and_saves_over_it() {
    window_->open(QStringLiteral("hexagon"));
    QCOMPARE(the<QLineEdit>("name")->text(), QStringLiteral("hexagon"));
    QCOMPARE(the<QComboBox>("kind")->currentText(), QStringLiteral("polygon"));
    QCOMPARE(the<QDoubleSpinBox>("field_radius")->value(), 0.75);
    QCOMPARE(the<QSpinBox>("field_iterations")->value(), 2);
    QCOMPARE(the<QDoubleSpinBox>("field_velocity")->value(), 1.5);
    QCOMPARE(window_->pattern(), *library_.find("hexagon"));
    number("field_radius", 0.9);
    the<QPushButton>("save")->click();
    QCOMPARE(laser::Pattern::load(dir_ / "hexagon.toml")->radius, 0.9);
    QCOMPARE(library_.find("hexagon")->radius, 0.9);
    // by the list too
    auto* list = the<QComboBox>("open_list");
    list->setCurrentIndex(list->findText(QStringLiteral("follow")));
    emit list->activated(list->currentIndex());
    QCOMPARE(the<QLineEdit>("name")->text(), QStringLiteral("follow"));
    QCOMPARE(the<QComboBox>("kind")->currentText(), QStringLiteral("dragonfly"));
  }

  void a_dragonfly_has_no_path() {
    window_->open(QStringLiteral("follow"));
    QVERIFY(!has("field_iterations"));
    QVERIFY(has("field_perimeter_radius"));
    QCOMPARE(the<QDoubleSpinBox>("field_duration")->value(), 12.0);
    QCOMPARE(the<QComboBox>("field_spiral")->currentText(), QStringLiteral("square"));
    QVERIFY2(text("summary").contains(QStringLiteral("follows the glow")), qPrintable(text("summary")));
    QCOMPARE(window_->preview_points(), 0);
    // no duration of its own: it runs for as long as the run says, and that can be saved
    number("field_duration", 0);
    QVERIFY2(text("problem").isEmpty(), qPrintable(text("problem")));
    the<QComboBox>("field_spiral")->setCurrentIndex(the<QComboBox>("field_spiral")->findText(QStringLiteral("hexagon")));
    the<QPushButton>("save")->click();
    const auto loaded = laser::Pattern::load(dir_ / "follow.toml");
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->duration_s, 0.0);
    QVERIFY(!loaded->square_spiral);
    window_->grab();
  }

  void a_random_walk_is_new_each_run_unless_its_seed_is_fixed() {
    kind("random");
    the<QLineEdit>("name")->setText(QStringLiteral("walk"));
    QVERIFY(!the<QCheckBox>("field_seed_fixed")->isChecked());
    QVERIFY(!the<QLineEdit>("field_seed")->isEnabled());
    QVERIFY(!window_->pattern().seed.has_value());
    QVERIFY2(text("summary").contains(QStringLiteral("each run")), qPrintable(text("summary")));
    the<QCheckBox>("field_seed_fixed")->setChecked(true);
    the<QLineEdit>("field_seed")->setText(QStringLiteral("7"));
    QCOMPARE(window_->pattern().seed, std::optional<std::uint64_t>(7));
    the<QPushButton>("save")->click();
    QCOMPARE(laser::Pattern::load(dir_ / "walk.toml")->seed, std::optional<std::uint64_t>(7));
  }

  // Opening a pattern and saving it changes nothing in it.
  void open_then_save_keeps_every_value() {
    std::ofstream(dir_ / "fine.toml") << "kind = \"dragonfly\"\nspiral_base = 0.0125\nmove_threshold = 0.0333\n";
    std::ofstream(dir_ / "walk.toml") << "kind = \"random\"\nseed = 9007199254740993\nwalk_x = 0.1234567\n";
    library_ = laser::PatternLibrary::load(dir_);
    for (const char* name : {"fine", "walk"}) {
      const laser::Pattern before = *library_.find(name);
      window_->open(QString::fromLatin1(name));
      QCOMPARE(window_->pattern(), before);
      the<QPushButton>("save")->click();
      const auto after = laser::Pattern::load(dir_ / (std::string(name) + ".toml"));
      QVERIFY2(after.has_value(), name);
      QCOMPARE(*after, before);
    }
  }

  void new_clears_the_form() {
    window_->open(QStringLiteral("hexagon"));
    the<QPushButton>("new")->click();
    QVERIFY(the<QLineEdit>("name")->text().isEmpty());
    QCOMPARE(window_->pattern(), laser::Pattern::defaults(laser::PatternKind::Polygon));
  }

 private:
  fs::path dir_;
  laser::PatternLibrary library_;
  std::unique_ptr<PatternMakerWindow> window_;
};

QTEST_MAIN(PatternMakerTest)
#include "test_pattern_maker.moc"
