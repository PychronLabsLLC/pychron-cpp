// Building a release directory from the store (data publishing design,
// sections 5 and 6.1).

#include "pychron/publish/build.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "pychron/core/env.hpp"
#include "pychron/core/process.hpp"
#include "pychron/core/user_file.hpp"
#include "pychron/processing/report.hpp"
#include "pychron/processing/source.hpp"
#include "pychron/processing/units.hpp"
#include "pychron/publish/parquet.hpp"
#include "pychron/publish/records.hpp"
#include "pychron/reduction/arar_types.hpp"

namespace pychron::publish {

namespace fs = std::filesystem;
namespace pp = processing;
namespace ps = persistence;
using Json = nlohmann::ordered_json;

// ---------------------------------------------------------------- options

std::string release_options_json(const ReleaseOptions& o) {
  Json j;
  j["sigma"] = o.sigma;
  j["constants"] = o.constants;
  j["decay_error"] = o.decay_error;
  j["plateau"] = o.plateau;
  j["plateau_steps"] = o.plateau_steps;
  j["plateau_gas"] = o.plateau_gas;
  j["group_by"] = o.group_by;
  j["laboratory"] = o.laboratory;
  j["notes"] = o.notes;
  return j.dump();
}

Result<ReleaseOptions> parse_release_options(std::string_view json) {
  const Json j = Json::parse(json, nullptr, false);
  if (!j.is_object()) return fail(ErrorKind::Config, "release options are not a JSON object");
  ReleaseOptions o;
  o.sigma = j.value("sigma", o.sigma);
  if (o.sigma != 1 && o.sigma != 2) return fail(ErrorKind::Config, "release options: sigma is 1 or 2");
  o.constants = j.value("constants", o.constants);
  bool known = false;
  for (const auto p : {reduction::ConstantsPreset::Default, reduction::ConstantsPreset::Legacy,
                       reduction::ConstantsPreset::LegacyPreferences})
    if (reduction::to_string(p) == o.constants) known = true;
  if (!known) return fail(ErrorKind::Config, "release options: unknown constants preset '" + o.constants + "'");
  o.decay_error = j.value("decay_error", o.decay_error);
  o.plateau = j.value("plateau", o.plateau);
  if (o.plateau != "fleck" && o.plateau != "mahon") return fail(ErrorKind::Config, "release options: plateau is fleck or mahon");
  o.plateau_steps = j.value("plateau_steps", o.plateau_steps);
  o.plateau_gas = j.value("plateau_gas", o.plateau_gas);
  o.group_by = j.value("group_by", o.group_by);
  for (const char* k : {"auto", "aliquot", "identifier", "sample", "none"})
    if (o.group_by == k) known = true;
  if (!known) return fail(ErrorKind::Config, "release options: unknown group_by '" + o.group_by + "'");
  o.laboratory = j.value("laboratory", o.laboratory);
  if (j.contains("notes") && j["notes"].is_array())
    for (const auto& n : j["notes"]) o.notes.push_back(n.get<std::string>());
  return o;
}

fs::path release_dir(const fs::path& out, const ps::ReleaseRow& release) {
  return out / (file_safe(release.name) + "-v" + std::to_string(release.version));
}

namespace {

constexpr std::string_view kReleaseFormatDoc = "docs/release_format.md";

struct Progress {
  IProgress* p;
  void operator()(const char* stage) const {
    if (p) p->stage(stage);
  }
};

// The licence text shipped with the program: beside it (share/pychron or the
// bundle's Resources), else the source tree it was built from.
std::optional<fs::path> licence_file(std::string_view spdx) {
  const std::string name = std::string(spdx) + ".txt";
  std::vector<fs::path> candidates;
  if (const auto named = env_var("PYCHRON_PUBLISH_LICENCES"); named && !named->empty())
    candidates.emplace_back(fs::path(*named) / name);
  const fs::path program = executable_dir();
  if (!program.empty()) {
    candidates.push_back(program / ".." / "share" / "pychron" / "publish" / "licences" / name);
    candidates.push_back(program / ".." / "Resources" / "publish" / "licences" / name);
  }
#ifdef PYCHRON_PUBLISH_SOURCE_LICENCE_DIR
  candidates.push_back(fs::path(PYCHRON_PUBLISH_SOURCE_LICENCE_DIR) / name);
#endif
  std::error_code ec;
  for (const auto& c : candidates)
    if (fs::is_regular_file(c, ec)) return c;
  return std::nullopt;
}

Result<std::string> slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string yaml_quote(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out + "\"";
}

std::string citation_cff(const Manifest& m) {
  std::string out;
  out += "cff-version: 1.2.0\n";
  out += "message: \"If you use this data, please cite it as below.\"\n";
  out += "type: dataset\n";
  out += "title: " + yaml_quote(m.release.title) + "\n";
  out += "version: \"" + std::to_string(m.release.version) + "\"\n";
  out += "date-released: " + (m.release.published ? m.release.published->iso() : m.release.created.iso()).substr(0, 10) + "\n";
  out += "license: " + m.release.licence + "\n";
  if (m.release.doi) out += "doi: " + *m.release.doi + "\n";
  if (m.release.description) out += "abstract: " + yaml_quote(*m.release.description) + "\n";
  out += "authors:\n";
  for (const auto& a : m.release.authors) {
    const auto space = a.name.rfind(' ');
    if (space == std::string::npos || a.name.find(',') != std::string::npos) {
      out += "  - name: " + yaml_quote(a.name) + "\n";
    } else {
      out += "  - family-names: " + yaml_quote(a.name.substr(space + 1)) + "\n";
      out += "    given-names: " + yaml_quote(a.name.substr(0, space)) + "\n";
    }
    if (a.orcid) out += "    orcid: https://orcid.org/" + *a.orcid + "\n";
    if (a.affiliation) out += "    affiliation: " + yaml_quote(*a.affiliation) + "\n";
  }
  out += "identifiers:\n  - type: other\n    value: " + yaml_quote("pychron-release:" + m.release.uuid.str()) + "\n";
  return out;
}

std::string readme_md(const Manifest& m, const ReleaseTables& t) {
  std::string out = "# " + m.release.title + "\n\n";
  out += "Release `" + m.release.name + "` version " + std::to_string(m.release.version) + " of " +
         (m.release.published ? *m.release.published : m.release.created).iso().substr(0, 10) + ", built by " +
         m.build.software + ": " +
         std::to_string(t.analyses.rows.size()) + " analyses of " + std::to_string(t.samples.rows.size()) +
         " samples, with " + std::to_string(t.interpreted_ages.rows.size()) + " interpreted ages. ";
  out += "Licence " + m.release.licence + " (LICENSE.txt). ";
  if (m.release.doi) out += "Cite it as https://doi.org/" + *m.release.doi + " (CITATION.cff). ";
  else out += "How to cite it is in CITATION.cff. ";
  out += "The tables are `data/*.parquet` (GeoParquet 1.1) and, as text, `tables/release.csv`, one report per "
         "sample under `tables/`; `catalog.json` and `records/` are an OGC API - Records catalogue; "
         "`manifest.json` lists every file with its hash and what the release pins. ";
  out += "Serve the directory over HTTP (`python3 -m http.server`) to read it with a viewer; the format is "
         "documented in pychron-cpp's `" + std::string(kReleaseFormatDoc) + "`.\n";
  return out;
}

std::string constants_json(const pp::Report& report) {
  Json j = Json::object();
  const auto& t = report.constants;
  const auto col = [&](const std::string& name) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < t.columns.size(); ++i)
      if (t.columns[i] == name || t.columns[i].starts_with(name + " ±(")) return i;
    return std::nullopt;
  };
  const auto item = col("item"), value = col("value"), units = col("units");
  std::optional<std::size_t> error;
  for (std::size_t i = 0; i < t.columns.size(); ++i)
    if (t.columns[i].starts_with("value ±(")) error = i;
  const auto cell = [](const pp::ReportCell& c) -> Json {
    if (const auto* d = std::get_if<double>(&c)) return *d;
    if (const auto* s = std::get_if<std::string>(&c)) return *s;
    return nullptr;
  };
  for (const auto& row : t.rows) {
    if (!item) continue;
    const auto* name = std::get_if<std::string>(&row[*item]);
    if (!name) continue;
    Json entry;
    entry["value"] = value ? cell(row[*value]) : Json(nullptr);
    entry["error"] = error ? cell(row[*error]) : Json(nullptr);
    entry["units"] = units ? cell(row[*units]) : Json(nullptr);
    j[*name] = entry;
  }
  return j.dump(2) + "\n";
}

bool any_step(const pp::Dataset& d) {
  for (const auto& it : d.items())
    if (it.analysis->analysis->increment >= 0) return true;
  return false;
}

// The dataset's items of one sample (by the analyses that are its), as a dataset of its own.
pp::Dataset dataset_of_sample(const pp::Dataset& d, const std::set<std::string>& analyses) {
  std::vector<pp::DatasetItem> items;
  for (const auto& it : d.items())
    if (analyses.contains(it.analysis->analysis->uuid)) items.push_back(it);
  pp::Dataset out(std::move(items));
  out.group_names = d.group_names;
  out.graph_names = d.graph_names;
  out.reduction_tag = d.reduction_tag;
  return out;
}

class Writer {
 public:
  Writer(fs::path root, Manifest& manifest) : root_(std::move(root)), manifest_(manifest) {}

  Result<void> write(const std::string& rel, std::string_view bytes) {
    const fs::path path = root_ / rel;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
      std::ofstream out(path, std::ios::binary);
      if (!out) return fail(ErrorKind::Io, "cannot write " + path.string());
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (!out) return fail(ErrorKind::Io, "cannot write " + path.string());
    }
    mark_as_user_file(path);
    manifest_.files.emplace_back(rel, bytes.size(), sha256(bytes), std::string(media_type_of(rel)));
    return {};
  }

  // A file written by someone else (the Parquet writer): hash it from disk.
  Result<void> record(const std::string& rel) {
    const fs::path path = root_ / rel;
    auto bytes = slurp(path);
    if (!bytes) return fail(bytes.error());
    mark_as_user_file(path);
    manifest_.files.emplace_back(rel, bytes->size(), sha256(std::string_view(*bytes)), std::string(media_type_of(rel)));
    return {};
  }

 private:
  fs::path root_;
  Manifest& manifest_;
};

}  // namespace

Result<BuildResult> build_release(ps::IStore& store, pp::StoreSource& source, Uuid release_uuid,
                                  const BuildOptions& options, IProgress* progress) {
  const Progress stage{progress};
  stage("select");
  auto row = store.release(release_uuid);
  if (!row) return fail(row.error());
  if (!*row) return fail(ErrorKind::Config, "no release " + release_uuid.str());
  const ps::ReleaseRow& release = **row;
  auto opts = parse_release_options(release.options_json);
  if (!opts) return fail(opts.error());

  const fs::path dir = release_dir(options.out, release);
  std::error_code ec;
  if (fs::exists(dir, ec) && !fs::is_empty(dir, ec))
    return fail(ErrorKind::Config, dir.string() + " exists and is not empty");
  if (!fs::exists(dir, ec) && !fs::create_directories(dir, ec))
    return fail(ErrorKind::Io, "cannot create " + dir.string() + ": " + ec.message());

  // The members and what the bookmark pins.
  auto pins = store.bookmark_heads(release.bookmark);
  if (!pins) return fail(pins.error());
  std::set<Uuid> member_set;
  for (const auto& p : *pins)
    if (p.kind != ps::Kind::RefValue && p.kind != ps::Kind::InterpretedAge) member_set.insert(p.subject);
  std::vector<Uuid> members(member_set.begin(), member_set.end());
  auto sample_uuids = store.release_samples(release_uuid);
  if (!sample_uuids) return fail(sample_uuids.error());
  auto ia_pins = store.release_interpreted_ages(release_uuid);
  if (!ia_pins) return fail(ia_pins.error());

  // Embargoes: dropped from a public build, kept and declared in a private one.
  BuildResult result;
  result.dir = dir;
  Manifest& m = result.manifest;
  const UtcTime now = UtcTime::now();
  std::set<Uuid> hidden_samples, hidden_analyses;
  {
    auto es = store.embargoed_samples(*sample_uuids, now);
    if (!es) return fail(es.error());
    auto ea = store.embargoed_analyses(members, now);
    if (!ea) return fail(ea.error());
    if (!options.private_build) {
      hidden_samples.insert(es->begin(), es->end());
      hidden_analyses.insert(ea->begin(), ea->end());
    }
    m.build.omitted_samples = options.private_build ? 0 : es->size();
    m.build.omitted_analyses = options.private_build ? 0 : ea->size();
    m.build.private_build = options.private_build;
  }

  // The samples' catalog rows.
  stage("load");
  TableInputs inputs;
  inputs.release = release;
  for (const auto& s : *sample_uuids) {
    if (hidden_samples.contains(s)) continue;
    ps::SampleQuery q;
    q.uuid = s;
    q.counts = false;
    q.limit = 1;
    auto rows = store.samples(q);
    if (!rows) return fail(rows.error());
    if (!rows->empty()) inputs.samples.push_back(rows->front());
  }
  std::map<Uuid, std::string> sample_name_of;
  for (const auto& s : inputs.samples) sample_name_of[s.uuid] = s.name;

  // The members at their pins, as analyses.
  std::vector<pp::AnalysisPtr> loaded;
  std::vector<std::string> uuids;
  for (const auto& a : members) {
    if (hidden_analyses.contains(a)) continue;
    auto detail = store.load_analysis_detail(a);
    if (!detail) return fail(detail.error());
    if (*detail && (*detail)->row.sample_uuid && hidden_samples.contains(*(*detail)->row.sample_uuid)) continue;
    auto analysis = source.load_at(a.str(), release.bookmark.str());
    if (!analysis) return fail(analysis.error());
    loaded.push_back(*analysis);
    uuids.push_back(a.str());
    AnalysisPins p;
    p.analysis = a;
    if (*detail) p.sample = (*detail)->row.sample_uuid;
    auto view = store.load_analysis_at(a, release.bookmark);
    if (!view) return fail(view.error());
    if (*view) {
      for (const auto& h : (*view)->heads) p.heads[h.kind] = h.revision;
      if (auto it = (*view)->payloads.find(ps::Kind::Signals); it != (*view)->payloads.end())
        if (const auto* s = std::get_if<ps::SignalRefs>(&it->second)) p.signals = *s;
      if (auto it = (*view)->payloads.find(ps::Kind::Intercepts); it != (*view)->payloads.end())
        if (const auto* s = std::get_if<ps::Intercepts>(&it->second)) p.intercepts = *s;
      p.record_sha256 = (*view)->summary.record_sha256;
    }
    auto refs = store.resolve_refs_at(a, release.bookmark);
    if (!refs) return fail(refs.error());
    p.refs = refs->refs;
    inputs.pins.push_back(std::move(p));
  }
  if (loaded.empty()) return fail(ErrorKind::Config, "nothing to build: every member of the release is under embargo");

  // Interpreted ages at their pinned revisions: those of a kept sample whose
  // members are all kept (spec R4: an embargoed analysis leaks through no age).
  auto ia_rows = store.interpreted_ages();
  if (!ia_rows) return fail(ia_rows.error());
  std::set<Uuid> kept_members;
  for (const auto& p : inputs.pins) kept_members.insert(p.analysis);
  for (const auto& [ia, revision] : *ia_pins) {
    auto payload = store.load_payload(revision);
    if (!payload) return fail(payload.error());
    const auto* value = *payload ? std::get_if<ps::InterpretedAgeValue>(&**payload) : nullptr;
    if (!value) continue;
    InterpretedAgeRecord rec;
    rec.ia = ia;
    rec.revision = revision;
    rec.value = *value;
    // The sample and identifier: those of its first member.
    for (const auto& member : value->members) {
      for (const auto& p : inputs.pins)
        if (p.analysis == member.analysis && p.sample) rec.sample = *p.sample;
      for (const auto& l : loaded)
        if (l->uuid == member.analysis.str()) rec.identifier = l->identifier;
      if (!rec.sample.is_nil()) break;
    }
    if (rec.sample.is_nil() || hidden_samples.contains(rec.sample)) continue;
    const bool every_member_kept = std::all_of(value->members.begin(), value->members.end(),
                                               [&](const ps::InterpretedAgeMember& member) { return kept_members.contains(member.analysis); });
    if (!every_member_kept) continue;
    for (const auto& r : *ia_rows)
      if (r.uuid == ia) rec.name = r.name;
    inputs.interpreted_ages.push_back(std::move(rec));
  }

  // Reduce and group as `elctl export` does, over the loaded analyses.
  stage("reduce");
  pp::MemorySource memory(loaded);
  const auto& reg = pp::UnitRegistry::builtin();
  pp::Pipeline pipeline;
  pipeline.name = "release";
  auto& select = pipeline.add(reg, "select", "select");
  std::optional<Error> bad;
  const auto set = [&](pp::NodeSpec& node, std::string_view key, pp::OptionValue value) {
    auto ok = node.options.set(key, std::move(value));
    if (!ok && !bad) bad = ok.error();
  };
  set(select, "uuids", uuids);
  set(select, "remove_tags", std::vector<std::string>{});
  set(select, "limit", static_cast<std::int64_t>(uuids.size()));
  auto& reduce = pipeline.add(reg, "reduce", "reduce", {"select"});
  set(reduce, "constants", opts->constants);
  set(reduce, "include_decay_error", opts->decay_error);
  if (bad) return fail(*bad);
  pp::Runner runner(reg, &memory);
  auto reduced = runner.run(pipeline, "reduce");
  if (!reduced) return fail(reduced.error());
  const auto& before_grouping = std::get<pp::DatasetPtr>(reduced->at(0));
  std::string key = opts->group_by;
  if (key == "auto") key = any_step(*before_grouping) ? "aliquot" : "identifier";
  auto& group = pipeline.add(reg, "group", "group", {"reduce"});
  if (auto ok = group.options.set("key", key); !ok) return fail(ok.error());
  auto grouped = runner.run(pipeline, "group");
  if (!grouped) return fail(grouped.error());
  const pp::DatasetPtr dataset = std::get<pp::DatasetPtr>(grouped->at(0));
  for (const auto& d : runner.diagnostics()) result.warnings.push_back(d);

  pp::ReportOptions report_options;
  report_options.nsigma = opts->sigma;
  report_options.laboratory = opts->laboratory;
  report_options.notes = opts->notes;
  report_options.software = release.software_version;
  report_options.generated = static_cast<double>(release.created.micros) / 1e6;
  report_options.plateau.method = opts->plateau == "mahon" ? reduction::PlateauMethod::Mahon : reduction::PlateauMethod::Fleck;
  report_options.plateau.nsteps = opts->plateau_steps;
  report_options.plateau.gas_fraction = opts->plateau_gas;
  report_options.notes.push_back("release " + release.name + " v" + std::to_string(release.version));
  if (release.doi) report_options.notes.push_back("doi " + *release.doi);
  report_options.notes.push_back("licence " + release.licence);
  const pp::Report report = pp::make_report(*dataset, report_options);
  for (const auto& w : report.warnings) result.warnings.push_back(w);

  // The row tables.
  stage("tables");
  inputs.dataset = *dataset;
  inputs.report = report;
  const ReleaseTables tables = build_tables(inputs);

  // The manifest's identity, before any file is listed.
  m.release.uuid = release.uuid;
  m.release.name = release.name;
  m.release.version = release.version;
  m.release.title = release.title;
  m.release.description = release.description;
  m.release.state = std::string(ps::to_string(release.state));
  m.release.visibility = std::string(ps::to_string(release.visibility));
  m.release.licence = release.licence;
  if (auto authors = parse_authors(release.authors_json)) m.release.authors = *authors;
  m.release.publisher = release.publisher;
  m.release.doi = release.doi;
  m.release.supersedes = release.supersedes;
  if (release.supersedes)
    if (auto prior = store.release(*release.supersedes); prior && *prior) m.release.supersedes_doi = (*prior)->doi;
  m.release.created = release.created;
  m.release.published = release.published;
  m.release.embargo_until = release.embargo_until;
  m.build.built = now;
  m.build.software = release.software_version;
  m.build.reduction_version = std::string(reduction::kReductionVersion);
  m.build.constants_preset = opts->constants;
  m.build.options_json = release_options_json(*opts);
  if (const auto colon = options.store_url.find(':'); colon != std::string::npos)
    m.build.store_kind = options.store_url.substr(0, colon);
  if (!options.store_url.empty()) m.build.store_id = to_hex(sha256(std::string_view(options.store_url))).substr(0, 16);
  if (auto status = store.schema_status(); status && !status->empty()) m.build.schema_version = status->back().version;
  m.pins.bookmark = release.bookmark;
  for (const auto& p : inputs.pins) {
    ManifestAnalysisPins mp;
    mp.uuid = p.analysis;
    for (const auto& l : loaded)
      if (l->uuid == p.analysis.str()) mp.runid = l->runid;
    if (p.record_sha256) mp.record_sha256 = to_hex(*p.record_sha256);
    for (const auto& [kind, revision] : p.heads) mp.heads[std::string(ps::to_string(kind))] = revision.str();
    for (const auto& r : p.refs) mp.refs[r.ref_object.str()] = r.revision.str();
    m.pins.analyses.push_back(std::move(mp));
  }
  for (const auto& ia : inputs.interpreted_ages) m.pins.interpreted_ages.emplace_back(ia.ia, ia.revision);
  for (const auto& s : inputs.samples) m.pins.samples.push_back(s.uuid);
  std::sort(m.pins.samples.begin(), m.pins.samples.end());
  for (const Table* t : {&tables.samples, &tables.positions, &tables.analyses, &tables.isotopes, &tables.interpreted_ages}) {
    ManifestTable mt;
    mt.path = "data/" + t->name + ".parquet";
    mt.rows = t->rows.size();
    mt.rows_sha256 = rows_sha256(*t);
    for (const auto& c : t->columns) mt.columns.push_back(c.name);
    m.tables.push_back(std::move(mt));
  }

  // The files.
  stage("files");
  Writer writer(dir, m);
  if (auto r = writer.write("tables/release.csv", pp::report_csv(report)); !r) return fail(r.error());
  if (auto r = writer.write("tables/release.json", pp::report_json(report)); !r) return fail(r.error());
  for (std::size_t r = 0; r < tables.samples.rows.size(); ++r) {
    const auto uuid_col = tables.samples.column("sample_uuid");
    const auto sample_uuid = Uuid::parse(std::get<std::string>(tables.samples.rows[r][*uuid_col]));
    std::set<std::string> of_sample;
    for (const auto& p : inputs.pins)
      if (sample_uuid && p.sample == *sample_uuid) of_sample.insert(p.analysis.str());
    const pp::Dataset sub = dataset_of_sample(*dataset, of_sample);
    if (sub.empty()) continue;
    const pp::Report sample_report = pp::make_report(sub, report_options);
    const std::string sdir = "tables/" + sample_dir_name(tables.samples, r) + "/";
    if (auto w = writer.write(sdir + "report.csv", pp::report_csv(sample_report)); !w) return fail(w.error());
    if (auto w = writer.write(sdir + "report.json", pp::report_json(sample_report)); !w) return fail(w.error());
  }
  if (auto r = writer.write("constants.json", constants_json(report)); !r) return fail(r.error());
  {
    std::string licence;
    if (const auto file = licence_file(release.licence)) {
      auto text = slurp(*file);
      if (!text) return fail(text.error());
      licence = *text;
    } else {
      licence = "SPDX-License-Identifier: " + release.licence + "\n";
      result.warnings.push_back("no text is shipped for licence " + release.licence + "; LICENSE.txt names it only");
    }
    if (auto r = writer.write("LICENSE.txt", licence); !r) return fail(r.error());
  }
  if (auto r = writer.write("CITATION.cff", citation_cff(m)); !r) return fail(r.error());
  if (auto r = writer.write("README.md", readme_md(m, tables)); !r) return fail(r.error());
  if (options.with_figures) result.warnings.emplace_back("figures are not written yet (phase 2); --with-figures ignored");
  if (options.with_viewer) result.warnings.emplace_back("the viewer is not shipped yet (phase 3); --with-viewer ignored");

  stage("parquet");
  if (auto parquet = make_parquet_writer()) {
    for (const Table* t : {&tables.samples, &tables.positions, &tables.analyses, &tables.isotopes, &tables.interpreted_ages}) {
      const std::string rel = "data/" + t->name + ".parquet";
      fs::create_directories(dir / "data", ec);
      if (auto w = parquet->write(*t, dir / rel); !w) return fail(w.error());
      if (auto r = writer.record(rel); !r) return fail(r.error());
    }
    result.parquet_written = true;
  } else {
    result.warnings.emplace_back("built without Parquet support (PYCHRON_PUBLISH): data/*.parquet were not written");
  }

  stage("records");
  for (std::size_t r = 0; r < tables.samples.rows.size(); ++r) {
    const auto col = tables.samples.column("sample_uuid");
    const auto uuid = Uuid::parse(std::get<std::string>(tables.samples.rows[r][*col]));
    if (!uuid) continue;
    auto record = sample_record_json(m, tables, *uuid);
    if (!record) return fail(record.error());
    if (auto w = writer.write("records/" + uuid->str() + ".json", *record); !w) return fail(w.error());
  }
  if (auto r = writer.write("records/release.json", release_record_json(m, tables)); !r) return fail(r.error());
  if (auto r = writer.write("catalog.json", catalog_json(m, tables)); !r) return fail(r.error());

  stage("manifest");
  std::sort(m.files.begin(), m.files.end(), [](const ManifestFile& a, const ManifestFile& b) { return a.path < b.path; });
  {
    const fs::path path = dir / kManifestFile;
    std::ofstream out(path, std::ios::binary);
    if (!out) return fail(ErrorKind::Io, "cannot write " + path.string());
    out << manifest_json(m);
    if (!out) return fail(ErrorKind::Io, "cannot write " + path.string());
    out.close();
    mark_as_user_file(path);
  }
  return result;
}

}  // namespace pychron::publish
