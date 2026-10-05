#include "pychron/processing/report.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "fixtures.hpp"
#include "pychron/processing/quantity.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
using pp::test::make_air;
using pp::test::make_step;
using pp::test::make_unknown;

namespace {

// Eight steps of one aliquot, grouped as one group named by the aliquot.
// `f_of_step` lets one step disagree with the others.
pp::Dataset steps(int n = 8, std::map<int, double> f_of_step = {}) {
  pp::Dataset d;
  const double ar39[] = {5, 20, 40, 60, 50, 30, 15, 5, 8, 12};
  const double ar36[] = {0.5, 0.3, 0.2, 0.1, 0.08, 0.06, 0.1, 0.2, 0.3, 0.4};
  for (int i = 0; i < n; ++i) {
    pp::DatasetItem item;
    const double f = f_of_step.count(i) ? f_of_step[i] : 10.0;
    auto a = make_step(i, ar39[i], ar36[i], f);
    a->material = "sanidine";
    a->project = "Fish Canyon";
    a->irradiation = "NM-300";
    a->level = "A";
    a->position = "3";
    a->mass_spectrometer = "jan";
    a->extract_device = "co2";
    a->analyst = "jross";
    a->sample_info.latitude = 37.75;
    a->sample_info.longitude = -106.9;
    a->sample_info.elevation = 2850.0;
    a->sample_info.lithology = "ash-flow tuff";
    a->sample_info.igsn = "IEFC20001";
    a->monitor.name = "FC-2";
    a->monitor.material = "sanidine";
    a->monitor.age = pp::Value{28.201, 0.023};
    a->context.reactor = "Triga";
    item.analysis = pp::reduce_analysis(a, {});
    d.mutable_items().push_back(item);
  }
  d.group_names = {"S1-1"};
  d.reduction_tag = "arar-1/default/d0";
  return d;
}

const std::vector<pp::ReportCell>& row(const pp::ReportTable& t, std::size_t i) { return t.rows.at(i); }

std::size_t column(const pp::ReportTable& t, const std::string& name) {
  for (std::size_t i = 0; i < t.columns.size(); ++i)
    if (t.columns[i] == name) return i;
  ADD_FAILURE() << "no column " << name << " in " << t.name;
  return 0;
}

std::optional<double> number(const pp::ReportTable& t, std::size_t r, const std::string& col) {
  const auto& cell = row(t, r).at(column(t, col));
  if (const auto* d = std::get_if<double>(&cell)) return *d;
  return std::nullopt;
}

std::string text(const pp::ReportTable& t, std::size_t r, const std::string& col) {
  const auto& cell = row(t, r).at(column(t, col));
  if (const auto* s = std::get_if<std::string>(&cell)) return *s;
  return {};
}

std::string metadata(const pp::Report& rep, const std::string& item) {
  for (const auto& r : rep.metadata.rows)
    if (std::get<std::string>(r.at(0)) == item) {
      if (const auto* s = std::get_if<std::string>(&r.at(1))) return *s;
      if (const auto* d = std::get_if<double>(&r.at(1))) return std::to_string(*d);
    }
  return {};
}

double age_of_f(double f) {
  auto d = steps(1, {{0, f}});
  return pp::Quantity::parse("age")->eval(*d.items()[0].analysis)->value;
}

// RFC 4180 fields of one line.
std::vector<std::string> csv_fields(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (quoted) {
      if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        field += '"';
        ++i;
      } else if (ch == '"') {
        quoted = false;
      } else {
        field += ch;
      }
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == ',') {
      out.push_back(field);
      field.clear();
    } else {
      field += ch;
    }
  }
  out.push_back(field);
  return out;
}

TEST(Report, EveryTableHasOneCellPerColumn) {
  const auto rep = pp::make_report(steps());
  for (const auto* t : {&rep.metadata, &rep.constants, &rep.irradiation, &rep.analyses, &rep.summary}) {
    EXPECT_FALSE(t->columns.empty()) << t->name;
    for (const auto& r : t->rows) EXPECT_EQ(r.size(), t->columns.size()) << t->name;
  }
  EXPECT_EQ(rep.table("analyses"), &rep.analyses);
  EXPECT_EQ(rep.table("nothing"), nullptr);
  EXPECT_TRUE(rep.warnings.empty()) << rep.warnings.front();
}

TEST(Report, AnalysesRowsInStepOrderWithCumulativeGas) {
  const auto rep = pp::make_report(steps());
  const auto& t = rep.analyses;
  ASSERT_EQ(t.rows.size(), 8u);
  const char* letters[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
  double sum = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(text(t, i, "step"), letters[i]);
    EXPECT_EQ(text(t, i, "run id"), std::string("S1-01") + letters[i]);
    EXPECT_EQ(text(t, i, "group"), "S1-1");
    EXPECT_EQ(text(t, i, "included"), "yes");
    EXPECT_EQ(text(t, i, "sample"), "FC-2");
    EXPECT_EQ(text(t, i, "material"), "sanidine");
    sum += *number(t, i, "39ArK (% of group)");
    EXPECT_NEAR(*number(t, i, "39ArK cumulative (%)"), sum, 1e-9);
    EXPECT_TRUE(number(t, i, "40Ar (fA)"));
    EXPECT_TRUE(number(t, i, "36Ar blank (fA)"));
    EXPECT_NEAR(*number(t, i, "40Ar*/39ArK"), 10.0, 1e-9);
    EXPECT_NEAR(*number(t, i, "age (Ma)"), age_of_f(10.0), 1e-9);
    EXPECT_GT(*number(t, i, "age (Ma) ±(2s) with J"), *number(t, i, "age (Ma) ±(2s) analytical"));
    EXPECT_TRUE(number(t, i, "36Ar/40Ar"));
    EXPECT_TRUE(number(t, i, "rho (39/40, 36/40)"));
    EXPECT_EQ(text(t, i, "note"), "");
  }
  EXPECT_NEAR(sum, 100.0, 1e-9);
  EXPECT_NEAR(*number(t, 7, "39ArK cumulative (%)"), 100.0, 1e-9);
  // The row's intensity is the decay-corrected stage at the requested sigma.
  const auto same = steps();
  const auto& ra = *same.items()[0].analysis;
  EXPECT_NEAR(*number(t, 0, "40Ar (fA)"), ra.stage("Ar40", pp::Stage::DecayCorrected)->nominal(), 1e-9);
  EXPECT_NEAR(*number(t, 0, "40Ar (fA) ±(2s)"), 2 * ra.stage("Ar40", pp::Stage::DecayCorrected)->std_dev(), 1e-12);
}

TEST(Report, SummaryHasPlateauIntegratedMeanAndIsochron) {
  const auto rep = pp::make_report(steps());
  const auto& t = rep.summary;
  ASSERT_EQ(t.rows.size(), 1u);
  const double age = age_of_f(10.0);
  EXPECT_EQ(text(t, 0, "group"), "S1-1");
  EXPECT_EQ(text(t, 0, "step heated"), "yes");
  EXPECT_EQ(*number(t, 0, "analyses"), 8.0);
  EXPECT_EQ(*number(t, 0, "included"), 8.0);
  EXPECT_NEAR(*number(t, 0, "integrated age (Ma)"), age, 1e-9);
  EXPECT_EQ(text(t, 0, "plateau steps"), "A-H");
  EXPECT_EQ(*number(t, 0, "plateau n"), 8.0);
  EXPECT_NEAR(*number(t, 0, "plateau 39ArK (%)"), 100.0, 1e-9);
  EXPECT_NEAR(*number(t, 0, "plateau age (Ma)"), age, 1e-9);
  EXPECT_GT(*number(t, 0, "plateau age ±(2s) with J"), *number(t, 0, "plateau age ±(2s) analytical"));
  EXPECT_TRUE(number(t, 0, "plateau MSWD"));
  EXPECT_TRUE(number(t, 0, "plateau p"));
  EXPECT_EQ(*number(t, 0, "weighted mean n"), 8.0);
  EXPECT_NEAR(*number(t, 0, "weighted mean age (Ma)"), age, 1e-9);
  EXPECT_EQ(*number(t, 0, "isochron n"), 8.0);
  EXPECT_NEAR(*number(t, 0, "isochron 40Ar/36Ar trapped"), 298.56, 0.5);
  EXPECT_NEAR(*number(t, 0, "isochron age (Ma)"), age, 1e-3);
  EXPECT_TRUE(number(t, 0, "isochron MSWD"));
  EXPECT_EQ(text(t, 0, "note"), "");
}

TEST(Report, ExcludedAnalysesLeaveTheStatistics) {
  // Step D disagrees; the user excludes it and step A.
  auto d = steps(8, {{3, 12.0}});
  d.mutable_items()[0].exclusion.user = true;
  d.mutable_items()[3].exclusion.user = true;
  pp::ReportOptions o;
  const auto rep = pp::make_report(d, o);
  EXPECT_EQ(text(rep.analyses, 0, "included"), "no");
  EXPECT_EQ(text(rep.analyses, 3, "included"), "no");
  EXPECT_EQ(text(rep.analyses, 1, "included"), "yes");
  const auto& s = rep.summary;
  EXPECT_EQ(*number(s, 0, "included"), 6.0);
  EXPECT_EQ(*number(s, 0, "weighted mean n"), 6.0);
  EXPECT_NEAR(*number(s, 0, "weighted mean age (Ma)"), age_of_f(10.0), 1e-9);
  const auto plateau_n = number(s, 0, "plateau n");
  ASSERT_TRUE(plateau_n);
  EXPECT_LE(*plateau_n, 6.0);
  EXPECT_EQ(*number(s, 0, "isochron n"), 6.0);
  // The integrated age uses every step by default, and only the included ones when asked.
  const double with_all = *number(s, 0, "integrated age (Ma)");
  o.integrated_includes_excluded = false;
  const auto rep2 = pp::make_report(d, o);
  const double included_only = *number(rep2.summary, 0, "integrated age (Ma)");
  EXPECT_GT(with_all, included_only);
  EXPECT_NEAR(included_only, age_of_f(10.0), 1e-9);
}

TEST(Report, UncertaintiesScaleWithTheSigmaLevel) {
  pp::ReportOptions one;
  one.nsigma = 1;
  pp::ReportOptions two;
  two.nsigma = 2;
  const auto d = steps();
  const auto r1 = pp::make_report(d, one);
  const auto r2 = pp::make_report(d, two);
  EXPECT_NEAR(*number(r2.analyses, 0, "40Ar (fA) ±(2s)"), 2 * *number(r1.analyses, 0, "40Ar (fA) ±(1s)"), 1e-12);
  EXPECT_NEAR(*number(r2.analyses, 0, "age (Ma) ±(2s) analytical"),
              2 * *number(r1.analyses, 0, "age (Ma) ±(1s) analytical"), 1e-12);
  EXPECT_NEAR(*number(r2.irradiation, 0, "J ±(2s)"), 2 * *number(r1.irradiation, 0, "J ±(1s)"), 1e-15);
  EXPECT_NEAR(*number(r2.summary, 0, "plateau age ±(2s) analytical"),
              2 * *number(r1.summary, 0, "plateau age ±(1s) analytical"), 1e-12);
  EXPECT_EQ(metadata(r2, "uncertainty level"), "2 sigma (every ± column)");
  EXPECT_EQ(metadata(r1, "uncertainty level"), "1 sigma (every ± column)");
  EXPECT_TRUE(r2.header[2].find("2 sigma") != std::string::npos) << r2.header[2];
}

TEST(Report, IrradiationRowPerIdentifierWithSampleMonitorAndProduction) {
  auto d = steps();
  // A second identifier, without any of the metadata.
  pp::DatasetItem other;
  other.analysis = pp::reduce_analysis(make_unknown(0), {});
  d.mutable_items().push_back(other);
  d.group_names = {"S1-1", "U1-1"};
  d.mutable_items().back().path.group = 1;
  const auto rep = pp::make_report(d);
  const auto& t = rep.irradiation;
  ASSERT_EQ(t.rows.size(), 2u);
  EXPECT_EQ(text(t, 0, "identifier"), "S1");
  EXPECT_EQ(text(t, 0, "sample"), "FC-2");
  EXPECT_EQ(text(t, 0, "project"), "Fish Canyon");
  EXPECT_NEAR(*number(t, 0, "latitude (deg)"), 37.75, 1e-12);
  EXPECT_NEAR(*number(t, 0, "longitude (deg)"), -106.9, 1e-12);
  EXPECT_NEAR(*number(t, 0, "elevation (m)"), 2850.0, 1e-12);
  EXPECT_EQ(text(t, 0, "lithology"), "ash-flow tuff");
  EXPECT_EQ(text(t, 0, "IGSN"), "IEFC20001");
  EXPECT_EQ(text(t, 0, "irradiation"), "NM-300");
  EXPECT_EQ(text(t, 0, "level"), "A");
  EXPECT_EQ(text(t, 0, "position"), "3");
  EXPECT_EQ(text(t, 0, "reactor"), "Triga");
  EXPECT_EQ(text(t, 0, "fluence monitor"), "FC-2");
  EXPECT_NEAR(*number(t, 0, "monitor age (Ma)"), 28.201, 1e-12);
  EXPECT_NEAR(*number(t, 0, "monitor age (Ma) ±(2s)"), 0.046, 1e-12);
  EXPECT_NEAR(*number(t, 0, "J"), 0.001, 1e-15);
  EXPECT_NEAR(*number(t, 0, "J ±(2s)"), 2e-6, 1e-15);
  EXPECT_NEAR(*number(t, 0, "irradiation duration (h)"), 1.0, 1e-9);
  EXPECT_EQ(*number(t, 0, "irradiation segments"), 1.0);
  EXPECT_FALSE(text(t, 0, "irradiation start (UTC)").empty());
  EXPECT_EQ(*number(t, 0, "(40Ar/39Ar)K"), 0.0);  // the fixture's production ratios are zero
  EXPECT_FALSE(number(t, 0, "Ca/K"));             // optional ratios the source does not have
  EXPECT_EQ(text(t, 1, "identifier"), "U1");
  EXPECT_FALSE(number(t, 1, "latitude (deg)"));
  EXPECT_EQ(text(t, 1, "fluence monitor"), "");
  EXPECT_EQ(metadata(rep, "fluence monitors"), "FC-2 sanidine 28.201 ± 0.046 Ma (2s)");
  EXPECT_EQ(metadata(rep, "samples"), "FC-2; air");
  EXPECT_EQ(rep.summary.rows.size(), 2u);
  EXPECT_EQ(text(rep.summary, 1, "step heated"), "no");
}

TEST(Report, AnalysesWithoutAnAgeKeepTheirRowAndANote) {
  pp::Dataset d;
  pp::DatasetItem air;
  air.analysis = pp::reduce_analysis(make_air(0), {});
  pp::DatasetItem unknown;  // no flux: no age
  unknown.analysis = pp::reduce_analysis(make_air(1, 295.5, "unknown", "U9"), {});
  d.mutable_items() = {air, unknown};
  const auto rep = pp::make_report(d);
  ASSERT_EQ(rep.analyses.rows.size(), 2u);
  EXPECT_EQ(text(rep.analyses, 0, "analysis type"), "air");
  EXPECT_TRUE(number(rep.analyses, 0, "40Ar (fA)"));
  EXPECT_FALSE(number(rep.analyses, 0, "age (Ma)"));
  EXPECT_FALSE(number(rep.analyses, 1, "age (Ma)"));
  EXPECT_FALSE(text(rep.analyses, 1, "note").empty());
  ASSERT_EQ(rep.summary.rows.size(), 1u);
  EXPECT_EQ(text(rep.summary, 0, "group"), d.group_name(0));  // the dataset's default name
  EXPECT_FALSE(number(rep.summary, 0, "weighted mean age (Ma)"));
  EXPECT_TRUE(text(rep.summary, 0, "note").find("no analysis of this group has an age") == 0)
      << text(rep.summary, 0, "note");
}

TEST(Report, EmptyDatasetWarns) {
  const auto rep = pp::make_report(pp::Dataset{});
  EXPECT_TRUE(rep.analyses.rows.empty());
  EXPECT_TRUE(rep.summary.rows.empty());
  ASSERT_FALSE(rep.warnings.empty());
  EXPECT_FALSE(pp::report_csv(rep).empty());
  EXPECT_FALSE(pp::report_json(rep).empty());
}

TEST(Report, ConstantsListWhatTheReductionUsed) {
  const auto rep = pp::make_report(steps());
  const auto c = r::constants_preset(r::ConstantsPreset::Default);
  const auto& t = rep.constants;
  auto find = [&](const std::string& item) -> std::optional<double> {
    for (std::size_t i = 0; i < t.rows.size(); ++i)
      if (text(t, i, "item") == item) return number(t, i, "value");
    return std::nullopt;
  };
  EXPECT_NEAR(*find("lambda_e (40K -> 40Ar)"), c.lambda_e.value, 1e-20);
  EXPECT_NEAR(*find("lambda_beta (40K -> 40Ca)"), c.lambda_b.value, 1e-20);
  EXPECT_NEAR(*find("lambda_K total"), c.lambda_b.value + c.lambda_e.value, 1e-20);
  EXPECT_NEAR(*find("atmospheric 40Ar/36Ar"), c.atm4036.value, 1e-9);
  EXPECT_NEAR(*find("lambda_39Ar"), c.lambda_ar39.value, 1e-15);
  EXPECT_EQ(metadata(rep, "reduction"), "arar-1/default/d0");
  EXPECT_EQ(metadata(rep, "age units"), "Ma");
  EXPECT_TRUE(metadata(rep, "age uncertainties").find("Decay-constant uncertainty included: no") != std::string::npos);
  EXPECT_TRUE(metadata(rep, "standard").find("Schaen") == 0);
}

TEST(Report, CsvIsWellFormedAndQuotesSpecialText) {
  auto d = steps(3);
  for (auto& it : d.mutable_items()) {
    auto a = std::make_shared<pp::Analysis>(*it.analysis->analysis);
    a->sample = "FC-2, \"split\" B";
    it.analysis = pp::reduce_analysis(a, {});
  }
  const auto rep = pp::make_report(d);
  const std::string csv = pp::report_csv(rep);
  std::istringstream in(csv);
  std::string line;
  std::size_t header_lines = 0, sections = 0;
  std::optional<std::size_t> width;
  std::vector<std::string> order;
  bool saw_sample = false;
  while (std::getline(in, line)) {
    if (line.rfind("# ", 0) == 0) {
      ++header_lines;
      continue;
    }
    if (line.empty()) {
      width.reset();
      continue;
    }
    if (line.front() == '[') {
      ++sections;
      order.push_back(line);
      continue;
    }
    const auto fields = csv_fields(line);
    if (!width) width = fields.size();
    EXPECT_EQ(fields.size(), *width) << line;
    for (const auto& f : fields) saw_sample = saw_sample || f == "FC-2, \"split\" B";
  }
  EXPECT_GE(header_lines, 3u);
  EXPECT_EQ(sections, 5u);
  EXPECT_EQ(order, (std::vector<std::string>{"[metadata]", "[constants]", "[irradiation]", "[analyses]", "[summary]"}));
  EXPECT_TRUE(saw_sample);
  EXPECT_TRUE(csv.find("\"FC-2, \"\"split\"\" B\"") != std::string::npos);
}

TEST(Report, JsonCarriesEveryTableAndEscapes) {
  auto d = steps(2);
  {
    auto a = std::make_shared<pp::Analysis>(*d.items()[0].analysis->analysis);
    a->comment = "line\nbreak \"quoted\"";
    a->sample = "tab\there";
    d.mutable_items()[0].analysis = pp::reduce_analysis(a, {});
  }
  const auto rep = pp::make_report(d);
  const std::string json = pp::report_json(rep);
  EXPECT_EQ(json.front(), '{');
  EXPECT_TRUE(json.find("\"metadata\": {") != std::string::npos);
  EXPECT_TRUE(json.find("\"constants\": [") != std::string::npos);
  EXPECT_TRUE(json.find("\"irradiation\": [") != std::string::npos);
  EXPECT_TRUE(json.find("\"analyses\": [") != std::string::npos);
  EXPECT_TRUE(json.find("\"summary\": [") != std::string::npos);
  EXPECT_TRUE(json.find("\"run id\": \"S1-01A\"") != std::string::npos);
  EXPECT_TRUE(json.find("\"sample\": \"tab\\there\"") != std::string::npos);
  EXPECT_TRUE(json.find("\"standard\": \"Schaen") != std::string::npos);
  EXPECT_TRUE(json.find("\"Ca/K\": null") != std::string::npos);  // an optional ratio the source lacks
  // Every value column of the analyses rows appears once per row.
  std::size_t count = 0;
  for (std::size_t p = json.find("\"40Ar (fA)\":"); p != std::string::npos; p = json.find("\"40Ar (fA)\":", p + 1))
    ++count;
  EXPECT_EQ(count, 2u);
}

TEST(Report, SaveWritesCsvOrJsonByExtension) {
  const auto dir = std::filesystem::temp_directory_path() / "pychron_report_test";
  std::filesystem::create_directories(dir);
  const auto rep = pp::make_report(steps(2));
  const auto csv = dir / "report.csv";
  const auto json = dir / "report.JSON";
  ASSERT_TRUE(pp::save_report(rep, csv));
  ASSERT_TRUE(pp::save_report(rep, json));
  std::string first;
  std::getline(std::ifstream(csv), first);
  EXPECT_EQ(first.rfind("# 40Ar/39Ar data report after Schaen", 0), 0u) << first;
  std::getline(std::ifstream(json), first);
  EXPECT_EQ(first, "{");
  EXPECT_FALSE(pp::save_report(rep, dir / "no" / "such" / "dir" / "x.csv"));
  std::filesystem::remove_all(dir);
}

}  // namespace
