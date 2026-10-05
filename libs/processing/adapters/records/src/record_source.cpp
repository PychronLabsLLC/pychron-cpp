#include "pychron/processing/record_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "pychron/experiment/record/serialize.hpp"

namespace pychron::processing {

namespace fs = std::filesystem;
namespace rec = pychron::experiment::record;

namespace {

// Howard Hinnant's days_from_civil.
long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

Result<std::string> read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Value v(double value, double error) { return Value{value, error}; }

}  // namespace

std::optional<double> parse_utc(std::string_view t) {
  // YYYY-MM-DDTHH:MM:SS[.fff]Z (a space instead of T is accepted)
  if (t.size() < 19) return std::nullopt;
  auto num = [&](std::size_t pos, std::size_t len) -> std::optional<int> {
    int out = 0;
    for (std::size_t i = pos; i < pos + len; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(t[i]))) return std::nullopt;
      out = out * 10 + (t[i] - '0');
    }
    return out;
  };
  const auto y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), s = num(17, 2);
  if (!y || !mo || !d || !h || !mi || !s || t[4] != '-' || t[7] != '-' || (t[10] != 'T' && t[10] != ' ') ||
      t[13] != ':' || t[16] != ':')
    return std::nullopt;
  if (*mo < 1 || *mo > 12 || *d < 1 || *d > 31 || *h > 23 || *mi > 59 || *s > 60) return std::nullopt;
  double frac = 0;
  std::size_t i = 19;
  if (i < t.size() && t[i] == '.') {
    double scale = 0.1;
    for (++i; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, scale /= 10) frac += (t[i] - '0') * scale;
  }
  if (i < t.size() && t[i] != 'Z') return std::nullopt;
  const long long days = days_from_civil(*y, static_cast<unsigned>(*mo), static_cast<unsigned>(*d));
  return static_cast<double>(days * 86400LL + *h * 3600LL + *mi * 60LL + *s) + frac;
}

int increment_from_step(std::string_view step) {
  if (step.empty()) return -1;
  int n = 0;
  for (char c : step) {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u < 'A' || u > 'Z') return -1;
    n = n * 26 + (u - 'A' + 1);
  }
  return n - 1;
}

Analysis analysis_from_record(const rec::AnalysisRecord& r) {
  Analysis a;
  a.uuid = r.identity.uuid;
  a.identifier = r.identity.identifier;
  a.aliquot = r.identity.aliquot;
  a.increment = increment_from_step(r.identity.step);
  a.runid = make_runid(a.identifier, a.aliquot, a.increment);
  a.analysis_type = r.identity.analysis_type;
  a.timestamp = parse_utc(r.identity.timestamp).value_or(0.0);
  a.sample = r.sample.sample;
  a.project = r.sample.project;
  a.material = r.sample.material;
  a.principal_investigator = r.sample.pi;
  a.irradiation = r.sample.irradiation;
  a.level = r.sample.level;
  a.position = r.sample.position;
  a.comment = r.sample.note;
  a.mass_spectrometer = r.instrument.mass_spectrometer;
  a.extract_device = r.instrument.extract_device;
  a.analyst = r.instrument.analyst;

  const auto& act = r.extraction.actuals;
  const auto& spec = r.extraction.spec;
  a.extraction.value = act.value != 0 ? act.value : spec.value;
  a.extraction.duration = act.duration != 0 ? act.duration : spec.duration;
  a.extraction.cleanup = act.cleanup != 0 ? act.cleanup : spec.cleanup;
  if (act.beam_diameter != 0) a.extraction.beam_diameter = act.beam_diameter;
  a.extraction.units = spec.units;
  a.extraction.pattern = act.pattern.empty() ? spec.pattern : act.pattern;
  a.extraction.positions = act.positions.empty() ? spec.positions : act.positions;
  a.extraction.cryo_temperature = spec.cryo_temperature;
  a.extraction.cryo_measured = act.cryo_measured;

  a.gains = r.spectrometer.gains;
  a.deflections = r.spectrometer.deflections;
  a.source = r.spectrometer.source_params;

  // Detector of each isotope from its signal series.
  std::map<std::string, std::string> detector_of;
  for (const auto& s : r.data.series)
    if (s.kind == "signal" && !s.iso.empty() && !detector_of.count(s.iso)) detector_of[s.iso] = s.det;

  for (const auto& [iso, ir] : r.results.intercepts) {
    IsotopeData d;
    d.key = iso;
    d.isotope = iso.substr(iso.find(':') == std::string::npos ? 0 : iso.find(':') + 1);
    d.detector = detector_of.count(iso) ? detector_of[iso] : "";
    d.intercept = v(ir.intercept.value, ir.intercept.error);
    d.fit = ir.fit;
    d.n = static_cast<int>(ir.intercept.n_used);
    if (auto b = r.results.baselines.find(d.detector); b != r.results.baselines.end()) {
      d.baseline = v(b->second.value, b->second.error);
      d.baseline_fit = b->second.fit;
    }
    if (auto ic = r.results.icfactors.find(d.detector); ic != r.results.icfactors.end()) d.ic_factor = v(ic->second, 0.0);
    d.blank_source = r.results.blanks_ref;
    a.isotopes.push_back(std::move(d));
  }
  // Natural isotope order: Ar40 first.
  std::stable_sort(a.isotopes.begin(), a.isotopes.end(), [](const IsotopeData& x, const IsotopeData& y) {
    return x.isotope > y.isotope;
  });
  return a;
}

RawData raw_from_record(const rec::AnalysisRecord& r) {
  RawData raw;
  for (const auto& s : r.data.series) {
    RawSeries out;
    if (s.kind == "baseline") {
      out.kind = SeriesKind::Baseline;
      out.key = s.det;
    } else if (s.kind == "sniff") {
      out.kind = SeriesKind::Sniff;
      out.key = s.iso;
    } else if (s.kind == "signal") {
      out.kind = SeriesKind::Signal;
      out.key = s.iso;
    } else {
      continue;
    }
    out.detector = s.det;
    out.t.assign(s.trace.t.begin(), s.trace.t.end());
    out.v.assign(s.trace.v.begin(), s.trace.v.end());
    raw.series.push_back(std::move(out));
  }
  return raw;
}

// ---------------------------------------------------------------- source

RecordDirectorySource::RecordDirectorySource(fs::path root) : root_(std::move(root)) {}

std::string RecordDirectorySource::name() const { return "records:" + root_.string(); }

std::vector<std::string> RecordDirectorySource::problems() const {
  std::lock_guard lock(mutex_);
  return problems_;
}

Result<void> RecordDirectorySource::load_references() {
  const fs::path p = root_ / "references.toml";
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) {
    flux_.clear();
    production_.clear();
    chronology_.clear();
    references_mtime_.reset();
    return {};
  }
  const auto mtime = fs::last_write_time(p, ec);
  if (references_mtime_ && *references_mtime_ == mtime) return {};
  auto text = read_text(p);
  if (!text) return fail(text.error());
  auto parsed = toml::parse(*text, p.string());
  if (!parsed) return fail(ErrorKind::Config, p.string() + ": " + std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();
  flux_.clear();
  production_.clear();
  chronology_.clear();
  if (const auto* f = root.get("flux") ? root.get("flux")->as_table() : nullptr) {
    for (const auto& [id, node] : *f) {
      const auto* t = node.as_table();
      if (!t) continue;
      reduction::Flux fl;
      fl.j.value = t->get("j") ? t->get("j")->value<double>().value_or(0.0) : 0.0;
      fl.j.error = t->get("j_err") ? t->get("j_err")->value<double>().value_or(0.0) : 0.0;
      fl.position_jerr = t->get("position_jerr") ? t->get("position_jerr")->value<double>().value_or(0.0) : 0.0;
      flux_[std::string(id.str())] = fl;
    }
  }
  if (const auto* pr = root.get("production") ? root.get("production")->as_table() : nullptr) {
    for (const auto& [irr, node] : *pr) {
      const auto* t = node.as_table();
      if (!t) continue;
      std::map<std::string, reduction::Measured, std::less<>> rows;
      for (const auto& [k, val] : *t) {
        const auto* arr = val.as_array();
        if (!arr || arr->size() != 2) continue;
        rows[std::string(k.str())] = {(*arr)[0].value<double>().value_or(0.0), (*arr)[1].value<double>().value_or(0.0)};
      }
      auto ratios = reduction::production_from_rows(rows);
      if (!ratios) return fail(ErrorKind::Config, p.string() + ": production " + std::string(irr.str()) + ": " +
                                                      ratios.error().what);
      production_[std::string(irr.str())] = *ratios;
    }
  }
  if (const auto* ch = root.get("chronology") ? root.get("chronology")->as_table() : nullptr) {
    for (const auto& [irr, node] : *ch) {
      const auto* arr = node.as_array();
      if (!arr) continue;
      std::vector<reduction::Dose> doses;
      for (const auto& e : *arr) {
        const auto* t = e.as_table();
        if (!t) continue;
        reduction::Dose d;
        d.power = t->get("power") ? t->get("power")->value<double>().value_or(1.0) : 1.0;
        const auto start = parse_utc(t->get("start") ? t->get("start")->value<std::string>().value_or("") : "");
        const auto end = parse_utc(t->get("end") ? t->get("end")->value<std::string>().value_or("") : "");
        if (!start || !end) return fail(ErrorKind::Config, p.string() + ": chronology " + std::string(irr.str()) + ": bad date");
        d.start_utc_s = static_cast<std::int64_t>(*start);
        d.end_utc_s = static_cast<std::int64_t>(*end);
        doses.push_back(d);
      }
      chronology_[std::string(irr.str())] = doses;
    }
  }
  references_mtime_ = mtime;
  return {};
}

void RecordDirectorySource::apply_references(Analysis& a) const {
  if (auto f = flux_.find(a.identifier); f != flux_.end()) a.context.flux = f->second;
  if (auto p = production_.find(a.irradiation); p != production_.end()) a.context.production = p->second;
  if (auto c = chronology_.find(a.irradiation); c != chronology_.end()) a.context.chronology = c->second;
}

Result<void> RecordDirectorySource::refresh() {
  std::lock_guard lock(mutex_);
  bool changed = !scanned_;
  scanned_ = true;
  const auto before = references_mtime_;
  if (auto ok = load_references(); !ok) return ok;
  const bool refs_changed = before != references_mtime_;
  changed = changed || refs_changed;

  std::error_code ec;
  if (!fs::is_directory(root_, ec)) {
    if (!by_path_.empty()) changed = true;
    by_path_.clear();
    path_of_uuid_.clear();
    if (changed) ++generation_;
    return {};
  }
  std::map<std::string, Entry> next;
  problems_.clear();
  for (const auto& dir : fs::directory_iterator(root_, ec)) {
    if (!dir.is_directory() || dir.path().filename() == "artifacts") continue;
    for (const auto& f : fs::directory_iterator(dir.path(), ec)) {
      const fs::path& p = f.path();
      const std::string name = p.filename().string();
      if (!f.is_regular_file() || p.extension() != ".json" || name.ends_with(".extraction.json")) continue;
      const auto mtime = fs::last_write_time(p, ec);
      const auto size = fs::file_size(p, ec);
      auto old = by_path_.find(p.string());
      if (old != by_path_.end() && old->second.mtime == mtime && old->second.size == size && !refs_changed) {
        next[p.string()] = std::move(old->second);
        continue;
      }
      auto text = read_text(p);
      if (!text) {
        problems_.push_back(p.string() + ": " + text.error().what);
        continue;
      }
      auto r = rec::from_json(*text);
      if (!r) {
        problems_.push_back(p.string() + ": " + r.error().what);
        continue;
      }
      Analysis a = analysis_from_record(*r);
      if (a.uuid.empty()) {
        problems_.push_back(p.string() + ": record has no uuid");
        continue;
      }
      apply_references(a);
      Entry e;
      e.path = p;
      e.mtime = mtime;
      e.size = size;
      e.summary = summarize(a);
      e.analysis = std::make_shared<const Analysis>(std::move(a));
      next[p.string()] = std::move(e);
      changed = true;
    }
  }
  if (next.size() != by_path_.size()) changed = true;
  by_path_ = std::move(next);
  path_of_uuid_.clear();
  for (const auto& [path, e] : by_path_) path_of_uuid_[e.analysis->uuid] = path;
  if (changed) ++generation_;
  return {};
}

Result<BrowsePage> RecordDirectorySource::browse(const BrowseQuery& query) {
  if (!scanned_)
    if (auto ok = refresh(); !ok) return fail(ok.error());
  std::lock_guard lock(mutex_);
  std::vector<AnalysisSummary> rows;
  rows.reserve(by_path_.size());
  for (const auto& [_, e] : by_path_) rows.push_back(e.summary);
  return browse_summaries(rows, query);
}

Result<std::vector<std::string>> RecordDirectorySource::facet(Facet f, const BrowseQuery& query) {
  if (!scanned_)
    if (auto ok = refresh(); !ok) return fail(ok.error());
  std::lock_guard lock(mutex_);
  std::vector<AnalysisSummary> rows;
  for (const auto& [_, e] : by_path_) rows.push_back(e.summary);
  return facet_values(rows, f, query);
}

Result<AnalysisPtr> RecordDirectorySource::load(const std::string& uuid) {
  if (!scanned_)
    if (auto ok = refresh(); !ok) return fail(ok.error());
  std::lock_guard lock(mutex_);
  auto p = path_of_uuid_.find(uuid);
  if (p == path_of_uuid_.end()) return fail(ErrorKind::Config, "no analysis " + uuid + " in " + root_.string());
  return by_path_.at(p->second).analysis;
}

Result<RawData> RecordDirectorySource::load_raw(const std::string& uuid) {
  fs::path path;
  {
    if (!scanned_)
      if (auto ok = refresh(); !ok) return fail(ok.error());
    std::lock_guard lock(mutex_);
    auto p = path_of_uuid_.find(uuid);
    if (p == path_of_uuid_.end()) return fail(ErrorKind::Config, "no analysis " + uuid + " in " + root_.string());
    path = by_path_.at(p->second).path;
  }
  auto text = read_text(path);
  if (!text) return fail(text.error());
  auto r = rec::from_json(*text);
  if (!r) return fail(r.error());
  return raw_from_record(*r);
}

}  // namespace pychron::processing
