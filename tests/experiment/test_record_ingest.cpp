#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>

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

}  // namespace

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
  EXPECT_EQ(a.record_schema_version, record::kRecordSchemaVersion);
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

TEST(RecordIngest, OneIsotopeOnTwoDetectorsIsRefused) {
  auto rec = sample_record();
  rec.data.series.push_back({"Ar40", "H2", "signal", rec.data.series[0].trace});
  const auto e = error_of(rec);
  EXPECT_NE(e.find("Ar40"), std::string::npos);
  EXPECT_NE(e.find("H1"), std::string::npos);
  EXPECT_NE(e.find("H2"), std::string::npos);
}

TEST(RecordIngest, TwoBaselinesOnOneDetectorIsRefused) {
  auto rec = sample_record();
  rec.data.series.push_back({"Ar39", "H1", "baseline", rec.data.series[2].trace});
  const auto e = error_of(rec);
  EXPECT_NE(e.find("baseline"), std::string::npos);
  EXPECT_NE(e.find("H1"), std::string::npos);
  EXPECT_NE(e.find("detectors H1, H1"), std::string::npos);  // a baseline is keyed by its detector
}

TEST(RecordIngest, AnInterceptTakesItsDetectorFromAnySeriesOfItsIsotope) {
  auto rec = sample_record();
  // Ar37 has only baselines-free, non-signal series on two detectors.
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
    if (r.isotope == "Ar40") EXPECT_EQ(r.detector, "H1");  // its signal series
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
