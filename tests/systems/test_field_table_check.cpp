#include "pychron/systems/spectrometer/field_table_check.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"

namespace ps = pychron::spectrometer;
namespace cfg = pychron::spectrometer::cfg;

namespace {

constexpr double kAr36 = 35.967545;
constexpr double kAr38 = 37.962732;
constexpr double kAr40 = 39.962383;

// Monotonic over [36, 40]: vertex of 0.5 + 0.1 m + 0.001 m^2 is at m = -50.
double curve(double m) { return 0.5 + 0.1 * m + 0.001 * m * m; }

ps::FieldTable small_table(ps::FitKind fit = ps::FitKind::Quadratic) {
  std::vector<ps::ControlPoint> pts;
  for (const auto& [iso, mass] : std::vector<std::pair<std::string, double>>{
           {"Ar36", kAr36}, {"Ar38", kAr38}, {"Ar40", kAr40}}) {
    pts.push_back({iso, mass, {{"H1", curve(mass)}, {"AX", curve(mass) - 0.2}}});
  }
  return ps::FieldTable(fit, ps::TableAxis::Dac, std::move(pts));
}

cfg::SpectrometerConfig config() {
  cfg::SpectrometerConfig c;
  c.magnet.field_table = "argon";
  cfg::DetectorConfig h1;
  h1.name = "H1";
  cfg::DetectorConfig ax;
  ax.name = "AX";
  c.detectors = {h1, ax};
  return c;
}

const ps::MolecularWeights& weights() {
  static const ps::MolecularWeights w(std::map<std::string, double, std::less<>>{{"Ar36", kAr36}, {"Ar38", kAr38}, {"Ar40", kAr40}});
  return w;
}

ps::TableDiagnostics check(const ps::FieldTable& t, const cfg::SpectrometerConfig& c = config(),
                           std::string_view name = "argon") {
  return ps::check_field_table(t, c, weights(), name);
}

std::vector<ps::TableDiagnostic> of(const ps::TableDiagnostics& d, ps::TableSeverity s) {
  std::vector<ps::TableDiagnostic> out;
  std::copy_if(d.begin(), d.end(), std::back_inserter(out), [&](const auto& e) { return e.severity == s; });
  return out;
}

std::string dump(const ps::TableDiagnostics& d) {
  std::string out;
  for (const auto& e : d) out += ps::to_string(e) + "\n";
  return out;
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(CheckFieldTable, CleanTableHasNoDiagnostics) {
  const auto d = check(small_table());
  EXPECT_TRUE(d.empty()) << dump(d);
}

TEST(CheckFieldTable, EmptyIsotope) {
  auto t = small_table();
  t.mutable_points()[1].isotope = "  ";
  const auto errs = of(check(t), ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U) << dump(check(t));
  EXPECT_EQ(errs[0].point, std::optional<std::size_t>(1));
  EXPECT_TRUE(errs[0].detector.empty());
  EXPECT_TRUE(contains(errs[0].message, "empty")) << errs[0].message;
}

TEST(CheckFieldTable, DuplicateIsotope) {
  auto t = small_table();
  t.mutable_points()[2].isotope = "Ar36";
  t.mutable_points()[2].mass = kAr36 + 4.0;  // keep the fit well determined
  const auto errs = of(check(t), ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U) << dump(check(t));
  EXPECT_EQ(errs[0].point, std::optional<std::size_t>(2));
  EXPECT_TRUE(contains(errs[0].message, "duplicate")) << errs[0].message;
}

TEST(CheckFieldTable, IsotopeNamesAreTrimmedBeforeTheDuplicateCheck) {
  auto t = small_table();
  t.mutable_points()[1].isotope = "Ar40 ";
  const auto d = check(t);
  const auto errs = of(d, ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U) << dump(d);
  EXPECT_EQ(errs[0].point, std::optional<std::size_t>(2));
  EXPECT_TRUE(contains(errs[0].message, "duplicate")) << errs[0].message;
}

TEST(CheckFieldTable, MassNotFiniteOrNotPositive) {
  for (double bad : {std::numeric_limits<double>::quiet_NaN(), 0.0, -1.0}) {
    auto t = small_table();
    t.mutable_points()[0].mass = bad;
    const auto d = check(t);
    const auto errs = of(d, ps::TableSeverity::Error);
    const bool found = std::any_of(errs.begin(), errs.end(), [](const auto& e) {
      return e.point == std::optional<std::size_t>(0) && e.detector.empty() && contains(e.message, "mass");
    });
    EXPECT_TRUE(found) << "mass " << bad << "\n" << dump(d);
    EXPECT_TRUE(ps::has_error(d));
  }
}

TEST(CheckFieldTable, ValueNotFinite) {
  auto t = small_table();
  t.mutable_points()[1].values["AX"] = std::numeric_limits<double>::infinity();
  const auto d = check(t);
  const auto errs = of(d, ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U) << dump(d);
  EXPECT_EQ(errs[0].point, std::optional<std::size_t>(1));
  EXPECT_EQ(errs[0].detector, "AX");
  // No fit-dependent warning for a detector with a broken value.
  for (const auto& w : of(d, ps::TableSeverity::Warning)) EXPECT_NE(w.detector, "AX") << dump(d);
}

TEST(CheckFieldTable, FitFailsWithTooFewPoints) {
  auto t = small_table();
  t.set_fit("H1", ps::FitKind::Cubic);
  const auto fit = t.check_fit("H1");
  ASSERT_FALSE(fit.has_value());
  const auto d = check(t);
  const auto errs = of(d, ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U) << dump(d);
  EXPECT_EQ(errs[0].detector, "H1");
  EXPECT_FALSE(errs[0].point.has_value());
  EXPECT_EQ(errs[0].message, fit.error().what);
  EXPECT_TRUE(of(d, ps::TableSeverity::Warning).empty()) << dump(d);
}

TEST(CheckFieldTable, RepeatedMasses) {
  auto t = small_table();
  t.mutable_points()[1].mass = kAr36;
  const auto fit = t.check_fit("H1");
  ASSERT_FALSE(fit.has_value());
  EXPECT_TRUE(contains(fit.error().what, "repeated masses")) << fit.error().what;
  const auto errs = of(check(t), ps::TableSeverity::Error);
  const bool found = std::any_of(errs.begin(), errs.end(),
                                 [&](const auto& e) { return e.detector == "H1" && e.message == fit.error().what; });
  EXPECT_TRUE(found) << dump(check(t));
}

TEST(CheckFieldTable, ActiveDetectorWithNoValueWhenTableIsTheFieldTable) {
  auto c = config();
  cfg::DetectorConfig cdd;
  cdd.name = "CDD";
  c.detectors.push_back(cdd);
  const auto errs = of(check(small_table(), c, "argon"), ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U);
  EXPECT_EQ(errs[0].detector, "CDD");
  EXPECT_FALSE(errs[0].point.has_value());

  // An inactive one is not required.
  c.detectors.back().active = false;
  EXPECT_FALSE(ps::has_error(check(small_table(), c, "argon")));
}

TEST(CheckFieldTable, ActiveDetectorWithNoValueWhenTableIsTheHvTable) {
  auto c = config();
  c.magnet.hv_table = "argon_hv";
  cfg::DetectorConfig cdd;
  cdd.name = "CDD";
  c.detectors.push_back(cdd);
  const auto errs = of(check(small_table(), c, "argon_hv"), ps::TableSeverity::Error);
  ASSERT_EQ(errs.size(), 1U);
  EXPECT_EQ(errs[0].detector, "CDD");
}

TEST(CheckFieldTable, ActiveDetectorMissingIsNotAnErrorForAnotherTableName) {
  auto c = config();
  cfg::DetectorConfig cdd;
  cdd.name = "CDD";
  c.detectors.push_back(cdd);
  EXPECT_FALSE(ps::has_error(check(small_table(), c, "spare")));
}

TEST(CheckFieldTable, NonMonotonicFit) {
  auto t = small_table();
  // H1 = (m - 38)^2: vertex inside [36, 40].
  for (auto& p : t.mutable_points()) p.values["H1"] = (p.mass - 38.0) * (p.mass - 38.0);
  const auto d = check(t);
  EXPECT_FALSE(ps::has_error(d)) << dump(d);
  const auto warns = of(d, ps::TableSeverity::Warning);
  ASSERT_EQ(warns.size(), 1U) << dump(d);
  EXPECT_EQ(warns[0].detector, "H1");
  EXPECT_FALSE(warns[0].point.has_value());
  EXPECT_TRUE(contains(warns[0].message, "monotonic")) << warns[0].message;
}

TEST(CheckFieldTable, ResidualAboveOnePercentOfSpan) {
  auto with_offset = [](double fraction) {
    auto t = small_table(ps::FitKind::Linear);
    auto& pts = t.mutable_points();
    for (auto& p : pts) p.values["H1"] = 0.25 * p.mass;  // span = 0.25 * (40 - 36)
    const double span = 0.25 * (pts[2].mass - pts[0].mass);
    pts[1].values["H1"] += fraction * span;
    return t;
  };
  // A middle point off by f*span leaves residual 2/3 f*span there; 2 % => 1.33 %.
  const auto d = check(with_offset(0.02));
  const auto warns = of(d, ps::TableSeverity::Warning);
  ASSERT_FALSE(warns.empty()) << dump(d);
  for (const auto& w : warns) {
    EXPECT_EQ(w.detector, "H1");
    EXPECT_TRUE(w.point.has_value());
    EXPECT_TRUE(contains(w.message, "residual")) << w.message;
  }
  EXPECT_TRUE(std::any_of(warns.begin(), warns.end(),
                          [](const auto& w) { return w.point == std::optional<std::size_t>(1); }));

  const auto quiet = check(with_offset(0.005));
  EXPECT_TRUE(quiet.empty()) << dump(quiet);
}

TEST(CheckFieldTable, DetectorNotInConfig) {
  auto t = small_table();
  for (auto& p : t.mutable_points()) p.values["H2"] = curve(p.mass) + 0.4;
  const auto d = check(t);
  const auto warns = of(d, ps::TableSeverity::Warning);
  ASSERT_EQ(warns.size(), 1U) << dump(d);
  EXPECT_EQ(warns[0].detector, "H2");
  EXPECT_TRUE(contains(warns[0].message, "not in config")) << warns[0].message;
  EXPECT_FALSE(ps::has_error(d));
}

TEST(CheckFieldTable, IsotopeNotInWeights) {
  auto t = small_table();
  t.mutable_points()[1].isotope = "Xx38";
  const auto d = check(t);
  const auto warns = of(d, ps::TableSeverity::Warning);
  ASSERT_EQ(warns.size(), 1U) << dump(d);
  EXPECT_EQ(warns[0].point, std::optional<std::size_t>(1));
  EXPECT_FALSE(ps::has_error(d));
}

TEST(CheckFieldTable, MassDiffersFromWeightBy0_01Amu) {
  auto with_shift = [](double shift) {
    auto t = small_table();
    t.mutable_points()[1].mass += shift;
    return t;
  };
  const auto d = check(with_shift(0.011));
  const auto warns = of(d, ps::TableSeverity::Warning);
  ASSERT_EQ(warns.size(), 1U) << dump(d);
  EXPECT_EQ(warns[0].point, std::optional<std::size_t>(1));
  EXPECT_TRUE(contains(warns[0].message, "weight")) << warns[0].message;

  const auto quiet = check(with_shift(0.009));
  EXPECT_TRUE(quiet.empty()) << dump(quiet);
}

TEST(CheckFieldTable, HasErrorAndFirstError) {
  ps::TableDiagnostics d;
  EXPECT_FALSE(ps::has_error(d));
  EXPECT_EQ(ps::first_error(d), nullptr);
  d.push_back({ps::TableSeverity::Warning, std::nullopt, "H2", "w"});
  EXPECT_FALSE(ps::has_error(d));
  EXPECT_EQ(ps::first_error(d), nullptr);
  d.push_back({ps::TableSeverity::Error, std::size_t{0}, "H1", "e1"});
  d.push_back({ps::TableSeverity::Error, std::nullopt, "", "e2"});
  EXPECT_TRUE(ps::has_error(d));
  ASSERT_NE(ps::first_error(d), nullptr);
  EXPECT_EQ(ps::first_error(d)->message, "e1");
}

TEST(CheckFieldTable, ToStringNamesSeverityRowAndDetector) {
  EXPECT_EQ(ps::to_string(ps::TableDiagnostic{ps::TableSeverity::Error, std::size_t{2}, "H1", "bad"}),
            "error: row 3/H1: bad");
  EXPECT_EQ(ps::to_string(ps::TableDiagnostic{ps::TableSeverity::Warning, std::nullopt, "H2", "odd"}),
            "warning: H2: odd");
  EXPECT_EQ(ps::to_string(ps::TableDiagnostic{ps::TableSeverity::Error, std::size_t{0}, "", "x"}),
            "error: row 1: x");
  EXPECT_EQ(ps::to_string(ps::TableDiagnostic{ps::TableSeverity::Error, std::nullopt, "", "t"}), "error: t");
}

TEST(FieldTable, ClearFitFallsBackToDefault) {
  auto t = small_table();
  t.set_fit("H1", ps::FitKind::Linear);
  EXPECT_EQ(t.fit("H1"), ps::FitKind::Linear);
  t.clear_fit("H1");
  EXPECT_EQ(t.fit("H1"), ps::FitKind::Quadratic);
  EXPECT_TRUE(t.fit_overrides().empty());
  t.clear_fit("nobody");  // no override: no-op
  EXPECT_TRUE(t.fit_overrides().empty());
}

TEST(FieldTable, SetDefaultFit) {
  auto t = small_table();
  t.set_fit("AX", ps::FitKind::Quadratic);
  t.set_default_fit(ps::FitKind::Linear);
  EXPECT_EQ(t.default_fit(), ps::FitKind::Linear);
  EXPECT_EQ(t.fit("H1"), ps::FitKind::Linear);
  EXPECT_EQ(t.fit("AX"), ps::FitKind::Quadratic);
}

TEST(FieldTable, CheckFitMatchesValueFor) {
  auto t = small_table();
  EXPECT_TRUE(t.check_fit("H1").has_value());
  EXPECT_FALSE(t.check_fit("CDD").has_value());
  EXPECT_EQ(t.check_fit("CDD").error().what, t.value_for(kAr40, "CDD").error().what);
  t.set_fit("H1", ps::FitKind::Cubic);
  EXPECT_EQ(t.check_fit("H1").error().what, t.value_for(kAr40, "H1").error().what);
  t.set_fit("H1", ps::FitKind::Discrete);
  EXPECT_TRUE(t.check_fit("H1").has_value());
}

// ---- diff_field_tables ----

namespace {

const ps::CellDiff* cell(const ps::TableDiff& d, std::string_view iso, std::string_view det) {
  const auto it = std::find_if(d.cells.begin(), d.cells.end(),
                               [&](const ps::CellDiff& c) { return c.isotope == iso && c.detector == det; });
  return it == d.cells.end() ? nullptr : &*it;
}

}  // namespace

TEST(DiffFieldTables, IdenticalIsEmpty) {
  const auto t = small_table();
  const auto d = ps::diff_field_tables(t, t);
  EXPECT_TRUE(d.empty());
  EXPECT_TRUE(d.fit_changes.empty());
  EXPECT_TRUE(d.rows.empty());
  EXPECT_TRUE(d.masses.empty());
  EXPECT_TRUE(d.cells.empty());
}

TEST(DiffFieldTables, MatchesByIsotopeNotRowIndex) {
  const auto a = small_table();
  auto b = small_table();
  std::reverse(b.mutable_points().begin(), b.mutable_points().end());
  EXPECT_TRUE(ps::diff_field_tables(a, b).empty());
}

TEST(DiffFieldTables, AddedAndRemovedRows) {
  const auto a = small_table();
  auto b = small_table();
  auto& pts = b.mutable_points();
  pts.erase(pts.begin());  // Ar36 removed
  pts.push_back({"Ar39", 38.964, {{"H1", 4.0}}});
  pts.push_back({"Ar37", 36.966, {{"H1", 3.9}}});
  const auto d = ps::diff_field_tables(a, b);
  ASSERT_EQ(d.rows.size(), 3U);
  EXPECT_EQ(d.rows[0].isotope, "Ar39");
  EXPECT_EQ(d.rows[0].change, ps::RowChange::Added);
  EXPECT_EQ(d.rows[1].isotope, "Ar37");
  EXPECT_EQ(d.rows[1].change, ps::RowChange::Added);
  EXPECT_EQ(d.rows[2].isotope, "Ar36");
  EXPECT_EQ(d.rows[2].change, ps::RowChange::Removed);
  EXPECT_TRUE(d.cells.empty());  // added and removed rows have no cell diffs
  EXPECT_TRUE(d.masses.empty());
  EXPECT_FALSE(d.empty());
}

TEST(DiffFieldTables, ChangedValueHasOldNewAndDelta) {
  const auto a = small_table();
  auto b = small_table();
  b.mutable_points()[2].values["H1"] += 0.025;
  const auto d = ps::diff_field_tables(a, b);
  ASSERT_EQ(d.cells.size(), 1U);
  const auto& c = d.cells[0];
  EXPECT_EQ(c.isotope, "Ar40");
  EXPECT_EQ(c.detector, "H1");
  ASSERT_TRUE(c.old_value.has_value());
  ASSERT_TRUE(c.new_value.has_value());
  EXPECT_DOUBLE_EQ(*c.old_value, curve(kAr40));
  EXPECT_DOUBLE_EQ(*c.new_value, curve(kAr40) + 0.025);
  EXPECT_NEAR(*c.new_value - *c.old_value, 0.025, 1e-12);
  EXPECT_FALSE(d.empty());
}

TEST(DiffFieldTables, ValueOnlyOnOneSideHasNullopt) {
  const auto a = small_table();
  auto b = small_table();
  b.mutable_points()[0].values.erase("AX");
  b.mutable_points()[1].values["CDD"] = 7.5;
  const auto d = ps::diff_field_tables(a, b);
  ASSERT_EQ(d.cells.size(), 2U);
  const auto* gone = cell(d, "Ar36", "AX");
  ASSERT_NE(gone, nullptr);
  EXPECT_TRUE(gone->old_value.has_value());
  EXPECT_FALSE(gone->new_value.has_value());
  const auto* added = cell(d, "Ar38", "CDD");
  ASSERT_NE(added, nullptr);
  EXPECT_FALSE(added->old_value.has_value());
  ASSERT_TRUE(added->new_value.has_value());
  EXPECT_DOUBLE_EQ(*added->new_value, 7.5);
}

// Legacy compare matched by header and row index, and failed when the two
// versions had different detector columns.
TEST(DiffFieldTables, DifferentDetectorSetsStillMatchRows) {
  const auto a = small_table();
  auto b = small_table();
  for (auto& p : b.mutable_points()) {
    p.values.erase("AX");
    p.values["H2"] = p.values.at("H1") + 0.3;
  }
  const auto d = ps::diff_field_tables(a, b);
  EXPECT_TRUE(d.rows.empty());
  EXPECT_TRUE(d.masses.empty());
  ASSERT_EQ(d.cells.size(), 6U);  // 3 isotopes x (AX removed, H2 added)
  for (const auto* iso : {"Ar36", "Ar38", "Ar40"}) {
    EXPECT_NE(cell(d, iso, "AX"), nullptr) << iso;
    EXPECT_NE(cell(d, iso, "H2"), nullptr) << iso;
    EXPECT_EQ(cell(d, iso, "H1"), nullptr) << iso;
  }
}

TEST(DiffFieldTables, FitAndOverrideChangesListed) {
  auto a = small_table();
  a.set_fit("AX", ps::FitKind::Linear);
  a.set_fit("H1", ps::FitKind::Cubic);
  auto b = small_table(ps::FitKind::Linear);
  b.set_fit("H1", ps::FitKind::Quadratic);
  b.set_fit("CDD", ps::FitKind::Cubic);
  const auto d = ps::diff_field_tables(a, b);
  EXPECT_EQ(d.fit_changes, (std::vector<std::string>{
                               "fit: quadratic -> linear",
                               "fit AX: linear -> default",
                               "fit CDD: default -> cubic",
                               "fit H1: cubic -> quadratic",
                           }));
  EXPECT_TRUE(d.cells.empty());
  EXPECT_FALSE(d.empty());
}

TEST(DiffFieldTables, MassChangeListed) {
  const auto a = small_table();
  auto b = small_table();
  b.mutable_points()[1].mass = 37.9627;
  const auto d = ps::diff_field_tables(a, b);
  ASSERT_EQ(d.masses.size(), 1U);
  EXPECT_EQ(d.masses[0].isotope, "Ar38");
  EXPECT_DOUBLE_EQ(d.masses[0].old_mass, kAr38);
  EXPECT_DOUBLE_EQ(d.masses[0].new_mass, 37.9627);
  EXPECT_TRUE(d.cells.empty());
  EXPECT_FALSE(d.empty());
}

TEST(DiffFieldTables, AxisChangeListedFirst) {
  const auto a = small_table();
  const ps::FieldTable b(ps::FitKind::Linear, ps::TableAxis::Field, a.points());
  const auto d = ps::diff_field_tables(a, b);
  EXPECT_EQ(d.fit_changes, (std::vector<std::string>{"axis: dac -> field", "fit: quadratic -> linear"}));
}
