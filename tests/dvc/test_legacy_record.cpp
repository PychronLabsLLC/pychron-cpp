// parse_record, merge_satellite and parse_spectrometer against the real
// files. Every literal below was copied by hand from the fixture it names.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

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

const ParseContext kDenver{"America/Denver"};

// A minimal record; `more` is spliced into the object.
std::string record(const std::string& more = {}) {
  return R"({"uuid": "15fb3686-4aed-40c1-8987-e73a8a52b434", "identifier": "66052", "aliquot": 1, "increment": 4,
            "analysis_type": "unknown", "timestamp": "2018-02-20T00:27:08.852603", "mass_spectrometer": "Felix")" +
         (more.empty() ? "" : ", " + more) + "}";
}

json legacy(const ps::AnalysisIngest& a) {
  EXPECT_TRUE(a.meta.has_value());
  if (!a.meta || !a.meta->legacy_json) return json::object();
  return json::parse(*a.meta->legacy_json);
}

TEST(Layout, RecordFixture) {
  auto r = parse_record(fixture(kUnknown + "660/52-01E.json"), kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto& a = r->ingest;
  EXPECT_TRUE(r->had_uuid);
  EXPECT_EQ(a.analysis.str(), "15fb3686-4aed-40c1-8987-e73a8a52b434");
  EXPECT_EQ(a.identifier, "66052");
  EXPECT_EQ(a.aliquot, 1);
  EXPECT_EQ(a.increment, 4);
  EXPECT_EQ(r->runid, "66052-01E");
  EXPECT_EQ(a.analysis_type, "unknown");
  // The catalog names spectrometers in lower case ("felix.sens.json"); the
  // file's spelling is kept.
  EXPECT_EQ(a.mass_spectrometer, "felix");
  EXPECT_EQ(legacy(a)["record"]["mass_spectrometer"], "Felix");
  // "2018-02-20T00:27:08.852603" in America/Denver (MST, UTC-7).
  EXPECT_EQ(a.timestamp.iso(), "2018-02-20T07:27:08.852603Z");
  EXPECT_FALSE(a.time_zero.has_value());
  EXPECT_EQ(a.analyst, "MHeizler");
  EXPECT_FALSE(a.laboratory.has_value());        // "" in the file
  EXPECT_FALSE(a.instrument_name.has_value());   // "" in the file
  EXPECT_FALSE(a.experiment_type.has_value());
  EXPECT_TRUE(r->notes.empty());

  EXPECT_EQ(r->spec_sha, "6a9b4615cd24138b6ce541f75240dc4091378bb1");
  EXPECT_EQ(r->comment, "G:16 Plag, 4 mg");
  EXPECT_EQ(r->catalog.repository, "IR1010");
  EXPECT_EQ(r->catalog.sample, "SB15-03");
  EXPECT_EQ(r->catalog.material, "Feldspar");
  EXPECT_EQ(r->catalog.project, "IR1010");
  EXPECT_EQ(r->catalog.irradiation, "NM-293");
  EXPECT_EQ(r->catalog.irradiation_level, "G");
  EXPECT_EQ(r->catalog.irradiation_position, 16);
  EXPECT_FALSE(r->catalog.principal_investigator.has_value());
  EXPECT_EQ(r->script_names.measurement, "felix_analysis340_60_CDD_center.py");
  EXPECT_EQ(r->script_names.extraction, "felix_diode.py");
  EXPECT_EQ(r->script_names.post_measurement, "felix_pump_ms.py");
  EXPECT_EQ(r->script_names.post_equilibration, "felix_pump_extraction_line.py");

  // isotopes.<iso>.detector
  ASSERT_EQ(a.isotopes.size(), 5u);
  std::map<std::string, std::string> detector_of;
  for (const auto& i : a.isotopes) detector_of[i.isotope] = i.detector;
  EXPECT_EQ(detector_of["Ar36"], "L2(CDD)");
  EXPECT_EQ(detector_of["Ar37"], "L1");
  EXPECT_EQ(detector_of["Ar38"], "AX");
  EXPECT_EQ(detector_of["Ar39"], "H1");
  EXPECT_EQ(detector_of["Ar40"], "H2");
  // detectors.<det>.deflection, .gain
  ASSERT_EQ(a.detectors.size(), 5u);
  for (const auto& d : a.detectors) {
    EXPECT_EQ(d.deflection, 0.0) << d.detector;
    EXPECT_EQ(d.gain_used, 0.0) << d.detector;
  }

  // The JSON columns of analysis_meta.
  ASSERT_TRUE(a.meta.has_value());
  EXPECT_EQ(json::parse(a.meta->source_json.value()), json::parse(R"({"emission": 864.692, "trap": 248.831})"));
  EXPECT_EQ(a.meta->intensity_scalar, 0.0);
  EXPECT_EQ(json::parse(a.meta->environmental_json.value())["lab_humiditys"][0]["value"], 21.8);
  const auto conditionals = json::parse(a.meta->conditionals_json.value());
  ASSERT_EQ(conditionals.size(), 1u);
  EXPECT_EQ(conditionals[0]["teststr"], "L2(CDD).deflection==3250");
  EXPECT_EQ(conditionals[0]["hash_id"], -7399522718437156748LL);  // a 64-bit integer survives
  EXPECT_FALSE(a.meta->tripped_conditional_json.has_value());      // null in the file
  EXPECT_FALSE(a.meta->whiff_result_json.has_value());             // null in the file
  EXPECT_EQ(json::parse(a.meta->software_json.value()),
            json::parse(R"j({"acquisition_software": "Pychron17.7(Exp0.2,DVC0.1)",
                             "data_reduction_software": "Pychron17.7(DVC0.1)"})j"));
  EXPECT_EQ(json::parse(a.meta->queue_names_json.value()),
            json::parse(R"({"experiment_queue_name": "CurrentExperiment", "queue_conditionals_name": "normal"})"));

  // Everything without a typed home in the ingest is kept, under "record".
  const json rest = legacy(a)["record"];
  EXPECT_EQ(rest["commit"], "c27fcc912f0b5dc6e984fd84cdf21a21424f7c98");
  EXPECT_EQ(rest["comment"], "G:16 Plag, 4 mg");
  EXPECT_EQ(rest["sample"], "SB15-03");
  EXPECT_EQ(rest["irradiation_position"], 16);
  EXPECT_EQ(rest["repository_identifier"], "IR1010");
  EXPECT_EQ(rest["measurement"], "felix_analysis340_60_CDD_center.py");
  EXPECT_EQ(rest["spec_sha"], "6a9b4615cd24138b6ce541f75240dc4091378bb1");
  EXPECT_EQ(rest["analyst_name"], "MHeizler");
  // What a typed field carries is not repeated.
  for (const char* key : {"uuid", "identifier", "aliquot", "increment", "analysis_type", "timestamp",
                          "username", "source", "environmental", "conditionals", "detectors",
                          "intensity_scalar", "acquisition_software", "experiment_queue_name"})
    EXPECT_FALSE(rest.contains(key)) << key;
  // isotopes.<iso>.name has no column; it stays.
  EXPECT_EQ(rest["isotopes"]["Ar36"]["name"], "Ar36");
  EXPECT_FALSE(rest["isotopes"]["Ar36"].contains("detector"));

  // The roots are not this file's business: the default tag is untouched.
  EXPECT_EQ(a.roots.tag.name, "ok");
  EXPECT_TRUE(a.roots.intercepts_rows.empty());
}

TEST(Layout, RecordOfTheBlankHasNoStep) {
  auto r = parse_record(fixture(kBlank + "bu-FD/-F-789.json"), kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto& a = r->ingest;
  EXPECT_EQ(a.analysis.str(), "7834c4f8-f3b8-4aac-b586-53f7cb8d3d5c");
  EXPECT_EQ(a.identifier, "bu-FD-F");
  EXPECT_EQ(a.aliquot, 789);
  EXPECT_EQ(a.increment, -1);  // "increment": null
  EXPECT_EQ(r->runid, "bu-FD-F-789");
  EXPECT_EQ(a.analysis_type, "blank_unknown");
  EXPECT_EQ(a.timestamp.iso(), "2018-02-20T06:28:58.784926Z");  // 2018-02-19T23:28:58.784926 MST
  EXPECT_EQ(r->spec_sha, "fad234d16fee6c00a96e96e15edccd97faba9521");
  EXPECT_FALSE(r->comment.has_value());  // ""
  EXPECT_EQ(r->catalog.repository, "Felix_blank180");
  EXPECT_EQ(r->catalog.irradiation, "NoIrradiation");
  EXPECT_EQ(r->catalog.irradiation_position, 22);
  // The null increment is represented by -1 and not repeated.
  EXPECT_FALSE(legacy(a)["record"].contains("increment"));
}

TEST(Layout, RecordWithoutUuid) {
  auto r = parse_record(R"({"identifier": "66052", "aliquot": 1, "increment": 4, "analysis_type": "unknown",
                            "timestamp": "2018-02-20T00:27:08", "mass_spectrometer": "Felix"})",
                        kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  EXPECT_FALSE(r->had_uuid);
  EXPECT_TRUE(r->ingest.analysis.is_nil());
  EXPECT_EQ(r->runid, "66052-01E");

  // An empty or malformed uuid is no uuid; the malformed text is kept.
  auto empty = parse_record(R"({"uuid": "", "identifier": "66052", "aliquot": 1, "timestamp": "2018-02-20T00:27:08",
                                "mass_spectrometer": "Felix"})",
                            kDenver);
  ASSERT_TRUE(empty.has_value()) << empty.error().what;
  EXPECT_FALSE(empty->had_uuid);
  auto bad = parse_record(R"({"uuid": "15fb36864aed40c18987e73a8a52b434", "identifier": "66052", "aliquot": 1,
                              "timestamp": "2018-02-20T00:27:08", "mass_spectrometer": "Felix"})",
                          kDenver);
  ASSERT_TRUE(bad.has_value()) << bad.error().what;
  EXPECT_FALSE(bad->had_uuid);
  EXPECT_TRUE(bad->ingest.analysis.is_nil());
  EXPECT_EQ(legacy(bad->ingest)["record"]["uuid"], "15fb36864aed40c18987e73a8a52b434");
  ASSERT_EQ(bad->notes.size(), 1u);
  EXPECT_NE(bad->notes[0].find("uuid"), std::string::npos);
}

TEST(Layout, NaiveTimestampUsesLabZone) {
  auto r = parse_record(R"({"uuid": "15fb3686-4aed-40c1-8987-e73a8a52b434", "identifier": "66052", "aliquot": 1,
                            "timestamp": "2019-11-03 01:30:00", "mass_spectrometer": "Felix"})",
                        kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  EXPECT_EQ(r->ingest.timestamp.iso(), "2019-11-03T07:30:00.000000Z");
  ASSERT_EQ(r->notes.size(), 1u);
  EXPECT_NE(r->notes[0].find("ambiguous"), std::string::npos);
  EXPECT_NE(r->notes[0].find("2019-11-03 01:30:00"), std::string::npos);

  // Clocks set forward: 02:30 on 2019-03-10 does not exist in Denver.
  auto gap = parse_record(R"({"identifier": "66052", "aliquot": 1, "timestamp": "2019-03-10T02:30:00",
                              "mass_spectrometer": "Felix"})",
                          kDenver);
  ASSERT_TRUE(gap.has_value()) << gap.error().what;
  EXPECT_EQ(gap->ingest.timestamp.iso(), "2019-03-10T09:00:00.000000Z");
  ASSERT_EQ(gap->notes.size(), 1u);
  EXPECT_NE(gap->notes[0].find("nonexistent"), std::string::npos);

  // Another zone gives another instant.
  auto utc = parse_record(record(), ParseContext{"UTC"});
  ASSERT_TRUE(utc.has_value()) << utc.error().what;
  EXPECT_EQ(utc->ingest.timestamp.iso(), "2018-02-20T00:27:08.852603Z");
}

TEST(Layout, RecordTimeZeroIsConvertedToo) {
  auto r = parse_record(record(R"("time_zero_timestamp": "2018-02-20T00:20:00")"), kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  ASSERT_TRUE(r->ingest.time_zero.has_value());
  EXPECT_EQ(r->ingest.time_zero->iso(), "2018-02-20T07:20:00.000000Z");
}

TEST(Layout, RecordErrors) {
  EXPECT_FALSE(parse_record("not json", kDenver).has_value());
  EXPECT_FALSE(parse_record("[1, 2]", kDenver).has_value());
  // Required keys.
  EXPECT_FALSE(parse_record(R"({"aliquot": 1, "timestamp": "2018-02-20T00:27:08", "mass_spectrometer": "Felix"})",
                            kDenver)
                   .has_value());
  EXPECT_FALSE(parse_record(R"({"identifier": "66052", "timestamp": "2018-02-20T00:27:08",
                                "mass_spectrometer": "Felix"})",
                            kDenver)
                   .has_value());
  EXPECT_FALSE(parse_record(R"({"identifier": "66052", "aliquot": 1, "mass_spectrometer": "Felix"})", kDenver)
                   .has_value());
  EXPECT_FALSE(parse_record(R"({"identifier": "66052", "aliquot": 1, "timestamp": "2018-02-20T00:27:08"})", kDenver)
                   .has_value());
  // A timestamp that is not naive local time, and a zone that does not exist.
  auto offset = parse_record(R"({"identifier": "66052", "aliquot": 1, "timestamp": "2018-02-20T00:27:08-07:00",
                                 "mass_spectrometer": "Felix"})",
                             kDenver);
  ASSERT_FALSE(offset.has_value());
  EXPECT_NE(offset.error().what.find("timestamp"), std::string::npos);
  EXPECT_FALSE(parse_record(record(), ParseContext{"Mars/Olympus"}).has_value());
  // An aliquot that is not an integer.
  EXPECT_FALSE(parse_record(R"({"identifier": "66052", "aliquot": "x", "timestamp": "2018-02-20T00:27:08",
                                "mass_spectrometer": "Felix"})",
                            kDenver)
                   .has_value());
}

TEST(Layout, RecordToleratesOtherTypes) {
  // identifier as a number, aliquot and increment as strings.
  auto r = parse_record(R"({"identifier": 66052, "aliquot": "01", "increment": "4",
                            "timestamp": "2018-02-20T00:27:08", "mass_spectrometer": "Felix"})",
                        kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  EXPECT_EQ(r->ingest.identifier, "66052");
  EXPECT_EQ(r->ingest.aliquot, 1);
  EXPECT_EQ(r->ingest.increment, 4);
}

TEST(Layout, RecordAnalysisTypeSampleOrEmptyIsUnknown) {
  // README 5.1: the legacy reader maps "sample" and empty to "unknown".
  auto sample = parse_record(R"({"identifier": "66052", "aliquot": 1, "analysis_type": "sample",
                                 "timestamp": "2018-02-20T00:27:08", "mass_spectrometer": "Felix"})",
                             kDenver);
  ASSERT_TRUE(sample.has_value()) << sample.error().what;
  EXPECT_EQ(sample->ingest.analysis_type, "unknown");
  EXPECT_EQ(legacy(sample->ingest)["record"]["analysis_type"], "sample");  // the original is kept

  auto absent = parse_record(R"({"identifier": "66052", "aliquot": 1, "timestamp": "2018-02-20T00:27:08",
                                 "mass_spectrometer": "Felix"})",
                             kDenver);
  ASSERT_TRUE(absent.has_value()) << absent.error().what;
  EXPECT_EQ(absent->ingest.analysis_type, "unknown");
}

TEST(Layout, RecordUnknownKeysGoToLegacyJson) {
  // Keys of a 2022 record (README 5.1) and one nobody has seen.
  auto r = parse_record(record(R"("zz_new": {"a": [1, 2]}, "experiment_type": "Ar/Ar", "grainsize": "",
                                  "latitude": 32.031192, "principal_investigator": "Heizler, M",
                                  "arar_mapping": {"Ar40": "Ar40"}, "collection_version": "2.0:3.0",
                                  "isotopes": {"Ar40": {"detector": "H2", "name": "Ar40", "units": "fA",
                                                        "serial_id": "0001", "zz": 1}},
                                  "detectors": {"H2": {"deflection": 1.5, "gain": 1.0002, "zz": 2}})"),
                        kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto& a = r->ingest;
  EXPECT_EQ(a.experiment_type, "Ar/Ar");
  EXPECT_EQ(r->catalog.principal_investigator, "Heizler, M");
  EXPECT_EQ(json::parse(a.meta->arar_mapping_json.value()), json::parse(R"({"Ar40": "Ar40"})"));
  ASSERT_EQ(a.isotopes.size(), 1u);
  EXPECT_EQ(a.isotopes[0].units, "fA");
  EXPECT_EQ(a.isotopes[0].detector_serial, "0001");
  ASSERT_EQ(a.detectors.size(), 1u);
  EXPECT_EQ(a.detectors[0].deflection, 1.5);
  EXPECT_EQ(a.detectors[0].gain_used, 1.0002);

  const json rest = legacy(a)["record"];
  EXPECT_EQ(rest["zz_new"], json::parse(R"({"a": [1, 2]})"));
  EXPECT_EQ(rest["latitude"], 32.031192);
  EXPECT_EQ(rest["grainsize"], "");
  EXPECT_EQ(rest["collection_version"], "2.0:3.0");
  EXPECT_EQ(rest["isotopes"]["Ar40"]["zz"], 1);
  EXPECT_EQ(rest["detectors"]["H2"]["zz"], 2);
  EXPECT_FALSE(rest["detectors"]["H2"].contains("gain"));
}

TEST(Layout, RecordNanIsUnknownNotZero) {
  auto r = parse_record(record(R"("intensity_scalar": NaN, "source": {"trap": Infinity},
                                  "detectors": {"H2": {"deflection": NaN, "gain": 1.0}})"),
                        kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  const auto& a = r->ingest;
  EXPECT_FALSE(a.meta->intensity_scalar.has_value());
  ASSERT_EQ(a.detectors.size(), 1u);
  EXPECT_FALSE(a.detectors[0].deflection.has_value());
  EXPECT_EQ(a.detectors[0].gain_used, 1.0);
  EXPECT_EQ(json::parse(a.meta->source_json.value()), json::parse(R"({"trap": null})"));
  EXPECT_EQ(legacy(a)["nonfinite"]["record"],
            json::parse(R"({"/intensity_scalar": "NaN", "/source/trap": "Infinity",
                            "/detectors/H2/deflection": "NaN"})"));
}

// ---------------------------------------------------------------- extraction

TEST(Layout, ExtractionFixture) {
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  auto ok = merge_satellite(FileKind::Extraction, fixture(kUnknown + "660/extraction/52-01E.extr.json"), a, blobs);
  ASSERT_TRUE(ok.has_value()) << ok.error().what;
  EXPECT_TRUE(blobs.empty());
  EXPECT_EQ(a.extract_device, "Fusions Diode");
  EXPECT_EQ(a.extraction.extract_value, 4.0);
  EXPECT_EQ(a.extraction.extract_units, "watts");
  EXPECT_EQ(a.extraction.extract_duration, 40.0);
  EXPECT_EQ(a.extraction.cleanup_duration, 60.0);
  EXPECT_EQ(a.extraction.weight, 4.0);
  EXPECT_EQ(a.extraction.beam_diameter, 3.0);  // the string "3.0" in the file
  EXPECT_EQ(a.extraction.pattern, "diamond_37hole_3mmbeam.lp");
  EXPECT_EQ(a.extraction.ramp_duration, 0.0);
  EXPECT_EQ(a.extraction.ramp_rate, 0.0);
  EXPECT_EQ(a.extraction.tray, "37-hole");
  EXPECT_FALSE(a.extraction.pre_cleanup.has_value());
  ASSERT_EQ(a.measured_positions.size(), 1u);
  EXPECT_EQ(a.measured_positions[0].position, 25);  // the string "25" in the file
  EXPECT_FALSE(a.measured_positions[0].x.has_value());
  EXPECT_FALSE(a.measured_positions[0].is_degas);
  ASSERT_TRUE(a.meta.has_value());
  EXPECT_EQ(a.meta->snapshots_json, "[]");
  EXPECT_EQ(a.meta->videos_json, "[]");
  EXPECT_EQ(a.meta->grain_polygons_json, "[]");  // "grain_polygon_blob"

  const json rest = legacy(a)["extraction"];
  EXPECT_EQ(rest["commit"], "c27fcc912f0b5dc6e984fd84cdf21a21424f7c98");
  EXPECT_EQ(rest["setpoint_stream"], "");
  EXPECT_EQ(rest["measured_response"].get<std::string>().substr(0, 16), "lxe1TgAAAABAAtw9");
  EXPECT_EQ(rest["requested_output"].get<std::string>().substr(0, 20), "lxe1TgAAAABAAtw9PCum");
  for (const char* key : {"extract_device", "extract_value", "beam_diameter", "positions", "tray", "snapshots"})
    EXPECT_FALSE(rest.contains(key)) << key;
}

TEST(Layout, ExtractionOfTheBlankHasEmptyStrings) {
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  auto ok = merge_satellite(FileKind::Extraction, fixture(kBlank + "bu-FD/extraction/-F-789.extr.json"), a, blobs);
  ASSERT_TRUE(ok.has_value()) << ok.error().what;
  EXPECT_EQ(a.extract_device, "Fusions Diode");
  EXPECT_EQ(a.extraction.extract_value, 0.0);
  EXPECT_FALSE(a.extraction.extract_units.has_value());  // ""
  EXPECT_FALSE(a.extraction.beam_diameter.has_value());  // ""
  EXPECT_FALSE(a.extraction.pattern.has_value());        // ""
  EXPECT_EQ(a.extraction.weight, 8.0);
  ASSERT_EQ(a.measured_positions.size(), 1u);
  EXPECT_FALSE(a.measured_positions[0].position.has_value());  // "position": ""
  EXPECT_FALSE(legacy(a)["extraction"].contains("positions"));
}

TEST(Layout, ExtractionPositionMayBeAnIntegerAStringOrText) {
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  // The 2022 form: integer position, load and cleanup keys (README 5.7).
  auto ok = merge_satellite(FileKind::Extraction,
                            R"({"extract_device": "Fusions CO2", "load_name": "L-123", "load_holder": "221-hole",
                                "cryo_temperature": 80.5, "light_value": 12.0, "pre_cleanup_duration": 5.0,
                                "post_cleanup_duration": 7.0, "extraction_context": {"k": 1},
                                "grain_polygons": [[1, 2]],
                                "positions": [{"position": 4, "x": 1.5, "y": -2.5, "z": 0.25, "is_degas": true},
                                              {"position": "A3", "x": null, "y": null, "z": null, "is_degas": false},
                                              {"position": "7", "zz": 1}]})",
                            a, blobs);
  ASSERT_TRUE(ok.has_value()) << ok.error().what;
  EXPECT_EQ(a.load_name, "L-123");
  EXPECT_EQ(a.load_holder, "221-hole");
  EXPECT_EQ(a.extraction.cryo_temperature, 80.5);
  EXPECT_EQ(a.extraction.light_value, 12.0);
  EXPECT_EQ(a.extraction.pre_cleanup, 5.0);
  EXPECT_EQ(a.extraction.post_cleanup, 7.0);
  EXPECT_EQ(a.meta->extraction_context_json, R"({"k":1})");
  EXPECT_EQ(a.meta->grain_polygons_json, "[[1,2]]");
  ASSERT_EQ(a.measured_positions.size(), 3u);
  EXPECT_EQ(a.measured_positions[0].position, 4);
  EXPECT_EQ(a.measured_positions[0].x, 1.5);
  EXPECT_EQ(a.measured_positions[0].y, -2.5);
  EXPECT_EQ(a.measured_positions[0].z, 0.25);
  EXPECT_TRUE(a.measured_positions[0].is_degas);
  EXPECT_EQ(a.measured_positions[0].load_name, "L-123");
  EXPECT_FALSE(a.measured_positions[1].position.has_value());  // "A3" is not a hole number
  EXPECT_EQ(a.measured_positions[2].position, 7);
  // An entry the row cannot hold in full keeps the whole list verbatim.
  const json rest = legacy(a)["extraction"];
  ASSERT_TRUE(rest.contains("positions"));
  EXPECT_EQ(rest["positions"][1]["position"], "A3");
  EXPECT_EQ(rest["positions"][2]["zz"], 1);
}

TEST(Layout, ExtractionOlderDurationNames) {
  // README 5.7: older files may use `duration` and `cleanup`.
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  ASSERT_TRUE(merge_satellite(FileKind::Extraction, R"({"duration": 30.0, "cleanup": 90.0})", a, blobs).has_value());
  EXPECT_EQ(a.extraction.extract_duration, 30.0);
  EXPECT_EQ(a.extraction.cleanup_duration, 90.0);
  // The current names win when both are present; the older ones are kept.
  ps::AnalysisIngest b;
  ASSERT_TRUE(merge_satellite(FileKind::Extraction,
                              R"({"duration": 30.0, "extract_duration": 40.0, "cleanup_duration": NaN})", b, blobs)
                  .has_value());
  EXPECT_EQ(b.extraction.extract_duration, 40.0);
  EXPECT_FALSE(b.extraction.cleanup_duration.has_value());  // NaN is unknown, not 0
  EXPECT_EQ(legacy(b)["extraction"]["duration"], 30.0);
  EXPECT_EQ(legacy(b)["nonfinite"]["extraction"], json::parse(R"({"/cleanup_duration": "NaN"})"));
}

TEST(Layout, SatellitesKeepWhatTheRecordPutInLegacyJson) {
  auto r = parse_record(fixture(kUnknown + "660/52-01E.json"), kDenver);
  ASSERT_TRUE(r.has_value()) << r.error().what;
  std::vector<ps::BlobIngest> blobs;
  ASSERT_TRUE(
      merge_satellite(FileKind::Extraction, fixture(kUnknown + "660/extraction/52-01E.extr.json"), r->ingest, blobs)
          .has_value());
  const json all = legacy(r->ingest);
  EXPECT_EQ(all["record"]["sample"], "SB15-03");
  EXPECT_EQ(all["extraction"]["commit"], "c27fcc912f0b5dc6e984fd84cdf21a21424f7c98");
  EXPECT_EQ(r->ingest.meta->intensity_scalar, 0.0);  // record columns survive the merge
}

// ---------------------------------------------------------------- peak center

TEST(Layout, PeakCenterFixture) {
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  const std::string text = fixture(kUnknown + "660/peakcenter/52-01A.peak.json");
  auto ok = merge_satellite(FileKind::PeakCenter, text, a, blobs);
  ASSERT_TRUE(ok.has_value()) << ok.error().what;
  ASSERT_EQ(a.peak_centers.size(), 3u);
  ASSERT_EQ(blobs.size(), 3u);
  std::map<std::string, ps::PeakCenterRow> by;
  for (const auto& p : a.peak_centers) by[p.detector] = p;
  const auto& h1 = by.at("H1");
  EXPECT_EQ(h1.center_dac, 3.7644391643893833);
  EXPECT_EQ(h1.center_signal, 1.4684817659118043);
  EXPECT_EQ(h1.high_dac, 3.764469224509624);
  EXPECT_EQ(h1.high_signal, 1.0688531086719175);
  EXPECT_EQ(h1.low_dac, 3.7644091042691428);
  EXPECT_EQ(h1.low_signal, 1.0052969251564123);
  EXPECT_EQ(h1.reference_detector, "L2(CDD)");
  EXPECT_EQ(h1.reference_isotope, "Ar36");
  EXPECT_FALSE(h1.resolution.has_value());
  EXPECT_FALSE(by.at("H2").center_dac.has_value());  // null in the file
  EXPECT_FALSE(by.at("H2").low_signal.has_value());
  EXPECT_EQ(by.at("L2(CDD)").center_dac, 3.7643129118843732);

  // The scan: 67 (DAC, signal) pairs per detector, the same bytes as the
  // legacy base64 string.
  for (std::size_t i = 0; i < a.peak_centers.size(); ++i) {
    const auto& row = a.peak_centers[i];
    const auto& blob = blobs[i];
    EXPECT_EQ(blob.codec, ps::kCodecTv);
    EXPECT_EQ(blob.n_points, 67);
    ASSERT_TRUE(row.points_blob_sha.has_value());
    EXPECT_EQ(*row.points_blob_sha, ps::blob_sha256(blob.codec, blob.bytes));
    const auto points = ps::decode_tv(blob.bytes);
    ASSERT_TRUE(points.has_value());
    const auto legacy_points =
        ps::decode_legacy_ff_base64(json::parse(text)[row.detector]["points"].get<std::string>());
    ASSERT_TRUE(legacy_points.has_value());
    EXPECT_EQ(*points, *legacy_points);
  }
  // Nothing is left over: "fmt" is ">ff", which the blob codec stands for.
  EXPECT_FALSE(a.meta && a.meta->legacy_json);
}

TEST(Layout, PeakCenterNewerKeysAndLeftovers) {
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  // README 5.8: newer files add interpolation, resolution and resolving powers.
  auto ok = merge_satellite(FileKind::PeakCenter,
                            R"({"fmt": ">ff", "reference_detector": "H1", "reference_isotope": "Ar40",
                                "interpolation": "cubic", "zz_top": 1,
                                "H1": {"center_dac": 3.5, "low_dac": NaN, "resolution": 450.5,
                                       "low_resolving_power": 600.0, "high_resolving_power": 610.0,
                                       "points": "", "zz": 2},
                                "data": {"H1": "older form"}})",
                            a, blobs);
  ASSERT_TRUE(ok.has_value()) << ok.error().what;
  ASSERT_EQ(a.peak_centers.size(), 1u);
  const auto& p = a.peak_centers[0];
  EXPECT_EQ(p.detector, "H1");
  EXPECT_EQ(p.interpolation, "cubic");
  EXPECT_EQ(p.resolution, 450.5);
  EXPECT_EQ(p.low_resolving_power, 600.0);
  EXPECT_EQ(p.high_resolving_power, 610.0);
  EXPECT_EQ(p.center_dac, 3.5);
  EXPECT_FALSE(p.low_dac.has_value());
  EXPECT_FALSE(p.points_blob_sha.has_value());  // no scan
  EXPECT_TRUE(blobs.empty());
  const json all = legacy(a);
  EXPECT_EQ(all["peakcenter"]["zz_top"], 1);
  EXPECT_EQ(all["peakcenter"]["H1"]["zz"], 2);
  EXPECT_EQ(all["peakcenter"]["data"]["H1"], "older form");  // the older top-level form is kept, not read
  EXPECT_EQ(all["nonfinite"]["peakcenter"], json::parse(R"({"/H1/low_dac": "NaN"})"));

  // A scan in a format other than >ff, or one that does not decode, is an error.
  ps::AnalysisIngest b;
  EXPECT_FALSE(merge_satellite(FileKind::PeakCenter, R"({"fmt": "<dd", "H1": {"points": "AAAA"}})", b, blobs)
                   .has_value());
  EXPECT_FALSE(merge_satellite(FileKind::PeakCenter, R"({"fmt": ">ff", "H1": {"points": "!!!"}})", b, blobs)
                   .has_value());
}

// ---------------------------------------------------------------- monitor

TEST(Layout, MonitorChecks) {
  // Source only (README 3.2): a list of checks, `data` as base64 >ff pairs.
  const std::vector<ps::TvPoint> points = {{1.0f, 2.0f}, {3.0f, 4.5f}};
  const std::string data = ps::encode_legacy_ff_base64(points);
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  auto ok = merge_satellite(FileKind::Monitor,
                            R"([{"name": "pressure", "parameter": "CDD", "criterion": "10", "comparator": ">",
                                 "tripped": false, "data": ")" +
                                data + R"(", "zz": 1}, {"name": "second", "tripped": true}])",
                            a, blobs);
  ASSERT_TRUE(ok.has_value()) << ok.error().what;
  ASSERT_EQ(a.monitor_checks.size(), 2u);
  EXPECT_EQ(a.monitor_checks[0].ordinal, 0);
  EXPECT_EQ(a.monitor_checks[0].name, "pressure");
  EXPECT_EQ(a.monitor_checks[0].parameter, "CDD");
  EXPECT_EQ(a.monitor_checks[0].criterion, "10");
  EXPECT_EQ(a.monitor_checks[0].comparator, ">");
  EXPECT_EQ(a.monitor_checks[0].tripped, false);
  ASSERT_EQ(blobs.size(), 1u);
  EXPECT_EQ(a.monitor_checks[0].data_blob_sha, ps::blob_sha256(blobs[0].codec, blobs[0].bytes));
  EXPECT_EQ(ps::decode_tv(blobs[0].bytes).value(), points);
  EXPECT_EQ(a.monitor_checks[1].ordinal, 1);
  EXPECT_EQ(a.monitor_checks[1].tripped, true);
  EXPECT_FALSE(a.monitor_checks[1].data_blob_sha.has_value());
  EXPECT_EQ(legacy(a)["monitor"]["0"]["zz"], 1);

  ps::AnalysisIngest b;
  EXPECT_FALSE(merge_satellite(FileKind::Monitor, R"({"name": "not a list"})", b, blobs).has_value());
}

TEST(Layout, MergeSatelliteRejectsOtherKindsAndGarbage) {
  ps::AnalysisIngest a;
  std::vector<ps::BlobIngest> blobs;
  EXPECT_FALSE(merge_satellite(FileKind::Intercepts, "{}", a, blobs).has_value());
  EXPECT_FALSE(merge_satellite(FileKind::Record, "{}", a, blobs).has_value());
  EXPECT_FALSE(merge_satellite(FileKind::Extraction, "not json", a, blobs).has_value());
  EXPECT_FALSE(merge_satellite(FileKind::Extraction, "[]", a, blobs).has_value());
  EXPECT_FALSE(merge_satellite(FileKind::PeakCenter, "[]", a, blobs).has_value());
}

// ---------------------------------------------------------------- spectrometer

TEST(Layout, SpectrometerFixture) {
  const std::string sha = "6a9b4615cd24138b6ce541f75240dc4091378bb1";
  auto s = parse_spectrometer(fixture(kUnknown + sha + ".json"), sha);
  ASSERT_TRUE(s.has_value()) << s.error().what;
  EXPECT_EQ(s->legacy_sha1, sha);
  const json spectrometer = json::parse(s->spectrometer_json);
  EXPECT_EQ(spectrometer["ElectronEnergy"], 124.885552096686);
  EXPECT_EQ(spectrometer["IonRepeller"], -5.75);
  EXPECT_EQ(spectrometer.size(), 8u);
  const json gains = json::parse(s->gains_json);
  EXPECT_EQ(gains.size(), 10u);
  EXPECT_EQ(gains["L2(CDD)"], 0);
  const json deflections = json::parse(s->deflections_json);
  EXPECT_EQ(deflections.size(), 10u);
  EXPECT_EQ(deflections["AX(CDD)"], 0.0);
  EXPECT_EQ(s->settings_json, "{}");  // 2018 files have no settings

  // The blank's file differs in one value, so it is another snapshot.
  const std::string other = "fad234d16fee6c00a96e96e15edccd97faba9521";
  auto b = parse_spectrometer(fixture(kBlank + other + ".json"), other);
  ASSERT_TRUE(b.has_value()) << b.error().what;
  EXPECT_EQ(json::parse(b->spectrometer_json)["ElectronEnergy"], 124.904474150034);
  EXPECT_NE(ps::snapshot_sha256(*s), ps::snapshot_sha256(*b));
}

TEST(Layout, SpectrometerSettingsAndUnknownKeys) {
  auto s = parse_spectrometer(R"({"spectrometer": {"HV": 9.9}, "gains": {}, "deflections": {"H1": NaN},
                                  "settings": {"k": 1}, "zz_new": [1]})",
                              "abc");
  ASSERT_TRUE(s.has_value()) << s.error().what;
  EXPECT_EQ(s->spectrometer_json, R"({"HV":9.9})");
  EXPECT_EQ(s->gains_json, "{}");
  EXPECT_EQ(s->deflections_json, R"({"H1":null})");
  EXPECT_EQ(json::parse(s->settings_json),
            json::parse(R"({"k": 1, "legacy_extra": {"zz_new": [1], "nonfinite": {"/deflections/H1": "NaN"}}})"));
  EXPECT_FALSE(parse_spectrometer("nope", "abc").has_value());
  EXPECT_FALSE(parse_spectrometer("[]", "abc").has_value());
}

}  // namespace
}  // namespace pychron::dvc
