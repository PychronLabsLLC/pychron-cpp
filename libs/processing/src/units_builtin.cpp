// The V1 built-in units (design section 7.5): select, reduce, filter, group,
// edits, group_stats. time_series lives in time_series.cpp.

#include <algorithm>
#include <cmath>
#include <map>

#include "pychron/processing/arar_figures.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/time_series.hpp"
#include "pychron/processing/units.hpp"
#include "schema_builder.hpp"

namespace pychron::processing {

namespace {

using namespace detail;

const DatasetPtr& dataset_in(const std::vector<PortValue>& in, std::size_t i = 0) { return std::get<DatasetPtr>(in.at(i)); }

std::vector<PortValue> one(DatasetPtr d) { return {PortValue(std::move(d))}; }

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// ---------------------------------------------------------------- select

class SelectUnit final : public Unit {
 public:
  SelectUnit()
      : schema_(make_schema(
            "unit.select", "Select analyses",
            {strings("uuids", "Analyses", "Selection", {}, "Explicit analysis uuids; when set, the query is ignored"),
             text("text", "Run id / identifier / sample starts with", "Query"),
             strings("analysis_types", "Analysis types", "Query"),
             strings("identifiers", "Identifiers", "Query"),
             strings("samples", "Samples", "Query"),
             strings("projects", "Projects", "Query"),
             strings("mass_spectrometers", "Mass spectrometers", "Query"),
             strings("extract_devices", "Extract devices", "Query"),
             strings("loads", "Loads", "Query"),
             strings("irradiations", "Irradiations", "Query"),
             optional_number("from", "From (UTC epoch s)", "Query"),
             optional_number("to", "To (UTC epoch s)", "Query"),
             optional_number("last_hours", "Last hours", "Query"),
             integer("limit", "At most", "Query", 5000, 1, 1000000),
             strings("remove_tags", "Drop tags", "Tags", {"invalid"}, "Analyses with these tags are left out"),
             strings("omit_tags", "Omit tags", "Tags", {"omit", "outlier"},
                     "Analyses with these tags are kept but excluded from statistics")})) {}

  std::string_view kind() const override { return "select"; }
  std::string_view title() const override { return "Select"; }
  const SchemaPtr& schema() const override { return schema_; }
  std::vector<PortSpec> inputs() const override { return {}; }
  std::vector<PortSpec> outputs() const override { return {{"analyses", PortType::Dataset}}; }
  bool reads_source() const override { return true; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>&, const Options& o,
                                         RunContext& ctx) const override {
    if (!ctx.source) return fail(ErrorKind::Config, "select: no analysis source");
    std::vector<std::string> uuids = o.get_strings("uuids");
    const auto remove_tags = o.get_strings("remove_tags");
    const auto omit_tags = o.get_strings("omit_tags");
    if (uuids.empty()) {
      BrowseQuery q;
      q.text = o.get_string("text");
      q.analysis_types = o.get_strings("analysis_types");
      q.identifiers = o.get_strings("identifiers");
      q.samples = o.get_strings("samples");
      q.projects = o.get_strings("projects");
      q.mass_spectrometers = o.get_strings("mass_spectrometers");
      q.extract_devices = o.get_strings("extract_devices");
      q.loads = o.get_strings("loads");
      q.irradiations = o.get_strings("irradiations");
      q.from = o.get_optional_double("from");
      q.to = o.get_optional_double("to");
      q.last_hours = o.get_optional_double("last_hours");
      q.exclude_tags = remove_tags;
      const auto limit = static_cast<std::size_t>(o.get_int("limit"));
      q.limit = 500;
      while (uuids.size() < limit) {
        auto page = ctx.source->browse(q);
        if (!page) return fail(page.error());
        for (const auto& r : page->rows)
          if (uuids.size() < limit) uuids.push_back(r.uuid);
        if (!page->next) break;
        q.after = page->next;
        if (ctx.cancelled()) return fail(ErrorKind::Cancelled, "select: cancelled");
      }
    }
    Dataset d;
    std::size_t missing = 0;
    for (const auto& id : uuids) {
      if (ctx.cancelled()) return fail(ErrorKind::Cancelled, "select: cancelled");
      auto a = ctx.source->load(id);
      if (!a) {
        ++missing;
        continue;
      }
      if (contains(remove_tags, (*a)->tag)) continue;
      auto ra = std::make_shared<ReducedAnalysis>();
      ra->analysis = *a;
      DatasetItem item;
      item.analysis = std::move(ra);
      item.exclusion.tag = contains(omit_tags, (*a)->tag);
      d.mutable_items().push_back(std::move(item));
    }
    if (missing) ctx.diagnostics.push_back(std::to_string(missing) + " analyses could not be loaded");
    // Oldest first: the natural order of a run sequence.
    std::stable_sort(d.mutable_items().begin(), d.mutable_items().end(), [](const auto& a, const auto& b) {
      return a.analysis->analysis->timestamp < b.analysis->analysis->timestamp;
    });
    d.reduction_tag = "unreduced";
    return one(make_dataset(std::move(d)));
  }

 private:
  SchemaPtr schema_;
};

// ---------------------------------------------------------------- reduce

class ReduceUnit final : public Unit {
 public:
  ReduceUnit()
      : schema_(make_schema("unit.reduce", "Reduce",
                            {choice("constants", "Constants", "Reduction", {"default", "legacy", "legacy_preferences"}),
                             boolean("include_decay_error", "Include decay constant error", "Reduction", false),
                             boolean("use_irradiation_endtime", "Decay from the irradiation end", "Reduction", false)})) {}

  std::string_view kind() const override { return "reduce"; }
  std::string_view title() const override { return "Reduce"; }
  const SchemaPtr& schema() const override { return schema_; }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"reduced", PortType::Dataset}}; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext& ctx) const override {
    ReductionSettings s;
    const std::string c = o.get_string("constants");
    s.preset = c == "legacy"               ? reduction::ConstantsPreset::Legacy
               : c == "legacy_preferences" ? reduction::ConstantsPreset::LegacyPreferences
                                           : reduction::ConstantsPreset::Default;
    s.include_decay_error = o.get_bool("include_decay_error");
    s.use_irradiation_endtime = o.get_bool("use_irradiation_endtime");
    Dataset d = *dataset_in(in);
    std::size_t failed = 0;
    for (auto& item : d.mutable_items()) {
      if (ctx.cancelled()) return fail(ErrorKind::Cancelled, "reduce: cancelled");
      item.analysis = reduce_analysis(item.analysis->analysis, s);
      if (!item.analysis->reduction_error.empty()) ++failed;
    }
    if (failed) ctx.diagnostics.push_back(std::to_string(failed) + " analyses did not reduce");
    d.reduction_tag = std::string(reduction::kReductionVersion) + "/" + c + "/" + (s.include_decay_error ? "d1" : "d0") +
                      (s.use_irradiation_endtime ? "e1" : "e0");
    return one(make_dataset(std::move(d)));
  }

 private:
  SchemaPtr schema_;
};

// ---------------------------------------------------------------- filter

SchemaPtr filter_rule_schema() {
  static const SchemaPtr s = make_schema(
      "unit.filter.rule", "Rule",
      {quantity("quantity", "Quantity", "Rule", "age"),
       choice("comparator", "Comparator", "Rule", {"<", "<=", ">", ">=", "==", "!=", "between", "outside"}, ">"),
       number("value", "Value", "Rule", 0.0, -1e300, 1e300), optional_number("value2", "Second value", "Rule"),
       choice("chain", "Combine with previous", "Rule", {"and", "or"})});
  return s;
}

class FilterUnit final : public Unit {
 public:
  FilterUnit() {
    ListSpec rules;
    rules.key = "rules";
    rules.label = "Rules";
    rules.section = "Filter";
    rules.row = filter_rule_schema();
    rules.max_rows = 16;
    schema_ = make_schema("unit.filter", "Filter",
                          {choice("mode", "Analyses failing the rules are", "Filter", {"omitted", "removed"}),
                           choice("missing", "Analyses without the quantity", "Filter", {"pass", "fail"})},
                          {rules});
  }

  std::string_view kind() const override { return "filter"; }
  std::string_view title() const override { return "Filter"; }
  const SchemaPtr& schema() const override { return schema_; }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"filtered", PortType::Dataset}}; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    struct Rule {
      Quantity q;
      std::string cmp;
      double a;
      std::optional<double> b;
      bool is_or;
    };
    std::vector<Rule> rules;
    for (const auto& r : o.rows("rules")) {
      auto q = Quantity::parse(r.get_string("quantity"));
      if (!q) return fail(q.error());
      rules.push_back({*q, r.get_string("comparator"), r.get_double("value"), r.get_optional_double("value2"),
                       r.get_string("chain") == "or"});
    }
    const bool remove = o.get_string("mode") == "removed";
    const bool missing_passes = o.get_string("missing") == "pass";
    Dataset src = *dataset_in(in);
    Dataset out;
    out.group_names = src.group_names;
    out.graph_names = src.graph_names;
    out.reduction_tag = src.reduction_tag;
    for (auto item : src.items()) {
      bool pass = true;
      for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        bool ok = missing_passes;
        if (auto v = r.q.eval(*item.analysis)) {
          const double x = v->value, hi = r.b.value_or(r.a);
          const double lo = std::min(r.a, hi), up = std::max(r.a, hi);
          if (r.cmp == "<") ok = x < r.a;
          else if (r.cmp == "<=") ok = x <= r.a;
          else if (r.cmp == ">") ok = x > r.a;
          else if (r.cmp == ">=") ok = x >= r.a;
          else if (r.cmp == "==") ok = x == r.a;
          else if (r.cmp == "!=") ok = x != r.a;
          else if (r.cmp == "between") ok = x >= lo && x <= up;
          else if (r.cmp == "outside") ok = x < lo || x > up;
        }
        pass = i == 0 ? ok : (r.is_or ? (pass || ok) : (pass && ok));
      }
      if (!pass && remove) continue;
      item.exclusion.filter = !pass;
      out.mutable_items().push_back(std::move(item));
    }
    return one(make_dataset(std::move(out)));
  }

 private:
  SchemaPtr schema_;
};

// ---------------------------------------------------------------- group

std::string group_key(const Analysis& a, const std::string& key, double bin_hours, double t0) {
  if (key == "identifier") return a.identifier;
  if (key == "sample") return a.sample;
  if (key == "aliquot") return a.identifier + "-" + std::to_string(a.aliquot);
  if (key == "analysis_type") return a.analysis_type;
  if (key == "mass_spectrometer") return a.mass_spectrometer;
  if (key == "extract_device") return a.extract_device;
  if (key == "step") return a.step();
  if (key == "load") return a.load;
  if (key == "irradiation") return a.irradiation + (a.level.empty() ? "" : " " + a.level);
  if (key == "project") return a.project;
  if (key == "material") return a.material;
  if (key == "time_bin") {
    const double h = bin_hours > 0 ? bin_hours : 24.0;
    const long bin = static_cast<long>(std::floor((a.timestamp - t0) / (h * 3600.0)));
    return "bin " + std::to_string(bin + 1);
  }
  return {};
}

class GroupUnit final : public Unit {
 public:
  GroupUnit()
      : schema_(make_schema(
            "unit.group", "Group",
            {choice("level", "Level", "Grouping", {"group", "graph", "tab"}),
             choice("key", "Group by", "Grouping",
                    {"none", "identifier", "sample", "aliquot", "analysis_type", "mass_spectrometer", "extract_device",
                     "step", "load", "irradiation", "project", "material", "time_bin"}),
             when(number("bin_hours", "Bin (hours)", "Grouping", 24.0, 0.01, 1e6), "key == time_bin")})) {}

  std::string_view kind() const override { return "group"; }
  std::string_view title() const override { return "Group"; }
  const SchemaPtr& schema() const override { return schema_; }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"grouped", PortType::Dataset}}; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    Dataset d = *dataset_in(in);
    const std::string level = o.get_string("level"), key = o.get_string("key");
    const double bin = o.get_double("bin_hours");
    double t0 = 0;
    bool first = true;
    for (const auto& it : d.items()) {
      const double t = it.analysis->analysis->timestamp;
      if (first || t < t0) t0 = t;
      first = false;
    }
    // Ids by first appearance in time order.
    std::vector<std::size_t> order(d.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return d.items()[a].analysis->analysis->timestamp < d.items()[b].analysis->analysis->timestamp;
    });
    std::map<std::string, int> ids;
    std::vector<std::string> names;
    for (std::size_t i : order) {
      auto& item = d.mutable_items()[i];
      const std::string k = key == "none" ? std::string() : group_key(*item.analysis->analysis, key, bin, t0);
      auto [it, inserted] = ids.emplace(k, static_cast<int>(names.size()));
      if (inserted) names.push_back(k.empty() && key != "none" ? "(none)" : k);
      int& target = level == "graph" ? item.path.graph : level == "tab" ? item.path.tab : item.path.group;
      target = it->second;
    }
    if (key == "none") names.assign(1, "");
    if (level == "group")
      d.group_names = names;
    else if (level == "graph")
      d.graph_names = names;
    return one(make_dataset(std::move(d)));
  }

 private:
  SchemaPtr schema_;
};

// ---------------------------------------------------------------- edits

class EditsUnit final : public Unit {
 public:
  EditsUnit()
      : schema_(make_schema(
            "unit.edits", "User edits",
            {strings("exclude", "Excluded by the user", "Edits"), strings("include", "Included by the user", "Edits"),
             strings("groups", "Group assignments", "Edits", {}, "uuid=group index")})) {}

  std::string_view kind() const override { return "edits"; }
  std::string_view title() const override { return "Edits"; }
  const SchemaPtr& schema() const override { return schema_; }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"edited", PortType::Dataset}}; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    const auto ex = o.get_strings("exclude"), inc = o.get_strings("include");
    std::map<std::string, int> groups;
    for (const auto& g : o.get_strings("groups")) {
      const auto eq = g.find('=');
      if (eq == std::string::npos) continue;
      try {
        groups[g.substr(0, eq)] = std::stoi(g.substr(eq + 1));
      } catch (...) {
      }
    }
    Dataset d = *dataset_in(in);
    int max_group = -1;
    for (const auto& it : d.items()) max_group = std::max(max_group, it.path.group);
    for (auto& item : d.mutable_items()) {
      const auto& uuid = item.analysis->analysis->uuid;
      if (contains(ex, uuid)) item.exclusion.user = true;
      else if (contains(inc, uuid)) item.exclusion.user = false;
      if (auto g = groups.find(uuid); g != groups.end()) {
        item.path.group = g->second;
        max_group = std::max(max_group, g->second);
      }
    }
    while (static_cast<int>(d.group_names.size()) <= max_group && !groups.empty())
      d.group_names.push_back("Group " + std::to_string(d.group_names.size() + 1));
    return one(make_dataset(std::move(d)));
  }

 private:
  SchemaPtr schema_;
};

// ---------------------------------------------------------------- group_stats

class GroupStatsUnit final : public Unit {
 public:
  GroupStatsUnit()
      : schema_(make_schema("unit.group_stats", "Group statistics",
                            {quantity("quantity", "Quantity", "Statistics", "age"),
                             choice("mean", "Mean", "Statistics", {"weighted", "arithmetic"}),
                             choice("error_kind", "Error", "Statistics", {"msem", "sem", "sd"})})) {}

  std::string_view kind() const override { return "group_stats"; }
  std::string_view title() const override { return "Group statistics"; }
  const SchemaPtr& schema() const override { return schema_; }
  std::vector<PortSpec> inputs() const override { return {{"analyses", PortType::Dataset}}; }
  std::vector<PortSpec> outputs() const override { return {{"results", PortType::GroupResults}}; }

  Result<std::vector<PortValue>> execute(const std::vector<PortValue>& in, const Options& o,
                                         RunContext&) const override {
    auto q = Quantity::parse(o.get_string("quantity"));
    if (!q) return fail(q.error());
    const auto kind = reduction::parse_mean_error_kind(o.get_string("error_kind")).value_or(reduction::MeanErrorKind::Msem);
    const bool weighted = o.get_string("mean") == "weighted";
    const Dataset& d = *dataset_in(in);
    GroupResults out;
    for (int graph : d.graphs()) {
      for (const auto& [group, items] : d.groups_of_graph(graph)) {
        GroupResult r;
        r.graph = graph;
        r.group = group;
        r.name = d.group_name(group);
        r.quantity = q->text();
        r.n_total = items.size();
        std::vector<double> v, e;
        for (const auto* it : items) {
          if (!it->exclusion.included()) continue;
          if (auto x = q->eval(*it->analysis)) {
            v.push_back(x->value);
            e.push_back(x->error);
          }
        }
        r.n_included = v.size();
        auto m = weighted ? reduction::weighted_mean(v, e, kind) : reduction::arithmetic_mean(v, e, kind);
        if (m)
          r.mean = *m;
        else
          r.error = m.error().what;
        out.rows.push_back(std::move(r));
      }
    }
    return std::vector<PortValue>{PortValue(std::make_shared<const GroupResults>(std::move(out)))};
  }

 private:
  SchemaPtr schema_;
};

}  // namespace

const UnitRegistry& UnitRegistry::builtin() {
  static const UnitRegistry registry = [] {
    UnitRegistry r;
    r.add(std::make_unique<SelectUnit>());
    r.add(std::make_unique<ReduceUnit>());
    r.add(std::make_unique<FilterUnit>());
    r.add(std::make_unique<GroupUnit>());
    r.add(std::make_unique<EditsUnit>());
    r.add(std::make_unique<GroupStatsUnit>());
    r.add(make_time_series_unit());
    r.add(make_ideogram_unit());
    r.add(make_spectrum_unit());
    r.add(make_isochron_unit());
    return r;
  }();
  return registry;
}

}  // namespace pychron::processing
