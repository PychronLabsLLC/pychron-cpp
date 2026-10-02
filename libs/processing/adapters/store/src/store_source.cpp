#include "pychron/processing/store_source.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pychron::processing {

namespace ps = pychron::persistence;

namespace {

double seconds(ps::UtcTime t) { return static_cast<double>(t.micros) / 1e6; }
ps::UtcTime utc(double seconds) { return ps::UtcTime{std::llround(seconds * 1e6)}; }

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// "linear" + "SEM" -> FitSpec; outliers from the legacy filter_outliers dict.
std::optional<reduction::FitSpec> fit_spec(const std::optional<std::string>& fit,
                                           const std::optional<std::string>& error_type,
                                           const std::optional<std::string>& outliers_json) {
  if (!fit) return std::nullopt;
  const auto kind = reduction::parse_fit_kind(lower(*fit));
  if (!kind) return std::nullopt;
  reduction::FitSpec spec;
  spec.kind = *kind;
  if (error_type && lower(*error_type) == "sd") spec.error = reduction::ErrorType::Sd;
  if (outliers_json) {
    const auto o = flat_json_numbers(*outliers_json);
    if (auto f = o.find("filter_outliers"); f != o.end()) spec.outliers.enabled = f->second != 0;
    if (auto f = o.find("iterations"); f != o.end()) spec.outliers.iterations = static_cast<int>(f->second);
    if (auto f = o.find("std_devs"); f != o.end()) spec.outliers.std_devs = f->second;
  }
  return spec;
}

// The stored value with a manual override applied.
Value value_of(std::optional<double> value, std::optional<double> error, const ps::ManualOverride& m) {
  Value v{value.value_or(0.0), error.value_or(0.0)};
  if (m.use_value && m.value) v.value = *m.value;
  if (m.use_error && m.error) v.error = *m.error;
  return v;
}

template <class T>
const T* head_payload(const std::map<ps::Kind, ps::RevisionPayload>& heads, ps::Kind kind) {
  auto it = heads.find(kind);
  return it == heads.end() ? nullptr : std::get_if<T>(&it->second);
}

Result<ps::Uuid> parse_uuid(const std::string& text) {
  auto u = ps::Uuid::parse(text);
  if (!u) return fail(ErrorKind::Config, "not an analysis uuid: " + text);
  return *u;
}

}  // namespace

// ---------------------------------------------------------------- mappings

std::map<std::string, double> flat_json_numbers(std::string_view s) {
  std::map<std::string, double> out;
  std::size_t i = 0;
  auto ws = [&] {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  };
  auto string = [&](std::string* into) -> bool {  // at the opening quote
    if (i >= s.size() || s[i] != '"') return false;
    for (++i; i < s.size(); ++i) {
      if (s[i] == '"') {
        ++i;
        return true;
      }
      if (s[i] == '\\') {
        if (++i >= s.size()) return false;
        if (into) into->push_back(s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i]);
        continue;
      }
      if (into) into->push_back(s[i]);
    }
    return false;
  };
  auto skip_nested = [&]() -> bool {  // at '{' or '['
    int depth = 0;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') {
        if (!string(nullptr)) return false;
        continue;
      }
      if (c == '{' || c == '[') ++depth;
      if (c == '}' || c == ']') --depth;
      ++i;
      if (depth == 0) return true;
    }
    return false;
  };
  ws();
  if (i >= s.size() || s[i] != '{') return out;
  ++i;
  while (true) {
    ws();
    if (i < s.size() && s[i] == '}') return out;
    std::string key;
    if (!string(&key)) return out;
    ws();
    if (i >= s.size() || s[i] != ':') return out;
    ++i;
    ws();
    if (i >= s.size()) return out;
    const char c = s[i];
    if (c == '"') {
      if (!string(nullptr)) return out;
    } else if (c == '{' || c == '[') {
      if (!skip_nested()) return out;
    } else if (s.substr(i, 4) == "true") {
      out[key] = 1.0;
      i += 4;
    } else if (s.substr(i, 5) == "false") {
      out[key] = 0.0;
      i += 5;
    } else if (s.substr(i, 4) == "null") {
      i += 4;
    } else {
      double v = 0;
      const auto r = std::from_chars(s.data() + i, s.data() + s.size(), v);
      if (r.ec != std::errc{}) return out;
      i = static_cast<std::size_t>(r.ptr - s.data());
      out[key] = v;
    }
    ws();
    if (i < s.size() && s[i] == ',') {
      ++i;
      continue;
    }
    return out;
  }
}

std::string redact_password(std::string url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  const auto authority_end = url.find_first_of("/?#", scheme + 3);
  const auto at = url.rfind('@', authority_end == std::string::npos ? std::string::npos : authority_end);
  if (at == std::string::npos || at < scheme + 3) return url;
  const auto colon = url.find(':', scheme + 3);
  if (colon == std::string::npos || colon > at) return url;
  url.replace(colon + 1, at - colon - 1, "***");
  return url;
}

ps::BrowseFilter to_store_filter(const BrowseQuery& q) {
  ps::BrowseFilter f;
  f.text = q.text;
  f.identifiers = q.identifiers;
  f.samples = q.samples;
  f.projects = q.projects;
  f.principal_investigators = q.principal_investigators;
  f.materials = q.materials;
  f.analysis_types = q.analysis_types;
  f.mass_spectrometers = q.mass_spectrometers;
  f.extract_devices = q.extract_devices;
  f.loads = q.loads;
  f.irradiations = q.irradiations;
  f.levels = q.levels;
  f.repositories = q.repositories;
  if (q.from) f.from = utc(*q.from);
  if (q.to) f.to = utc(*q.to);
  f.last_hours = q.last_hours;
  f.exclude_tags = q.exclude_tags;
  return f;
}

ps::BrowseFacet to_store_facet(Facet f) noexcept {
  switch (f) {
    case Facet::AnalysisType: return ps::BrowseFacet::AnalysisType;
    case Facet::MassSpectrometer: return ps::BrowseFacet::MassSpectrometer;
    case Facet::ExtractDevice: return ps::BrowseFacet::ExtractDevice;
    case Facet::Project: return ps::BrowseFacet::Project;
    case Facet::PrincipalInvestigator: return ps::BrowseFacet::PrincipalInvestigator;
    case Facet::Sample: return ps::BrowseFacet::Sample;
    case Facet::Material: return ps::BrowseFacet::Material;
    case Facet::Identifier: return ps::BrowseFacet::Identifier;
    case Facet::Irradiation: return ps::BrowseFacet::Irradiation;
    case Facet::Level: return ps::BrowseFacet::Level;
    case Facet::Load: return ps::BrowseFacet::Load;
    case Facet::Repository: return ps::BrowseFacet::Repository;
  }
  return ps::BrowseFacet::AnalysisType;
}

AnalysisSummary summary_from_row(const ps::BrowseRow& r) {
  AnalysisSummary s;
  s.uuid = r.summary.uuid.str();
  s.runid = r.summary.runid;
  s.identifier = r.summary.identifier;
  s.aliquot = r.summary.aliquot;
  s.increment = r.summary.increment;
  s.analysis_type = r.summary.analysis_type;
  s.timestamp = seconds(r.summary.timestamp);
  s.mass_spectrometer = r.summary.mass_spectrometer;
  s.sample = r.sample;
  s.project = r.project;
  s.material = r.material;
  s.principal_investigator = r.principal_investigator;
  s.extract_device = r.extract_device;
  s.load = r.load;
  s.irradiation = r.irradiation;
  s.level = r.level;
  s.repository = r.repository;
  s.tag = r.tag.empty() ? "ok" : r.tag;
  s.extract_value = r.extract_value;
  s.extract_units = r.extract_units;
  return s;
}

Result<Analysis> analysis_from_store(const StoreAnalysisParts& parts) {
  const auto& d = parts.detail;
  const auto& row = d.row;
  Analysis a;
  a.uuid = row.summary.uuid.str();
  a.identifier = row.summary.identifier;
  a.aliquot = row.summary.aliquot;
  a.increment = row.summary.increment;
  a.runid = row.summary.runid.empty() ? make_runid(a.identifier, a.aliquot, a.increment) : row.summary.runid;
  a.analysis_type = row.summary.analysis_type;
  a.timestamp = seconds(row.summary.timestamp);
  a.mass_spectrometer = row.summary.mass_spectrometer;
  a.extract_device = row.extract_device;
  a.sample = row.sample;
  a.project = row.project;
  a.material = row.material;
  a.principal_investigator = row.principal_investigator;
  a.irradiation = row.irradiation;
  a.level = row.level;
  if (row.position) a.position = std::to_string(*row.position);
  a.load = row.load;
  a.repository = row.repository;
  a.analyst = d.analyst.value_or("");
  a.tag = row.tag.empty() ? "ok" : row.tag;
  if (const auto* t = head_payload<ps::TagValue>(parts.heads, ps::Kind::Tags); t && !t->name.empty()) a.tag = t->name;
  if (const auto* n = head_payload<ps::AnnotationValue>(parts.heads, ps::Kind::Annotation)) a.comment = n->comment.value_or("");

  const auto& x = d.extraction;
  a.extraction.value = x.extract_value;
  a.extraction.units = x.extract_units.value_or("");
  a.extraction.duration = x.extract_duration;
  a.extraction.cleanup = x.cleanup_duration;
  a.extraction.weight = x.weight;
  a.extraction.beam_diameter = x.beam_diameter;
  a.extraction.pattern = x.pattern.value_or("");

  for (const auto& det : d.detectors) {
    if (det.gain_used) a.gains[det.detector] = *det.gain_used;
    if (det.deflection) a.deflections[det.detector] = *det.deflection;
  }
  for (const auto& pc : d.peak_centers)
    if (pc.center_dac) a.peak_centers.push_back(PeakCenterInfo{pc.detector, *pc.center_dac, pc.resolution});
  if (d.environmental_json) a.environmentals = flat_json_numbers(*d.environmental_json);

  // Isotopes: one per intercept row; the detector from the row, else from the
  // analysis' isotope list.
  std::map<std::string, std::string> detector_of;
  for (const auto& iso : d.isotopes) detector_of.emplace(iso.isotope, iso.detector);
  const auto* intercepts = head_payload<ps::Intercepts>(parts.heads, ps::Kind::Intercepts);
  const auto* baselines = head_payload<ps::Baselines>(parts.heads, ps::Kind::Baselines);
  const auto* blanks = head_payload<ps::Blanks>(parts.heads, ps::Kind::Blanks);
  const auto* ics = head_payload<ps::IcFactors>(parts.heads, ps::Kind::IcFactors);
  if (intercepts) {
    for (const auto& ir : *intercepts) {
      IsotopeData iso;
      iso.key = ir.isotope;
      const auto colon = ir.isotope.find(':');
      iso.isotope = colon == std::string::npos ? ir.isotope : ir.isotope.substr(colon + 1);
      iso.detector = !ir.detector.empty() ? ir.detector
                     : detector_of.count(ir.isotope) ? detector_of[ir.isotope]
                                                     : (colon == std::string::npos ? "" : ir.isotope.substr(0, colon));
      iso.intercept = value_of(ir.value, ir.error, ir.manual);
      iso.fit = fit_spec(ir.fit, ir.error_type, ir.filter_outliers_json);
      iso.n = ir.n.value_or(0);
      iso.include_baseline_error = ir.include_baseline_error.value_or(false);
      if (baselines)
        for (const auto& b : *baselines)
          if (b.detector == iso.detector) {
            iso.baseline = value_of(b.value, b.error, b.manual);
            iso.baseline_fit = fit_spec(b.fit, b.error_type, b.filter_outliers_json);
          }
      if (blanks)
        for (const auto& b : *blanks)
          if (b.isotope == iso.key || b.isotope == iso.isotope) {
            iso.blank = value_of(b.value, b.error, b.manual);
            iso.blank_source = b.fit.value_or("");
            if (b.isotope == iso.key) break;
          }
      if (ics)
        for (const auto& ic : *ics)
          if (ic.detector == iso.detector && (ic.value || (ic.manual.use_value && ic.manual.value)))
            iso.ic_factor = value_of(ic.value, ic.error, ic.manual);
      a.isotopes.push_back(std::move(iso));
    }
  }
  std::stable_sort(a.isotopes.begin(), a.isotopes.end(),
                   [](const IsotopeData& p, const IsotopeData& q) { return p.isotope > q.isotope; });

  for (const auto& ref : parts.refs) {
    if (const auto* f = std::get_if<ps::FluxValue>(&ref)) {
      if (!f->j) continue;
      reduction::Flux flux;
      flux.j = {*f->j, f->j_err.value_or(0.0)};
      flux.position_jerr = f->position_jerr.value_or(0.0);
      if (f->lambda_k_total) flux.lambda_k_total = reduction::Measured{*f->lambda_k_total, f->lambda_k_total_err.value_or(0.0)};
      a.context.flux = flux;
    } else if (const auto* p = std::get_if<ps::ProductionValue>(&ref)) {
      std::map<std::string, reduction::Measured, std::less<>> rows;
      for (const auto& r : p->ratios) rows[r.key] = {r.value, r.error};
      auto ratios = reduction::production_from_rows(rows);
      if (!ratios) return fail(ErrorKind::Config, a.runid + ": production: " + ratios.error().what);
      a.context.production = *ratios;
    } else if (const auto* c = std::get_if<ps::ChronologyValue>(&ref)) {
      a.context.chronology.clear();
      for (const auto& dose : c->doses)
        a.context.chronology.push_back(
            reduction::Dose{dose.power, dose.start.micros / 1'000'000, dose.end.micros / 1'000'000});
    } else if (const auto* g = std::get_if<ps::GainsValue>(&ref)) {
      // Gains used at acquisition win; the reference fills the rest.
      for (const auto& gain : g->gains) a.gains.emplace(gain.detector, gain.gain);
    }
  }
  return a;
}

Result<RawSeries> series_from_blob(const ps::SignalRefRow& ref, const ps::BlobData& blob) {
  RawSeries out;
  if (ref.series_kind == "signal") {
    out.kind = SeriesKind::Signal;
  } else if (ref.series_kind == "baseline") {
    out.kind = SeriesKind::Baseline;
  } else if (ref.series_kind == "sniff") {
    out.kind = SeriesKind::Sniff;
  } else {
    return fail(ErrorKind::Protocol, "unsupported series kind " + ref.series_kind);
  }
  out.key = ref.series_key;
  out.detector = ref.detector;
  auto push = [&](double t, double v) {
    out.t.push_back(t);
    out.v.push_back(v);
  };
  if (blob.codec == ps::kCodecTv) {
    auto pts = ps::decode_tv(blob.bytes);
    if (!pts) return fail(pts.error());
    for (const auto& p : *pts) push(p.t, p.v);
  } else if (blob.codec == ps::kCodecTvs) {
    auto pts = ps::decode_tvs(blob.bytes);
    if (!pts) return fail(pts.error());
    for (const auto& p : *pts) push(p.t, p.v);
  } else {
    return fail(ErrorKind::Protocol, "unsupported codec " + blob.codec);
  }
  if (ref.start_index || ref.end_index) {
    const auto n = static_cast<int>(out.t.size());
    const int begin = std::clamp(ref.start_index.value_or(0), 0, n);
    const int end = std::clamp(ref.end_index.value_or(n), begin, n);
    out.t = std::vector<double>(out.t.begin() + begin, out.t.begin() + end);
    out.v = std::vector<double>(out.v.begin() + begin, out.v.begin() + end);
  }
  return out;
}

// ---------------------------------------------------------------- source

struct StoreSource::Impl {
  using Task = std::function<void(ps::IStore&)>;

  ps::StoreConfig config;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<Task> queue;
  bool stopping = false;
  std::vector<std::thread> workers;

  std::atomic<std::uint64_t> generation{1};
  std::mutex refresh_mutex;
  ps::ChangeSeq cursor = 0;

  std::mutex cache_mutex;
  std::map<std::string, AnalysisPtr> cache;

  ~Impl() {
    {
      std::lock_guard lock(queue_mutex);
      stopping = true;
    }
    queue_cv.notify_all();
    for (auto& w : workers)
      if (w.joinable()) w.join();
  }

  // Opens a store on this thread, reports the outcome, then runs tasks until
  // stopped. The store closes on the thread that opened it.
  void run(const std::shared_ptr<std::promise<Result<void>>>& opened, ps::StoreConfig cfg) {
    auto store = ps::open_store(cfg);
    if (!store) {
      opened->set_value(fail(store.error()));
      return;
    }
    opened->set_value({});
    while (true) {
      Task task;
      {
        std::unique_lock lock(queue_mutex);
        queue_cv.wait(lock, [&] { return stopping || !queue.empty(); });
        if (queue.empty()) return;
        task = std::move(queue.front());
        queue.pop_front();
      }
      task(**store);
    }
  }

  template <class R>
  Result<R> call(std::function<Result<R>(ps::IStore&)> fn) {
    auto promise = std::make_shared<std::promise<Result<R>>>();
    auto future = promise->get_future();
    {
      std::lock_guard lock(queue_mutex);
      if (stopping) return fail(ErrorKind::Cancelled, "store source is closing");
      queue.push_back([promise, fn = std::move(fn)](ps::IStore& s) { promise->set_value(fn(s)); });
    }
    queue_cv.notify_one();
    return future.get();
  }
};

StoreSource::StoreSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StoreSource::~StoreSource() = default;

Result<std::unique_ptr<StoreSource>> StoreSource::open(ps::StoreConfig config, StoreSourceOptions options) {
  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  const int n = std::max(1, options.connections);
  // The first connection opens alone, so only it applies migrations.
  for (int i = 0; i < n; ++i) {
    auto opened = std::make_shared<std::promise<Result<void>>>();
    auto future = opened->get_future();
    Impl* raw = impl.get();
    impl->workers.emplace_back([raw, opened, cfg = impl->config] { raw->run(opened, cfg); });
    auto ok = future.get();
    if (!ok) return fail(ok.error());  // ~Impl joins the workers
    if (i == 0) impl->config.migrate = false;
  }
  auto cursor = impl->call<ps::ChangeSeq>([](ps::IStore& s) { return s.latest_change_seq(); });
  if (!cursor) return fail(cursor.error());
  impl->cursor = *cursor;
  return std::unique_ptr<StoreSource>(new StoreSource(std::move(impl)));
}

std::string StoreSource::name() const { return "store:" + redact_password(impl_->config.url); }

std::uint64_t StoreSource::generation() const { return impl_->generation.load(); }

Result<void> StoreSource::refresh() {
  std::lock_guard lock(impl_->refresh_mutex);
  bool changed = false;
  while (true) {
    const ps::ChangeSeq cursor = impl_->cursor;
    auto page = impl_->call<ps::ChangePage>([cursor](ps::IStore& s) { return s.changes_since(cursor, 500); });
    if (!page) return fail(page.error());
    if (!page->entries.empty()) changed = true;
    impl_->cursor = page->cursor;
    if (!page->more) break;
  }
  if (changed) {
    {
      std::lock_guard cache_lock(impl_->cache_mutex);
      impl_->cache.clear();
    }
    ++impl_->generation;
  }
  return {};
}

Result<BrowsePage> StoreSource::browse(const BrowseQuery& query) {
  if (query.limit <= 0) return fail(ErrorKind::Config, "browse: limit must be positive");
  ps::BrowseRequest req;
  req.filter = to_store_filter(query);
  req.limit = query.limit;
  req.count_total = !query.after;  // the first page carries the total
  if (query.after) {
    auto uuid = parse_uuid(query.after->uuid);
    if (!uuid) return fail(uuid.error());
    req.after = ps::BrowseCursorKey{utc(query.after->timestamp), *uuid};
  }
  auto result = impl_->call<ps::BrowseResult>([req](ps::IStore& s) { return s.browse(req); });
  if (!result) return fail(result.error());
  BrowsePage page;
  page.rows.reserve(result->rows.size());
  for (const auto& row : result->rows) page.rows.push_back(summary_from_row(row));
  if (result->next) page.next = BrowseCursor{seconds(result->next->timestamp), result->next->uuid.str()};
  if (result->total) page.total = static_cast<std::size_t>(*result->total);
  return page;
}

Result<std::vector<std::string>> StoreSource::facet(Facet f, const BrowseQuery& query) {
  const auto facet = to_store_facet(f);
  auto filter = to_store_filter(query);
  return impl_->call<std::vector<std::string>>(
      [facet, filter = std::move(filter)](ps::IStore& s) { return s.facet(facet, filter); });
}

Result<AnalysisPtr> StoreSource::load(const std::string& uuid) {
  const std::uint64_t generation = impl_->generation.load();
  {
    std::lock_guard lock(impl_->cache_mutex);
    if (auto it = impl_->cache.find(uuid); it != impl_->cache.end()) return it->second;
  }
  auto id = parse_uuid(uuid);
  if (!id) return fail(id.error());
  auto parts = impl_->call<StoreAnalysisParts>([id = *id, &uuid](ps::IStore& s) -> Result<StoreAnalysisParts> {
    StoreAnalysisParts p;
    auto detail = s.load_analysis_detail(id);
    if (!detail) return fail(detail.error());
    if (!*detail) return fail(ErrorKind::Config, "no analysis " + uuid + " in the store");
    p.detail = std::move(**detail);
    auto view = s.load_analysis(id);
    if (!view) return fail(view.error());
    if (*view) p.heads = std::move((*view)->payloads);
    auto refs = s.resolve_refs(id, ps::RefPolicy{});
    if (!refs) return fail(refs.error());
    for (const auto& ref : refs->refs) {
      auto payload = s.load_payload(ref.revision);
      if (!payload) return fail(payload.error());
      if (*payload)
        if (const auto* r = std::get_if<ps::RefPayload>(&**payload)) p.refs.push_back(*r);
    }
    return p;
  });
  if (!parts) return fail(parts.error());
  auto analysis = analysis_from_store(*parts);
  if (!analysis) return fail(analysis.error());
  auto ptr = std::make_shared<const Analysis>(std::move(*analysis));
  std::lock_guard lock(impl_->cache_mutex);
  // A refresh while loading may have made this stale: then do not cache it.
  if (impl_->generation.load() == generation) impl_->cache[uuid] = ptr;
  return AnalysisPtr(ptr);
}

Result<RawData> StoreSource::load_raw(const std::string& uuid) {
  auto id = parse_uuid(uuid);
  if (!id) return fail(id.error());
  return impl_->call<RawData>([id = *id, &uuid](ps::IStore& s) -> Result<RawData> {
    auto head = s.head(id, ps::Kind::Signals);
    if (!head) return fail(head.error());
    if (!*head) return fail(ErrorKind::Config, "no analysis " + uuid + " in the store");
    auto payload = s.load_payload(**head);
    if (!payload) return fail(payload.error());
    RawData raw;
    const auto* refs = *payload ? std::get_if<ps::SignalRefs>(&**payload) : nullptr;
    if (!refs) return raw;
    for (const auto& ref : *refs) {
      if (ref.series_kind == "whiff") continue;
      auto blob = s.load_blob(ref.blob_sha);
      if (!blob) return fail(blob.error());
      if (!*blob) continue;  // not uploaded yet (signals_state pending)
      auto series = series_from_blob(ref, **blob);
      if (!series) return fail(series.error());
      raw.series.push_back(std::move(*series));
    }
    return raw;
  });
}

}  // namespace pychron::processing
