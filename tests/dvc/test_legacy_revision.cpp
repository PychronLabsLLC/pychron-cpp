// parse_data and parse_revision against the real files. Every literal below
// was copied by hand from the fixture it names.

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "pychron/dvc/legacy_layout.hpp"
#include "pychron/persistence/blob.hpp"

namespace pychron::dvc {
namespace {

namespace ps = pychron::persistence;
using nlohmann::json;
using testing::fixture;
using testing::kBlank;
using testing::kUnknown;

template <class T>
T rows_of(FileKind kind, const std::string& text) {
  auto r = parse_revision(kind, text);
  EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().what);
  if (!r) return {};
  const T* rows = std::get_if<T>(&r->payload);
  EXPECT_NE(rows, nullptr) << "wrong payload alternative";
  return rows ? *rows : T{};
}

template <class Rows>
const typename Rows::value_type& isotope_row(const Rows& rows, std::string_view isotope) {
  auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.isotope == isotope; });
  if (it == rows.end()) throw std::runtime_error("no row " + std::string(isotope));
  return *it;
}

template <class Rows>
const typename Rows::value_type& detector_row(const Rows& rows, std::string_view detector) {
  auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.detector == detector; });
  if (it == rows.end()) throw std::runtime_error("no row " + std::string(detector));
  return *it;
}

json extra(const std::optional<std::string>& text) { return text ? json::parse(*text) : json(nullptr); }

// ---------------------------------------------------------------- raw data

TEST(Layout, DataFixtureRoundTrips) {
  const std::string text = fixture(kUnknown + "660/.data/52-01E.dat.json");
  auto d = parse_data(text);
  ASSERT_TRUE(d.has_value()) << d.error().what;
  // 5 signals, 5 baselines, 5 sniffs.
  ASSERT_EQ(d->refs.size(), 15u);
  ASSERT_EQ(d->blobs.size(), 15u);
  for (std::size_t i = 0; i < d->refs.size(); ++i) {
    EXPECT_EQ(d->blobs[i].codec, "f32le-tv/1");
    EXPECT_EQ(d->refs[i].blob_sha, ps::blob_sha256(d->blobs[i].codec, d->blobs[i].bytes));
    EXPECT_EQ(d->refs[i].n_points, d->blobs[i].n_points);
  }

  // The first isotope of the file: signals[0] is Ar40 on H2.
  const json source = json::parse(text);
  ASSERT_EQ(source["signals"][0]["isotope"], "Ar40");
  const auto legacy_points = ps::decode_legacy_ff_base64(source["signals"][0]["blob"].get<std::string>());
  ASSERT_TRUE(legacy_points.has_value());
  const auto& ref = d->refs[0];
  EXPECT_EQ(ref.series_kind, "signal");
  EXPECT_EQ(ref.series_key, "Ar40");
  EXPECT_EQ(ref.detector, "H2");
  EXPECT_EQ(ref.n_points, 340);  // README 5.2, and "n": 340 in the intercepts file
  const auto points = ps::decode_tv(d->blobs[0].bytes);
  ASSERT_TRUE(points.has_value());
  EXPECT_EQ(*points, *legacy_points);
  ASSERT_EQ(points->size(), 340u);
  // README 5.2: first and last point of Ar40.
  EXPECT_EQ(points->front().t, 27.734310150146484f);
  EXPECT_EQ(points->front().v, 9.958504676818848f);
  EXPECT_EQ(points->back().t, 384.5098876953125f);
  EXPECT_EQ(points->back().v, 11.088024139404297f);

  auto count = [&](std::string_view kind) {
    return std::count_if(d->refs.begin(), d->refs.end(), [&](const auto& r) { return r.series_kind == kind; });
  };
  EXPECT_EQ(count("signal"), 5);
  EXPECT_EQ(count("baseline"), 5);
  EXPECT_EQ(count("sniff"), 5);
  for (const auto& r : d->refs) {
    if (r.series_kind == "signal") {
      EXPECT_EQ(r.n_points, 340) << r.series_key;
    }
    if (r.series_kind == "baseline") {
      EXPECT_EQ(r.n_points, 60) << r.series_key;
      EXPECT_EQ(r.series_key, r.detector);  // a baseline has no isotope: keyed by detector
    }
    if (r.series_kind == "sniff") {
      EXPECT_EQ(r.n_points, 25) << r.series_key;
    }
  }
  // README 5.2: first baseline time, first sniff time.
  const auto baseline = std::find_if(d->refs.begin(), d->refs.end(), [](const auto& r) {
    return r.series_kind == "baseline" && r.detector == "H2";
  });
  ASSERT_NE(baseline, d->refs.end());
  EXPECT_EQ(ps::decode_tv(d->blobs[static_cast<std::size_t>(baseline - d->refs.begin())].bytes)->front().t,
            401.80364990234375f);
  const auto sniff = std::find_if(d->refs.begin(), d->refs.end(), [](const auto& r) {
    return r.series_kind == "sniff" && r.series_key == "Ar40";
  });
  ASSERT_NE(sniff, d->refs.end());
  EXPECT_EQ(sniff->detector, "H2");
  EXPECT_EQ(ps::decode_tv(d->blobs[static_cast<std::size_t>(sniff - d->refs.begin())].bytes)->front().t,
            0.5804991722106934f);

  // SignalRefRow has no extra: the meta-repo commit comes back beside the rows.
  EXPECT_EQ(extra(d->extra_json), json::parse(R"({"commit": "c27fcc912f0b5dc6e984fd84cdf21a21424f7c98"})"));
}

TEST(Layout, DataOfTheBlankParses) {
  auto d = parse_data(fixture(kBlank + "bu-FD/.data/-F-789.dat.json"));
  ASSERT_TRUE(d.has_value()) << d.error().what;
  EXPECT_EQ(d->refs.size(), 15u);
}

TEST(Layout, DataUnknownKeysAndErrors) {
  const std::string blob = ps::encode_legacy_ff_base64(std::vector<ps::TvPoint>{{1.0f, 2.0f}});
  auto d = parse_data(R"({"format": ">ff", "encoding": "base64", "data_collection_commit": "abc", "zz": 1,
                          "signals": [{"isotope": "Ar40", "detector": "H2", "blob": ")" +
                      blob + R"(", "zz_entry": 2}], "baselines": [], "sniffs": []})");
  ASSERT_TRUE(d.has_value()) << d.error().what;
  ASSERT_EQ(d->refs.size(), 1u);
  EXPECT_EQ(extra(d->extra_json),
            json::parse(R"({"data_collection_commit": "abc", "zz": 1, "signals": {"0": {"zz_entry": 2}}})"));

  EXPECT_FALSE(parse_data("not json").has_value());
  EXPECT_FALSE(parse_data("[]").has_value());
  // Another packing or encoding cannot be read as >ff base64.
  EXPECT_FALSE(parse_data(R"({"format": "<dd", "encoding": "base64", "signals": []})").has_value());
  EXPECT_FALSE(parse_data(R"({"format": ">ff", "encoding": "hex", "signals": []})").has_value());
  // A blob that is not base64, or not a whole number of pairs.
  EXPECT_FALSE(parse_data(R"({"signals": [{"isotope": "Ar40", "detector": "H2", "blob": "!!"}]})").has_value());
  EXPECT_FALSE(parse_data(R"({"signals": [{"isotope": "Ar40", "detector": "H2", "blob": "AAAA"}]})").has_value());
  EXPECT_FALSE(parse_data(R"({"signals": [{"isotope": "Ar40", "detector": "H2"}]})").has_value());
  EXPECT_FALSE(parse_data(R"({"signals": {"Ar40": 1}})").has_value());
}

// ---------------------------------------------------------------- intercepts

TEST(Layout, InterceptsFixture) {
  const auto rows = rows_of<ps::Intercepts>(FileKind::Intercepts, fixture(kUnknown + "660/intercepts/52-01E.inte.json"));
  ASSERT_EQ(rows.size(), 5u);
  const auto& ar40 = isotope_row(rows, "Ar40");
  EXPECT_EQ(ar40.value, 9.65253181865139);
  EXPECT_EQ(ar40.error, 0.021947179349776885);
  EXPECT_EQ(ar40.fit, "parabolic");  // "Parabolic" in the file
  EXPECT_EQ(ar40.error_type, "SEM");
  EXPECT_EQ(ar40.n, 340);
  EXPECT_EQ(ar40.fn, 320);
  EXPECT_TRUE(ar40.reviewed);
  EXPECT_EQ(ar40.include_baseline_error, false);
  EXPECT_EQ(extra(ar40.filter_outliers_json),
            json::parse(R"({"filter_outliers": true, "iterations": 1, "std_devs": 2})"));
  EXPECT_FALSE(ar40.manual.use_value);
  EXPECT_EQ(extra(ar40.extra_json), json::parse(R"({"legacy_fit": "Parabolic"})"));
  EXPECT_TRUE(ar40.detector.empty());  // the file is keyed by isotope only

  const auto& ar38 = isotope_row(rows, "Ar38");
  EXPECT_EQ(ar38.value, -0.047598741642625474);
  EXPECT_EQ(ar38.fit, "linear");  // "Linear"
  EXPECT_EQ(ar38.fn, 326);
}

TEST(Layout, InterceptsCollectionDefaultOfTheBlank) {
  // README 5.3: only fit, error_type, filter_outliers_dict, value, error.
  const auto rows = rows_of<ps::Intercepts>(FileKind::Intercepts, fixture(kBlank + "bu-FD/intercepts/-F-789.inte.json"));
  ASSERT_EQ(rows.size(), 5u);
  const auto& ar36 = isotope_row(rows, "Ar36");
  EXPECT_EQ(ar36.value, 0.001739746496471459);
  EXPECT_EQ(ar36.error, 0.00016482549148590984);
  EXPECT_EQ(ar36.fit, "parabolic");
  EXPECT_FALSE(ar36.n.has_value());
  EXPECT_FALSE(ar36.fn.has_value());
  EXPECT_FALSE(ar36.reviewed);
  EXPECT_FALSE(ar36.include_baseline_error.has_value());
  EXPECT_EQ(extra(ar36.filter_outliers_json),
            json::parse(R"({"filter_outliers": true, "iterations": 2, "std_devs": 2})"));
  const auto& ar37 = isotope_row(rows, "Ar37");
  EXPECT_EQ(ar37.value, -0.18990919658164826);
  EXPECT_EQ(ar37.fit, "linear");                  // already "linear" in the file
  EXPECT_FALSE(ar37.extra_json.has_value());      // so nothing to remember
}

TEST(Layout, FitAndErrorTypeAreWrittenInTheStoreSpelling) {
  // The store reader lower-cases the fit and knows average, linear,
  // parabolic, cubic, exponential; error types SD and SEM.
  struct Case {
    const char* fit;
    const char* error_type;
    const char* want_fit;
    const char* want_error_type;
  };
  const Case cases[] = {
      {"Parabolic", "SEM", "parabolic", "SEM"},
      {"linear", "sem", "linear", "SEM"},
      {"Average", "SD", "average", "SD"},
      {"CUBIC", "Sd", "cubic", "SD"},
      {"Exponential", "SEM", "exponential", "SEM"},
      {"quadratic", "SEM", "parabolic", "SEM"},
      {"average_SEM", "", "average", "SEM"},  // the old "<fit>_<error type>" form
      {"average_sd", "", "average", "SD"},
      {"linear_SD", "SEM", "linear", "SEM"},  // an explicit error_type wins
      {" Linear ", "SEM", "linear", "SEM"},
  };
  for (const auto& c : cases) {
    const std::string text = std::string(R"({"Ar40": {"value": 1.0, "error": 0.1, "fit": ")") + c.fit +
                             R"(", "error_type": ")" + c.error_type + R"("}})";
    const auto rows = rows_of<ps::Intercepts>(FileKind::Intercepts, text);
    ASSERT_EQ(rows.size(), 1u) << c.fit;
    EXPECT_EQ(rows[0].fit, c.want_fit) << c.fit;
    EXPECT_EQ(rows[0].error_type, c.want_error_type) << c.fit << " / " << c.error_type;
    const json x = extra(rows[0].extra_json);
    // The file's own spelling is kept whenever it was changed.
    if (std::string(c.fit) != c.want_fit) {
      EXPECT_EQ(x["legacy_fit"], c.fit);
    } else {
      EXPECT_FALSE(x.is_object() && x.contains("legacy_fit")) << c.fit;
    }
    if (std::string(c.error_type) != c.want_error_type && std::string(c.error_type) != "") {
      EXPECT_EQ(x["legacy_error_type"], c.error_type);
    } else {
      EXPECT_FALSE(x.is_object() && x.contains("legacy_error_type")) << c.error_type;
    }
    // Baselines follow the same rule.
    const std::string base = std::string(R"({"H2": {"value": 1.0, "fit": ")") + c.fit + R"(", "error_type": ")" +
                             c.error_type + R"("}})";
    const auto baselines = rows_of<ps::Baselines>(FileKind::Baselines, base);
    ASSERT_EQ(baselines.size(), 1u);
    EXPECT_EQ(baselines[0].fit, c.want_fit) << c.fit;
    EXPECT_EQ(baselines[0].error_type, c.want_error_type) << c.fit;
  }

  // A fit the reader has no curve for is kept as written, not guessed.
  for (const char* fit : {"Weighted Mean", "Auto", "custom:3", "average_SEM_extra"}) {
    const auto rows = rows_of<ps::Intercepts>(
        FileKind::Intercepts, std::string(R"({"Ar40": {"fit": ")") + fit + R"(", "error_type": "CI"}})");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].fit, fit);
    EXPECT_EQ(rows[0].error_type, "CI");
    EXPECT_FALSE(rows[0].extra_json.has_value()) << fit;
  }
  // No fit at all.
  const auto none = rows_of<ps::Intercepts>(FileKind::Intercepts, R"({"Ar40": {"value": 1.0, "error_type": ""}})");
  ASSERT_EQ(none.size(), 1u);
  EXPECT_FALSE(none[0].fit.has_value());
  EXPECT_FALSE(none[0].error_type.has_value());
}

TEST(Layout, UnknownKeysGoToExtra) {
  json doc = json::parse(fixture(kUnknown + "660/intercepts/52-01E.inte.json"));
  doc["Ar40"]["zz_new"] = 1;
  const auto rows = rows_of<ps::Intercepts>(FileKind::Intercepts, doc.dump());
  const auto& ar40 = isotope_row(rows, "Ar40");
  ASSERT_TRUE(ar40.extra_json.has_value());
  EXPECT_EQ(extra(ar40.extra_json)["zz_new"], 1);
  EXPECT_EQ(ar40.value, 9.65253181865139);  // the known keys still land
  // Only that row is affected.
  EXPECT_FALSE(extra(isotope_row(rows, "Ar39").extra_json).contains("zz_new"));

  // The same holds for every row kind that has an extra.
  const auto base = rows_of<ps::Baselines>(FileKind::Baselines, R"({"H2": {"value": 1.0, "zz_new": [1]}})");
  EXPECT_EQ(extra(base.at(0).extra_json)["zz_new"], json::parse("[1]"));
  const auto blanks = rows_of<ps::Blanks>(FileKind::Blanks, R"({"Ar40": {"value": 1.0, "zz_new": "x"}})");
  EXPECT_EQ(extra(blanks.at(0).extra_json)["zz_new"], "x");
  const auto ics = rows_of<ps::IcFactors>(FileKind::IcFactors, R"({"H1": {"value": 1.0, "zz_new": null}})");
  EXPECT_TRUE(extra(ics.at(0).extra_json).contains("zz_new"));
  // A known key whose value cannot be converted is not dropped either.
  const auto odd = rows_of<ps::Intercepts>(FileKind::Intercepts, R"({"Ar40": {"value": "n/a", "n": 3.5}})");
  EXPECT_FALSE(odd.at(0).value.has_value());
  EXPECT_FALSE(odd.at(0).n.has_value());
  EXPECT_EQ(extra(odd.at(0).extra_json), json::parse(R"({"value": "n/a", "n": 3.5})"));
}

TEST(Layout, NanValueIsUnknownNotZero) {
  const auto rows = rows_of<ps::Intercepts>(
      FileKind::Intercepts,
      R"({"Ar40": {"value": NaN, "error": Infinity, "fit": "linear", "note": "NaN"}, "Ar39": {"value": 1.0}})");
  const auto& ar40 = isotope_row(rows, "Ar40");
  EXPECT_FALSE(ar40.value.has_value());
  EXPECT_FALSE(ar40.error.has_value());
  EXPECT_EQ(extra(ar40.extra_json),
            json::parse(R"({"note": "NaN", "nonfinite": {"/value": "NaN", "/error": "Infinity"}})"));
  EXPECT_FALSE(isotope_row(rows, "Ar39").extra_json.has_value());

  const auto base = rows_of<ps::Baselines>(FileKind::Baselines, R"({"H2": {"value": -Infinity, "error": 0.5}})");
  EXPECT_FALSE(base.at(0).value.has_value());
  EXPECT_EQ(base.at(0).error, 0.5);
  EXPECT_EQ(extra(base.at(0).extra_json), json::parse(R"({"nonfinite": {"/value": "-Infinity"}})"));
  const auto blanks = rows_of<ps::Blanks>(FileKind::Blanks, R"({"Ar40": {"value": NaN, "error": NaN}})");
  EXPECT_FALSE(blanks.at(0).value.has_value());
  EXPECT_FALSE(blanks.at(0).error.has_value());
  const auto ics = rows_of<ps::IcFactors>(FileKind::IcFactors, R"({"H1": {"value": NaN, "error": 1e-20}})");
  EXPECT_FALSE(ics.at(0).value.has_value());
  EXPECT_EQ(ics.at(0).error, 1e-20);
  EXPECT_EQ(extra(ics.at(0).extra_json), json::parse(R"({"nonfinite": {"/value": "NaN"}})"));
}

TEST(Layout, ManualOverride) {
  // README 5.3 (source only): manual_value, use_manual_value, manual_error, use_manual_error.
  const auto rows = rows_of<ps::Intercepts>(FileKind::Intercepts,
                                            R"({"Ar40": {"value": 1.0, "error": 0.1, "manual_value": 2.5,
                                                         "use_manual_value": true, "manual_error": 0.25,
                                                         "use_manual_error": false}})");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows[0].manual.use_value);
  EXPECT_EQ(rows[0].manual.value, 2.5);
  EXPECT_FALSE(rows[0].manual.use_error);
  EXPECT_EQ(rows[0].manual.error, 0.25);
  EXPECT_FALSE(rows[0].extra_json.has_value());
}

// ---------------------------------------------------------------- baselines

TEST(Layout, BaselinesFixture) {
  const auto rows = rows_of<ps::Baselines>(FileKind::Baselines, fixture(kUnknown + "660/baselines/52-01E.base.json"));
  ASSERT_EQ(rows.size(), 5u);
  // A reviewed detector (README 5.4).
  const auto& ax = detector_row(rows, "AX");
  EXPECT_EQ(ax.value, -0.0718735887795478);
  EXPECT_EQ(ax.error, 0.005122993635309044);
  EXPECT_EQ(ax.fit, "average");  // "Average" in the file
  EXPECT_EQ(ax.error_type, "SEM");
  EXPECT_EQ(ax.n, 60);
  EXPECT_EQ(ax.fn, 58);
  EXPECT_TRUE(ax.reviewed);
  // BaselineRow has no include_baseline_error column: it is kept in extra.
  EXPECT_EQ(extra(ax.extra_json), json::parse(R"({"include_baseline_error": false, "legacy_fit": "Average"})"));
  // An untouched one: collection keys only, "average" already.
  const auto& h2 = detector_row(rows, "H2");
  EXPECT_EQ(h2.value, -0.01751839358672034);
  EXPECT_EQ(h2.error, 0.015026090583809578);
  EXPECT_EQ(h2.fit, "average");
  EXPECT_FALSE(h2.n.has_value());
  EXPECT_FALSE(h2.reviewed);
  EXPECT_FALSE(h2.extra_json.has_value());
  const auto& cdd = detector_row(rows, "L2(CDD)");
  EXPECT_EQ(cdd.value, 0.0);  // a real zero stays a zero
  EXPECT_EQ(cdd.error, 0.0);
}

TEST(Layout, BaselineModifiers) {
  // README 5.4 (source only): modifier_value, modifier_error.
  const auto rows = rows_of<ps::Baselines>(FileKind::Baselines,
                                           R"({"H2": {"value": 1.0, "modifier_value": 0.5, "modifier_error": 0.05}})");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].modifier_value, 0.5);
  EXPECT_EQ(rows[0].modifier_error, 0.05);
}

// ---------------------------------------------------------------- blanks

TEST(Layout, BlanksFixtureReviewed) {
  const auto rows = rows_of<ps::Blanks>(FileKind::Blanks, fixture(kUnknown + "660/blanks/52-01E.blan.json"));
  ASSERT_EQ(rows.size(), 5u);
  const auto& ar36 = isotope_row(rows, "Ar36");
  EXPECT_EQ(ar36.value, 0.0018778210508019343);
  EXPECT_EQ(ar36.error, 0.00013800347662867014);
  EXPECT_EQ(ar36.fit, "Bracketing Interpolate");  // a method name: kept as written
  EXPECT_FALSE(ar36.error_type.has_value());      // "" in the file
  EXPECT_TRUE(ar36.reviewed);
  ASSERT_EQ(ar36.references.size(), 7u);
  for (int i = 0; i < 7; ++i) EXPECT_EQ(ar36.references[static_cast<std::size_t>(i)].ordinal, i);
  // The reference as written: record id and uuid, nothing looked up.
  EXPECT_EQ(ar36.references[0].record_id, "bu-FD-F-787");
  EXPECT_EQ(ar36.references[0].ref_analysis.value().str(), "c1498705-eda5-4fc2-994d-8420ac017578");
  EXPECT_FALSE(ar36.references[0].exclude);  // the string "ok"
  // The fixture blank is the third reference.
  EXPECT_EQ(ar36.references[2].record_id, "bu-FD-F-789");
  EXPECT_EQ(ar36.references[2].ref_analysis.value().str(), "7834c4f8-f3b8-4aac-b586-53f7cb8d3d5c");
  EXPECT_EQ(ar36.references[6].record_id, "bu-FD-F-793");
  // The list is also kept verbatim, so clearing ref_analysis loses nothing.
  const json x = extra(ar36.extra_json);
  ASSERT_EQ(x["references"].size(), 7u);
  EXPECT_EQ(x["references"][0],
            json::parse(R"({"exclude": "ok", "record_id": "bu-FD-F-787",
                            "uuid": "c1498705-eda5-4fc2-994d-8420ac017578"})"));

  const auto& ar40 = isotope_row(rows, "Ar40");
  EXPECT_EQ(ar40.value, 0.32487008636072684);
  EXPECT_EQ(ar40.error, 0.01729628495568356);
}

TEST(Layout, BlanksCollectionDefaultOfTheBlank) {
  // README 5.5: "previous", one reference with no uuid and a boolean exclude.
  const auto rows = rows_of<ps::Blanks>(FileKind::Blanks, fixture(kBlank + "bu-FD/blanks/-F-789.blan.json"));
  ASSERT_EQ(rows.size(), 5u);
  const auto& ar36 = isotope_row(rows, "Ar36");
  EXPECT_EQ(ar36.value, 0.0);
  EXPECT_EQ(ar36.error, 0.0);
  EXPECT_EQ(ar36.fit, "previous");
  EXPECT_FALSE(ar36.reviewed);
  ASSERT_EQ(ar36.references.size(), 1u);
  EXPECT_EQ(ar36.references[0].record_id, "bc-02-F-696");
  EXPECT_FALSE(ar36.references[0].ref_analysis.has_value());
  EXPECT_FALSE(ar36.references[0].exclude);  // the boolean false
  EXPECT_EQ(extra(ar36.extra_json),
            json::parse(R"({"references": [{"exclude": false, "record_id": "bc-02-F-696"}]})"));
}

TEST(Layout, ReferenceExcludeMayBeABooleanAStringOrANumber) {
  const auto rows = rows_of<ps::Blanks>(
      FileKind::Blanks,
      R"({"Ar40": {"value": 1.0, "references": [
            {"record_id": "a-01", "exclude": false},
            {"record_id": "a-02", "exclude": true},
            {"record_id": "a-03", "exclude": "ok"},
            {"record_id": "a-04", "exclude": "omit"},
            {"record_id": "a-05", "exclude": ""},
            {"record_id": "a-06", "exclude": 0},
            {"record_id": "a-07", "exclude": 1},
            {"record_id": "a-08", "exclude": null},
            {"record_id": "a-09"},
            {"record_id": "a-10", "uuid": "not-a-uuid", "exclude": "invalid"},
            {"record_id": "a-11", "uuid": "", "exclude": "OK"},
            "a-12"]}})");
  ASSERT_EQ(rows.size(), 1u);
  const auto& refs = rows[0].references;
  ASSERT_EQ(refs.size(), 12u);
  const bool want[] = {false, true, false, true, false, false, true, false, false, true, false, false};
  for (std::size_t i = 0; i < refs.size(); ++i) {
    EXPECT_EQ(refs[i].exclude, want[i]) << i;
    EXPECT_EQ(refs[i].ordinal, static_cast<int>(i));
    EXPECT_FALSE(refs[i].ref_analysis.has_value()) << i;
  }
  EXPECT_EQ(refs[9].record_id, "a-10");
  EXPECT_EQ(refs[11].record_id, "a-12");  // a bare record id
  // The malformed uuid survives in the verbatim list.
  EXPECT_EQ(extra(rows[0].extra_json)["references"][9]["uuid"], "not-a-uuid");
}

// ---------------------------------------------------------------- IC factors

TEST(Layout, IcFactorsFixtureHasThreeShapes) {
  const auto rows = rows_of<ps::IcFactors>(FileKind::IcFactors, fixture(kUnknown + "660/icfactors/52-01E.icfa.json"));
  ASSERT_EQ(rows.size(), 5u);
  // Default.
  const auto& ax = detector_row(rows, "AX");
  EXPECT_EQ(ax.value, 1.0);
  EXPECT_EQ(ax.error, 1e-20);
  EXPECT_EQ(ax.fit, "default");
  EXPECT_FALSE(ax.reviewed);
  EXPECT_TRUE(ax.references.empty());
  EXPECT_FALSE(ax.extra_json.has_value());  // "references": [] says nothing more
  // Reviewed, seven references.
  const auto& h1 = detector_row(rows, "H1");
  EXPECT_EQ(h1.value, 0.9984144337105232);
  EXPECT_EQ(h1.error, 0.0005459531450125758);
  EXPECT_EQ(h1.fit, "Bracketing Interpolate");
  EXPECT_TRUE(h1.reviewed);
  ASSERT_EQ(h1.references.size(), 7u);
  EXPECT_EQ(h1.references[0].record_id, "c-02-F-1474");
  EXPECT_EQ(h1.references[0].ref_analysis.value().str(), "00c4fcd4-496a-4761-ae65-0ca2eeae2ba8");
  EXPECT_FALSE(h1.references[0].exclude);
  EXPECT_EQ(h1.references[6].record_id, "c-02-F-1480");
  EXPECT_EQ(h1.references[6].ref_analysis.value().str(), "1c30e109-13ce-4ec4-9b0a-0ca17c5bd994");
  // Bulk edited: "references": "" (a string, not a list) and an extra key.
  const auto& cdd = detector_row(rows, "L2(CDD)");
  EXPECT_EQ(cdd.value, 1.033223810490805);
  EXPECT_EQ(cdd.error, 0.0008750041945287255);
  EXPECT_EQ(cdd.fit, "bulk_edit");
  EXPECT_TRUE(cdd.reviewed);
  EXPECT_TRUE(cdd.references.empty());
  EXPECT_EQ(extra(cdd.extra_json), json::parse(R"({"scalar": 0.9932318104906938})"));
}

TEST(Layout, IcFactorsCollectionDefaultOfTheBlank) {
  const auto rows = rows_of<ps::IcFactors>(FileKind::IcFactors, fixture(kBlank + "bu-FD/icfactors/-F-789.icfa.json"));
  ASSERT_EQ(rows.size(), 5u);
  for (const auto& r : rows) {
    EXPECT_EQ(r.value, 1.0) << r.detector;
    EXPECT_EQ(r.error, 1e-20) << r.detector;
    EXPECT_EQ(r.fit, "default") << r.detector;
    EXPECT_TRUE(r.references.empty()) << r.detector;
  }
}

TEST(Layout, IcFactorSourceOnlyKeys) {
  // README 5.5 (source only): standard_ratio, source_correction.
  const auto rows = rows_of<ps::IcFactors>(
      FileKind::IcFactors,
      R"({"H1": {"value": 1.01, "error": 0.001, "fit": "linear", "standard_ratio": 295.5,
                 "source_correction": true, "references": null}})");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].standard_ratio, 295.5);
  EXPECT_TRUE(rows[0].source_correction);
  EXPECT_EQ(rows[0].fit, "linear");
  EXPECT_FALSE(rows[0].extra_json.has_value());
  // A references value of an unexpected type is kept, not dropped.
  const auto odd = rows_of<ps::IcFactors>(FileKind::IcFactors, R"({"H1": {"references": {"a": 1}}})");
  EXPECT_TRUE(odd.at(0).references.empty());
  EXPECT_EQ(extra(odd.at(0).extra_json), json::parse(R"({"references": {"a": 1}})"));
}

// ---------------------------------------------------------------- tags, cosmogenic

TEST(Layout, TagsFixture) {
  auto r = parse_revision(FileKind::Tags, fixture(kUnknown + "660/tags/52-01E.tags.json"));
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto* tag = std::get_if<ps::TagValue>(&r->payload);
  ASSERT_NE(tag, nullptr);
  EXPECT_EQ(tag->name, "omit");
  EXPECT_FALSE(tag->note.has_value());           // "" in the file
  EXPECT_FALSE(tag->subgroup_json.has_value());  // "" in the file
  EXPECT_FALSE(r->extra_json.has_value());
}

TEST(Layout, TagNoteSubgroupAndUnknownKeys) {
  auto r = parse_revision(FileKind::Tags,
                          R"({"name": "invalid", "note": "air leak", "subgroup": "plateau", "zz_new": 1})");
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto& tag = std::get<ps::TagValue>(r->payload);
  EXPECT_EQ(tag.name, "invalid");
  EXPECT_EQ(tag.note, "air leak");
  EXPECT_EQ(tag.subgroup_json, R"("plateau")");
  // TagValue has no extra: the unknown key comes back beside the payload.
  EXPECT_EQ(extra(r->extra_json), json::parse(R"({"zz_new": 1})"));

  auto structured = parse_revision(FileKind::Tags, R"({"name": "ok", "subgroup": {"name": "a", "kind": "plateau"}})");
  ASSERT_TRUE(structured.has_value());
  EXPECT_EQ(extra(std::get<ps::TagValue>(structured->payload).subgroup_json),
            json::parse(R"({"name": "a", "kind": "plateau"})"));

  // A tags file without a tag is not a tag: no name is invented.
  EXPECT_FALSE(parse_revision(FileKind::Tags, R"({"note": "x"})").has_value());
  EXPECT_FALSE(parse_revision(FileKind::Tags, R"({"name": ""})").has_value());
}

TEST(Layout, CosmogenicKeepsTheWholeDocument) {
  // Source only (README 3.2): no real file seen, so the document is opaque.
  auto r = parse_revision(FileKind::Cosmogenic, "{\r\n \"Ar38\": {\"value\": 1.5, \"error\": NaN}, \"zz\": [1]}");
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto* c = std::get_if<ps::CosmogenicValue>(&r->payload);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(json::parse(c->doc_json), json::parse(R"({"Ar38": {"value": 1.5, "error": null}, "zz": [1]})"));
  EXPECT_EQ(extra(r->extra_json), json::parse(R"({"nonfinite": {"/Ar38/error": "NaN"}})"));
}

// ---------------------------------------------------------------- errors

TEST(Layout, GarbageIsError) {
  auto r = parse_revision(FileKind::Intercepts, "not json");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  for (FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors,
                        FileKind::Tags, FileKind::Cosmogenic}) {
    EXPECT_FALSE(parse_revision(kind, "").has_value());
    EXPECT_FALSE(parse_revision(kind, "[1, 2]").has_value());   // not an object
    EXPECT_FALSE(parse_revision(kind, R"({"Ar40": )").has_value());
  }
  // No entry at all, only values that are not entries.
  EXPECT_FALSE(parse_revision(FileKind::Intercepts, R"({"Ar40": 1.5})").has_value());
  EXPECT_FALSE(parse_revision(FileKind::Blanks, R"({"Ar40": "x"})").has_value());
}

TEST(Layout, TopLevelValueBesideTheEntriesGoesToExtra) {
  // A key that is not an isotope or detector entry does not fail the file.
  const std::string text = R"({"Ar40": {"value": 2.5, "error": 0.1, "fit": "linear"},
                               "reviewed": true, "note": "by hand", "version": 2, "nothing": null})";
  for (FileKind kind : {FileKind::Intercepts, FileKind::Baselines, FileKind::Blanks, FileKind::IcFactors}) {
    auto r = parse_revision(kind, text);
    ASSERT_TRUE(r.has_value()) << r.error().what;
    EXPECT_EQ(extra(r->extra_json), json::parse(R"({"reviewed": true, "note": "by hand", "version": 2, "nothing": null})"));
  }
  const auto rows = rows_of<ps::Intercepts>(FileKind::Intercepts, text);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].isotope, "Ar40");
  EXPECT_EQ(rows[0].value, std::optional<double>{2.5});
  // A bare NaN there is recorded like any other.
  auto nan = parse_revision(FileKind::Intercepts, R"({"Ar40": {"value": 1.0}, "scale": NaN})");
  ASSERT_TRUE(nan.has_value()) << nan.error().what;
  EXPECT_EQ(extra(nan->extra_json), json::parse(R"({"scale": null, "nonfinite": {"/scale": "NaN"}})"));
  // Without such keys there is no extra.
  EXPECT_FALSE(parse_revision(FileKind::Intercepts, R"({"Ar40": {"value": 1.0}})")->extra_json.has_value());
}

TEST(Layout, ParseRevisionRejectsKindsThatAreNotRevisions) {
  for (FileKind kind : {FileKind::Record, FileKind::Data, FileKind::PeakCenter, FileKind::Extraction,
                        FileKind::Monitor, FileKind::InterpretedAge, FileKind::Spectrometer,
                        FileKind::FrozenProduction, FileKind::Ignored, FileKind::Unknown})
    EXPECT_FALSE(parse_revision(kind, "{}").has_value());
}

TEST(Layout, PayloadsMatchTheirStoreKind) {
  auto check = [](FileKind file, ps::Kind kind, const std::string& text) {
    auto r = parse_revision(file, text);
    ASSERT_TRUE(r.has_value()) << r.error().what;
    EXPECT_TRUE(ps::payload_kind_matches(kind, r->payload));
  };
  check(FileKind::Intercepts, ps::Kind::Intercepts, "{}");
  check(FileKind::Baselines, ps::Kind::Baselines, "{}");
  check(FileKind::Blanks, ps::Kind::Blanks, "{}");
  check(FileKind::IcFactors, ps::Kind::IcFactors, "{}");
  check(FileKind::Tags, ps::Kind::Tags, R"({"name": "ok"})");
  check(FileKind::Cosmogenic, ps::Kind::Cosmogenic, "{}");
}

}  // namespace
}  // namespace pychron::dvc
