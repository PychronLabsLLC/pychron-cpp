// The layout of a legacy MetaData repository: which path is which reference
// file, and what each file holds. Expected values are the literals of the real
// files under fixtures/meta (fixtures/README.md, section 6).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fixture_files.hpp"
#include "meta_layout.hpp"

using namespace pychron;
using namespace pychron::dvc;
using namespace pychron::dvc::testing;
namespace P = pychron::persistence;

namespace {

const char* const kZone = "America/Denver";

MetaPath path(const char* text) { return classify_meta_path(text); }

}  // namespace

TEST(MetaLayout, ReferencePathsAreRecognised) {
  const auto is = [](const char* text, MetaKind kind, const char* irradiation, const char* name) {
    const MetaPath got = path(text);
    EXPECT_EQ(got.kind, kind) << text;
    EXPECT_EQ(got.irradiation, irradiation) << text;
    EXPECT_EQ(got.name, name) << text;
  };
  is("NM-293/G.json", MetaKind::Level, "NM-293", "G");
  is("NM-293/AA.json", MetaKind::Level, "NM-293", "AA");
  is("NM-293/productions.json", MetaKind::LevelProductions, "NM-293", "");
  is("NM-293/productions/Triga_PR.json", MetaKind::Production, "NM-293", "Triga_PR");
  is("NM-293/chronology.txt", MetaKind::Chronology, "NM-293", "");
  is("spectrometers/jan.gain.json", MetaKind::Gains, "", "jan");
  is("spectrometers/Felix.sens.json", MetaKind::Sensitivity, "", "Felix");
  is("irradiation_holders/24_hole.txt", MetaKind::IrradiationHolder, "", "24_hole");
  is("load_holders/37-hole.txt", MetaKind::LoadHolder, "", "37-hole");
}

TEST(MetaLayout, EverythingElseIsIgnored) {
  for (const char* text :
       {"README.md", ".gitignore", "molecular_weights.json", "reactors.json", "correlation_ellipses.json",
        "data_reduction_log.json", "dr_manifest.json", "sensitivity.json", "cocktail.json",
        // Not read by the legacy code: productions live under their irradiation.
        "productions/Triga_PR.json", "scripts/felix/measurement/unknown.py", "scripts/defaults.yaml",
        "experiments/felix/template.txt", "experiments/queue.txt",
        // In the reference directories, but not a reference file.
        "irradiation_holders/24_hole.xml", "irradiation_holders/.txt", "load_holders/notes.md",
        "spectrometers/felix.json", "spectrometers/.gain.json", "spectrometers/felix/detectors.yaml",
        // In an irradiation, but nothing the legacy code reads.
        "NM-293/notes.txt", "NM-293/.json", "NM-293/.hidden.json", "NM-293/productions/Triga_PR.txt",
        "NM-293/productions/.json", "NM-293/productions/old/Triga_PR.json", "NM-293/G/extra.json",
        ".github/workflows.json", ""})
    EXPECT_EQ(path(text).kind, MetaKind::Ignored) << text;
}

TEST(MetaLayout, LevelFileGivesItsPositionsByHoleNumber) {
  auto level = parse_level(fixture("meta/NM-293/G.json"));
  ASSERT_TRUE(level) << level.error().what;
  EXPECT_EQ(level->positions.size(), 23u);
  EXPECT_EQ(level->header, (Json{{"z", 0}}));

  const ParsedFlux flux = flux_value(level->positions.at(16));
  EXPECT_EQ(flux.value.j, std::optional<double>{0.0018848683037985877});
  EXPECT_EQ(flux.value.j_err, std::optional<double>{6.292765550352345e-07});
  EXPECT_EQ(flux.value.mean_j, std::optional<double>{0.0});
  EXPECT_EQ(flux.value.mean_j_err, std::optional<double>{0.0});
  EXPECT_EQ(flux.value.lambda_k_total, std::optional<double>{5.464e-10});
  EXPECT_EQ(flux.value.lambda_k_total_err, std::optional<double>{0.0});
  EXPECT_FALSE(flux.value.mean_j_mswd);
  EXPECT_TRUE(flux.value.analyses.empty());
  ASSERT_TRUE(flux.value.options_json);
  EXPECT_EQ(Json::parse(*flux.value.options_json).at("model_kind"), "Plane");
  // What FluxValue has no field for stays in its extra.
  ASSERT_TRUE(flux.value.extra_json);
  EXPECT_EQ(Json::parse(*flux.value.extra_json), (Json{{"identifier", "66052"}}));
  EXPECT_EQ(flux.detail, Json::object());

  // The monitor position: its analyses, `status` being the omitted flag.
  const ParsedFlux monitor = flux_value(level->positions.at(1));
  EXPECT_EQ(monitor.value.j, std::optional<double>{0.0018634713371872254});
  EXPECT_EQ(monitor.value.mean_j, std::optional<double>{0.0018828848695731024});
  ASSERT_EQ(monitor.value.analyses.size(), 6u);
  EXPECT_EQ(monitor.value.analyses[0].record_id, "25761-01");
  EXPECT_EQ(monitor.value.analyses[0].analysis, P::Uuid::parse("c6503871-b3d2-44e1-b045-170f36c30cf8"));
  EXPECT_FALSE(monitor.value.analyses[0].is_omitted);

  const ParsedLevelZ z = level_z_value(*level);
  EXPECT_EQ(z.value.z, std::optional<double>{0.0});
  EXPECT_EQ(z.detail, Json::object());
}

TEST(MetaLayout, LevelFileOlderFormsAndKeysOfTheSource) {
  // A bare list (the older form), the keys the fixture lacks, and a bare NaN.
  auto level = parse_level(R"([
    {"position": 3, "identifier": "100", "j": 0.001, "j_err": NaN, "mean_j_mswd": 1.5, "position_jerr": 2e-6,
     "monitor": {"name": "FC-2", "age": 28.201, "error": 0.046, "material": "sanidine", "reference": "Kuiper"},
     "decay_constants": {"lambda_k_total": 5.5e-10, "lambda_k_total_error": 1e-12, "lambda_b": 4.9e-10},
     "analyses": [{"record_id": "100-01", "uuid": "c6503871-b3d2-44e1-b045-170f36c30cf8", "is_omitted": true},
                  {"record_id": "100-02", "uuid": "not a uuid", "status": 1, "weight": 2},
                  {"record_id": "100-01", "uuid": "c1b6b24f-7359-41cf-a424-f6b4c7eed80d"},
                  "100-03"]},
    {"position": "4", "j": 0.002},
    {"position": 3, "j": 0.009}
  ])");
  ASSERT_TRUE(level) << level.error().what;
  EXPECT_EQ(level->header, Json::object());
  ASSERT_EQ(level->positions.size(), 2u);
  EXPECT_EQ(flux_value(level->positions.at(4)).value.j, std::optional<double>{0.002});

  const ParsedFlux flux = flux_value(level->positions.at(3));
  EXPECT_EQ(flux.value.j, std::optional<double>{0.001});
  EXPECT_FALSE(flux.value.j_err);  // unknown, never 0
  EXPECT_EQ(flux.value.mean_j_mswd, std::optional<double>{1.5});
  EXPECT_EQ(flux.value.position_jerr, std::optional<double>{2e-6});
  EXPECT_EQ(flux.value.monitor_name, std::optional<std::string>{"FC-2"});
  EXPECT_EQ(flux.value.monitor_material, std::optional<std::string>{"sanidine"});
  EXPECT_EQ(flux.value.monitor_age, std::optional<double>{28.201});
  EXPECT_EQ(flux.value.monitor_age_err, std::optional<double>{0.046});
  EXPECT_EQ(flux.value.lambda_k_total_err, std::optional<double>{1e-12});
  ASSERT_EQ(flux.value.analyses.size(), 2u);
  EXPECT_TRUE(flux.value.analyses[0].is_omitted);
  EXPECT_EQ(flux.value.analyses[1].record_id, "100-02");
  EXPECT_FALSE(flux.value.analyses[1].analysis);
  EXPECT_TRUE(flux.value.analyses[1].is_omitted);
  ASSERT_TRUE(flux.value.extra_json);
  EXPECT_EQ(Json::parse(*flux.value.extra_json),
            Json::parse(R"({"identifier": "100",
                            "monitor": {"reference": "Kuiper"},
                            "decay_constants": {"lambda_b": 4.9e-10},
                            "analyses": {"100-02": {"uuid": "not a uuid", "weight": 2}},
                            "analyses_unmapped": [{"record_id": "100-01", "uuid": "c1b6b24f-7359-41cf-a424-f6b4c7eed80d"},
                                                  "100-03"],
                            "nonfinite": {"/j_err": "NaN"}})"));
  // The first entry of a hole number is the one the legacy code reads.
  EXPECT_EQ(flux.detail, Json::parse(R"({"duplicate_entries": [{"position": 3, "j": 0.009}]})"));
}

TEST(MetaLayout, LevelFileThatCannotBeReadIsAnError) {
  EXPECT_FALSE(parse_level("not json"));
  EXPECT_FALSE(parse_level("{\"z\": 0}"));
  EXPECT_FALSE(parse_level("{\"positions\": {}}"));
  EXPECT_FALSE(parse_level("[1, 2]"));
  EXPECT_FALSE(parse_level("[{\"j\": 0.001}]"));
  EXPECT_FALSE(parse_level("[{\"position\": \"north\", \"j\": 0.001}]"));
  EXPECT_FALSE(parse_level("\"G\""));
  // Other top-level keys travel with z.
  auto level = parse_level(R"({"positions": [], "z": "1.5", "note": "top tray"})");
  ASSERT_TRUE(level);
  const ParsedLevelZ z = level_z_value(*level);
  EXPECT_EQ(z.value.z, std::optional<double>{1.5});
  EXPECT_EQ(z.detail, (Json{{"extra", {{"note", "top tray"}}}}));
}

TEST(MetaLayout, LevelProductions) {
  auto map = parse_level_productions(fixture("meta/NM-293/productions.json"));
  ASSERT_TRUE(map) << map.error().what;
  EXPECT_EQ(map->levels.size(), 12u);
  EXPECT_EQ(map->levels.at("G"), "Triga_PR");
  EXPECT_FALSE(map->note);
  EXPECT_EQ(map->extra, Json::object());

  auto noted = parse_level_productions(R"({"A": "Triga_PR", "note": "second batch", "B": 3, "C": ""})");
  ASSERT_TRUE(noted);
  EXPECT_EQ(noted->levels, (std::map<std::string, std::string>{{"A", "Triga_PR"}}));
  EXPECT_EQ(noted->note, std::optional<std::string>{"second batch"});
  EXPECT_EQ(noted->extra, Json::parse(R"({"B": 3, "C": ""})"));

  EXPECT_FALSE(parse_level_productions("[]"));
  EXPECT_FALSE(parse_level_productions("{"));
}

TEST(MetaLayout, ChronologyDosesAreLocalTimes) {
  auto chronology = parse_chronology(fixture("meta/NM-293/chronology.txt"), kZone);
  ASSERT_TRUE(chronology) << chronology.error().what;
  ASSERT_EQ(chronology->value.doses.size(), 1u);
  const P::Dose& dose = chronology->value.doses[0];
  EXPECT_EQ(dose.ordinal, 0);
  EXPECT_EQ(dose.power, 1.0);
  EXPECT_EQ(dose.start, *P::UtcTime::parse("2017-12-21T13:28:00Z"));  // 06:28 Mountain Standard Time
  EXPECT_EQ(dose.end, *P::UtcTime::parse("2017-12-21T21:28:00Z"));
  EXPECT_EQ(chronology->detail, Json::object());
}

TEST(MetaLayout, ChronologyKeepsTheLinesItCannotInterpret) {
  auto chronology = parse_chronology("# reactor log\r\n"
                                     "\r\n"
                                     "1.0,2017-12-21 06:28:00,2017-12-21 14:28:00\r\n"
                                     "2017-12-22 06:28:00%2017-12-22 14:28:00\r\n"
                                     "full,2017-12-23 06:28:00,2017-12-23 14:28:00\n"
                                     " 0.5 , 2017-12-24 06:28:00 , 2017-12-24 14:28:00 \n"
                                     "1.0,2018-11-04 01:30:00,2018-03-11 02:30:00\n",
                                     kZone);
  ASSERT_TRUE(chronology) << chronology.error().what;
  ASSERT_EQ(chronology->value.doses.size(), 3u);
  EXPECT_EQ(chronology->value.doses[1].ordinal, 1);
  EXPECT_EQ(chronology->value.doses[1].power, 0.5);
  EXPECT_EQ(chronology->value.doses[1].start, *P::UtcTime::parse("2017-12-24T13:28:00Z"));
  EXPECT_EQ(chronology->detail.at("uninterpreted_lines"),
            Json::parse(R"([{"line": 1, "text": "# reactor log"},
                            {"line": 4, "text": "2017-12-22 06:28:00%2017-12-22 14:28:00"},
                            {"line": 5, "text": "full,2017-12-23 06:28:00,2017-12-23 14:28:00"}])"));
  // A time the clocks went through twice, and one they skipped: said, not guessed silently.
  EXPECT_EQ(chronology->value.doses[2].start, *P::UtcTime::parse("2018-11-04T07:30:00Z"));
  EXPECT_EQ(chronology->value.doses[2].end, *P::UtcTime::parse("2018-03-11T09:00:00Z"));
  ASSERT_EQ(chronology->detail.at("notes").size(), 2u);
  EXPECT_NE(chronology->detail.at("notes")[0].get<std::string>().find("ambiguous"), std::string::npos);
  EXPECT_NE(chronology->detail.at("notes")[1].get<std::string>().find("nonexistent"), std::string::npos);

  // No dose yet is a chronology; text with no dose in it is not one.
  auto empty = parse_chronology("\n", kZone);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->value.doses.empty());
  EXPECT_FALSE(parse_chronology("\x89PNG\r\n\x1a\n", kZone));
  EXPECT_FALSE(parse_chronology("to be filled in\n", kZone));
}

TEST(MetaLayout, GainsFile) {
  auto none = parse_gains(fixture("meta/spectrometers/jan.gain.json"));
  ASSERT_TRUE(none) << none.error().what;
  EXPECT_TRUE(none->value.gains.empty());
  EXPECT_EQ(none->detail, Json::object());

  auto gains = parse_gains(R"({"H1": 1.0025, "AX": "0.998", "CDD": NaN, "comment": "after bakeout"})");
  ASSERT_TRUE(gains);
  EXPECT_EQ(gains->value.gains, (std::vector<P::DetectorGain>{{"AX", 0.998}, {"H1", 1.0025}}));
  EXPECT_EQ(gains->detail,
            Json::parse(R"({"extra": {"CDD": null, "comment": "after bakeout"}, "nonfinite": {"/CDD": "NaN"}})"));
  EXPECT_FALSE(parse_gains("[]"));
  EXPECT_FALSE(parse_gains(""));
}

TEST(MetaLayout, SensitivityEntriesKeepTheirOrder) {
  auto entries = parse_sensitivities(fixture("meta/spectrometers/felix.sens.json"));
  ASSERT_TRUE(entries) << entries.error().what;
  ASSERT_EQ(entries->size(), 4u);
  // Not in date order in the file: 2020, 2000, 2020, 2021.
  const ParsedSensitivity first = sensitivity_value((*entries)[0], kZone);
  EXPECT_EQ(first.value.sensitivity, 2.13e-16);
  ASSERT_TRUE(first.value.create_date);
  EXPECT_EQ(*first.value.create_date, *P::UtcTime::parse("2020-01-27T19:54:30Z"));
  ASSERT_TRUE(first.value.extra_json);
  EXPECT_EQ(Json::parse(*first.value.extra_json),
            Json::parse(R"({"mass_spectrometer": "felix", "note": "", "units": "mol/fA", "user": ""})"));
  EXPECT_EQ(first.detail, Json::object());
  const ParsedSensitivity second = sensitivity_value((*entries)[1], kZone);
  EXPECT_EQ(second.value.sensitivity, 4e-16);
  EXPECT_EQ(*second.value.create_date, *P::UtcTime::parse("2000-05-18T18:54:30Z"));  // daylight time
  EXPECT_EQ((*entries)[0], (*entries)[2]);
  EXPECT_EQ(sensitivity_value((*entries)[3], kZone).value.sensitivity, 5e-16);

  // A date that is not one stays as written.
  auto odd = parse_sensitivities(R"([{"sensitivity": "3e-16", "create_date": "last spring"}])");
  ASSERT_TRUE(odd);
  const ParsedSensitivity value = sensitivity_value(odd->front(), kZone);
  EXPECT_EQ(value.value.sensitivity, 3e-16);
  EXPECT_FALSE(value.value.create_date);
  EXPECT_EQ(Json::parse(*value.value.extra_json), (Json{{"create_date", "last spring"}}));

  EXPECT_FALSE(parse_sensitivities("{}"));
  EXPECT_FALSE(parse_sensitivities("[3e-16]"));
  EXPECT_FALSE(parse_sensitivities(R"([{"sensitivity": NaN}])"));
  EXPECT_FALSE(parse_sensitivities(R"([{"units": "mol/fA"}])"));
}

TEST(MetaLayout, IrradiationHolder) {
  auto holder = parse_holder(fixture("meta/irradiation_holders/24_hole.txt"));
  ASSERT_TRUE(holder) << holder.error().what;
  EXPECT_FALSE(holder->value.shape);  // the header's first field is a number here
  EXPECT_EQ(holder->value.radius, std::optional<double>{0.0175});
  EXPECT_FALSE(holder->value.has_hole_numbers);
  ASSERT_EQ(holder->value.holes.size(), 56u);
  EXPECT_EQ(holder->value.holes[0], (P::HolderHole{0, "1", 0.0, 0.4050, 0.0175}));
  EXPECT_EQ(holder->value.holes[55], (P::HolderHole{55, "56", -0.0990, 0.0990, 0.0175}));
  EXPECT_EQ(holder->detail, (Json{{"header", "56,0.0175"}}));
}

TEST(MetaLayout, LoadHolder) {
  auto holder = parse_holder(fixture("meta/load_holders/37-hole.txt"));
  ASSERT_TRUE(holder) << holder.error().what;
  EXPECT_EQ(holder->value.shape, std::optional<std::string>{"circle"});
  EXPECT_EQ(holder->value.radius, std::optional<double>{1.75});
  ASSERT_EQ(holder->value.holes.size(), 37u);
  EXPECT_EQ(holder->value.holes[0], (P::HolderHole{0, "1", -4.8006, 14.4018, std::nullopt}));
  EXPECT_EQ(holder->value.holes[36], (P::HolderHole{36, "37", 4.8006, -14.4018, std::nullopt}));
}

TEST(MetaLayout, HolderLinesThatAreNotHoles) {
  // A blank line and a comment still advance the hole number, as in the legacy reader.
  auto holder = parse_holder("circle,1.5\n"
                             "0,1\n"
                             "\n"
                             "# second row\n"
                             "2,3,0.5\n"
                             "east,west\n"
                             "4,5,6,7\n"
                             "8,9");
  ASSERT_TRUE(holder) << holder.error().what;
  EXPECT_EQ(holder->value.holes,
            (std::vector<P::HolderHole>{{0, "1", 0, 1, std::nullopt}, {1, "4", 2, 3, 0.5}, {2, "7", 8, 9, std::nullopt}}));
  EXPECT_EQ(holder->detail, Json::parse(R"({"header": "circle,1.5",
                                            "comments": [{"line": 4, "text": "# second row"}],
                                            "uninterpreted_lines": [{"line": 6, "text": "east,west"},
                                                                    {"line": 7, "text": "4,5,6,7"}]})"));

  auto numbered = parse_holder("grid,2.0,True\nA1,0,1\nA2,2,3,0.25\n5,6\n");
  ASSERT_TRUE(numbered);
  EXPECT_TRUE(numbered->value.has_hole_numbers);
  EXPECT_EQ(numbered->value.holes,
            (std::vector<P::HolderHole>{{0, "A1", 0, 1, std::nullopt}, {1, "A2", 2, 3, 0.25}}));
  EXPECT_EQ(numbered->detail.at("uninterpreted_lines"), Json::parse(R"([{"line": 4, "text": "5,6"}])"));

  EXPECT_FALSE(parse_holder(""));
  EXPECT_FALSE(parse_holder("<holder/>\n"));
  EXPECT_FALSE(parse_holder("circle,wide\n0,1\n"));
}
