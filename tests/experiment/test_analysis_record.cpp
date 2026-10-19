#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "pychron/experiment/record/builder.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/experiment/record/sha256.hpp"

using namespace pychron::experiment::record;

namespace {

RecordBuilder full_builder() {
  RecordBuilder b;
  b.set_identity({"u-1", "12345", 2, "A", "unknown", "2026-09-30T12:00:00Z", 7, "q-1"});
  b.set_sample({"FC-2", "proj", "sanidine", "NM-300", "A", "5", "Doe", "a \"note\"\nline2"});
  b.set_instrument({"jan", "co2", "lab", "me", "0.1.0", "abc123"});
  Extraction ex;
  ex.spec = {10.0, 30, 60, "watts", "circle", {1, 2}, 77.0};
  ex.actuals.value = 9.9;
  ex.actuals.duration = 30;
  ex.actuals.pid_params = {{"kp", 0.5}};
  ex.actuals.series["response"] = {{0.f, 1.f}, {0.1f, 0.2f}, {}};
  ex.actuals.grain_polygons = {{{0.0, 0.0}, {1.5, 2.5}}};
  ex.actuals.manometer_pressure = 1e-6;
  ex.actuals.cryo_measured = {{"A", 77.12}, {"B", 80.5}};
  ex.actuals.snapshot_refs = {"snap1.jpg"};
  b.set_extraction(ex);
  Measurement m;
  m.plan = {"argon", "3", "[plan]\nx=1\n", {{"count", "10"}}};
  m.hook = HookRef{"h", "deadbeef"};
  m.scripts["extraction"] = {"ext.py", "sha1", "ref1"};
  b.set_measurement(m);
  SpectrometerRec sp;
  sp.state_hash = "hash";
  sp.field_table_version = "ft1";
  sp.integration_time = 1.0;
  sp.gains = {{"H1", 1.0}};
  b.set_spectrometer(sp);
  b.add_series({"Ar40", "H1", "signal", {{0.f, 1.f, 2.f}, {1.f, 0.9f, 0.8f}, {0.01f, 0.01f, 0.01f}}});
  b.add_series({"Ar40", "H1", "baseline", {{0.f, 1.f}, {0.1f, 0.1f}, {}}});
  b.set_time_zero(3.5);
  b.set_count("Ar40", 3);
  InterceptResult ir;
  ir.intercept.value = 1.1;
  ir.intercept.error = 0.01;
  ir.intercept.n_used = 3;
  ir.intercept.filtered_idx = {1};
  ir.fit.kind = pychron::reduction::FitKind::Parabolic;
  b.set_intercept("Ar40", ir);
  BaselineResult br;
  br.value = 0.1;
  b.set_baseline("H1", br);
  b.set_whiff("run_remainder");
  Conditionals cond;
  InstalledConditional ic;
  ic.id = "abc";
  ic.name = "big";
  ic.kind = "truncation";
  ic.level = "queue";
  ic.location = "conditionals/q.toml";
  ic.check = "average(Ar40, window=5) > 800000";
  ic.start = 20;
  ic.frequency = 2;
  ic.ntrips = 3;
  ic.window = 5;
  ic.mapper = "x + 1";
  ic.analysis_types = {"unknown", "blank"};
  ic.abbreviated_count_ratio = 0.5;
  ic.action = "truncate";
  cond.installed.push_back(ic);
  TrippedConditional tc;
  tc.id = "abc";
  tc.name = "big";
  tc.kind = "truncation";
  tc.check = ic.check;
  tc.action = "truncate";
  tc.reading = 26;
  tc.count = 3;
  tc.t = 41.5;
  tc.value = 812345.5;
  tc.context = {{"Ar40", 812345.5}, {"Ar39", 1.25}};
  cond.tripped.push_back(tc);
  cond.errors.push_back({"other", "metric 'gauge.x.pressure' unavailable", 4});
  b.set_conditionals(cond);
  b.add_event({1.0, "state", "measuring"});
  b.add_persister_ref("db:1");
  return b;
}

}  // namespace

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(RecordBuilder, EmptyFailsEveryPhase) {
  RecordBuilder b;
  for (auto p : kAllPhases) {
    if (p == Phase::Results) continue;  // vacuously complete with no data
    EXPECT_FALSE(b.check(p).has_value()) << to_string(p);
  }
  EXPECT_FALSE(b.finalize().has_value());
}

// A failed run's record (spec 4.5) has what the run got to: only who it is
// is required, and it is finalized so that the store takes it.
TEST(RecordBuilder, AFailedRecordNeedsOnlyItsIdentity) {
  RecordBuilder b;
  b.set_identity({"u-1", "12345", 2, "A", "unknown", "2026-09-30T12:00:00Z", 7, "q-1"});
  b.set_instrument({"jan", "co2", "lab", "me", "0.1.0", "abc123"});
  EXPECT_FALSE(b.finalize().has_value());
  b.set_status(RecordStatus::Failed, "laser link down");
  auto r = b.finalize();
  ASSERT_TRUE(r.has_value()) << r.error().what;
  EXPECT_TRUE(verify_sha(*r));
  RecordBuilder nobody;
  nobody.set_status(RecordStatus::Failed, "laser link down");
  EXPECT_FALSE(nobody.finalize().has_value());
}

TEST(RecordBuilder, PhaseCheckIsIndependent) {
  auto b = full_builder();
  for (auto p : kAllPhases) EXPECT_TRUE(b.check(p).has_value()) << to_string(p);
  RecordBuilder partial;
  partial.set_identity({"u", "1", 0, "", "air", "t", 0, ""}).set_instrument({"jan", "", "", "", "", ""});
  EXPECT_TRUE(partial.check(Phase::Identity).has_value());
  EXPECT_FALSE(partial.check(Phase::Data).has_value());
}

TEST(RecordBuilder, ResultsRequireInterceptPerSignalIsotope) {
  RecordBuilder b;
  b.add_series({"Ar39", "CDD", "signal", {{0.f}, {1.f}, {}}});
  auto r = b.check(Phase::Results);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("results.intercepts.Ar39"), std::string::npos);
}

// An isotope measured on several detectors (a detector intercalibration) has
// an intercept a detector, under the name the acquisition fits it by
// (measurement::fit_results): the record of such a run is complete with those.
TEST(RecordBuilder, AnIsotopeOnSeveralDetectorsNeedsAnInterceptADetector) {
  RecordBuilder b;
  b.add_series({"Ar40", "H1", "signal", {{0.f}, {1.f}, {}}});
  b.add_series({"Ar40", "AX", "signal", {{0.f}, {1.f}, {}}});
  b.add_series({"Ar36", "CDD", "signal", {{0.f}, {1.f}, {}}});
  b.set_intercept("Ar36", {});
  b.set_intercept("Ar40:H1", {});
  auto r = b.check(Phase::Results);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("results.intercepts.Ar40:AX missing"), std::string::npos) << r.error().what;
  EXPECT_EQ(r.error().what.find("results.intercepts.Ar40 missing"), std::string::npos) << r.error().what;
  b.set_intercept("Ar40:AX", {});
  EXPECT_TRUE(b.check(Phase::Results).has_value());
}

TEST(RecordBuilder, DetectsTraceLengthMismatch) {
  RecordBuilder b;
  b.add_series({"Ar40", "H1", "signal", {{0.f, 1.f}, {1.f}, {}}});
  EXPECT_FALSE(b.check(Phase::Data).has_value());
}

TEST(RecordBuilder, FinalizeStampsVersionAndSha) {
  auto rec = full_builder().finalize();
  ASSERT_TRUE(rec.has_value()) << rec.error().what;
  EXPECT_EQ(rec->provenance.schema_version, kRecordSchemaV2);  // nothing in it needs v3
  EXPECT_EQ(rec->provenance.sha.size(), 64u);
  EXPECT_TRUE(verify_sha(*rec));
  auto tampered = *rec;
  tampered.identity.aliquot = 99;
  EXPECT_FALSE(verify_sha(tampered));
}

TEST(RecordSerialize, DefaultIcfactorIsOne) {
  Results r;
  EXPECT_EQ(icfactor(r, "H1"), 1.0);
  r.icfactors["H1"] = 1.02;
  EXPECT_EQ(icfactor(r, "H1"), 1.02);
}

TEST(RecordSerialize, TomlRoundTrip) {
  auto rec = full_builder().finalize().value();
  const auto text = to_toml(rec);
  auto back = from_toml(text);
  ASSERT_TRUE(back.has_value()) << back.error().what;
  EXPECT_EQ(*back, rec);
  EXPECT_EQ(to_toml(*back), text);
}

TEST(RecordSerialize, JsonRoundTrip) {
  auto rec = full_builder().finalize().value();
  const auto text = to_json(rec);
  auto back = from_json(text);
  ASSERT_TRUE(back.has_value()) << back.error().what;
  EXPECT_EQ(*back, rec);
  EXPECT_EQ(to_json(*back), text);
  EXPECT_TRUE(verify_sha(*back));
}

TEST(RecordSerialize, Deterministic) {
  EXPECT_EQ(to_toml(full_builder().finalize().value()), to_toml(full_builder().finalize().value()));
  EXPECT_EQ(full_builder().finalize()->provenance.sha, full_builder().finalize()->provenance.sha);
}

TEST(RecordSerialize, ShaChangesWithContent) {
  auto b = full_builder();
  auto a = b.finalize().value();
  b.add_event({2.0, "alarm", "x"});
  EXPECT_NE(a.provenance.sha, b.finalize()->provenance.sha);
}

TEST(RecordSerialize, SchemaVersionBumpIsRejected) {
  auto rec = full_builder().finalize().value();
  EXPECT_EQ(rec.provenance.schema_version, 2);  // a record v2 can say all of is v2
  rec.provenance.schema_version = kRecordSchemaVersion + 1;
  auto r = from_toml(to_toml(rec));
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("schema_version"), std::string::npos);
  auto j = from_json(to_json(rec));
  EXPECT_FALSE(j.has_value());
}

TEST(RecordSerialize, RejectsMalformed) {
  EXPECT_FALSE(from_toml("this is = = bad").has_value());
  EXPECT_FALSE(from_json("{\"identity\": ").has_value());
  EXPECT_FALSE(from_json("[]").has_value());
  EXPECT_FALSE(from_toml("[identity]\naliquot = \"x\"\n").has_value());
}

TEST(RecordSerialize, CryoTemperaturesAreKeptAndOmittedWhenAbsent) {
  // Owner decision 2026-10-05: the measured cryo temperatures are recorded
  // beside the requested one.
  auto rec = full_builder().finalize().value();
  const auto text = to_toml(rec);
  EXPECT_NE(text.find("cryo_temperature = 77.0"), std::string::npos) << text;
  EXPECT_NE(text.find("cryo_measured"), std::string::npos);
  auto back = from_toml(text);
  ASSERT_TRUE(back.has_value()) << back.error().what;
  EXPECT_EQ(back->extraction.spec.cryo_temperature, 77.0);
  EXPECT_EQ(back->extraction.actuals.cryo_measured, (std::map<std::string, double>{{"A", 77.12}, {"B", 80.5}}));

  // A run without a cryostat writes neither, and a record written before
  // them still reads.
  auto b = full_builder();
  Extraction ex;
  ex.spec = {10.0, 30, 60, "watts", "circle", {1, 2}, std::nullopt};
  b.set_extraction(ex);
  const auto plain = to_toml(b.finalize().value());
  EXPECT_EQ(plain.find("cryo"), std::string::npos) << plain;
  auto old = from_toml(plain);
  ASSERT_TRUE(old.has_value()) << old.error().what;
  EXPECT_FALSE(old->extraction.spec.cryo_temperature);
  EXPECT_TRUE(old->extraction.actuals.cryo_measured.empty());
}

namespace {

std::string fixture(const char* name) {
  std::ifstream in(std::string(PYCHRON_TEST_DATA_DIR) + "/records/" + name, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// tests/data/records/v2.{json,toml} were written by the serializer as it was
// before a record could say that it is simulated: what a lab's data/records
// and spools hold. They still parse, as not simulated, their sha is still
// theirs, and they are written back byte for byte.
TEST(RecordSerialize, ARecordWrittenBeforeTheSimulatedMarkStillReadsWithItsSha) {
  const char* const sha = "c2402e2b974f61619f4123ddd4eecbcf94419fd4cb0a5a7a86233aab1f4a0102";
  const std::string json = fixture("v2.json");
  ASSERT_FALSE(json.empty());
  EXPECT_EQ(json.find("simulated"), std::string::npos);
  auto from_j = from_json(json);
  ASSERT_TRUE(from_j.has_value()) << from_j.error().what;
  EXPECT_EQ(from_j->provenance.schema_version, 2);
  EXPECT_EQ(from_j->provenance.sha, sha);
  EXPECT_TRUE(verify_sha(*from_j));
  EXPECT_EQ(compute_sha(*from_j), sha);
  EXPECT_FALSE(from_j->instrument.simulated);
  EXPECT_EQ(to_json(*from_j), json);

  const std::string toml = fixture("v2.toml");
  ASSERT_FALSE(toml.empty());
  auto from_t = from_toml(toml);
  ASSERT_TRUE(from_t.has_value()) << from_t.error().what;
  EXPECT_EQ(from_t->provenance.sha, sha);
  EXPECT_TRUE(verify_sha(*from_t));
  EXPECT_FALSE(from_t->instrument.simulated);
  EXPECT_EQ(to_toml(*from_t), toml);
  EXPECT_EQ(*from_t, *from_j);
  // And what the code builds today for the same run is that record.
  EXPECT_EQ(full_builder().finalize()->provenance.sha, sha);
  EXPECT_EQ(*full_builder().finalize(), *from_t);
}

// The mark is part of the record: it is in both renderings and in the sha,
// and it comes back. A record that is not simulated has no such key.
TEST(RecordSerialize, ASimulatedRecordSaysSoAndKeepsSayingSo) {
  auto b = full_builder();
  const auto real = b.finalize().value();
  auto meta = real.instrument;
  meta.simulated = true;
  b.set_instrument(meta);
  const auto simulated = b.finalize().value();
  EXPECT_EQ(simulated.provenance.schema_version, kRecordSchemaV2);
  EXPECT_NE(simulated.provenance.sha, real.provenance.sha);
  EXPECT_TRUE(verify_sha(simulated));

  const auto json = to_json(simulated);
  EXPECT_NE(json.find("\"simulated\" : true"), std::string::npos) << json.substr(0, 400);
  EXPECT_EQ(to_json(real).find("simulated"), std::string::npos);
  auto back = from_json(json);
  ASSERT_TRUE(back.has_value()) << back.error().what;
  EXPECT_TRUE(back->instrument.simulated);
  EXPECT_EQ(*back, simulated);
  EXPECT_EQ(to_json(*back), json);
  auto back_toml = from_toml(to_toml(simulated));
  ASSERT_TRUE(back_toml.has_value()) << back_toml.error().what;
  EXPECT_TRUE(back_toml->instrument.simulated);
  EXPECT_TRUE(verify_sha(*back_toml));

  // "simulated = false" written out by hand is the same record as none.
  auto text = to_toml(real);
  const auto at = text.find("software_version");
  ASSERT_NE(at, std::string::npos);
  text.insert(at, "simulated = false\n");
  auto explicit_false = from_toml(text);
  ASSERT_TRUE(explicit_false.has_value()) << explicit_false.error().what;
  EXPECT_FALSE(explicit_false->instrument.simulated);
  EXPECT_TRUE(verify_sha(*explicit_false));
  // Not a boolean: refused, not read as real.
  text.replace(text.find("simulated = false"), std::string("simulated = false").size(), "simulated = \"yes\"");
  EXPECT_FALSE(from_toml(text).has_value());
}

// ---- how the run ended (executor hardening, spec 4.5) ------------------------

// A complete record has neither key, so it keeps the bytes and the sha it had
// before records could say how their run ended; the others say so.
TEST(AnalysisRecord, StatusIsWrittenOnlyWhenNotComplete) {
  auto b = full_builder();
  const auto complete = b.finalize().value();
  EXPECT_EQ(complete.provenance.status, RecordStatus::Complete);
  for (const auto& text : {to_toml(complete), to_json(complete)}) {
    EXPECT_EQ(text.find("status"), std::string::npos) << text;
    EXPECT_EQ(text.find("failure"), std::string::npos);
  }

  b.set_status(RecordStatus::Truncated);
  const auto truncated = b.finalize().value();
  EXPECT_NE(to_toml(truncated).find("status = 'truncated'"), std::string::npos) << to_toml(truncated);
  EXPECT_NE(to_json(truncated).find("\"status\" : \"truncated\""), std::string::npos);
  EXPECT_EQ(to_toml(truncated).find("failure"), std::string::npos);  // empty: not written
  EXPECT_NE(truncated.provenance.sha, complete.provenance.sha);

  b.set_status(RecordStatus::Failed, "spectrometer link down");
  const auto failed = b.finalize().value();
  const auto toml = to_toml(failed);
  EXPECT_NE(toml.find("status = 'failed'"), std::string::npos) << toml;
  EXPECT_NE(toml.find("failure = 'spectrometer link down'"), std::string::npos) << toml;
  EXPECT_NE(to_json(failed).find("\"failure\" : \"spectrometer link down\""), std::string::npos);
}

TEST(AnalysisRecord, StatusRoundTrips) {
  for (const auto status : {RecordStatus::Complete, RecordStatus::Truncated, RecordStatus::Failed}) {
    auto b = full_builder();
    b.set_status(status, status == RecordStatus::Failed ? "lost" : "");
    const auto rec = b.finalize().value();
    auto t = from_toml(to_toml(rec));
    ASSERT_TRUE(t.has_value()) << t.error().what;
    EXPECT_EQ(*t, rec);
    EXPECT_TRUE(verify_sha(*t));
    auto j = from_json(to_json(rec));
    ASSERT_TRUE(j.has_value()) << j.error().what;
    EXPECT_EQ(*j, rec);
    EXPECT_EQ(to_json(*j), to_json(rec));
  }
}

TEST(AnalysisRecord, AnUnknownStatusIsRejected) {
  auto b = full_builder();
  b.set_status(RecordStatus::Truncated);
  auto text = to_toml(b.finalize().value());
  const auto at = text.find("'truncated'");
  ASSERT_NE(at, std::string::npos);
  text.replace(at, std::string("'truncated'").size(), "'half'");
  auto r = from_toml(text);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(r.error().what.find("status"), std::string::npos) << r.error().what;
}

// Records written before the key read as complete, with their own sha.
TEST(AnalysisRecord, ARecordWrittenBeforeStatusStillReadsWithItsSha) {
  for (const char* name : {"v2.json", "v2.toml"}) {
    const std::string text = fixture(name);
    ASSERT_FALSE(text.empty()) << name;
    EXPECT_EQ(text.find("status"), std::string::npos) << name;
    auto rec = std::string(name).ends_with(".json") ? from_json(text) : from_toml(text);
    ASSERT_TRUE(rec.has_value()) << rec.error().what;
    EXPECT_EQ(rec->provenance.status, RecordStatus::Complete);
    EXPECT_TRUE(rec->provenance.failure.empty());
    EXPECT_TRUE(verify_sha(*rec));
    EXPECT_EQ(rec->provenance.schema_version, kRecordSchemaV2);
  }
}

// ---- v3 (measurement plan v2 spec 6.1) ----------------------------------------

// A v2 record keeps its sha and its bytes: it is written back in the v2
// layout, whatever v3 adds when it is read.
TEST(AnalysisRecord, V2FixturesKeepTheirSha) {
  for (const char* name : {"v2.json", "v2.toml"}) {
    const std::string text = fixture(name);
    ASSERT_FALSE(text.empty()) << name;
    const bool json = std::string(name).ends_with(".json");
    auto rec = json ? from_json(text) : from_toml(text);
    ASSERT_TRUE(rec.has_value()) << rec.error().what;
    EXPECT_TRUE(verify_sha(*rec)) << name;
    EXPECT_EQ(json ? to_json(*rec) : to_toml(*rec), text) << name;
  }
}

TEST(AnalysisRecord, V2LoadsAsOneImplicitGas) {
  auto rec = from_toml(fixture("v2.toml"));
  ASSERT_TRUE(rec.has_value()) << rec.error().what;
  EXPECT_EQ(rec->provenance.schema_version, kRecordSchemaV2);
  ASSERT_EQ(rec->data.gases.size(), 1u);
  EXPECT_EQ(rec->data.gases[0].name, "");
  EXPECT_EQ(rec->data.gases[0].time_zero, 3.5);
  ASSERT_EQ(rec->data.gases[0].states.size(), 1u);
  EXPECT_EQ(rec->data.gases[0].states[0].integration_time, 1.0);
  for (const auto& s : rec->data.series) {
    EXPECT_EQ(s.gas, "");
    EXPECT_EQ(s.segment, 0);
    EXPECT_EQ(s.position, 0);
    EXPECT_EQ(s.state, 0);
  }
  ASSERT_EQ(rec->results.intercepts.size(), 1u);
  EXPECT_EQ(rec->results.intercepts.begin()->first, "Ar40@H1");
  ASSERT_EQ(rec->results.baseline_positions.size(), 1u);
  EXPECT_EQ(rec->results.baseline_positions[0].detector, "H1");
  EXPECT_EQ(rec->results.baseline_positions[0].result.value, 0.1);
  EXPECT_TRUE(rec->results.baseline_choices.empty());
}

namespace {

// Two gases, three states, two baseline positions on H1 and one choice.
AnalysisRecord v3_record() {
  RecordBuilder v3 = full_builder();
  v3.add_series({"He4", "H1", "signal", {{0.f, 1.f}, {5.f, 4.f}, {}}, "He", 0, 0, 0});
  v3.add_series({"Ne20", "H1", "signal", {{10.f, 11.f}, {2.f, 2.5f}, {}}, "Ne", 1, 0, 1});
  v3.add_series({"Ne22", "AX", "signal", {{10.f, 11.f}, {0.5f, 0.6f}, {}}, "Ne", 1, 1, 0});
  GasRec he{"He", 2.5, {{{{"CDD", 110.0}}, 1.0, "", ""}}, std::nullopt, PumpOutRec{"ion_pump", 120}};
  GasRec ne{"Ne", 9.75,
            {{{}, 1.0, "ne_optics", "ic_run"}, {{{"CDD", 2000.0}}, 2.0, "", ""}},
            PrepareRec{"cryo:release", {{"cryo_temp", "75"}, {"hold_s", "300"}}, "2026-09-30T12:01:00Z",
                       "2026-09-30T12:06:00Z"},
            std::nullopt};
  v3.add_gas(he).add_gas(ne);
  InterceptResult ir;
  ir.intercept.value = 4.5;
  v3.set_intercept("He4@H1", ir);
  v3.set_intercept("Ne20@H1", ir);
  v3.set_intercept("Ne22@AX", ir);
  BaselineRec before{"H1", "Ne", 0, 0, "before_inlet", 34.2, 110.0, {0.05, 0.001, {}}};
  BaselineRec after{"H1", "Ne", 2, 0, "after_main", std::nullopt, std::nullopt, {0.07, 0.002, {}}};
  v3.add_baseline_position(before).add_baseline_position(after);
  v3.set_baseline_choice("Ne20@H1", {"bracketing_average", {0, 1}, 0.06, 0.0011180339887498948});
  v3.set_baseline_modifier("CDD", {"Ar40+Ar39", "0.01*x", "0.001", 1.5, 0.002});
  return v3.finalize().value();
}

}  // namespace

TEST(AnalysisRecord, V3RoundTrips) {
  const auto rec = v3_record();
  EXPECT_EQ(rec.provenance.schema_version, 3);
  EXPECT_TRUE(verify_sha(rec));
  const auto toml = to_toml(rec);
  auto t = from_toml(toml);
  ASSERT_TRUE(t.has_value()) << t.error().what;
  EXPECT_EQ(*t, rec);
  EXPECT_TRUE(verify_sha(*t));
  EXPECT_EQ(to_toml(*t), toml);
  const auto json = to_json(rec);
  auto j = from_json(json);
  ASSERT_TRUE(j.has_value()) << j.error().what;
  EXPECT_EQ(*j, rec);
  EXPECT_TRUE(verify_sha(*j));
  EXPECT_EQ(to_json(*j), json);
  // What is absent is not written.
  EXPECT_EQ(t->data.gases[0].prepare, std::nullopt);
  EXPECT_EQ(t->data.gases[1].pump_out, std::nullopt);
  EXPECT_EQ(t->results.baseline_positions[1].magnet, std::nullopt);
}

// tests/data/records/v3.{toml,json} were written by this serializer on macOS:
// a record written on one compiler reads with its sha on every other (Windows
// CI checks these). PYCHRON_UPDATE_GOLDENS=1 rewrites them.
TEST(AnalysisRecord, V3FixtureSha) {
  const auto rec = v3_record();
  if (const char* update = std::getenv("PYCHRON_UPDATE_GOLDENS"); update && std::string(update) == "1") {
    for (const char* name : {"v3.toml", "v3.json"}) {
      std::ofstream out(std::string(PYCHRON_TEST_DATA_DIR) + "/records/" + name, std::ios::binary);
      out << (std::string(name).ends_with(".json") ? to_json(rec) : to_toml(rec));
    }
    FAIL() << "golden rewritten: review the diff";
  }
  for (const char* name : {"v3.toml", "v3.json"}) {
    const std::string text = fixture(name);
    ASSERT_FALSE(text.empty()) << name;
    const bool json = std::string(name).ends_with(".json");
    auto back = json ? from_json(text) : from_toml(text);
    ASSERT_TRUE(back.has_value()) << back.error().what;
    EXPECT_TRUE(verify_sha(*back)) << name;
    EXPECT_EQ(back->provenance.sha, rec.provenance.sha) << name;
    EXPECT_EQ(*back, rec) << name;
    EXPECT_EQ(json ? to_json(*back) : to_toml(*back), text) << name;
  }
}
