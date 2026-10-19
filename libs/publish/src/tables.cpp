// The row tables of a release and their canonical text (data publishing
// design, sections 5.3 and 5.7).

#include "pychron/publish/tables.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "pychron/processing/reduced.hpp"
#include "pychron/reduction/fits.hpp"

namespace pychron::publish {

namespace pp = processing;
namespace ps = persistence;

std::optional<std::size_t> Table::column(std::string_view column_name) const {
  for (std::size_t i = 0; i < columns.size(); ++i)
    if (columns[i].name == column_name) return i;
  return std::nullopt;
}

// ---------------------------------------------------------------- canonical text

std::string canonical_double(double value) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, value);
  return std::string(buf, r.ptr);
}

namespace {

std::string hex_of(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const unsigned char c : bytes) {
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0xF]);
  }
  return out;
}

struct CanonicalText {
  std::string operator()(std::monostate) const { return {}; }
  std::string operator()(double v) const { return std::isfinite(v) ? canonical_double(v) : std::string{}; }
  std::string operator()(std::int32_t v) const { return std::to_string(v); }
  std::string operator()(std::int64_t v) const { return std::to_string(v); }
  std::string operator()(bool v) const { return v ? "true" : "false"; }
  std::string operator()(const std::string& v) const { return v; }
  std::string operator()(const UtcTime& v) const { return v.iso(); }
  std::string operator()(const JsonText& v) const { return v.value; }
  std::string operator()(const WkbBytes& v) const { return hex_of(v.value); }
};

}  // namespace

std::string canonical_row_text(const std::vector<Cell>& row) {
  std::string out;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i) out.push_back('\x1f');
    out += std::visit(CanonicalText{}, row[i]);
  }
  return out;
}

Sha256Digest rows_sha256(const Table& table) {
  Sha256 hasher;
  for (std::size_t i = 0; i < table.rows.size(); ++i) {
    if (i) hasher.update(std::string_view{"\x1e", 1});
    hasher.update(canonical_row_text(table.rows[i]));
  }
  return hasher.finish();
}

std::string wkb_point(double lon, double lat) {
  std::string out(21, '\0');
  out[0] = '\x01';  // little endian
  const std::uint32_t type = 1;
  std::memcpy(out.data() + 1, &type, 4);
  std::memcpy(out.data() + 5, &lon, 8);
  std::memcpy(out.data() + 13, &lat, 8);
  return out;
}

std::string file_safe(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
    if (!ok) c = '_';
  }
  return out;
}

// ---------------------------------------------------------------- building

namespace {

Cell num(std::optional<double> v) {
  if (!v || !std::isfinite(*v)) return std::monostate{};
  return *v;
}
Cell num(double v) { return num(std::optional<double>{v}); }
Cell text(const std::string& s) {
  if (s.empty()) return std::monostate{};
  return s;
}
Cell text(const std::optional<std::string>& s) { return s ? text(*s) : Cell{std::monostate{}}; }
Cell i32(int v) { return static_cast<std::int32_t>(v); }

// A report cell as a table cell: a number, text, or null.
Cell of_report(const pp::ReportCell& c) {
  if (const auto* d = std::get_if<double>(&c)) return num(*d);
  if (const auto* s = std::get_if<std::string>(&c)) return text(*s);
  return std::monostate{};
}

// The report's columns by name; a cell by row and column name.
class ReportView {
 public:
  explicit ReportView(const pp::ReportTable& t) : t_(t) {
    for (std::size_t i = 0; i < t.columns.size(); ++i) index_[t.columns[i]] = i;
  }
  const pp::ReportCell* cell(std::size_t row, const std::string& column) const {
    const auto it = index_.find(column);
    if (it == index_.end() || row >= t_.rows.size()) return nullptr;
    return &t_.rows[row][it->second];
  }
  Cell get(std::size_t row, const std::string& column) const {
    const auto* c = cell(row, column);
    return c ? of_report(*c) : Cell{std::monostate{}};
  }
  std::string str(std::size_t row, const std::string& column) const {
    const auto* c = cell(row, column);
    if (!c) return {};
    if (const auto* s = std::get_if<std::string>(c)) return *s;
    return {};
  }
  // The first column whose name starts with `prefix` (the age columns carry
  // the unit and the sigma level in their names).
  std::string column_starting(const std::string& prefix) const {
    for (const auto& c : t_.columns)
      if (c.starts_with(prefix)) return c;
    return {};
  }
  // The column that starts with `prefix` and ends with `suffix`.
  std::string column_matching(const std::string& prefix, const std::string& suffix) const {
    for (const auto& c : t_.columns)
      if (c.starts_with(prefix) && c.ends_with(suffix)) return c;
    return {};
  }
  // The "±" column of `name`: "<name> ±(<n>s)".
  std::string err_column(const std::string& name) const { return column_matching(name + " ±(", "s)"); }
  std::size_t rows() const { return t_.rows.size(); }

 private:
  const pp::ReportTable& t_;
  std::map<std::string, std::size_t> index_;
};

struct Builder {
  Table t;
  explicit Builder(std::string name, bool geo) {
    t.name = std::move(name);
    t.geo = geo;
  }
  void col(const char* name, ColumnType type) { t.columns.push_back({name, type, true}); }
  void pm(const char* name) {
    col(name, ColumnType::Double);
    t.columns.push_back({std::string(name) + "_err", ColumnType::Double, true});
  }
  void geometry_columns() {
    col("geometry", ColumnType::Wkb);
    col("bbox", ColumnType::Json);
  }
  std::vector<Cell>& row() {
    t.rows.emplace_back(t.columns.size(), Cell{std::monostate{}});
    return t.rows.back();
  }
  void set(std::vector<Cell>& row, const char* name, Cell value) {
    const auto i = t.column(name);
    if (i) row[*i] = std::move(value);
  }
};

struct Point {
  double lon, lat;
};

std::optional<Point> point_of(const ps::SampleFields& f) {
  if (!f.lat || !f.lon || !std::isfinite(*f.lat) || !std::isfinite(*f.lon)) return std::nullopt;
  return Point{*f.lon, *f.lat};
}

void set_geometry(Builder& b, std::vector<Cell>& row, std::optional<Point> p) {
  if (!p) return;
  b.set(row, "geometry", WkbBytes{wkb_point(p->lon, p->lat)});
  b.set(row, "bbox", JsonText{"{\"xmin\":" + canonical_double(p->lon) + ",\"ymin\":" + canonical_double(p->lat) +
                              ",\"xmax\":" + canonical_double(p->lon) + ",\"ymax\":" + canonical_double(p->lat) +
                              "}"});
}

std::string age_kind_of(const std::optional<std::string>& kind) {
  if (!kind) return "other";
  std::string k = *kind;
  std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::replace(k.begin(), k.end(), ' ', '_');
  for (const char* known : {"plateau", "weighted_mean", "isochron", "integrated"})
    if (k.find(known) != std::string::npos) return known;
  return "other";
}

std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out.push_back(c);
    }
  }
  return out + "\"";
}

const char* kKindColumns[][2] = {{"signals", "rev_signals"},     {"intercepts", "rev_intercepts"},
                                 {"baselines", "rev_baselines"}, {"blanks", "rev_blanks"},
                                 {"icfactors", "rev_icfactors"}, {"tags", "rev_tags"},
                                 {"annotation", "rev_annotation"}};

}  // namespace

ReleaseTables build_tables(const TableInputs& in) {
  ReleaseTables out;
  std::map<Uuid, const AnalysisPins*> pins_of;
  for (const auto& p : in.pins) pins_of[p.analysis] = &p;
  std::map<Uuid, const ps::SampleRow*> sample_of;
  for (const auto& s : in.samples) sample_of[s.uuid] = &s;

  // The dataset's items in release order, each with its pins and sample.
  struct Member {
    const pp::DatasetItem* item;
    const AnalysisPins* pins;
    const ps::SampleRow* sample;
    Uuid uuid;
    std::optional<std::size_t> report_row;
  };
  std::vector<Member> members;
  const ReportView analyses_report(in.report.analyses);
  std::map<std::string, std::size_t> report_row_of;  // run id -> row
  for (std::size_t i = 0; i < analyses_report.rows(); ++i) report_row_of[analyses_report.str(i, "run id")] = i;
  for (const auto& item : in.dataset.items()) {
    const auto& a = *item.analysis->analysis;
    Member m{&item, nullptr, nullptr, Uuid{}, std::nullopt};
    for (const auto& p : in.pins) {
      // Pins are keyed by the store's uuid; the dataset's analysis carries it as text.
      if (p.analysis.str() == a.uuid) m.pins = &p;
    }
    if (!m.pins) {
      // A member without pins (tests, or an analysis the store did not pin) keys by its text uuid.
      if (auto u = Uuid::parse(a.uuid)) m.uuid = *u;
    } else {
      m.uuid = m.pins->analysis;
      if (m.pins->sample)
        if (auto it = sample_of.find(*m.pins->sample); it != sample_of.end()) m.sample = it->second;
    }
    if (!m.sample) {
      for (const auto& s : in.samples)
        if (s.name == a.sample) m.sample = &s;
    }
    if (auto it = report_row_of.find(a.runid); it != report_row_of.end()) m.report_row = it->second;
    members.push_back(m);
  }
  std::stable_sort(members.begin(), members.end(), [](const Member& x, const Member& y) {
    const double tx = x.item->analysis->analysis->timestamp, ty = y.item->analysis->analysis->timestamp;
    if (tx != ty) return tx < ty;
    return x.uuid < y.uuid;
  });

  // ---------------------------------------------------------------- analyses
  {
    Builder b("analyses", true);
    for (const char* c : {"analysis_uuid", "runid", "identifier"}) b.col(c, ColumnType::String);
    b.col("aliquot", ColumnType::Int32);
    b.col("increment", ColumnType::Int32);
    for (const char* c : {"step", "sample_uuid", "sample", "project", "material", "analysis_type", "experiment_type"})
      b.col(c, ColumnType::String);
    b.col("timestamp_utc", ColumnType::TimestampUtc);
    for (const char* c : {"mass_spectrometer", "extract_device", "analyst", "laboratory"}) b.col(c, ColumnType::String);
    b.col("extract_value", ColumnType::Double);
    b.col("extract_units", ColumnType::String);
    for (const char* c : {"extract_duration_s", "cleanup_duration_s", "weight_mg", "beam_diameter"})
      b.col(c, ColumnType::Double);
    for (const char* c : {"pattern", "irradiation", "level", "position", "tag"}) b.col(c, ColumnType::String);
    b.col("included", ColumnType::Bool);
    b.col("group_id", ColumnType::String);
    b.col("comment", ColumnType::String);
    b.pm("j");
    for (const char* c : {"ar40_fa", "ar39_fa", "ar38_fa", "ar37_fa", "ar36_fa"}) b.pm(c);
    for (const char* c : {"ar40_blank_fa", "ar39_blank_fa", "ar38_blank_fa", "ar37_blank_fa", "ar36_blank_fa"}) b.pm(c);
    b.pm("f");
    b.col("rad40_pct", ColumnType::Double);
    b.col("k39_fa", ColumnType::Double);
    b.col("k39_pct_group", ColumnType::Double);
    b.col("k39_cum_pct", ColumnType::Double);
    b.pm("kca");
    b.pm("kcl");
    b.pm("iso_39_40");
    b.pm("iso_36_40");
    b.col("iso_corr", ColumnType::Double);
    for (const char* c : {"age_ma", "age_err_analytical_ma", "age_err_with_j_ma"}) b.col(c, ColumnType::Double);
    b.col("age_note", ColumnType::String);
    b.col("record_sha256", ColumnType::String);
    for (const auto& kc : kKindColumns) b.col(kc[1], ColumnType::String);
    b.geometry_columns();

    const ReportView rep = analyses_report;
    const std::string age = rep.column_starting("age (");
    const std::string age_analytical = rep.column_matching(age + " ±(", " analytical");
    const std::string age_j = rep.column_matching(age + " ±(", " with J");
    const char* iso_names[5] = {"40Ar", "39Ar", "38Ar", "37Ar", "36Ar"};
    const char* iso_cols[5] = {"ar40_fa", "ar39_fa", "ar38_fa", "ar37_fa", "ar36_fa"};
    const char* blank_cols[5] = {"ar40_blank_fa", "ar39_blank_fa", "ar38_blank_fa", "ar37_blank_fa", "ar36_blank_fa"};
    for (const auto& m : members) {
      const auto& ra = *m.item->analysis;
      const auto& a = *ra.analysis;
      auto& row = b.row();
      b.set(row, "analysis_uuid", m.uuid.str());
      b.set(row, "runid", a.runid);
      b.set(row, "identifier", a.identifier);
      b.set(row, "aliquot", i32(a.aliquot));
      b.set(row, "increment", i32(a.increment));
      b.set(row, "step", text(a.step()));
      if (m.sample) b.set(row, "sample_uuid", m.sample->uuid.str());
      b.set(row, "sample", text(a.sample));
      b.set(row, "project", text(a.project));
      b.set(row, "material", text(a.material));
      b.set(row, "analysis_type", text(a.analysis_type));
      b.set(row, "timestamp_utc", UtcTime{static_cast<std::int64_t>(std::llround(a.timestamp * 1e6))});
      b.set(row, "mass_spectrometer", text(a.mass_spectrometer));
      b.set(row, "extract_device", text(a.extract_device));
      b.set(row, "analyst", text(a.analyst));
      b.set(row, "extract_value", num(a.extraction.value));
      b.set(row, "extract_units", text(a.extraction.units));
      b.set(row, "extract_duration_s", num(a.extraction.duration));
      b.set(row, "cleanup_duration_s", num(a.extraction.cleanup));
      b.set(row, "weight_mg", num(a.extraction.weight));
      b.set(row, "beam_diameter", num(a.extraction.beam_diameter));
      b.set(row, "pattern", text(a.extraction.pattern));
      b.set(row, "irradiation", text(a.irradiation));
      b.set(row, "level", text(a.level));
      b.set(row, "position", text(a.position));
      b.set(row, "tag", text(a.tag));
      b.set(row, "included", m.item->exclusion.included());
      b.set(row, "comment", text(a.comment));
      if (ra.j) {
        b.set(row, "j", num(ra.j->nominal()));
        b.set(row, "j_err", num(ra.j->std_dev()));
      }
      if (m.report_row) {
        const std::size_t r = *m.report_row;
        b.set(row, "group_id", rep.get(r, "group"));
        for (int i = 0; i < 5; ++i) {
          const std::string iso = std::string(iso_names[i]) + " (fA)";
          const std::string blank = std::string(iso_names[i]) + " blank (fA)";
          b.set(row, iso_cols[i], rep.get(r, iso));
          b.set(row, (std::string(iso_cols[i]) + "_err").c_str(), rep.get(r, rep.err_column(iso)));
          b.set(row, blank_cols[i], rep.get(r, blank));
          b.set(row, (std::string(blank_cols[i]) + "_err").c_str(), rep.get(r, rep.err_column(blank)));
        }
        b.set(row, "f", rep.get(r, "40Ar*/39ArK"));
        b.set(row, "f_err", rep.get(r, rep.err_column("40Ar*/39ArK")));
        b.set(row, "rad40_pct", rep.get(r, "40Ar* (%)"));
        b.set(row, "k39_fa", rep.get(r, "39ArK (fA)"));
        b.set(row, "k39_pct_group", rep.get(r, "39ArK (% of group)"));
        b.set(row, "k39_cum_pct", rep.get(r, "39ArK cumulative (%)"));
        b.set(row, "kca", rep.get(r, "K/Ca"));
        b.set(row, "kca_err", rep.get(r, rep.err_column("K/Ca")));
        b.set(row, "kcl", rep.get(r, "K/Cl"));
        b.set(row, "kcl_err", rep.get(r, rep.err_column("K/Cl")));
        b.set(row, "iso_39_40", rep.get(r, "39Ar/40Ar"));
        b.set(row, "iso_39_40_err", rep.get(r, rep.err_column("39Ar/40Ar")));
        b.set(row, "iso_36_40", rep.get(r, "36Ar/40Ar"));
        b.set(row, "iso_36_40_err", rep.get(r, rep.err_column("36Ar/40Ar")));
        b.set(row, "iso_corr", rep.get(r, "rho (39/40, 36/40)"));
        b.set(row, "age_ma", rep.get(r, age));
        b.set(row, "age_err_analytical_ma", rep.get(r, age_analytical));
        b.set(row, "age_err_with_j_ma", rep.get(r, age_j));
        Cell note = rep.get(r, "note");
        if (std::holds_alternative<std::monostate>(note) && std::holds_alternative<std::monostate>(rep.get(r, age)))
          note = std::string("no age: ") + (ra.reduction_error.empty() ? "the analysis has no J" : ra.reduction_error);
        b.set(row, "age_note", std::move(note));
      }
      if (m.pins) {
        if (m.pins->record_sha256) b.set(row, "record_sha256", to_hex(*m.pins->record_sha256));
        for (const auto& kc : kKindColumns) {
          const auto kind = ps::parse_kind(kc[0]);
          if (!kind) continue;
          if (auto it = m.pins->heads.find(*kind); it != m.pins->heads.end()) b.set(row, kc[1], it->second.str());
        }
      }
      if (m.sample) set_geometry(b, row, point_of(m.sample->fields));
    }
    out.analyses = std::move(b.t);
  }

  // ---------------------------------------------------------------- isotopes
  {
    Builder b("isotopes", false);
    for (const char* c : {"analysis_uuid", "isotope", "detector", "units"}) b.col(c, ColumnType::String);
    b.pm("intercept");
    b.col("fit", ColumnType::String);
    b.col("error_type", ColumnType::String);
    b.col("n", ColumnType::Int32);
    b.col("fn", ColumnType::Int32);
    b.col("reviewed", ColumnType::Bool);
    b.pm("baseline");
    b.col("baseline_fit", ColumnType::String);
    b.pm("blank");
    b.col("blank_fit", ColumnType::String);
    b.pm("ic_factor");
    b.col("discrimination", ColumnType::Bool);
    b.col("decay_corrected", ColumnType::Bool);
    b.pm("corrected");
    b.col("blob_sha256", ColumnType::String);
    b.col("n_points", ColumnType::Int32);
    for (const auto& m : members) {
      const auto& ra = *m.item->analysis;
      const auto& a = *ra.analysis;
      std::map<std::string, const ps::InterceptRow*> intercept_rows;
      std::map<std::string, const ps::SignalRefRow*> signal_rows;
      if (m.pins) {
        for (const auto& r : m.pins->intercepts) intercept_rows[r.isotope] = &r;
        for (const auto& s : m.pins->signals)
          if (s.series_kind == "signal") signal_rows[s.series_key] = &s;
      }
      for (const auto& iso : a.isotopes) {
        auto& row = b.row();
        b.set(row, "analysis_uuid", m.uuid.str());
        b.set(row, "isotope", iso.isotope);
        b.set(row, "detector", text(iso.detector));
        b.set(row, "units", std::string("fA"));
        b.set(row, "intercept", num(iso.intercept.value));
        b.set(row, "intercept_err", num(iso.intercept.error));
        if (iso.fit) b.set(row, "fit", std::string(reduction::to_string(iso.fit->kind)));
        b.set(row, "n", i32(iso.n));
        b.set(row, "baseline", num(iso.baseline.value));
        b.set(row, "baseline_err", num(iso.baseline.error));
        if (iso.baseline_fit) b.set(row, "baseline_fit", std::string(reduction::to_string(iso.baseline_fit->kind)));
        b.set(row, "blank", num(iso.blank.value));
        b.set(row, "blank_err", num(iso.blank.error));
        b.set(row, "blank_fit", text(iso.blank_source));
        b.set(row, "ic_factor", num(iso.ic_factor.value));
        b.set(row, "ic_factor_err", num(iso.ic_factor.error));
        b.set(row, "decay_corrected", iso.isotope == "Ar37" || iso.isotope == "Ar39");
        if (auto v = ra.stage(iso.key, pp::Stage::DecayCorrected)) {
          b.set(row, "corrected", num(v->nominal()));
          b.set(row, "corrected_err", num(v->std_dev()));
        }
        if (auto it = intercept_rows.find(iso.key); it != intercept_rows.end()) {
          b.set(row, "error_type", text(it->second->error_type));
          if (it->second->fn) b.set(row, "fn", i32(*it->second->fn));
          b.set(row, "reviewed", it->second->reviewed);
        }
        if (auto it = signal_rows.find(iso.key); it != signal_rows.end()) {
          b.set(row, "blob_sha256", to_hex(it->second->blob_sha));
          if (it->second->n_points) b.set(row, "n_points", i32(*it->second->n_points));
        }
      }
    }
    out.isotopes = std::move(b.t);
  }

  // ---------------------------------------------------------------- positions
  {
    Builder b("positions", false);
    for (const char* c : {"identifier", "identifier_uuid", "sample_uuid", "irradiation", "level"})
      b.col(c, ColumnType::String);
    b.col("position", ColumnType::Int32);
    b.col("reactor", ColumnType::String);
    b.col("irradiation_start_utc", ColumnType::TimestampUtc);
    b.col("irradiation_duration_h", ColumnType::Double);
    for (const char* c : {"holder", "packet"}) b.col(c, ColumnType::String);
    b.col("weight_mg", ColumnType::Double);
    for (const char* c : {"monitor_name", "monitor_material"}) b.col(c, ColumnType::String);
    b.pm("monitor_age_ma");
    b.pm("j");
    b.col("j_position_err", ColumnType::Double);
    b.pm("lambda_k_total");
    for (const char* c : {"p_40_39_k", "p_38_39_k", "p_37_39_k", "p_39_37_ca", "p_38_37_ca", "p_36_37_ca", "p_36_38_cl",
                          "ca_k", "cl_k"})
      b.pm(c);
    for (const char* c : {"flux_revision_uuid", "production_revision_uuid", "chronology_revision_uuid"})
      b.col(c, ColumnType::String);
    const ReportView rep(in.report.irradiation);
    struct PosRow {
      std::string irradiation, level;
      int position;
      std::string identifier;
      std::size_t row;
    };
    std::vector<PosRow> rows;
    for (std::size_t r = 0; r < rep.rows(); ++r) {
      PosRow p{rep.str(r, "irradiation"), rep.str(r, "level"), 0, rep.str(r, "identifier"), r};
      if (const auto* c = rep.cell(r, "position")) {
        if (const auto* d = std::get_if<double>(c)) p.position = static_cast<int>(std::lround(*d));
        if (const auto* t = std::get_if<std::string>(c)) {
          int n = 0;
          if (std::from_chars(t->data(), t->data() + t->size(), n).ec == std::errc{}) p.position = n;
        }
      }
      rows.push_back(p);
    }
    // By irradiation, level, position, identifier; identifiers without an irradiation last.
    std::stable_sort(rows.begin(), rows.end(), [](const PosRow& x, const PosRow& y) {
      if (x.irradiation.empty() != y.irradiation.empty()) return !x.irradiation.empty();
      return std::tie(x.irradiation, x.level, x.position, x.identifier) <
             std::tie(y.irradiation, y.level, y.position, y.identifier);
    });
    // The first member of each identifier gives its sample and references.
    std::map<std::string, const Member*> member_of;
    for (const auto& m : members) member_of.emplace(m.item->analysis->analysis->identifier, &m);
    const auto pm_from = [&](std::vector<Cell>& row, std::size_t r, const char* column, const std::string& report) {
      b.set(row, column, rep.get(r, report));
      b.set(row, (std::string(column) + "_err").c_str(), rep.get(r, rep.err_column(report)));
    };
    for (const auto& p : rows) {
      auto& row = b.row();
      b.set(row, "identifier", text(p.identifier));
      b.set(row, "irradiation", text(p.irradiation));
      b.set(row, "level", text(p.level));
      if (!p.irradiation.empty()) b.set(row, "position", i32(p.position));
      b.set(row, "reactor", rep.get(p.row, "reactor"));
      if (const auto start = rep.str(p.row, "irradiation start (UTC)"); !start.empty())
        if (auto t = UtcTime::parse(start)) b.set(row, "irradiation_start_utc", *t);
      b.set(row, "irradiation_duration_h", rep.get(p.row, "irradiation duration (h)"));
      b.set(row, "monitor_name", rep.get(p.row, "fluence monitor"));
      b.set(row, "monitor_material", rep.get(p.row, "monitor material"));
      pm_from(row, p.row, "monitor_age_ma", "monitor age (Ma)");
      pm_from(row, p.row, "j", "J");
      b.set(row, "j_position_err", rep.get(p.row, rep.column_starting("J position error")));
      pm_from(row, p.row, "lambda_k_total", "lambda_K total override (1/a)");
      pm_from(row, p.row, "p_40_39_k", "(40Ar/39Ar)K");
      pm_from(row, p.row, "p_38_39_k", "(38Ar/39Ar)K");
      pm_from(row, p.row, "p_37_39_k", "(37Ar/39Ar)K");
      pm_from(row, p.row, "p_39_37_ca", "(39Ar/37Ar)Ca");
      pm_from(row, p.row, "p_38_37_ca", "(38Ar/37Ar)Ca");
      pm_from(row, p.row, "p_36_37_ca", "(36Ar/37Ar)Ca");
      pm_from(row, p.row, "p_36_38_cl", "(36Ar/38Ar)Cl");
      pm_from(row, p.row, "ca_k", "Ca/K");
      pm_from(row, p.row, "cl_k", "Cl/K");
      if (auto it = member_of.find(p.identifier); it != member_of.end()) {
        const Member& m = *it->second;
        if (m.sample) b.set(row, "sample_uuid", m.sample->uuid.str());
        if (m.pins) {
          for (const auto& ref : m.pins->refs) {
            if (ref.type == ps::RefType::FluxPosition) b.set(row, "flux_revision_uuid", ref.revision.str());
            if (ref.type == ps::RefType::Production) b.set(row, "production_revision_uuid", ref.revision.str());
            if (ref.type == ps::RefType::Chronology) b.set(row, "chronology_revision_uuid", ref.revision.str());
          }
        }
      }
    }
    out.positions = std::move(b.t);
  }

  // ---------------------------------------------------------------- samples
  {
    Builder b("samples", true);
    for (const char* c : {"sample_uuid", "name", "igsn", "project", "project_uuid", "principal_investigator",
                          "material", "grainsize"})
      b.col(c, ColumnType::String);
    b.geometry_columns();
    b.col("elevation_m", ColumnType::Double);
    for (const char* c : {"location", "unit", "lithology", "lithology_class", "lithology_type", "lithology_group"})
      b.col(c, ColumnType::String);
    b.col("approximate_age_ma", ColumnType::Double);
    b.col("note", ColumnType::String);
    for (const char* c : {"n_analyses", "n_ages", "n_interpreted_ages"}) b.col(c, ColumnType::Int32);
    b.col("first_analysis_utc", ColumnType::TimestampUtc);
    b.col("last_analysis_utc", ColumnType::TimestampUtc);
    b.col("record_path", ColumnType::String);
    // Counts and times per sample from the members (the analyses table has the ages).
    struct Counts {
      int n = 0, ages = 0, ias = 0;
      std::optional<double> first, last;
    };
    std::map<Uuid, Counts> counts;
    const auto age_col = out.analyses.column("age_ma");
    const auto sample_col = out.analyses.column("sample_uuid");
    const auto time_col = out.analyses.column("timestamp_utc");
    for (const auto& row : out.analyses.rows) {
      if (!sample_col || std::holds_alternative<std::monostate>(row[*sample_col])) continue;
      auto u = Uuid::parse(std::get<std::string>(row[*sample_col]));
      if (!u) continue;
      auto& c = counts[*u];
      ++c.n;
      if (age_col && !std::holds_alternative<std::monostate>(row[*age_col])) ++c.ages;
      if (time_col)
        if (const auto* t = std::get_if<UtcTime>(&row[*time_col])) {
          const double secs = static_cast<double>(t->micros) / 1e6;
          c.first = c.first ? std::min(*c.first, secs) : secs;
          c.last = c.last ? std::max(*c.last, secs) : secs;
        }
    }
    for (const auto& ia : in.interpreted_ages) ++counts[ia.sample].ias;
    std::vector<std::size_t> order(in.samples.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t xi, std::size_t yi) {
      const auto& x = in.samples[xi];
      const auto& y = in.samples[yi];
      if (x.name != y.name) return x.name < y.name;
      return x.uuid < y.uuid;
    });
    for (const std::size_t index : order) {
      const auto* s = &in.samples[index];
      auto& row = b.row();
      b.set(row, "sample_uuid", s->uuid.str());
      b.set(row, "name", s->name);
      b.set(row, "igsn", text(s->fields.igsn));
      b.set(row, "project", text(s->project_name));
      b.set(row, "project_uuid", s->project.str());
      b.set(row, "principal_investigator", text(s->principal_investigator_name));
      b.set(row, "material", text(s->material_name));
      b.set(row, "grainsize", text(s->grainsize));
      set_geometry(b, row, point_of(s->fields));
      b.set(row, "elevation_m", num(s->fields.elevation));
      b.set(row, "location", text(s->fields.location));
      b.set(row, "unit", text(s->fields.unit));
      b.set(row, "lithology", text(s->fields.lithology));
      b.set(row, "lithology_class", text(s->fields.lithology_class));
      b.set(row, "lithology_type", text(s->fields.lithology_type));
      b.set(row, "lithology_group", text(s->fields.lithology_group));
      b.set(row, "approximate_age_ma", num(s->fields.approximate_age));
      b.set(row, "note", text(s->fields.note));
      const Counts c = counts.contains(s->uuid) ? counts.at(s->uuid) : Counts{};
      b.set(row, "n_analyses", i32(c.n));
      b.set(row, "n_ages", i32(c.ages));
      b.set(row, "n_interpreted_ages", i32(c.ias));
      if (c.first) b.set(row, "first_analysis_utc", UtcTime{static_cast<std::int64_t>(std::llround(*c.first * 1e6))});
      if (c.last) b.set(row, "last_analysis_utc", UtcTime{static_cast<std::int64_t>(std::llround(*c.last * 1e6))});
      b.set(row, "record_path", "records/" + s->uuid.str() + ".json");
    }
    out.samples = std::move(b.t);
  }

  // ---------------------------------------------------------------- interpreted ages
  {
    Builder b("interpreted_ages", true);
    for (const char* c : {"ia_uuid", "name", "sample_uuid", "identifier", "age_kind"}) b.col(c, ColumnType::String);
    b.pm("age_ma");
    b.col("mswd", ColumnType::Double);
    b.col("n", ColumnType::Int32);
    b.pm("kca");
    b.col("plateau_k39_pct", ColumnType::Double);
    b.pm("trapped_40_36");
    b.col("members", ColumnType::Json);
    b.col("revision_uuid", ColumnType::String);
    b.col("doc", ColumnType::Json);
    b.geometry_columns();
    std::vector<std::size_t> order(in.interpreted_ages.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    const auto sample_name = [&](Uuid u) {
      auto it = sample_of.find(u);
      return it == sample_of.end() ? std::string{} : it->second->name;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t xi, std::size_t yi) {
      const auto& x = in.interpreted_ages[xi];
      const auto& y = in.interpreted_ages[yi];
      const std::string sx = sample_name(x.sample), sy = sample_name(y.sample);
      if (sx != sy) return sx < sy;
      if (x.name != y.name) return x.name < y.name;
      return x.ia < y.ia;
    });
    for (const std::size_t index : order) {
      const auto* ia = &in.interpreted_ages[index];
      auto& row = b.row();
      b.set(row, "ia_uuid", ia->ia.str());
      b.set(row, "name", ia->name);
      b.set(row, "sample_uuid", ia->sample.str());
      b.set(row, "identifier", text(ia->identifier));
      b.set(row, "age_kind", age_kind_of(ia->value.age_kind));
      b.set(row, "age_ma", num(ia->value.age));
      b.set(row, "age_ma_err", num(ia->value.age_err));
      b.set(row, "mswd", num(ia->value.mswd));
      if (ia->value.nanalyses) b.set(row, "n", i32(*ia->value.nanalyses));
      b.set(row, "kca", num(ia->value.kca));
      b.set(row, "kca_err", num(ia->value.kca_err));
      std::string members_json = "[";
      for (std::size_t i = 0; i < ia->value.members.size(); ++i) {
        const auto& m = ia->value.members[i];
        if (i) members_json += ',';
        members_json += "{\"analysis_uuid\":" + json_string(m.analysis.str());
        members_json += ",\"plateau_step\":" + (m.plateau_step ? std::string(*m.plateau_step ? "true" : "false")
                                                               : std::string("null"));
        members_json += ",\"tag\":" + (m.tag ? json_string(*m.tag) : std::string("null")) + "}";
      }
      members_json += ']';
      b.set(row, "members", JsonText{members_json});
      b.set(row, "revision_uuid", ia->revision.str());
      b.set(row, "doc", JsonText{ia->value.doc_json});
      if (auto it = sample_of.find(ia->sample); it != sample_of.end()) set_geometry(b, row, point_of(it->second->fields));
    }
    out.interpreted_ages = std::move(b.t);
  }
  return out;
}

}  // namespace pychron::publish
