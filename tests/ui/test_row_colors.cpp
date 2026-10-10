// The analysis table's row tints (data browser search and display design,
// section 3.4): what a row is coloured by, and the colour it gets.

#include <QtTest/QtTest>

#include "analysis_table_model.hpp"
#include "row_colors.hpp"
#include "theme.hpp"

using namespace pychron;
namespace pp = pychron::processing;
using ui::ColorBy;

namespace {

pp::AnalysisSummary row(const char* type, const char* spectrometer = "jan", const char* tag = "ok") {
  pp::AnalysisSummary s;
  s.uuid = std::string(type) + "/" + spectrometer + "/" + tag;
  s.analysis_type = type;
  s.mass_spectrometer = spectrometer;
  s.tag = tag;
  return s;
}

QColor tint(const ui::AnalysisTableModel& model, int r) {
  const QVariant v = model.data(model.index(r, 0), Qt::BackgroundRole);
  return v.isValid() ? v.value<QBrush>().color() : QColor();
}

}  // namespace

class RowColorsTest : public QObject {
  Q_OBJECT

 private slots:
  void a_choice_is_saved_by_name() {
    for (const ColorBy by : ui::kColorByChoices) QCOMPARE(ui::color_by_from_text(ui::to_text(by)), by);
    QCOMPARE(ui::color_by_from_text(QStringLiteral("rainbow")), ColorBy::AnalysisType);
    QCOMPARE(ui::color_by_from_text(QString()), ColorBy::AnalysisType);
  }

  void analysis_types_fall_into_classes() {
    QCOMPARE(ui::type_class("unknown"), std::string_view("unknown"));
    QCOMPARE(ui::type_class("blank_unknown"), std::string_view("blank"));
    QCOMPARE(ui::type_class("blank_air"), std::string_view("blank"));
    QCOMPARE(ui::type_class("air"), std::string_view("air"));
    QCOMPARE(ui::type_class("cocktail"), std::string_view("cocktail"));
    QCOMPARE(ui::type_class("detector_ic"), std::string_view("detector_ic"));
    QCOMPARE(ui::type_class("background"), std::string_view("other"));
  }

  void by_type_each_class_has_its_colour() {
    const auto& t = ui::theme();
    const ui::TypeColors types = ui::default_type_colors(t);
    QCOMPARE(ui::row_color(ColorBy::AnalysisType, row("blank_unknown"), types, {}, t), t.row_blank);
    QCOMPARE(ui::row_color(ColorBy::AnalysisType, row("air"), types, {}, t), t.row_air);
    QCOMPARE(ui::row_color(ColorBy::AnalysisType, row("cocktail"), types, {}, t), t.row_cocktail);
    QCOMPARE(ui::row_color(ColorBy::AnalysisType, row("detector_ic"), types, {}, t), t.row_detector_ic);
    QVERIFY(!ui::row_color(ColorBy::AnalysisType, row("unknown"), types, {}, t).isValid());
    QVERIFY(!ui::row_color(ColorBy::AnalysisType, row("background"), types, {}, t).isValid());

    ui::TypeColors mine = types;
    mine.unknown = QColor(0x12, 0x34, 0x56);
    mine.air = QColor();  // no colour
    QCOMPARE(ui::row_color(ColorBy::AnalysisType, row("unknown"), mine, {}, t), QColor(0x12, 0x34, 0x56));
    QVERIFY(!ui::row_color(ColorBy::AnalysisType, row("air"), mine, {}, t).isValid());
  }

  void a_bad_tag_is_the_error_colour_whatever_the_choice() {
    const auto& t = ui::theme();
    const ui::TypeColors types = ui::default_type_colors(t);
    for (const ColorBy by : {ColorBy::AnalysisType, ColorBy::Spectrometer, ColorBy::IrradiationLevel, ColorBy::None}) {
      QCOMPARE(ui::row_color(by, row("air", "jan", "invalid"), types, {"jan"}, t), t.error_bg);
      QCOMPARE(ui::row_color(by, row("air", "jan", "omit"), types, {"jan"}, t), t.error_bg);
    }
    QVERIFY(!ui::row_color(ColorBy::None, row("air"), types, {}, t).isValid());
  }

  void by_tag_the_tags_are_told_apart() {
    const auto& t = ui::theme();
    const ui::TypeColors types = ui::default_type_colors(t);
    const std::vector<std::string> keys{"omit", "skip"};
    QCOMPARE(ui::color_key(ColorBy::Tag, row("air", "jan", "omit")), std::string("omit"));
    QVERIFY(ui::color_key(ColorBy::Tag, row("air", "jan", "invalid")).empty());
    QVERIFY(ui::color_key(ColorBy::Tag, row("air", "jan", "ok")).empty());
    QCOMPARE(ui::row_color(ColorBy::Tag, row("air", "jan", "invalid"), types, keys, t), t.error_bg);
    QCOMPARE(ui::row_color(ColorBy::Tag, row("air", "jan", "omit"), types, keys, t), t.row_category[0]);
    QCOMPARE(ui::row_color(ColorBy::Tag, row("air", "jan", "skip"), types, keys, t), t.row_category[1]);
    QVERIFY(!ui::row_color(ColorBy::Tag, row("air", "jan", "ok"), types, keys, t).isValid());
    QVERIFY(!ui::row_color(ColorBy::Tag, row("air", "jan", ""), types, keys, t).isValid());
  }

  void categories_go_by_the_sorted_values_and_come_round_again() {
    const auto& t = ui::theme();
    const ui::TypeColors types = ui::default_type_colors(t);
    std::vector<std::string> keys;
    for (char c = 'a'; c < 'a' + 10; ++c) keys.emplace_back(1, c);
    QCOMPARE(ui::row_color(ColorBy::Spectrometer, row("air", "a"), types, keys, t), t.row_category[0]);
    QCOMPARE(ui::row_color(ColorBy::Spectrometer, row("air", "h"), types, keys, t), t.row_category[7]);
    QCOMPARE(ui::row_color(ColorBy::Spectrometer, row("air", "i"), types, keys, t), t.row_category[0]);
    QVERIFY(!ui::row_color(ColorBy::Spectrometer, row("air", "z"), types, keys, t).isValid());  // not a key shown
    for (const QColor& c : t.row_category) QVERIFY(c.isValid() && c != t.error_bg);
  }

  void by_level_an_analysis_with_no_irradiation_has_no_tint() {
    const auto& t = ui::theme();
    const ui::TypeColors types = ui::default_type_colors(t);
    pp::AnalysisSummary a = row("unknown");
    QVERIFY(ui::color_key(ColorBy::IrradiationLevel, a).empty());
    QVERIFY(!ui::row_color(ColorBy::IrradiationLevel, a, types, {"NM-293 A"}, t).isValid());
    a.irradiation = "NM-293";
    a.level = "A";
    QCOMPARE(ui::color_key(ColorBy::IrradiationLevel, a), std::string("NM-293 A"));
    QCOMPARE(ui::row_color(ColorBy::IrradiationLevel, a, types, {"NM-293 A"}, t), t.row_category[0]);
  }

  void chosen_colours_are_kept_as_what_differs_from_the_theme() {
    const auto& t = ui::theme();
    QVERIFY(ui::type_color_overrides(ui::default_type_colors(t), t).empty());
    ui::TypeColors mine = ui::default_type_colors(t);
    mine.unknown = QColor(0xAB, 0xCD, 0xEF);
    mine.blank = QColor();
    const ui::TypeColorOverrides kept = ui::type_color_overrides(mine, t);
    QCOMPARE(kept, (ui::TypeColorOverrides{{"unknown", "#abcdef"}, {"blank", ""}}));
    QCOMPARE(ui::type_colors(kept, t), mine);
    // What is not a class or not a colour is passed over.
    QCOMPARE(ui::type_colors({{"nonsense", "#000000"}, {"air", "red"}, {"cocktail", "#12345"}}, t),
             ui::default_type_colors(t));
  }

  void the_model_tints_by_the_choice() {
    const auto& t = ui::theme();
    ui::AnalysisTableModel model;
    model.set_rows({row("air", "obama"), row("blank_air", "jan"), row("unknown", "obama", "omit")});
    QCOMPARE(tint(model, 0), t.row_air);
    QCOMPARE(tint(model, 1), t.row_blank);
    QCOMPARE(tint(model, 2), t.error_bg);

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.set_coloring(ColorBy::Spectrometer, ui::default_type_colors(t));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(tint(model, 0), t.row_category[1]);  // obama, after jan
    QCOMPARE(tint(model, 1), t.row_category[0]);
    QCOMPARE(tint(model, 2), t.error_bg);

    model.set_coloring(ColorBy::Tag, ui::default_type_colors(t));
    QVERIFY(!tint(model, 0).isValid());
    QCOMPARE(tint(model, 2), t.row_category[0]);  // omit

    model.set_coloring(ColorBy::None, ui::default_type_colors(t));
    QVERIFY(!tint(model, 0).isValid());
    QCOMPARE(tint(model, 2), t.error_bg);
  }
};

QTEST_MAIN(RowColorsTest)
#include "test_row_colors.moc"
