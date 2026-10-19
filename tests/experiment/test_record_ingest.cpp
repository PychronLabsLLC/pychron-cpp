#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/experiment/store/record_ingest.hpp"
#include "pychron/persistence/blob.hpp"
#include "store_records.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::testing;
namespace ps = pychron::persistence;
using nlohmann::json;

namespace {

ps::Uuid client() { return ps::Uuid::v5(ps::Uuid{}, "test client"); }

const ps::AnalysisIngest& body_of(const store::RunIngest& r) { return std::get<ps::AnalysisIngest>(r.analysis.body); }
const ps::BlobIngest& blob_of(const ps::IngestItem& i) { return std::get<ps::BlobIngest>(i.body); }

store::RunIngest convert(const record::AnalysisRecord& r) {
  auto out = store::to_ingest(r, client());
  EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().what);
  return out.value();
}

std::string error_of(const record::AnalysisRecord& r) {
  auto out = store::to_ingest(r, client());
  EXPECT_FALSE(out.has_value());
  return out ? "" : out.error().what;
}

record::AnalysisRecord finalized(const record::RecordBuilder& b) {
  auto r = b.finalize();
  EXPECT_TRUE(r.has_value());
  return r.value();
}

// Everything of a conversion that the store keeps, as text: one line a row.
std::string describe(const store::RunIngest& r) {
  const auto opt = [](const auto& v) -> std::string {
    if (!v) return "-";
    std::ostringstream s;
    s << std::setprecision(17) << *v;
    return s.str();
  };
  std::ostringstream o;
  o << std::setprecision(17);
  o << "item " << r.analysis.item.str() << " " << to_hex(r.analysis.payload_sha256) << "\n";
  for (const auto& b : r.blobs)
    o << "blob " << b.item.str() << " " << blob_of(b).codec << " " << to_hex(b.payload_sha256) << " n="
      << opt(blob_of(b).n_points) << "\n";
  const auto& a = body_of(r);
  o << "analysis " << a.analysis.str() << " changeset " << a.changeset.str() << "\n";
  o << "identity " << a.identifier << " " << a.aliquot << " " << a.increment << " " << a.analysis_type << " ms="
    << a.mass_spectrometer << " device=" << opt(a.extract_device) << " analyst=" << a.analyst << " lab="
    << opt(a.laboratory) << " run_index=" << opt(a.run_index) << " schema=" << a.record_schema_version
    << " time_zero=" << (a.time_zero ? "set" : "-") << "\n";
  const auto& x = a.extraction;
  o << "extraction " << opt(x.extract_value) << " " << opt(x.extract_units) << " " << opt(x.extract_duration) << " "
    << opt(x.cleanup_duration) << " " << opt(x.pattern) << " " << opt(x.cryo_temperature) << " "
    << opt(x.beam_diameter) << "\n";
  for (const auto& p : a.measured_positions) o << "position " << opt(p.position) << "\n";
  for (const auto& i : a.isotopes) o << "isotope " << i.isotope << " " << i.detector << "\n";
  for (const auto& d : a.detectors)
    o << "detector " << d.detector << " " << opt(d.deflection) << " " << opt(d.gain_used) << "\n";
  o << "roots " << a.roots.signals.str() << " " << a.roots.intercepts.str() << " " << a.roots.baselines.str() << " "
    << a.roots.blanks.str() << " " << a.roots.icfactors.str() << " " << a.roots.tags.str() << "\n";
  for (const auto& s : a.roots.signal_refs)
    o << "ref " << s.series_kind << " " << s.series_key << " " << s.detector << " " << to_hex(s.blob_sha) << " n="
      << opt(s.n_points) << "\n";
  for (const auto& i : a.roots.intercepts_rows)
    o << "intercept " << i.isotope << " " << i.detector << " " << opt(i.value) << " " << opt(i.error) << " n="
      << opt(i.n) << " " << opt(i.fit) << " " << opt(i.error_type) << " " << opt(i.filter_outliers_json) << "\n";
  for (const auto& b : a.roots.baselines_rows)
    o << "baseline " << b.detector << " " << opt(b.value) << " " << opt(b.error) << " " << opt(b.fit) << " "
      << opt(b.error_type) << " " << opt(b.filter_outliers_json) << "\n";
  for (const auto& i : a.roots.icfactors_rows) o << "icfactor " << i.detector << " " << opt(i.value) << " " << opt(i.error) << "\n";
  if (a.meta)
    o << "meta " << opt(a.meta->conditionals_json) << " " << opt(a.meta->tripped_conditional_json) << " "
      << opt(a.meta->whiff_result_json) << " " << opt(a.meta->grain_polygons_json) << " "
      << opt(a.meta->pipette_counts_json) << " " << opt(a.meta->software_json) << " " << opt(a.meta->queue_names_json)
      << "\n";
  if (a.spectrometer_snapshot)
    o << "snapshot " << a.spectrometer_snapshot->gains_json << " " << a.spectrometer_snapshot->deflections_json << " "
      << a.spectrometer_snapshot->settings_json << " " << a.spectrometer_snapshot->spectrometer_json << "\n";
  o << "plan " << opt(a.scripts.measurement) << "\n";
  return o.str();
}

}  // namespace

// An ordinary run (each isotope on one detector, one baseline series a
// detector) converts to exactly what it did before the store learned to keep
// the other shapes: this text was taken from the conversion as it was then.
// A change here is a change to what every stored run looks like, and to its
// ids; it is a file format.
TEST(RecordIngest, AnOrdinaryRunConvertsAsItAlwaysDid) {
  const std::string was = R"pin(item 7724152d-7830-57a0-8b39-dc22695b5e26 fff84d742c321c0324526321f241d02f2ceac57a957d20c6926621a3a72c57a0
blob 96435447-fd66-5c39-bdda-acf533f0b1ff f32le-tv/1 f27b17b7ce8edb655e40206ee3c527155b406ea5986cb1af4fa3d4e39a65187b n=3
blob d56e3412-facb-5d0d-8a62-66e4981f519d f32le-tvs/1 583500082c68138223fc9fc9cd58ebda519cd452e8a734f493a88a9dc0cfd5e5 n=3
blob 4f1c4852-af6f-5513-9553-bf5088fec67b f32le-tv/1 ac2d19e5fea167311042c147a022b1c3f5543d41241ea1602ce552d32a91de79 n=2
analysis 0190a5e0-7c3b-4d5e-8f10-2a4b6c8d0e12 changeset 2faec66f-e44d-506c-87b4-7942b00e4d3f
identity 66573 1 -1 unknown ms=jan device=co2 analyst=jdoe lab=lab run_index=3 schema=2 time_zero=-
extraction 10 watts 30 60 circle 77 -
position 4
position 7
isotope Ar36 CDD
isotope Ar40 H1
detector CDD 150 2
detector H1 - 1
roots f9201f8b-31be-515e-84ad-b94d5a848fd3 9041f264-e6a3-5cc3-ba95-308981ebae33 1284f765-45ab-59cf-91e9-46b95750d6e4 2b60b298-d9a7-572d-b33e-5b3b756c6b08 f87508df-be6f-506b-88a7-b818f496890b 639e7a80-40c3-5c67-a9c2-fa1abeb957d1
ref signal Ar40 H1 f27b17b7ce8edb655e40206ee3c527155b406ea5986cb1af4fa3d4e39a65187b n=3
ref signal Ar36 CDD 583500082c68138223fc9fc9cd58ebda519cd452e8a734f493a88a9dc0cfd5e5 n=3
ref baseline H1 H1 ac2d19e5fea167311042c147a022b1c3f5543d41241ea1602ce552d32a91de79 n=2
intercept Ar36 CDD 0.089999999999999997 0.01 n=3 linear SEM {"filter_outliers":0,"iterations":1,"std_devs":2.0}
intercept Ar40 H1 1.1000000000000001 0.01 n=3 linear SEM {"filter_outliers":0,"iterations":1,"std_devs":2.0}
baseline H1 0.044999999999999998 0.0050000000000000001 linear SEM {"filter_outliers":0,"iterations":1,"std_devs":2.0}
icfactor CDD 1.02 0
meta - - - - - {"software_git_sha":"abc123","software_version":"0.1.0"} ["q-1"]
snapshot {"CDD":2.0,"H1":1.0} {"CDD":150.0} {"integration_time":1.0,"ys":1.5} {}
plan [plan]
x = 1

)pin";
  EXPECT_EQ(describe(convert(sample_record())), was);
}

TEST(RecordIngest, TheSameRecordConvertsToTheSameItems) {
  const auto rec = sample_record();
  const auto a = convert(rec);
  const auto b = convert(rec);
  const auto& x = body_of(a);
  const auto& y = body_of(b);
  EXPECT_EQ(a.analysis.item, b.analysis.item);
  EXPECT_EQ(a.analysis.payload_sha256, b.analysis.payload_sha256);
  EXPECT_EQ(x.analysis, y.analysis);
  EXPECT_EQ(x.changeset, y.changeset);
  EXPECT_EQ(x.roots.signals, y.roots.signals);
  EXPECT_EQ(x.roots.signal_refs, y.roots.signal_refs);
  EXPECT_EQ(x.roots.intercepts_rows, y.roots.intercepts_rows);
  EXPECT_EQ(x.roots.baselines_rows, y.roots.baselines_rows);
  EXPECT_EQ(x.spectrometer_snapshot->settings_json, y.spectrometer_snapshot->settings_json);
  ASSERT_EQ(a.blobs.size(), b.blobs.size());
  for (std::size_t i = 0; i < a.blobs.size(); ++i) {
    EXPECT_EQ(a.blobs[i].item, b.blobs[i].item);
    EXPECT_EQ(blob_of(a.blobs[i]).bytes, blob_of(b.blobs[i]).bytes);
    EXPECT_EQ(blob_of(a.blobs[i]).codec, blob_of(b.blobs[i]).codec);
  }
}

TEST(RecordIngest, TwoRecordsShareNoId) {
  auto r1 = sample_record();
  auto r2 = r1;
  r2.identity.uuid = "0190a5e0-7c3b-4d5e-8f10-2a4b6c8d0e13";
  const auto a = convert(r1);
  const auto b = convert(r2);
  const auto& x = body_of(a);
  const auto& y = body_of(b);
  EXPECT_NE(x.analysis, y.analysis);
  EXPECT_NE(x.changeset, y.changeset);
  EXPECT_NE(x.roots.signals, y.roots.signals);
  EXPECT_NE(x.roots.intercepts, y.roots.intercepts);
  EXPECT_NE(x.roots.baselines, y.roots.baselines);
  EXPECT_NE(x.roots.blanks, y.roots.blanks);
  EXPECT_NE(x.roots.icfactors, y.roots.icfactors);
  EXPECT_NE(x.roots.tags, y.roots.tags);
  EXPECT_NE(a.analysis.item, b.analysis.item);
  ASSERT_EQ(a.blobs.size(), b.blobs.size());
  for (std::size_t i = 0; i < a.blobs.size(); ++i) EXPECT_NE(a.blobs[i].item, b.blobs[i].item);
}

TEST(RecordIngest, IdsAreTheDocumentedRecipe) {
  const auto rec = sample_record();
  const auto out = convert(rec);
  const auto& a = body_of(out);
  const auto id = *ps::Uuid::parse(rec.identity.uuid);
  EXPECT_EQ(a.analysis, id);
  EXPECT_EQ(a.changeset, ps::Uuid::v5(id, "changeset"));
  EXPECT_EQ(a.roots.signals, ps::Uuid::v5(id, "signals"));
  EXPECT_EQ(a.roots.intercepts, ps::Uuid::v5(id, "intercepts"));
  EXPECT_EQ(a.roots.baselines, ps::Uuid::v5(id, "baselines"));
  EXPECT_EQ(a.roots.blanks, ps::Uuid::v5(id, "blanks"));
  EXPECT_EQ(a.roots.icfactors, ps::Uuid::v5(id, "icfactors"));
  EXPECT_EQ(a.roots.tags, ps::Uuid::v5(id, "tags"));
  EXPECT_EQ(out.analysis.item, ps::Uuid::v5(id, "item"));
  EXPECT_EQ(out.analysis.client, client());
  for (const auto& b : out.blobs) {
    EXPECT_EQ(b.item, ps::Uuid::v5(id, "blob:" + to_hex(b.payload_sha256)));
    EXPECT_EQ(b.payload_sha256, ps::blob_sha256(blob_of(b).codec, blob_of(b).bytes));
    EXPECT_EQ(b.client, client());
  }
  EXPECT_EQ(to_hex(out.analysis.payload_sha256), rec.provenance.sha);
  EXPECT_EQ(to_hex(*a.record_sha256), rec.provenance.sha);
}

TEST(RecordIngest, StepsBecomeIncrements) {
  EXPECT_EQ(store::increment_of(""), -1);
  EXPECT_EQ(store::increment_of("A"), 0);
  EXPECT_EQ(store::increment_of("Z"), 25);
  EXPECT_EQ(store::increment_of("AA"), 26);
  EXPECT_EQ(store::increment_of("a"), 0);
  EXPECT_EQ(store::increment_of("aa"), 26);
  EXPECT_EQ(store::increment_of("aB"), 27);
  EXPECT_FALSE(store::increment_of("A1"));
  EXPECT_FALSE(store::increment_of("-"));

  auto b = sample_builder();
  auto id = b.draft().identity;
  id.step = "B";
  b.set_identity(id);
  EXPECT_EQ(body_of(convert(finalized(b))).increment, 1);
  id.step = "A1";
  b.set_identity(id);
  EXPECT_NE(error_of(finalized(b)).find("A1"), std::string::npos);
}

TEST(RecordIngest, SeriesTakeTheirCodec) {
  const auto rec = sample_record();
  const auto out = convert(rec);
  ASSERT_EQ(out.blobs.size(), 3u);
  // series order: Ar40/H1 signal, Ar36/CDD signal (sigma), Ar40/H1 baseline
  EXPECT_EQ(blob_of(out.blobs[0]).codec, ps::kCodecTv);
  EXPECT_EQ(blob_of(out.blobs[1]).codec, ps::kCodecTvs);
  EXPECT_EQ(blob_of(out.blobs[2]).codec, ps::kCodecTv);
  EXPECT_EQ(blob_of(out.blobs[1]).n_points, 3);

  const auto tvs = ps::decode_tvs(blob_of(out.blobs[1]).bytes);
  ASSERT_TRUE(tvs.has_value());
  const auto& want = rec.data.series[1].trace;
  ASSERT_EQ(tvs->size(), want.t.size());
  for (std::size_t i = 0; i < want.t.size(); ++i) {
    EXPECT_EQ((*tvs)[i].t, want.t[i]);
    EXPECT_EQ((*tvs)[i].v, want.v[i]);
    EXPECT_EQ((*tvs)[i].s, want.sigma[i]);
  }
  const auto tv = ps::decode_tv(blob_of(out.blobs[0]).bytes);
  ASSERT_TRUE(tv.has_value());
  const auto& want0 = rec.data.series[0].trace;
  ASSERT_EQ(tv->size(), want0.t.size());
  for (std::size_t i = 0; i < want0.t.size(); ++i) {
    EXPECT_EQ((*tv)[i].t, want0.t[i]);
    EXPECT_EQ((*tv)[i].v, want0.v[i]);
  }
}

TEST(RecordIngest, IdenticalSeriesAreOneBlob) {
  auto b = sample_builder();
  // Replace the series by two with the same trace and no sigma.
  record::RecordBuilder c;
  const auto& d = b.draft();
  c.set_identity(d.identity).set_sample(d.sample).set_instrument(d.instrument).set_extraction(d.extraction);
  c.set_measurement(d.measurement).set_spectrometer(d.spectrometer);
  const record::Trace tr{{0.f, 1.f}, {1.f, 2.f}, {}};
  c.add_series({"Ar40", "H1", "signal", tr});
  c.add_series({"Ar39", "H2", "signal", tr});
  record::InterceptResult ir;
  c.set_intercept("Ar40", ir);
  c.set_intercept("Ar39", ir);
  const auto out = convert(finalized(c));
  EXPECT_EQ(out.blobs.size(), 1u);
  const auto& refs = body_of(out).roots.signal_refs;
  ASSERT_EQ(refs.size(), 2u);
  EXPECT_EQ(refs[0].blob_sha, refs[1].blob_sha);
  EXPECT_EQ(refs[0].blob_sha, out.blobs[0].payload_sha256);
}

TEST(RecordIngest, ABaselineIsKeyedByDetector) {
  const auto out = convert(sample_record());
  const auto& refs = body_of(out).roots.signal_refs;
  ASSERT_EQ(refs.size(), 3u);
  EXPECT_EQ(refs[0].series_kind, "signal");
  EXPECT_EQ(refs[0].series_key, "Ar40");
  EXPECT_EQ(refs[0].detector, "H1");
  EXPECT_EQ(refs[0].n_points, 3);
  EXPECT_EQ(refs[1].series_key, "Ar36");
  EXPECT_EQ(refs[2].series_kind, "baseline");
  EXPECT_EQ(refs[2].series_key, "H1");
  EXPECT_EQ(refs[2].detector, "H1");
  EXPECT_EQ(refs[2].n_points, 2);
}

TEST(RecordIngest, IdentityInstrumentAndExtractionAreCarried) {
  auto b = sample_builder("66573", 4, "obama");
  auto id = b.draft().identity;
  id.step = "C";
  b.set_identity(id);
  auto ex = b.draft().extraction;
  ex.actuals.beam_diameter = 1.5;
  b.set_extraction(ex);
  const auto rec = finalized(b);
  const auto out = convert(rec);
  const auto& a = body_of(out);

  EXPECT_EQ(a.identifier, "66573");
  EXPECT_EQ(a.aliquot, 4);
  EXPECT_EQ(a.increment, 2);
  EXPECT_EQ(a.analysis_type, "unknown");
  EXPECT_EQ(a.run_index, 3);
  EXPECT_EQ(a.created, *ps::UtcTime::parse("2026-10-10T12:30:45Z"));
  EXPECT_EQ(a.timestamp, *ps::UtcTime::parse("2026-10-10T12:30:45Z"));
  EXPECT_FALSE(a.time_zero);
  EXPECT_EQ(a.mass_spectrometer, "obama");
  EXPECT_EQ(a.laboratory, "lab");
  EXPECT_EQ(a.analyst, "jdoe");
  EXPECT_EQ(a.extract_device, "co2");
  EXPECT_EQ(a.record_schema_version, record::kRecordSchemaV2);  // an ordinary run needs nothing of v3
  EXPECT_FALSE(a.queue);
  EXPECT_FALSE(a.load_name);
  EXPECT_TRUE(a.peak_centers.empty());
  EXPECT_TRUE(a.monitor_checks.empty());
  EXPECT_TRUE(a.artifacts.empty());
  EXPECT_TRUE(a.roots.blanks_rows.empty());
  EXPECT_EQ(a.roots.tag, (ps::TagValue{"ok", std::nullopt, std::nullopt}));

  EXPECT_EQ(a.extraction.extract_value, 10.0);
  EXPECT_EQ(a.extraction.extract_units, "watts");
  EXPECT_EQ(a.extraction.extract_duration, 30.0);
  EXPECT_EQ(a.extraction.cleanup_duration, 60.0);
  EXPECT_EQ(a.extraction.pattern, "circle");
  EXPECT_EQ(a.extraction.cryo_temperature, 77.0);
  EXPECT_EQ(a.extraction.beam_diameter, 1.5);

  ASSERT_EQ(a.measured_positions.size(), 2u);
  EXPECT_EQ(a.measured_positions[0].position, 4);
  EXPECT_EQ(a.measured_positions[1].position, 7);
  EXPECT_FALSE(a.measured_positions[0].load_name);
  EXPECT_FALSE(a.measured_positions[0].x);

  // isotopes sorted by isotope; detectors sorted, with their deflection and gain
  ASSERT_EQ(a.isotopes.size(), 2u);
  EXPECT_EQ(a.isotopes[0].isotope, "Ar36");
  EXPECT_EQ(a.isotopes[0].detector, "CDD");
  EXPECT_EQ(a.isotopes[1].isotope, "Ar40");
  EXPECT_EQ(a.isotopes[1].detector, "H1");
  ASSERT_EQ(a.detectors.size(), 2u);
  EXPECT_EQ(a.detectors[0].detector, "CDD");
  EXPECT_EQ(a.detectors[0].deflection, 150.0);
  EXPECT_EQ(a.detectors[0].gain_used, 2.0);
  EXPECT_EQ(a.detectors[1].detector, "H1");
  EXPECT_FALSE(a.detectors[1].deflection);
  EXPECT_EQ(a.detectors[1].gain_used, 1.0);

  // ic factors, scripts, snapshot
  ASSERT_EQ(a.roots.icfactors_rows.size(), 1u);
  EXPECT_EQ(a.roots.icfactors_rows[0].detector, "CDD");
  EXPECT_EQ(a.roots.icfactors_rows[0].value, 1.02);
  EXPECT_EQ(a.roots.icfactors_rows[0].error, 0.0);
  EXPECT_EQ(a.scripts.measurement, "[plan]\nx = 1\n");
  EXPECT_FALSE(a.scripts.extraction);
  EXPECT_FALSE(a.scripts.post_equilibration);
  EXPECT_FALSE(a.scripts.post_measurement);
  EXPECT_FALSE(a.scripts.hops);
  ASSERT_TRUE(a.spectrometer_snapshot);
  EXPECT_EQ(a.spectrometer_snapshot->spectrometer_json, "{}");
  EXPECT_EQ(json::parse(a.spectrometer_snapshot->gains_json), (json{{"CDD", 2.0}, {"H1", 1.0}}));
  EXPECT_EQ(json::parse(a.spectrometer_snapshot->deflections_json), (json{{"CDD", 150.0}}));
  EXPECT_EQ(json::parse(a.spectrometer_snapshot->settings_json), (json{{"ys", 1.5}, {"integration_time", 1.0}}));
  EXPECT_EQ(a.spectrometer_snapshot->gains_json, R"({"CDD":2.0,"H1":1.0})");  // sorted keys

  // software and nothing else in meta
  ASSERT_TRUE(a.meta);
  EXPECT_EQ(json::parse(*a.meta->software_json), (json{{"software_version", "0.1.0"}, {"software_git_sha", "abc123"}}));
  EXPECT_FALSE(a.meta->conditionals_json);
  EXPECT_FALSE(a.meta->tripped_conditional_json);
  EXPECT_FALSE(a.meta->whiff_result_json);
  EXPECT_FALSE(a.meta->grain_polygons_json);
  EXPECT_FALSE(a.meta->pipette_counts_json);
}

TEST(RecordIngest, EmptyOptionalsAreUnset) {
  auto b = sample_builder();
  auto in = b.draft().instrument;
  in.laboratory.clear();
  in.software_version.clear();
  in.software_git_sha.clear();
  b.set_instrument(in);
  auto id = b.draft().identity;
  id.queue_uuid.clear();
  b.set_identity(id);
  auto ex = b.draft().extraction;
  ex.spec.units = "w";
  ex.spec.pattern.clear();
  ex.spec.cryo_temperature.reset();
  b.set_extraction(ex);
  const auto out = convert(finalized(b));
  const auto& a = body_of(out);
  EXPECT_FALSE(a.laboratory);
  EXPECT_FALSE(a.extraction.pattern);
  EXPECT_FALSE(a.extraction.cryo_temperature);
  EXPECT_FALSE(a.extraction.beam_diameter);  // actuals.beam_diameter is 0
  EXPECT_FALSE(a.meta);                      // nothing for it to hold
}

TEST(RecordIngest, MetaCarriesWhatTheRecordHas) {
  auto b = sample_builder();
  b.set_whiff("pump");
  record::Conditionals c;
  record::InstalledConditional ic;
  ic.id = "i1";
  ic.name = "big";
  ic.check = "average(Ar40) > 1";
  c.installed.push_back(ic);
  record::TrippedConditional tc;
  tc.id = "i1";
  tc.reading = 5;
  tc.context = {{"Ar40", 2.0}};
  c.tripped.push_back(tc);
  b.set_conditionals(c);
  auto ex = b.draft().extraction;
  ex.actuals.grain_polygons = {{{0.0, 1.0}, {2.0, 3.0}}};
  ex.actuals.pipette_counts = 2;
  b.set_extraction(ex);
  const auto out = convert(finalized(b));
  const auto& a = body_of(out);
  ASSERT_TRUE(a.meta);
  EXPECT_EQ(json::parse(*a.meta->conditionals_json).at(0).at("id"), "i1");
  EXPECT_EQ(json::parse(*a.meta->conditionals_json).at(0).at("check"), "average(Ar40) > 1");
  EXPECT_EQ(json::parse(*a.meta->tripped_conditional_json).at(0).at("reading"), 5);
  EXPECT_EQ(json::parse(*a.meta->whiff_result_json), "pump");
  EXPECT_EQ(json::parse(*a.meta->grain_polygons_json), (json{{{0.0, 1.0}, {2.0, 3.0}}}));
  EXPECT_EQ(json::parse(*a.meta->pipette_counts_json), 2);
}

// A simulated run is stored as one: the mark is the record's, so it does not
// matter which session sends it. A run that is not has no such key.
TEST(RecordIngest, ASimulatedRecordIsStoredAsSimulated) {
  auto b = sample_builder();
  auto meta = b.draft().instrument;
  meta.simulated = true;
  b.set_instrument(meta);
  const auto out = convert(finalized(b));
  ASSERT_TRUE(body_of(out).meta && body_of(out).meta->software_json);
  const auto software = json::parse(*body_of(out).meta->software_json);
  EXPECT_EQ(software.at("simulated"), true);
  EXPECT_EQ(software.at("software_version"), "0.1.0");

  const auto real = convert(sample_record());
  EXPECT_FALSE(json::parse(*body_of(real).meta->software_json).contains("simulated"));

  // With no software named at all, the mark is still written.
  meta.software_version.clear();
  meta.software_git_sha.clear();
  b.set_instrument(meta);
  const auto bare = convert(finalized(b));
  ASSERT_TRUE(body_of(bare).meta && body_of(bare).meta->software_json);
  EXPECT_EQ(json::parse(*body_of(bare).meta->software_json).at("simulated"), true);
}

// The mark is the record's, not the sender's: a simulated run left in the
// spool is read back and sent, by whatever session comes next, as simulated.
TEST(RecordIngest, ASpooledSimulatedRecordIsStillSimulatedWhenSentLater) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("pychron_spooled_sim_" + std::string(kSampleUuid));
  fs::remove_all(dir);
  auto b = sample_builder();
  auto meta = b.draft().instrument;
  meta.simulated = true;
  b.set_instrument(meta);
  const auto rec = finalized(b);
  {
    persist::Spool spool(dir);
    ASSERT_TRUE(spool.put(rec));
  }
  persist::Spool later(dir);  // another session's
  const auto back = later.get(rec.identity.uuid);
  ASSERT_TRUE(back.has_value()) << back.error().what;
  EXPECT_EQ(back->provenance.sha, rec.provenance.sha);
  const auto out = convert(*back);
  EXPECT_EQ(json::parse(*body_of(out).meta->software_json).at("simulated"), true);
  EXPECT_EQ(to_hex(out.analysis.payload_sha256), rec.provenance.sha);
  fs::remove_all(dir);
}

// Text that is not UTF-8 does not stop a conversion (the JSON writer would
// throw): the bad bytes are replaced, the same way every time.
TEST(RecordIngest, TextThatIsNotUtf8IsReplacedNotThrown) {
  auto rec = sample_record();
  rec.identity.queue_uuid = "queue\xff name";
  rec.instrument.software_version = "0.1\xc3";
  rec.results.whiff = "run\xfe";
  record::InstalledConditional c;
  c.name = "big\xff";
  rec.conditionals.installed.push_back(c);
  rec.spectrometer.source_params["y\xffs"] = 1.0;
  const auto a = convert(rec);
  const auto b = convert(rec);
  const auto& meta = *body_of(a).meta;
  EXPECT_EQ(*meta.queue_names_json, *body_of(b).meta->queue_names_json);
  EXPECT_EQ(json::parse(*meta.queue_names_json), (json{"queue\xEF\xBF\xBD name"}));  // U+FFFD
  EXPECT_EQ(json::parse(*meta.software_json).at("software_version"), "0.1\xEF\xBF\xBD");
  // Parsed into a value: gcc holds json::parse to its result whatever it is cast to.
  EXPECT_NO_THROW({ const json parsed = json::parse(*meta.whiff_result_json); });
  EXPECT_NO_THROW({ const json parsed = json::parse(*meta.conditionals_json); });
  EXPECT_NO_THROW({ const json parsed = json::parse(body_of(a).spectrometer_snapshot->settings_json); });
  EXPECT_EQ(body_of(a).spectrometer_snapshot->settings_json, body_of(b).spectrometer_snapshot->settings_json);
}

TEST(RecordIngest, AnEmptyExtractDeviceIsNone) {
  auto b = sample_builder();
  auto in = b.draft().instrument;
  in.extract_device = "";
  b.set_instrument(in);
  EXPECT_FALSE(body_of(convert(finalized(b))).extract_device);
  in.extract_device = "(none)";
  b.set_instrument(in);
  EXPECT_FALSE(body_of(convert(finalized(b))).extract_device);
}

TEST(RecordIngest, FitsAreWrittenAsTheStoreReadsThem) {
  auto b = sample_builder();
  record::InterceptResult ir;
  ir.intercept.value = 2.5;
  ir.intercept.error = 0.25;
  ir.intercept.n_used = 7;
  ir.fit.kind = reduction::FitKind::Parabolic;
  ir.fit.error = reduction::ErrorType::Sd;
  ir.fit.outliers = {true, 2, 2.5};
  b.set_intercept("Ar40", ir);
  record::BaselineResult br;
  br.value = 0.045;
  br.error = 0.005;
  br.fit.kind = reduction::FitKind::Average;
  b.set_baseline("H1", br);
  const auto out = convert(finalized(b));
  const auto& a = body_of(out);

  const ps::InterceptRow* row = nullptr;
  for (const auto& r : a.roots.intercepts_rows)
    if (r.isotope == "Ar40") row = &r;
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->detector, "H1");
  EXPECT_EQ(row->value, 2.5);
  EXPECT_EQ(row->error, 0.25);
  EXPECT_EQ(row->n, 7);
  EXPECT_EQ(row->fit, std::string(reduction::to_string(reduction::FitKind::Parabolic)));
  EXPECT_EQ(row->fit, "parabolic");
  EXPECT_EQ(row->error_type, "SD");
  const auto j = json::parse(*row->filter_outliers_json);
  EXPECT_EQ(j.at("filter_outliers"), 1);
  EXPECT_EQ(j.at("iterations"), 2);
  EXPECT_EQ(j.at("std_devs"), 2.5);

  ASSERT_EQ(a.roots.baselines_rows.size(), 1u);
  const auto& bl = a.roots.baselines_rows[0];
  EXPECT_EQ(bl.detector, "H1");
  EXPECT_EQ(bl.value, 0.045);
  EXPECT_EQ(bl.error, 0.005);
  EXPECT_EQ(bl.fit, "average");
  EXPECT_EQ(bl.error_type, "SEM");
  EXPECT_EQ(json::parse(*bl.filter_outliers_json).at("filter_outliers"), 0);
}

TEST(RecordIngest, ANonFiniteInterceptIsUnknown) {
  auto b = sample_builder();
  record::InterceptResult ir;
  ir.intercept.value = std::numeric_limits<double>::quiet_NaN();
  ir.intercept.error = std::numeric_limits<double>::infinity();
  b.set_intercept("Ar40", ir);
  record::BaselineResult br;
  br.value = std::numeric_limits<double>::quiet_NaN();
  b.set_baseline("H1", br);
  const auto out = convert(finalized(b));
  const auto& a = body_of(out);
  bool found = false;
  for (const auto& r : a.roots.intercepts_rows)
    if (r.isotope == "Ar40") {
      found = true;
      EXPECT_FALSE(r.value);
      EXPECT_FALSE(r.error);
    }
  EXPECT_TRUE(found);
  EXPECT_FALSE(a.roots.baselines_rows.at(0).value);
}

TEST(RecordIngest, ARecordWithNoSeriesConverts) {
  auto rec = sample_record();
  rec.data.series.clear();
  rec.results.intercepts.clear();
  rec.results.baselines.clear();
  const auto out = convert(rec);
  EXPECT_TRUE(out.blobs.empty());
  const auto& a = body_of(out);
  EXPECT_TRUE(a.roots.signal_refs.empty());
  EXPECT_TRUE(a.isotopes.empty());
  EXPECT_TRUE(a.detectors.empty());
}

TEST(RecordIngest, ADraftRecordIsRefused) {
  auto rec = sample_record();
  rec.provenance.sha.clear();
  EXPECT_NE(error_of(rec).find("not finalized"), std::string::npos);
  rec.provenance.sha = "abc";
  EXPECT_NE(error_of(rec).find("not finalized"), std::string::npos);
  rec.provenance.sha = std::string(64, 'z');
  EXPECT_NE(error_of(rec).find("not finalized"), std::string::npos);
}

TEST(RecordIngest, AUuidThatDoesNotParseIsRefused) {
  auto rec = sample_record();
  rec.identity.uuid = "u-1";
  EXPECT_NE(error_of(rec).find("uuid"), std::string::npos);
}

TEST(RecordIngest, ATimestampThatDoesNotParseIsRefused) {
  auto rec = sample_record();
  rec.identity.timestamp = "yesterday";
  EXPECT_NE(error_of(rec).find("timestamp"), std::string::npos);
}

TEST(RecordIngest, TheSampleTextIsNotCarried) {
  auto b1 = sample_builder();
  auto b2 = sample_builder();
  auto s = b2.draft().sample;
  s.sample = "OTHER";
  s.note = "changed";
  b2.set_sample(s);
  const auto r1 = finalized(b1);
  const auto r2 = finalized(b2);
  ASSERT_NE(r1.provenance.sha, r2.provenance.sha);
  auto x = convert(r1);
  auto y = convert(r2);
  auto& ax = std::get<ps::AnalysisIngest>(x.analysis.body);
  auto& ay = std::get<ps::AnalysisIngest>(y.analysis.body);
  EXPECT_NE(ax.record_sha256, ay.record_sha256);
  EXPECT_NE(x.analysis.payload_sha256, y.analysis.payload_sha256);
  ay.record_sha256 = ax.record_sha256;
  y.analysis.payload_sha256 = x.analysis.payload_sha256;
  // Everything else is equal: compare field by field what the sample could touch.
  EXPECT_EQ(ax.identifier, ay.identifier);
  EXPECT_EQ(ax.extraction.extract_value, ay.extraction.extract_value);
  EXPECT_EQ(ax.roots.signal_refs, ay.roots.signal_refs);
  EXPECT_EQ(ax.roots.intercepts_rows, ay.roots.intercepts_rows);
  EXPECT_EQ(ax.scripts.measurement, ay.scripts.measurement);
  EXPECT_EQ(ax.meta.has_value(), ay.meta.has_value());
  EXPECT_EQ(ax.queue.has_value(), ay.queue.has_value());
  EXPECT_EQ(ax.load_name, ay.load_name);
  EXPECT_EQ(ax.analyst, ay.analyst);
}

TEST(RecordIngest, TheQueueNameIsCarried) {
  auto rec = sample_record();  // queue_uuid holds the queue's name
  rec.identity.queue_uuid = "first-queue";
  const auto out = convert(rec);
  ASSERT_TRUE(body_of(out).meta);
  EXPECT_EQ(json::parse(*body_of(out).meta->queue_names_json), (json{"first-queue"}));
  rec.identity.queue_uuid.clear();
  const auto none = convert(rec);
  EXPECT_FALSE(body_of(none).meta && body_of(none).meta->queue_names_json);
}

// A detector intercalibration as the acquisition records it: Ar40 hopped onto
// three Faradays, its intercepts under "Ar40:<detector>", the sniff of the
// first hop, a baseline block on each detector.
record::AnalysisRecord detector_ic_record() {
  using namespace record;
  auto b = sample_builder("ic");
  auto r = b.draft();
  r.identity.analysis_type = "detector_ic";
  r.data.series.clear();
  r.results.intercepts.clear();
  r.results.baselines.clear();
  InterceptResult ir;
  ir.intercept.error = 0.01;
  ir.intercept.n_used = 3;
  ir.fit.kind = reduction::FitKind::Linear;
  BaselineResult br;
  br.error = 0.005;
  float v = 100.f;
  for (const char* det : {"AX", "H1", "L1"}) {  // the collector's order: by isotope, then detector
    r.data.series.push_back({"Ar40", det, "signal", {{0.f, 1.f, 2.f}, {v, v - 1, v - 2}, {}}});
    ir.intercept.value = v;
    r.results.intercepts[std::string("Ar40:") + det] = ir;
    v += 1.f;
  }
  r.data.series.push_back({"Ar40", "H1", "sniff", {{0.f, 1.f}, {99.f, 98.f}, {}}});
  float base = 0.01f;
  for (const char* det : {"AX", "H1", "L1"}) {
    r.data.series.push_back({"", det, "baseline", {{9.f, 10.f}, {base, base}, {}}});
    br.value = base;
    r.results.baselines[det] = br;
    base += 0.01f;
  }
  r.results.icfactors = {{"AX", 1.01}, {"L1", 0.99}};
  r.provenance.sha = record::compute_sha(r);
  return r;
}

TEST(RecordIngest, AnIsotopeOnSeveralDetectorsIsStoredUnderEachDetector) {
  const auto rec = detector_ic_record();
  const auto out = convert(rec);
  const auto& a = body_of(out);

  // One isotope row a detector, named as the store's readers name it.
  ASSERT_EQ(a.isotopes.size(), 3u);
  EXPECT_EQ(a.isotopes[0].isotope, "AX:Ar40");
  EXPECT_EQ(a.isotopes[0].detector, "AX");
  EXPECT_EQ(a.isotopes[1].isotope, "H1:Ar40");
  EXPECT_EQ(a.isotopes[1].detector, "H1");
  EXPECT_EQ(a.isotopes[2].isotope, "L1:Ar40");
  EXPECT_EQ(a.isotopes[2].detector, "L1");

  // The raw series, in the record's order: the keys are the isotopes' (the
  // sniff's too, so that an isotope's sniff is found by the isotope's key).
  std::vector<std::string> refs;
  for (const auto& r : a.roots.signal_refs) refs.push_back(r.series_kind + " " + r.series_key + " " + r.detector);
  EXPECT_EQ(refs, (std::vector<std::string>{"signal AX:Ar40 AX", "signal H1:Ar40 H1", "signal L1:Ar40 L1",
                                            "sniff H1:Ar40 H1", "baseline AX AX", "baseline H1 H1", "baseline L1 L1"}));
  // No two refs share the store's key (kind, key).
  std::set<std::pair<std::string, std::string>> keys;
  for (const auto& r : a.roots.signal_refs) EXPECT_TRUE(keys.emplace(r.series_kind, r.series_key).second) << r.series_key;

  ASSERT_EQ(a.roots.intercepts_rows.size(), 3u);
  EXPECT_EQ(a.roots.intercepts_rows[0].isotope, "AX:Ar40");
  EXPECT_EQ(a.roots.intercepts_rows[0].detector, "AX");
  EXPECT_EQ(a.roots.intercepts_rows[0].value, 100.0);
  EXPECT_EQ(a.roots.intercepts_rows[1].isotope, "H1:Ar40");
  EXPECT_EQ(a.roots.intercepts_rows[1].detector, "H1");
  EXPECT_EQ(a.roots.intercepts_rows[1].value, 101.0);
  EXPECT_EQ(a.roots.intercepts_rows[2].isotope, "L1:Ar40");
  EXPECT_EQ(a.roots.intercepts_rows[2].detector, "L1");
  ASSERT_EQ(a.roots.baselines_rows.size(), 3u);
  EXPECT_EQ(a.roots.baselines_rows[1].detector, "H1");
  ASSERT_EQ(a.detectors.size(), 3u);
  ASSERT_EQ(a.roots.icfactors_rows.size(), 2u);
  EXPECT_EQ(a.roots.icfactors_rows[0].detector, "AX");
}

// A baseline block and a baseline hop on one detector: one stored series,
// the block's points and then the hop's, as the collector fitted them.
TEST(RecordIngest, ADetectorsBaselineSeriesAreStoredAsOne) {
  auto rec = sample_record();  // has a baseline hop on H1 ("Ar40", H1, baseline): t 0, 1
  // The collector's order: the block (no isotope) comes before the hop.
  rec.data.series.insert(rec.data.series.begin(), {"", "H1", "baseline", {{90.f, 91.f, 92.f}, {0.06f, 0.07f, 0.08f}, {}}});
  const auto out = convert(rec);
  const auto& a = body_of(out);
  ASSERT_EQ(a.roots.signal_refs.size(), 3u);  // two signals and the one baseline
  const auto& ref = a.roots.signal_refs[0];
  EXPECT_EQ(ref.series_kind, "baseline");
  EXPECT_EQ(ref.series_key, "H1");
  EXPECT_EQ(ref.detector, "H1");
  EXPECT_EQ(ref.n_points, 5);
  ASSERT_EQ(out.blobs.size(), 3u);
  const auto& blob = blob_of(out.blobs[0]);
  EXPECT_EQ(out.blobs[0].payload_sha256, ref.blob_sha);
  EXPECT_EQ(blob.codec, ps::kCodecTv);
  const auto points = ps::decode_tv(blob.bytes);
  ASSERT_TRUE(points.has_value());
  ASSERT_EQ(points->size(), 5u);
  const std::vector<float> t{90.f, 91.f, 92.f, 0.f, 1.f}, v{0.06f, 0.07f, 0.08f, 0.05f, 0.04f};
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ((*points)[i].t, t[i]) << i;
    EXPECT_EQ((*points)[i].v, v[i]) << i;
  }
  ASSERT_EQ(a.roots.baselines_rows.size(), 1u);  // the record's one fitted baseline of H1
  EXPECT_EQ(a.roots.baselines_rows[0].detector, "H1");
}

// What no measurement produces and the store's keys cannot hold is still
// refused: two series of one kind for one isotope on one detector.
TEST(RecordIngest, TwoSeriesOfOneKindIsotopeAndDetectorAreRefused) {
  auto rec = sample_record();
  rec.data.series.push_back(rec.data.series[0]);  // Ar40 on H1, signal, again
  const auto e = error_of(rec);
  EXPECT_NE(e.find("two signal series"), std::string::npos) << e;
  EXPECT_NE(e.find("Ar40"), std::string::npos) << e;
}

TEST(RecordIngest, AnInterceptTakesItsDetectorFromAnySeriesOfItsIsotope) {
  auto rec = sample_record();
  // Ar37 has no signal series: a sniff on L1 and a whiff on H2.
  rec.data.series.push_back({"Ar37", "L1", "sniff", rec.data.series[0].trace});
  rec.data.series.push_back({"Ar37", "H2", "whiff", rec.data.series[0].trace});
  rec.results.intercepts["Ar37"] = rec.results.intercepts["Ar40"];
  const auto out = convert(rec);
  bool found = false;
  for (const auto& r : body_of(out).roots.intercepts_rows) {
    if (r.isotope == "Ar37") {
      found = true;
      EXPECT_EQ(r.detector, "H2");  // the lowest name
    }
    if (r.isotope == "Ar40") {
      EXPECT_EQ(r.detector, "H1");  // its signal series
    }
  }
  EXPECT_TRUE(found);
}

TEST(RecordIngest, AnInterceptWithNoSeriesIsRefused) {
  auto rec = sample_record();
  rec.results.intercepts["Ar38"] = rec.results.intercepts["Ar40"];
  EXPECT_NE(error_of(rec).find("Ar38"), std::string::npos);
}

TEST(RecordIngest, NonFiniteNumbersAreUnset) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto rec = sample_record();
  rec.spectrometer.gains["CDD"] = nan;
  rec.spectrometer.deflections["CDD"] = nan;
  rec.extraction.spec.value = nan;
  rec.extraction.spec.cryo_temperature = nan;
  rec.extraction.actuals.beam_diameter = std::numeric_limits<double>::infinity();
  rec.results.icfactors["CDD"] = nan;
  const auto out = convert(rec);
  const auto& a = body_of(out);
  EXPECT_EQ(a.detectors[0].detector, "CDD");
  EXPECT_FALSE(a.detectors[0].gain_used);
  EXPECT_FALSE(a.detectors[0].deflection);
  EXPECT_FALSE(a.extraction.extract_value);
  EXPECT_FALSE(a.extraction.cryo_temperature);
  EXPECT_FALSE(a.extraction.beam_diameter);
  ASSERT_EQ(a.roots.icfactors_rows.size(), 1u);
  EXPECT_FALSE(a.roots.icfactors_rows[0].value);
}

TEST(RecordIngest, ASeriesOfMismatchedLengthsIsRefused) {
  auto rec = sample_record();  // a hand-made record can hold what the builder refuses
  rec.data.series[0].trace.v.pop_back();
  EXPECT_NE(error_of(rec).find("mismatched"), std::string::npos);
}

// How the run ended (spec 4.5, owner 2026-10-13): a failed run is stored as
// any other and tagged invalid, with what ended it as the tag's note, so the
// browser and figures leave it out by default and recall can still open it.
// A truncated run is stored as a run always was, tagged ok.
TEST(RecordIngest, AFailedRecordIsStoredTaggedInvalid) {
  auto b = sample_builder();
  b.set_status(record::RecordStatus::Failed, "spectrometer link down");
  const auto out = convert(finalized(b));
  EXPECT_EQ(body_of(out).roots.tag, (ps::TagValue{"invalid", "spectrometer link down", std::nullopt}));

  b.set_status(record::RecordStatus::Failed);
  EXPECT_EQ(body_of(convert(finalized(b))).roots.tag, (ps::TagValue{"invalid", std::nullopt, std::nullopt}));
}

TEST(RecordIngest, ATruncatedRecordIsStoredAsAnyRun) {
  auto b = sample_builder();
  const auto plain = convert(finalized(b));
  b.set_status(record::RecordStatus::Truncated);
  const auto truncated = convert(finalized(b));
  EXPECT_EQ(body_of(truncated).roots.tag, (ps::TagValue{"ok", std::nullopt, std::nullopt}));
  // Everything stored is the same but the item's payload sha: the record's
  // own bytes say truncated.
  const auto after_item = [](const std::string& d) { return d.substr(d.find('\n')); };
  EXPECT_EQ(after_item(describe(truncated)), after_item(describe(plain)));
  EXPECT_NE(describe(truncated), describe(plain));
}

// A detector read at several baseline positions (v3) keeps one stored value,
// the record's; every position and each signal's choice go in the row's
// extra. A detector with one position has none (the ordinary run's rows).
TEST(RecordIngest, SeveralBaselinePositionsGoToExtra) {
  auto rec = sample_record();
  ASSERT_EQ(rec.results.baseline_positions.size(), 1u);  // the v2 record's one
  EXPECT_FALSE(body_of(convert(rec)).roots.baselines_rows[0].extra_json);

  rec.provenance.schema_version = record::kRecordSchemaVersion;
  rec.results.baseline_positions = {{"H1", "", 0, 0, "before_inlet", 34.2, {}, {0.04, 0.004, {}}},
                                    {"H1", "", 2, 0, "after_main", {}, 110.0, {0.05, 0.005, {}}}};
  rec.results.baseline_choices["Ar40@H1"] = {"bracketing_average", {0, 1}, 0.045, 0.0032};
  rec.results.baseline_choices["Ar36@CDD"] = {"nearest", {0}, 0.0, 0.0};  // another detector's
  const auto out = convert(rec);
  const auto& rows = body_of(out).roots.baselines_rows;
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].value, 0.045);  // Results::baselines, unchanged
  ASSERT_TRUE(rows[0].extra_json);
  const auto extra = json::parse(*rows[0].extra_json);
  ASSERT_EQ(extra.at("positions").size(), 2u);
  EXPECT_EQ(extra.at("positions")[0].at("when"), "before_inlet");
  EXPECT_EQ(extra.at("positions")[0].at("magnet"), 34.2);
  EXPECT_EQ(extra.at("positions")[1].at("segment"), 2);
  EXPECT_EQ(extra.at("positions")[1].at("deflection"), 110.0);
  ASSERT_EQ(extra.at("choices").size(), 1u);
  EXPECT_EQ(extra.at("choices").at("Ar40@H1").at("rule"), "bracketing_average");
  EXPECT_EQ(extra.at("choices").at("Ar40@H1").at("baselines"), json::array({0, 1}));
}

// Baseline modifiers (measurement plan v2 spec 3.5) have a producer: the
// detector's row carries the modifier's value and error, the analysis's
// meta every modifier.
TEST(RecordIngest, BaselineModifiersHaveAProducer) {
  auto rec = sample_record();
  EXPECT_FALSE(body_of(convert(rec)).meta->baseline_modifiers_json);
  rec.provenance.schema_version = record::kRecordSchemaVersion;
  rec.results.baseline_modifiers["H1"] = {"Ar40+Ar36", "0.01*x", "counting_statistics", 11.0, 0.5};
  const auto out = convert(rec);
  const auto& a = body_of(out);
  ASSERT_EQ(a.roots.baselines_rows.size(), 1u);
  EXPECT_EQ(a.roots.baselines_rows[0].modifier_value, 11.0);
  EXPECT_EQ(a.roots.baselines_rows[0].modifier_error, 0.5);
  ASSERT_TRUE(a.meta && a.meta->baseline_modifiers_json);
  const auto mods = json::parse(*a.meta->baseline_modifiers_json);
  EXPECT_EQ(mods.at("H1").at("variable"), "Ar40+Ar36");
  EXPECT_EQ(mods.at("H1").at("model"), "0.01*x");
  EXPECT_EQ(mods.at("H1").at("value"), 11.0);
}

// Worked plan 7.6's shape: three gases with labels apart, every series stored.
TEST(RecordIngest, MultiGasRecordStores) {
  auto b = sample_builder();
  b.add_series({"He4", "H1", "signal", {{1.f, 2.f}, {5.f, 5.f}, {}}, "He", 0, 0, 0});
  b.add_series({"Ne20", "CDD", "signal", {{1.f, 2.f}, {3.f, 3.f}, {}}, "Ne", 0, 0, 0});
  b.add_series({"Ne22", "CDD", "signal", {{1.f, 2.f}, {1.f, 1.f}, {}}, "Ne", 0, 1, 0});
  b.add_gas({"He", 10.0, {{{}, 1.0, "", ""}}, std::nullopt, std::nullopt, {}});
  b.add_gas({"Ne", 50.0, {{{}, 2.0, "", ""}}, std::nullopt, std::nullopt, {}});
  record::InterceptResult ir;
  ir.intercept.value = 5;
  ir.intercept.n_used = 2;
  b.set_intercept("He4@H1", ir);
  b.set_intercept("Ne20@CDD", ir);
  b.set_intercept("Ne22@CDD", ir);
  const auto rec = finalized(b);
  EXPECT_EQ(rec.provenance.schema_version, record::kRecordSchemaVersion);
  const auto out = convert(rec);
  const auto& a = body_of(out);
  std::set<std::string> keys;
  for (const auto& ref : a.roots.signal_refs) keys.insert(ref.series_kind + " " + ref.series_key);
  for (const char* k : {"signal Ar40", "signal Ar36", "signal He4", "signal Ne20", "signal Ne22"})
    EXPECT_TRUE(keys.contains(k)) << k;
  std::set<std::string> intercepts;
  for (const auto& row : a.roots.intercepts_rows) intercepts.insert(row.isotope);
  EXPECT_EQ(intercepts, (std::set<std::string>{"Ar36", "Ar40", "He4", "Ne20", "Ne22"}));
}

// A gas's prepare and pump-out (staged release, spec 11.5) are the record's:
// the store keeps no row of them, and a record with them converts as one
// without (the e2e spec's not-stored list).
TEST(RecordIngest, PrepareRecordIsNotStored) {
  auto b = sample_builder();
  b.add_series({"He4", "H1", "signal", {{1.f, 2.f}, {5.f, 5.f}, {}}, "He", 0, 0, 0});
  record::InterceptResult ir;
  ir.intercept.value = 5;
  ir.intercept.n_used = 2;
  b.set_intercept("He4@H1", ir);
  const auto plain = finalized(b);
  b.add_gas({"He", 10.0, {{{}, 1.0, "", ""}},
             record::PrepareRec{"cryo:release", {{"cryo_temp", "35"}}, "2026-10-10T12:31:00Z", "2026-10-10T12:36:00Z"},
             record::PumpOutRec{"IP", 120}, {}});
  const auto staged = finalized(b);
  auto rows = body_of(convert(staged));
  auto without = body_of(convert(plain));
  EXPECT_EQ(rows.roots.signal_refs.size(), without.roots.signal_refs.size());
  EXPECT_EQ(rows.roots.intercepts_rows, without.roots.intercepts_rows);
  EXPECT_EQ(rows.roots.baselines_rows, without.roots.baselines_rows);
  ASSERT_TRUE(rows.meta && without.meta);
  EXPECT_EQ(rows.meta->software_json, without.meta->software_json);
  EXPECT_EQ(rows.meta->baseline_modifiers_json, without.meta->baseline_modifiers_json);
}
