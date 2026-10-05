#include <gtest/gtest.h>

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

TEST(RecordBuilder, DetectsTraceLengthMismatch) {
  RecordBuilder b;
  b.add_series({"Ar40", "H1", "signal", {{0.f, 1.f}, {1.f}, {}}});
  EXPECT_FALSE(b.check(Phase::Data).has_value());
}

TEST(RecordBuilder, FinalizeStampsVersionAndSha) {
  auto rec = full_builder().finalize();
  ASSERT_TRUE(rec.has_value()) << rec.error().what;
  EXPECT_EQ(rec->provenance.schema_version, kRecordSchemaVersion);
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
  EXPECT_EQ(rec.provenance.schema_version, 2);  // bump kRecordSchemaVersion deliberately, with a migration
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
