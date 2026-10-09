#include "pychron/processing/report.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <utility>

#include "figure_common.hpp"
#include "pychron/core/user_file.hpp"
#include "pychron/processing/arar_groups.hpp"
#include "pychron/reduction/arar_reduction.hpp"

namespace pychron::processing {

namespace r = reduction;

namespace {

constexpr std::string_view kStandard =
    "Schaen, A.J., et al., 2021, Interpreting and reporting 40Ar/39Ar geochronologic data: "
    "GSA Bulletin, v. 133, p. 461-487, doi:10.1130/B35560.1";

using Cell = ReportCell;
using Row = std::vector<Cell>;

Cell num(double v) { return std::isfinite(v) ? Cell{v} : Cell{}; }
Cell num(std::optional<double> v) { return v ? num(*v) : Cell{}; }
Cell txt(std::string s) { return s.empty() ? Cell{} : Cell{std::move(s)}; }
Cell yes_no(bool b) { return Cell{std::string(b ? "yes" : "no")}; }

std::string pm_name(const std::string& name, int nsigma) {
  return name + " ±(" + std::to_string(nsigma) + "s)";
}

// Columns by name; rows filled by index.
class Columns {
 public:
  explicit Columns(ReportTable& t) : t_(t) {}
  std::size_t add(std::string name) {
    t_.columns.push_back(std::move(name));
    return t_.columns.size() - 1;
  }
  // The value column and its "±" neighbour.
  std::pair<std::size_t, std::size_t> add_pm(const std::string& name, int nsigma) {
    const auto v = add(name);
    const auto e = add(pm_name(name, nsigma));
    return {v, e};
  }
  Row& row() {
    t_.rows.emplace_back(t_.columns.size());
    return t_.rows.back();
  }

 private:
  ReportTable& t_;
};

using Pm = std::pair<std::size_t, std::size_t>;

void set_pm(Row& row, Pm c, double value, double error, int nsigma) {
  row[c.first] = num(value);
  row[c.second] = num(error * nsigma);
}
void set_pm(Row& row, Pm c, const std::optional<r::UFloat>& u, int nsigma) {
  if (u) set_pm(row, c, u->nominal(), u->std_dev(), nsigma);
}
void set_pm(Row& row, Pm c, const r::Measured& m, int nsigma) { set_pm(row, c, m.value, m.error, nsigma); }

std::string join(const std::set<std::string>& items, const char* sep = "; ") {
  std::string out;
  for (const auto& i : items) {
    if (!out.empty()) out += sep;
    out += i;
  }
  return out;
}

const char* age_units_name(r::AgeUnits u) {
  switch (u) {
    case r::AgeUnits::a:
      return "a";
    case r::AgeUnits::ka:
      return "ka";
    case r::AgeUnits::Ma:
      return "Ma";
    case r::AgeUnits::Ga:
      return "Ga";
  }
  return "Ma";
}

const char* plateau_method_name(r::PlateauMethod m) { return m == r::PlateauMethod::Mahon ? "Mahon 1996" : "Fleck 1977"; }
const char* weighting_name(r::PlateauWeighting w) {
  return w == r::PlateauWeighting::VolumeFraction ? "volume fraction" : "inverse variance";
}

double now_utc_seconds() {
  using namespace std::chrono;
  return static_cast<double>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// Step order within a group: increment, then time (as the spectrum).
GroupItems ordered(GroupItems items) {
  std::stable_sort(items.begin(), items.end(), [](const DatasetItem* a, const DatasetItem* b) {
    const auto& x = *a->analysis->analysis;
    const auto& y = *b->analysis->analysis;
    if (x.increment != y.increment) return x.increment < y.increment;
    return x.timestamp < y.timestamp;
  });
  return items;
}

// The dataset's group name ("Group N" when the grouping named none), under
// the graph's when there are several graphs.
std::string group_label(const Dataset& d, int graph, int group) {
  const std::string name = d.group_name(group);
  if (d.graphs().size() > 1) return d.graph_name(graph) + " / " + name;
  return name;
}

bool has_ages(const DatasetItem& it) { return it.analysis->arar && it.analysis->arar->ages; }

// The analytical age of an item (no J error), when it has one.
std::optional<r::UFloat> analytical_age(const DatasetItem& it) {
  if (!has_ages(it)) return std::nullopt;
  return it.analysis->arar->ages->age;
}

double k39_of(const DatasetItem& it) {
  if (!it.analysis->arar) return std::numeric_limits<double>::quiet_NaN();
  return it.analysis->arar->f.interference.k39.nominal();
}

// ---------------------------------------------------------------- metadata

struct Inventory {
  std::set<std::string> spectrometers, devices, analysts, projects, samples, materials, irradiations, monitors;
  std::size_t total = 0, included = 0, groups = 0, reduced = 0;
  double first = 0, last = 0;
  const ReducedAnalysis* reference = nullptr;  // the first reduced analysis: its constants and units
  bool constants_differ = false;
};

Inventory take_inventory(const Dataset& d, int nsigma) {
  Inventory inv;
  bool first = true;
  for (const auto& it : d.items()) {
    const auto& a = *it.analysis->analysis;
    ++inv.total;
    if (it.exclusion.included()) ++inv.included;
    if (!a.mass_spectrometer.empty()) inv.spectrometers.insert(a.mass_spectrometer);
    if (!a.extract_device.empty()) inv.devices.insert(a.extract_device);
    if (!a.analyst.empty()) inv.analysts.insert(a.analyst);
    if (!a.project.empty()) inv.projects.insert(a.project);
    if (!a.sample.empty()) inv.samples.insert(a.sample);
    if (!a.material.empty()) inv.materials.insert(a.material);
    if (!a.irradiation.empty()) inv.irradiations.insert(a.irradiation);
    if (!a.monitor.name.empty()) {
      std::string m = a.monitor.name;
      if (!a.monitor.material.empty()) m += " " + a.monitor.material;
      if (a.monitor.age)
        m += " " + detail::format_sig(a.monitor.age->value, 6) + " ± " +
             detail::format_sig(a.monitor.age->error * nsigma, 3) + " Ma (" + std::to_string(nsigma) + "s)";
      inv.monitors.insert(m);
    }
    if (first || a.timestamp < inv.first) inv.first = a.timestamp;
    if (first || a.timestamp > inv.last) inv.last = a.timestamp;
    first = false;
    if (it.analysis->constants) {
      ++inv.reduced;
      if (inv.reference == nullptr) {
        inv.reference = it.analysis.get();
      } else {
        const auto& c = *it.analysis->constants;
        const auto& ref = *inv.reference->constants;
        if (c.lambda_b.value != ref.lambda_b.value || c.lambda_e.value != ref.lambda_e.value ||
            c.atm4036.value != ref.atm4036.value || c.lambda_ar37.value != ref.lambda_ar37.value ||
            c.lambda_ar39.value != ref.lambda_ar39.value || c.include_decay_error != ref.include_decay_error)
          inv.constants_differ = true;
      }
    }
  }
  for (int graph : d.graphs()) inv.groups += d.groups_of_graph(graph).size();
  return inv;
}

void metadata_table(const Dataset& d, const ReportOptions& o, const Inventory& inv, Report& rep) {
  auto& t = rep.metadata;
  t.name = "metadata";
  t.title = "Report metadata";
  t.columns = {"item", "value"};
  auto item = [&](std::string name, Cell value) {
    if (std::holds_alternative<std::monostate>(value)) return;
    t.rows.push_back({Cell{std::move(name)}, std::move(value)});
  };
  item("standard", Cell{std::string(kStandard)});
  item("generated (UTC)", Cell{detail::format_utc(now_utc_seconds())});
  item("software", Cell{o.software.empty() ? std::string("pychron-cpp") : o.software});
  item("reduction", Cell{d.reduction_tag.empty() ? std::string("unreduced") : d.reduction_tag});
  item("laboratory", txt(o.laboratory));
  item("analyses", Cell{static_cast<double>(inv.total)});
  item("analyses included in statistics", Cell{static_cast<double>(inv.included)});
  item("groups", Cell{static_cast<double>(inv.groups)});
  if (inv.total > 0)
    item("analysis dates (UTC)", Cell{detail::format_utc(inv.first) + " to " + detail::format_utc(inv.last)});
  item("mass spectrometers", txt(join(inv.spectrometers)));
  item("extraction devices", txt(join(inv.devices)));
  item("analysts", txt(join(inv.analysts)));
  item("projects", txt(join(inv.projects)));
  item("samples", txt(join(inv.samples)));
  item("materials", txt(join(inv.materials)));
  item("irradiations", txt(join(inv.irradiations)));
  item("fluence monitors", txt(join(inv.monitors)));
  item("uncertainty level", Cell{std::to_string(o.nsigma) + " sigma (every ± column)"});
  const r::AgeUnits units = inv.reference ? inv.reference->constants->age_units : r::AgeUnits::Ma;
  item("age units", Cell{std::string(age_units_name(units))});
  item("intensity units", Cell{std::string("fA")});
  item("isotope intensities",
       Cell{std::string("corrected for baseline, blank, detector intercalibration and discrimination, and (37Ar, "
                        "39Ar) for radioactive decay since irradiation; the interfering-reaction corrections are "
                        "applied in 40Ar*/39ArK, 39ArK, K/Ca and K/Cl")});
  item("blanks", Cell{std::string("the blank subtracted from each isotope is listed with every analysis")});
  const bool decay_error = inv.reference && inv.reference->constants->include_decay_error;
  item("age uncertainties",
       Cell{std::string("analytical: measured intensities, baselines, blanks and detector factors; with J: analytical "
                        "plus the J uncertainty of the irradiation position. Decay-constant uncertainty included: ") +
            (decay_error ? "yes" : "no") + ". The fluence monitor age uncertainty is not propagated."});
  item("plateau criterion",
       Cell{std::string(plateau_method_name(o.plateau.method)) + ", at least " + std::to_string(o.plateau.nsteps) +
            " steps and " + detail::format_sig(o.plateau.gas_fraction, 4) + "% of the 39ArK" +
            (o.plateau.method == r::PlateauMethod::Fleck
                 ? ", every pair of steps overlapping at " + detail::format_sig(o.plateau.overlap_sigma, 3) + " sigma"
                 : "") +
            "; plateau mean weighted by " + weighting_name(o.plateau_weighting) + ", error " +
            std::string(r::to_string(o.mean_error))});
  item("weighted mean", Cell{std::string("inverse-variance weighted mean of the included analyses, error ") +
                             std::string(r::to_string(o.mean_error))});
  item("isochron", Cell{std::string("inverse isochron (36Ar/40Ar against 39Ar/40Ar) of the included analyses, "
                                    "regression ") +
                        std::string(r::to_string(o.york)) + "; trapped 40Ar/36Ar from the y intercept"});
  for (const auto& n : o.notes) item("note", txt(n));
}

// ---------------------------------------------------------------- constants

void constants_table(const ReportOptions& o, const Inventory& inv, Report& rep) {
  auto& t = rep.constants;
  t.name = "constants";
  t.title = "Decay constants and atmospheric ratios used";
  t.columns = {"item", "value", pm_name("value", o.nsigma), "units"};
  if (inv.reference == nullptr) {
    rep.warnings.emplace_back("no analysis reduced: the constants are unknown");
    return;
  }
  if (inv.constants_differ)
    rep.warnings.emplace_back("the analyses were reduced with different constants; those of the first are listed");
  const auto& c = *inv.reference->constants;
  auto row = [&](std::string name, const r::Measured& m, const char* units) {
    t.rows.push_back({Cell{std::move(name)}, num(m.value), num(m.error * o.nsigma), Cell{std::string(units)}});
  };
  auto plain = [&](std::string name, Cell value, const char* units) {
    t.rows.push_back({Cell{std::move(name)}, std::move(value), Cell{}, Cell{std::string(units)}});
  };
  row("lambda_e (40K -> 40Ar)", c.lambda_e, "1/a");
  row("lambda_beta (40K -> 40Ca)", c.lambda_b, "1/a");
  row("lambda_K total", r::lambda_k(c), "1/a");
  row("lambda_37Ar", c.lambda_ar37, "1/day");
  row("lambda_39Ar", c.lambda_ar39, "1/day");
  row("lambda_36Cl", c.lambda_cl36, "1/day");
  row("atmospheric 40Ar/36Ar", c.atm4036, "");
  row("atmospheric 40Ar/38Ar", c.atm4038, "");
  plain("abundance sensitivity", num(c.abundance_sensitivity), "");
  if (c.k3739_mode == r::K3739Mode::Fixed) row("fixed (37Ar/39Ar)K", c.fixed_k3739, "");
  if (c.cosmogenic) {
    row("solar 38Ar/36Ar", c.cosmogenic->solar3836, "");
    row("cosmogenic 38Ar/36Ar", c.cosmogenic->cosmo3836, "");
  }
  plain("decay-constant uncertainty in ages", yes_no(c.include_decay_error), "");
  plain("decay time from", Cell{std::string(c.use_irradiation_endtime ? "end of irradiation" : "start of irradiation")},
        "");
  plain("age units", Cell{std::string(age_units_name(c.age_units))}, "");
}

// ---------------------------------------------------------------- irradiation

void irradiation_table(const Dataset& d, const ReportOptions& o, Report& rep) {
  auto& t = rep.irradiation;
  t.name = "irradiation";
  t.title = "Samples, irradiation, J and production ratios, one row per identifier";
  Columns c(t);
  const auto identifier = c.add("identifier");
  const auto sample = c.add("sample");
  const auto material = c.add("material");
  const auto project = c.add("project");
  const auto pi = c.add("principal investigator");
  const auto lat = c.add("latitude (deg)");
  const auto lon = c.add("longitude (deg)");
  const auto elevation = c.add("elevation (m)");
  const auto lithology = c.add("lithology");
  const auto unit = c.add("unit");
  const auto location = c.add("location");
  const auto igsn = c.add("IGSN");
  const auto irradiation = c.add("irradiation");
  const auto level = c.add("level");
  const auto position = c.add("position");
  const auto reactor = c.add("reactor");
  const auto start = c.add("irradiation start (UTC)");
  const auto hours = c.add("irradiation duration (h)");
  const auto doses = c.add("irradiation segments");
  const auto monitor = c.add("fluence monitor");
  const auto monitor_material = c.add("monitor material");
  const auto monitor_age = c.add_pm("monitor age (Ma)", o.nsigma);
  const auto j = c.add_pm("J", o.nsigma);
  const auto j_position = c.add(pm_name("J position error", o.nsigma));
  const auto lambda_k = c.add_pm("lambda_K total override (1/a)", o.nsigma);
  const auto k4039 = c.add_pm("(40Ar/39Ar)K", o.nsigma);
  const auto k3839 = c.add_pm("(38Ar/39Ar)K", o.nsigma);
  const auto k3739 = c.add_pm("(37Ar/39Ar)K", o.nsigma);
  const auto ca3937 = c.add_pm("(39Ar/37Ar)Ca", o.nsigma);
  const auto ca3837 = c.add_pm("(38Ar/37Ar)Ca", o.nsigma);
  const auto ca3637 = c.add_pm("(36Ar/37Ar)Ca", o.nsigma);
  const auto cl3638 = c.add_pm("(36Ar/38Ar)Cl", o.nsigma);
  const auto ca_k = c.add_pm("Ca/K", o.nsigma);
  const auto cl_k = c.add_pm("Cl/K", o.nsigma);

  std::set<std::string> seen;
  for (const auto& it : d.items()) {
    const auto& a = *it.analysis->analysis;
    if (!seen.insert(a.identifier).second) continue;
    Row& row = c.row();
    row[identifier] = txt(a.identifier);
    row[sample] = txt(a.sample);
    row[material] = txt(a.material);
    row[project] = txt(a.project);
    row[pi] = txt(a.principal_investigator);
    row[lat] = num(a.sample_info.latitude);
    row[lon] = num(a.sample_info.longitude);
    row[elevation] = num(a.sample_info.elevation);
    row[lithology] = txt(a.sample_info.lithology);
    row[unit] = txt(a.sample_info.unit);
    row[location] = txt(a.sample_info.location);
    row[igsn] = txt(a.sample_info.igsn);
    row[irradiation] = txt(a.irradiation);
    row[level] = txt(a.level);
    row[position] = txt(a.position);
    row[reactor] = txt(a.context.reactor);
    if (!a.context.chronology.empty()) {
      const auto& ch = a.context.chronology;
      double total = 0;
      for (const auto& dose : ch) total += static_cast<double>(dose.end_utc_s - dose.start_utc_s) / 3600.0;
      row[start] = Cell{detail::format_utc(static_cast<double>(ch.front().start_utc_s))};
      row[hours] = num(total);
      row[doses] = Cell{static_cast<double>(ch.size())};
    }
    row[monitor] = txt(a.monitor.name);
    row[monitor_material] = txt(a.monitor.material);
    if (a.monitor.age) set_pm(row, monitor_age, a.monitor.age->value, a.monitor.age->error, o.nsigma);
    if (a.context.flux) {
      set_pm(row, j, a.context.flux->j, o.nsigma);
      if (a.context.flux->position_jerr > 0) row[j_position] = num(a.context.flux->position_jerr * o.nsigma);
      if (a.context.flux->lambda_k_total &&
          !(a.context.flux->lambda_k_total->value == 0 && a.context.flux->lambda_k_total->error == 0))
        set_pm(row, lambda_k, *a.context.flux->lambda_k_total, o.nsigma);
    }
    if (a.context.production) {
      const auto& p = *a.context.production;
      set_pm(row, k4039, p.k4039, o.nsigma);
      set_pm(row, k3839, p.k3839, o.nsigma);
      set_pm(row, k3739, p.k3739, o.nsigma);
      set_pm(row, ca3937, p.ca3937, o.nsigma);
      set_pm(row, ca3837, p.ca3837, o.nsigma);
      set_pm(row, ca3637, p.ca3637, o.nsigma);
      set_pm(row, cl3638, p.cl3638, o.nsigma);
      if (p.ca_k) set_pm(row, ca_k, *p.ca_k, o.nsigma);
      if (p.cl_k) set_pm(row, cl_k, *p.cl_k, o.nsigma);
    }
  }
}

// ---------------------------------------------------------------- analyses

struct GroupRef {
  int graph = 0, group = 0;
  std::string label;
  GroupItems items;  // step order
};

std::vector<GroupRef> groups_of(const Dataset& d) {
  std::vector<GroupRef> out;
  for (int graph : d.graphs())
    for (const auto& [group, items] : d.groups_of_graph(graph))
      out.push_back({graph, group, group_label(d, graph, group), ordered(items)});
  return out;
}

void analyses_table(const std::vector<GroupRef>& groups, const r::AgeUnits units, const ReportOptions& o,
                    Report& rep) {
  auto& t = rep.analyses;
  t.name = "analyses";
  t.title = "One row per analysis";
  Columns c(t);
  const int ns = o.nsigma;
  const auto run_id = c.add("run id");
  const auto identifier = c.add("identifier");
  const auto sample = c.add("sample");
  const auto material = c.add("material");
  const auto type = c.add("analysis type");
  const auto group = c.add("group");
  const auto aliquot = c.add("aliquot");
  const auto step = c.add("step");
  const auto time = c.add("analysis time (UTC)");
  const auto extract = c.add("extract value");
  const auto extract_units = c.add("extract units");
  const auto duration = c.add("extract duration (s)");
  const auto cleanup = c.add("cleanup (s)");
  const auto weight = c.add("weight (mg)");
  const auto included = c.add("included");
  const auto tag = c.add("tag");
  const Pm iso[5] = {c.add_pm("40Ar (fA)", ns), c.add_pm("39Ar (fA)", ns), c.add_pm("38Ar (fA)", ns),
                     c.add_pm("37Ar (fA)", ns), c.add_pm("36Ar (fA)", ns)};
  const Pm blank[5] = {c.add_pm("40Ar blank (fA)", ns), c.add_pm("39Ar blank (fA)", ns),
                       c.add_pm("38Ar blank (fA)", ns), c.add_pm("37Ar blank (fA)", ns),
                       c.add_pm("36Ar blank (fA)", ns)};
  const auto f = c.add_pm("40Ar*/39ArK", ns);
  const auto yield = c.add_pm("40Ar* (%)", ns);
  const auto k39 = c.add_pm("39ArK (fA)", ns);
  const auto k39_pct = c.add("39ArK (% of group)");
  const auto k39_cum = c.add("39ArK cumulative (%)");
  const auto kca = c.add_pm("K/Ca", ns);
  const auto kcl = c.add_pm("K/Cl", ns);
  const auto x = c.add_pm("39Ar/40Ar", ns);
  const auto y = c.add_pm("36Ar/40Ar", ns);
  const auto rho = c.add("rho (39/40, 36/40)");
  const std::string age_name = std::string("age (") + age_units_name(units) + ")";
  const auto age = c.add(age_name);
  const auto age_analytical = c.add(pm_name(age_name, ns) + " analytical");
  const auto age_j = c.add(pm_name(age_name, ns) + " with J");
  const auto note = c.add("note");

  for (const auto& g : groups) {
    double total39 = 0;
    for (const auto* it : g.items) {
      const double k = k39_of(*it);
      if (std::isfinite(k) && k > 0) total39 += k;
    }
    std::map<const DatasetItem*, IsochronPoint> points;
    for (const auto& p : isochron_points(g.items)) points[p.item] = p;

    double cum = 0;
    for (const auto* it : g.items) {
      const auto& ra = *it->analysis;
      const auto& a = *ra.analysis;
      Row& row = c.row();
      row[run_id] = txt(a.runid);
      row[identifier] = txt(a.identifier);
      row[sample] = txt(a.sample);
      row[material] = txt(a.material);
      row[type] = txt(a.analysis_type);
      row[group] = txt(g.label);
      row[aliquot] = Cell{static_cast<double>(a.aliquot)};
      row[step] = txt(a.step());
      row[time] = Cell{detail::format_utc(a.timestamp)};
      row[extract] = num(a.extraction.value);
      row[extract_units] = txt(a.extraction.units);
      row[duration] = num(a.extraction.duration);
      row[cleanup] = num(a.extraction.cleanup);
      row[weight] = num(a.extraction.weight);
      row[included] = yes_no(it->exclusion.included());
      row[tag] = txt(a.tag);
      for (std::size_t i = 0; i < 5; ++i) {
        const auto name = r::to_string(r::kArgonKeys[i]);
        set_pm(row, iso[i], ra.stage(name, Stage::DecayCorrected), ns);
        set_pm(row, blank[i], ra.stage(name, Stage::Blank), ns);
      }
      if (ra.arar) {
        const auto& res = *ra.arar;
        set_pm(row, f, res.f.f, ns);
        set_pm(row, yield, res.f.radiogenic_yield, ns);
        set_pm(row, k39, res.f.interference.k39, ns);
        const double k = res.f.interference.k39.nominal();
        if (total39 > 0 && std::isfinite(k) && k > 0) {
          row[k39_pct] = num(k / total39 * 100.0);
          cum += k;
          row[k39_cum] = num(cum / total39 * 100.0);
        }
        set_pm(row, kca, res.kca, ns);
        set_pm(row, kcl, res.kcl, ns);
        if (res.ages) {
          row[age] = num(res.ages->age.nominal());
          row[age_analytical] = num(res.ages->age.std_dev() * ns);
          row[age_j] = num(res.ages->age_w_j_err.std_dev() * ns);
        }
      }
      if (auto p = points.find(it); p != points.end()) {
        set_pm(row, x, p->second.x, p->second.sx, ns);
        set_pm(row, y, p->second.y, p->second.sy, ns);
        row[rho] = num(p->second.rho);
      }
      row[note] = txt(ra.reduction_error);
    }
  }
}

// ---------------------------------------------------------------- summary

void summary_table(const std::vector<GroupRef>& groups, const r::AgeUnits units, const ReportOptions& o,
                   Report& rep) {
  auto& t = rep.summary;
  t.name = "summary";
  t.title = "Summary ages, one row per group";
  Columns c(t);
  const int ns = o.nsigma;
  const std::string au = std::string(" (") + age_units_name(units) + ")";
  auto age_columns = [&](const std::string& what) {
    struct AgeCols {
      std::size_t value, analytical, with_j;
    } cols{c.add(what + " age" + au), c.add(pm_name(what + " age", ns) + " analytical"),
           c.add(pm_name(what + " age", ns) + " with J")};
    return cols;
  };
  const auto group = c.add("group");
  const auto samples = c.add("sample");
  const auto identifiers = c.add("identifier");
  const auto materials = c.add("material");
  const auto n_total = c.add("analyses");
  const auto n_included = c.add("included");
  const auto heated = c.add("step heated");
  const auto integrated = age_columns("integrated");
  const auto plateau_steps = c.add("plateau steps");
  const auto plateau_n = c.add("plateau n");
  const auto plateau_gas = c.add("plateau 39ArK (%)");
  const auto plateau = age_columns("plateau");
  const auto plateau_mswd = c.add("plateau MSWD");
  const auto plateau_p = c.add("plateau p");
  const auto mean_n = c.add("weighted mean n");
  const auto mean = age_columns("weighted mean");
  const auto mean_mswd = c.add("weighted mean MSWD");
  const auto mean_p = c.add("weighted mean p");
  const auto iso_n = c.add("isochron n");
  const auto iso = age_columns("isochron");
  const auto trapped = c.add_pm("isochron 40Ar/36Ar trapped", ns);
  const auto iso_mswd = c.add("isochron MSWD");
  const auto iso_p = c.add("isochron p");
  const auto note = c.add("note");

  auto set_age = [&](Row& row, const auto& cols, double value, double analytical, double with_j) {
    row[cols.value] = num(value);
    row[cols.analytical] = num(analytical * ns);
    row[cols.with_j] = num(with_j * ns);
  };

  for (const auto& g : groups) {
    Row& row = c.row();
    std::set<std::string> ss, ids, ms;
    GroupItems reduced, included;
    bool all_steps = true;
    std::vector<std::string> notes;
    for (const auto* it : g.items) {
      const auto& a = *it->analysis->analysis;
      if (!a.sample.empty()) ss.insert(a.sample);
      if (!a.identifier.empty()) ids.insert(a.identifier);
      if (!a.material.empty()) ms.insert(a.material);
      if (a.increment < 0) all_steps = false;
      if (has_ages(*it)) {
        reduced.push_back(it);
        if (it->exclusion.included()) included.push_back(it);
      } else if (it->exclusion.included() && !it->analysis->reduction_error.empty()) {
        notes.push_back(a.runid + ": " + it->analysis->reduction_error);
      }
    }
    row[group] = txt(g.label);
    row[samples] = txt(join(ss));
    row[identifiers] = txt(join(ids));
    row[materials] = txt(join(ms));
    row[n_total] = Cell{static_cast<double>(g.items.size())};
    row[n_included] = Cell{static_cast<double>(included.size())};
    const bool step_heated = all_steps && g.items.size() >= 2;
    row[heated] = yes_no(step_heated);
    const double j_rel = j_relative_error(reduced);

    // Integrated (total gas) age.
    {
      const GroupItems& chosen = o.integrated_includes_excluded ? reduced : included;
      auto plain = integrated_age(chosen, false);
      auto with_j = integrated_age(chosen, true);
      if (plain && with_j) set_age(row, integrated, plain->nominal(), plain->std_dev(), with_j->std_dev());
    }

    // Plateau, over the steps with an age and gas, excluded ones flagged.
    if (step_heated && !reduced.empty()) {
      std::vector<double> vs, es, ws;
      std::vector<const DatasetItem*> steps;
      for (const auto* it : reduced) {
        const double k = k39_of(*it);
        if (!(k > 0)) continue;
        const auto age = analytical_age(*it);
        vs.push_back(age->nominal());
        es.push_back(age->std_dev());
        ws.push_back(k);
        steps.push_back(it);
      }
      const std::size_t n = steps.size();
      std::unique_ptr<bool[]> ex(new bool[std::max<std::size_t>(1, n)]);
      for (std::size_t i = 0; i < n; ++i) ex[i] = steps[i]->exclusion.excluded();
      const std::span<const bool> excluded(ex.get(), n);
      if (auto range = r::find_plateau(vs, es, ws, excluded, o.plateau)) {
        if (auto pm = r::plateau_mean(vs, es, ws, excluded, *range, o.plateau_weighting, o.mean_error)) {
          row[plateau_steps] = Cell{steps[range->first]->analysis->analysis->step() + "-" +
                                    steps[range->last]->analysis->analysis->step()};
          row[plateau_n] = Cell{static_cast<double>(pm->nsteps)};
          row[plateau_gas] = num(pm->gas_fraction);
          set_age(row, plateau, pm->mean.value, pm->mean.error,
                  with_external_error(pm->mean.value, pm->mean.error, j_rel));
          row[plateau_mswd] = num(pm->mean.mswd);
          row[plateau_p] = num(r::mswd_probability(pm->mean.mswd, static_cast<int>(pm->nsteps) - 1));
        }
      }
    }

    // Weighted mean of the included ages.
    if (!included.empty()) {
      std::vector<double> vs, es;
      for (const auto* it : included) {
        const auto age = analytical_age(*it);
        vs.push_back(age->nominal());
        es.push_back(age->std_dev());
      }
      if (auto m = r::weighted_mean(vs, es, o.mean_error)) {
        row[mean_n] = Cell{static_cast<double>(m->n)};
        set_age(row, mean, m->value, m->error, with_external_error(m->value, m->error, j_rel));
        row[mean_mswd] = num(m->mswd);
        row[mean_p] = num(r::mswd_probability(m->mswd, static_cast<int>(m->n) - 1));
      }
    }

    // Inverse isochron of the included analyses.
    {
      const auto points = isochron_points(included);
      auto plain = isochron_age(points, o.york, false, false);
      auto with_j = isochron_age(points, o.york, false, true);
      if (plain && with_j) {
        row[iso_n] = Cell{static_cast<double>(plain->fit.n)};
        if (plain->age && with_j->age)
          set_age(row, iso, plain->age->nominal(), plain->age->std_dev(), with_j->age->std_dev());
        set_pm(row, trapped, plain->trapped.value, plain->trapped.error, ns);
        row[iso_mswd] = num(plain->fit.mswd);
        row[iso_p] = num(plain->fit.probability);
      }
    }

    if (reduced.empty()) notes.insert(notes.begin(), "no analysis of this group has an age");
    std::string text;
    for (const auto& n : notes) text += (text.empty() ? "" : "; ") + n;
    row[note] = txt(text);
  }
}

// ---------------------------------------------------------------- writers

std::string format_number(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.12g", v);
  return buf;
}

std::string csv_field(const Cell& cell) {
  if (const auto* d = std::get_if<double>(&cell)) return format_number(*d);
  const auto* s = std::get_if<std::string>(&cell);
  return s ? csv_quote(*s) : std::string();
}

void csv_table(std::string& out, const ReportTable& t) {
  out += "[" + t.name + "]\n";
  for (std::size_t i = 0; i < t.columns.size(); ++i) out += (i ? "," : "") + csv_field(Cell{t.columns[i]});
  out += '\n';
  for (const auto& row : t.rows) {
    for (std::size_t i = 0; i < row.size(); ++i) out += (i ? "," : "") + csv_field(row[i]);
    out += '\n';
  }
  out += '\n';
}

void json_string(std::string& out, std::string_view s) {
  out += '"';
  for (const unsigned char ch : s) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", ch);
          out += buf;
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  out += '"';
}

void json_cell(std::string& out, const Cell& cell) {
  if (const auto* d = std::get_if<double>(&cell)) {
    out += std::isfinite(*d) ? format_number(*d) : "null";
  } else if (const auto* s = std::get_if<std::string>(&cell)) {
    json_string(out, *s);
  } else {
    out += "null";
  }
}

void json_rows(std::string& out, const ReportTable& t) {
  out += '[';
  for (std::size_t r = 0; r < t.rows.size(); ++r) {
    out += r ? ",\n    {" : "\n    {";
    for (std::size_t i = 0; i < t.columns.size() && i < t.rows[r].size(); ++i) {
      if (i) out += ", ";
      json_string(out, t.columns[i]);
      out += ": ";
      json_cell(out, t.rows[r][i]);
    }
    out += '}';
  }
  out += t.rows.empty() ? "]" : "\n  ]";
}

void json_strings(std::string& out, const std::vector<std::string>& items) {
  out += '[';
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += ", ";
    json_string(out, items[i]);
  }
  out += ']';
}

}  // namespace

const ReportTable* Report::table(std::string_view name) const {
  for (const auto* t : {&metadata, &constants, &irradiation, &analyses, &summary})
    if (t->name == name) return t;
  return nullptr;
}

Report make_report(const Dataset& dataset, const ReportOptions& options) {
  Report rep;
  ReportOptions o = options;
  o.nsigma = std::max(o.nsigma, 1);
  const Inventory inv = take_inventory(dataset, o.nsigma);
  rep.header.push_back("40Ar/39Ar data report after " + std::string(kStandard));
  rep.header.push_back("Generated by " + (o.software.empty() ? std::string("pychron-cpp") : o.software) + " on " +
                       detail::format_utc(now_utc_seconds()));
  rep.header.push_back("Uncertainties in every ± column are " + std::to_string(o.nsigma) +
                       " sigma; ages are analytical and with J, see [metadata]");
  metadata_table(dataset, o, inv, rep);
  constants_table(o, inv, rep);
  irradiation_table(dataset, o, rep);
  const r::AgeUnits units = inv.reference ? inv.reference->constants->age_units : r::AgeUnits::Ma;
  const auto groups = groups_of(dataset);
  analyses_table(groups, units, o, rep);
  summary_table(groups, units, o, rep);
  if (dataset.empty()) rep.warnings.emplace_back("the dataset is empty");
  return rep;
}

std::string csv_quote(std::string_view text) {
  const bool quote = text.find_first_of(",\"\r\n") != std::string_view::npos ||
                     (!text.empty() && (text.front() == ' ' || text.back() == ' '));
  if (!quote) return std::string(text);
  std::string out = "\"";
  for (const char ch : text) {
    if (ch == '"') out += '"';
    out += ch;
  }
  out += '"';
  return out;
}

std::string report_csv(const Report& report) {
  std::string out;
  for (const auto& line : report.header) out += "# " + line + "\n";
  for (const auto& w : report.warnings) out += "# warning: " + w + "\n";
  out += '\n';
  for (const auto* t : {&report.metadata, &report.constants, &report.irradiation, &report.analyses, &report.summary})
    csv_table(out, *t);
  return out;
}

std::string report_json(const Report& report) {
  std::string out = "{\n  \"header\": ";
  json_strings(out, report.header);
  out += ",\n  \"warnings\": ";
  json_strings(out, report.warnings);
  out += ",\n  \"metadata\": {";
  for (std::size_t i = 0; i < report.metadata.rows.size(); ++i) {
    const auto& row = report.metadata.rows[i];
    if (row.size() < 2) continue;
    out += i ? ",\n    " : "\n    ";
    json_cell(out, row[0]);
    out += ": ";
    json_cell(out, row[1]);
  }
  out += report.metadata.rows.empty() ? "}" : "\n  }";
  for (const auto* t : {&report.constants, &report.irradiation, &report.analyses, &report.summary}) {
    out += ",\n  ";
    json_string(out, t->name);
    out += ": ";
    json_rows(out, *t);
  }
  out += "\n}\n";
  return out;
}

Result<void> save_report(const Report& report, const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  const std::string text = ext == ".json" ? report_json(report) : report_csv(report);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return fail(ErrorKind::Io, "report: cannot write " + path.string());
  out << text;
  out.close();
  if (!out) return fail(ErrorKind::Io, "report: writing " + path.string() + " failed");
  mark_as_user_file(path);  // the user's own output: no quarantine on macOS
  return {};
}

}  // namespace pychron::processing
