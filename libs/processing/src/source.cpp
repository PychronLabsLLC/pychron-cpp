#include "pychron/processing/source.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace pychron::processing {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool in(const std::vector<std::string>& set, const std::string& v) {
  return set.empty() || std::find(set.begin(), set.end(), v) != set.end();
}

bool starts_with_ci(const std::string& s, const std::string& prefix_lower) {
  return lower(s).starts_with(prefix_lower);
}

// Newest first; ties by uuid descending so the order is total.
bool newer(const AnalysisSummary& a, const AnalysisSummary& b) {
  if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
  return a.uuid > b.uuid;
}

double newest_of(const std::vector<AnalysisSummary>& rows) {
  double t = 0;
  for (const auto& r : rows) t = std::max(t, r.timestamp);
  return t;
}

}  // namespace

std::string_view to_string(Facet f) noexcept {
  switch (f) {
    case Facet::AnalysisType:
      return "analysis_type";
    case Facet::MassSpectrometer:
      return "mass_spectrometer";
    case Facet::ExtractDevice:
      return "extract_device";
    case Facet::Project:
      return "project";
    case Facet::PrincipalInvestigator:
      return "principal_investigator";
    case Facet::Sample:
      return "sample";
    case Facet::Material:
      return "material";
    case Facet::Identifier:
      return "identifier";
    case Facet::Irradiation:
      return "irradiation";
    case Facet::Level:
      return "level";
    case Facet::Load:
      return "load";
    case Facet::Repository:
      return "repository";
  }
  return "";
}

std::string facet_value(const AnalysisSummary& s, Facet f) {
  switch (f) {
    case Facet::AnalysisType:
      return s.analysis_type;
    case Facet::MassSpectrometer:
      return s.mass_spectrometer;
    case Facet::ExtractDevice:
      return s.extract_device;
    case Facet::Project:
      return s.project;
    case Facet::PrincipalInvestigator:
      return s.principal_investigator;
    case Facet::Sample:
      return s.sample;
    case Facet::Material:
      return s.material;
    case Facet::Identifier:
      return s.identifier;
    case Facet::Irradiation:
      return s.irradiation;
    case Facet::Level:
      return s.level;
    case Facet::Load:
      return s.load;
    case Facet::Repository:
      return s.repository;
  }
  return {};
}

bool matches(const BrowseQuery& q, const AnalysisSummary& s, double newest, std::optional<Facet> ignore) {
  auto check = [&](Facet f, const std::vector<std::string>& set) { return ignore == f || in(set, facet_value(s, f)); };
  if (!check(Facet::Identifier, q.identifiers) || !check(Facet::Sample, q.samples) ||
      !check(Facet::Project, q.projects) || !check(Facet::PrincipalInvestigator, q.principal_investigators) ||
      !check(Facet::Material, q.materials) || !check(Facet::AnalysisType, q.analysis_types) ||
      !check(Facet::MassSpectrometer, q.mass_spectrometers) || !check(Facet::ExtractDevice, q.extract_devices) ||
      !check(Facet::Load, q.loads) || !check(Facet::Irradiation, q.irradiations) || !check(Facet::Level, q.levels) ||
      !check(Facet::Repository, q.repositories))
    return false;
  if (q.from && s.timestamp < *q.from) return false;
  if (q.to && s.timestamp > *q.to) return false;
  if (q.last_hours && s.timestamp < newest - *q.last_hours * 3600.0) return false;
  if (std::find(q.exclude_tags.begin(), q.exclude_tags.end(), s.tag) != q.exclude_tags.end()) return false;
  if (!q.text.empty()) {
    const std::string t = lower(q.text);
    if (!starts_with_ci(s.runid, t) && !starts_with_ci(s.identifier, t) && !starts_with_ci(s.sample, t)) return false;
  }
  return true;
}

AnalysisSummary summarize(const Analysis& a) {
  AnalysisSummary s;
  s.uuid = a.uuid;
  s.runid = a.runid.empty() ? make_runid(a.identifier, a.aliquot, a.increment) : a.runid;
  s.identifier = a.identifier;
  s.sample = a.sample;
  s.project = a.project;
  s.material = a.material;
  s.principal_investigator = a.principal_investigator;
  s.analysis_type = a.analysis_type;
  s.mass_spectrometer = a.mass_spectrometer;
  s.extract_device = a.extract_device;
  s.load = a.load;
  s.irradiation = a.irradiation;
  s.level = a.level;
  s.repository = a.repository;
  s.tag = a.tag;
  s.aliquot = a.aliquot;
  s.increment = a.increment;
  s.timestamp = a.timestamp;
  s.extract_value = a.extraction.value;
  s.extract_units = a.extraction.units;
  return s;
}

BrowsePage browse_summaries(const std::vector<AnalysisSummary>& rows, const BrowseQuery& q) {
  const double newest = newest_of(rows);
  std::vector<const AnalysisSummary*> hits;
  for (const auto& r : rows)
    if (matches(q, r, newest)) hits.push_back(&r);
  // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order): ordered by newer(), not by address
  std::sort(hits.begin(), hits.end(), [](const auto* a, const auto* b) { return newer(*a, *b); });
  BrowsePage page;
  page.total = hits.size();
  auto it = hits.begin();
  if (q.after) {
    AnalysisSummary cursor;
    cursor.timestamp = q.after->timestamp;
    cursor.uuid = q.after->uuid;
    it = std::find_if(hits.begin(), hits.end(), [&](const auto* h) { return newer(cursor, *h); });
  }
  const std::size_t limit = q.limit > 0 ? static_cast<std::size_t>(q.limit) : hits.size();
  for (; it != hits.end() && page.rows.size() < limit; ++it) page.rows.push_back(**it);
  if (it != hits.end() && !page.rows.empty()) page.next = BrowseCursor{page.rows.back().timestamp, page.rows.back().uuid};
  return page;
}

std::vector<std::string> facet_values(const std::vector<AnalysisSummary>& rows, Facet f, const BrowseQuery& q) {
  const double newest = newest_of(rows);
  BrowseQuery unpaged = q;
  unpaged.after.reset();
  std::set<std::string> values;
  for (const auto& r : rows) {
    if (!matches(unpaged, r, newest, f)) continue;
    auto v = facet_value(r, f);
    if (!v.empty()) values.insert(std::move(v));
  }
  return {values.begin(), values.end()};
}

// ---------------------------------------------------------------- MemorySource

MemorySource::MemorySource(std::vector<AnalysisPtr> analyses, std::map<std::string, RawData> raw)
    : analyses_(std::move(analyses)), raw_(std::move(raw)) {}

void MemorySource::add(AnalysisPtr analysis, std::optional<RawData> raw) {
  std::lock_guard lock(mutex_);
  if (raw) raw_[analysis->uuid] = std::move(*raw);
  auto same = std::find_if(analyses_.begin(), analyses_.end(),
                           [&](const AnalysisPtr& a) { return a->uuid == analysis->uuid; });
  if (same != analyses_.end()) {
    *same = std::move(analysis);
  } else {
    analyses_.push_back(std::move(analysis));
  }
  ++generation_;
}

std::uint64_t MemorySource::generation() const {
  std::lock_guard lock(mutex_);
  return generation_;
}

Result<BrowsePage> MemorySource::browse(const BrowseQuery& query) {
  std::lock_guard lock(mutex_);
  std::vector<AnalysisSummary> rows;
  for (const auto& a : analyses_) rows.push_back(summarize(*a));
  return browse_summaries(rows, query);
}

Result<std::vector<std::string>> MemorySource::facet(Facet f, const BrowseQuery& query) {
  std::lock_guard lock(mutex_);
  std::vector<AnalysisSummary> rows;
  for (const auto& a : analyses_) rows.push_back(summarize(*a));
  return facet_values(rows, f, query);
}

Result<AnalysisPtr> MemorySource::load(const std::string& uuid) {
  std::lock_guard lock(mutex_);
  for (const auto& a : analyses_)
    if (a->uuid == uuid) return a;
  return fail(ErrorKind::Config, "no analysis " + uuid);
}

Result<RawData> MemorySource::load_raw(const std::string& uuid) {
  std::lock_guard lock(mutex_);
  auto it = raw_.find(uuid);
  if (it == raw_.end()) return RawData{};
  return it->second;
}

}  // namespace pychron::processing
