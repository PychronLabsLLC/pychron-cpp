#include "pychron/experiment/factory/form.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "pychron/experiment/model/positions.hpp"
#include "pychron/experiment/model/rules.hpp"
#include "pychron/core/number.hpp"

namespace pychron::experiment {

namespace {

constexpr std::size_t kMaxStepValues = 1000;

Unexpected<Error> form_error(std::string what) { return fail(ErrorKind::Config, std::move(what), "run factory"); }

std::string trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string_view::npos) return {};
  const auto e = s.find_last_not_of(" \t");
  return std::string(s.substr(b, e - b + 1));
}

std::optional<double> number(std::string_view s) { return parse_double(trim(s)); }

}  // namespace

Result<std::vector<double>> parse_step_values(std::string_view text) {
  std::vector<double> out;
  const std::string t = trim(text);
  if (t.empty()) return out;
  if (t.find(':') != std::string::npos) {
    std::vector<std::string_view> parts;
    std::string_view rest = t;
    for (auto at = rest.find(':'); at != std::string_view::npos; at = rest.find(':')) {
      parts.push_back(rest.substr(0, at));
      rest.remove_prefix(at + 1);
    }
    parts.push_back(rest);
    if (parts.size() != 3) return form_error("step heat '" + t + "': expected start:increment:count");
    auto start = number(parts[0]), inc = number(parts[1]), count = number(parts[2]);
    if (!start || !inc || !count || *count != std::floor(*count))
      return form_error("step heat '" + t + "': expected numbers start:increment:count");
    if (*count < 1 || *count > static_cast<double>(kMaxStepValues))
      return form_error("step heat '" + t + "': count must be 1 to " + std::to_string(kMaxStepValues));
    return step_values(*start, *inc, static_cast<int>(*count));
  }
  std::string token;
  auto flush = [&]() -> Result<void> {
    if (token.empty()) return {};
    auto v = number(token);
    if (!v) return form_error("step heat: '" + token + "' is not a number");
    out.push_back(*v);
    token.clear();
    return {};
  };
  for (char c : t) {
    if (c == ',' || c == ';' || c == ' ' || c == '\t') {
      if (auto r = flush(); !r) return fail(r.error());
    } else {
      token += c;
    }
  }
  if (auto r = flush(); !r) return fail(r.error());
  if (out.size() > kMaxStepValues) return form_error("step heat: more than " + std::to_string(kMaxStepValues) + " values");
  return out;
}

FieldRules form_rules(const FactoryForm& form, const IdentifierRules& ids) {
  return rules_for(ids.classify(trim(form.identifier)));
}

namespace {

// The single run the form describes (position unsplit), before stripping.
Result<RunSpec> template_run(const FactoryForm& form, const IdentifierRules& ids) {
  RunSpec r;
  r.id.identifier = trim(form.identifier);
  if (r.id.identifier.empty()) return form_error("identifier is empty");
  if (auto ok = ids.validate_identifier(r.id.identifier); !ok) return fail(ok.error());
  r.id.type = ids.classify(r.id.identifier);
  r.id.aliquot = form.aliquot;
  r.id.step = trim(form.step);
  if (!r.id.step.empty() && !ids.valid_step(r.id.step)) return form_error("step '" + r.id.step + "' is not valid");
  auto& e = r.extraction;
  e.device = trim(form.extract_device);
  if (const std::string p = trim(form.position); !p.empty()) {
    auto pos = parse_position(p);
    if (!pos) return fail(pos.error());
    e.position = std::move(*pos);
  }
  e.value = form.value;
  e.units = form.units;
  e.duration = Duration(form.duration_s);
  e.cleanup = Duration(form.cleanup_s);
  e.script = trim(form.script);
  r.measurement.plan = trim(form.plan);
  r.measurement.overrides = form.overrides;
  if (auto s = trim(form.post_equilibration); !s.empty()) r.post_equilibration = s;
  if (auto s = trim(form.post_measurement); !s.empty()) r.post_measurement = s;
  r.comment = form.comment;
  for (const auto& name : form.conditionals) r.conditionals.push_back(ConditionalRef{name});
  return r;
}

}  // namespace

Result<std::vector<RunSpec>> build_runs(const FactoryForm& form, const IdentifierRules& ids) {
  auto tmpl = template_run(form, ids);
  if (!tmpl) return fail(tmpl.error());
  const FieldRules rules = rules_for(tmpl->id.type);
  auto values = parse_step_values(form.step_heat);
  if (!values) return fail(values.error());
  strip_for_type(*tmpl);

  std::vector<RunSpec> runs;
  if (!values->empty()) {
    if (!rules.heating)
      return form_error("a step heat needs an analysis type that heats ('" + std::string(to_string(tmpl->id.type)) + "' does not)");
    runs = make_step_heat(*tmpl, *values);
  } else if (form.one_run_per_hole && tmpl->extraction.position && tmpl->extraction.position->holes.size() > 1) {
    if (form.identifier_step != 0 && !ids.is_special(tmpl->id.identifier)) {
      auto expanded = expand_positions(*tmpl, *tmpl->extraction.position, form.identifier_step);
      if (!expanded) return fail(expanded.error());
      runs = std::move(*expanded);
    } else {
      runs = expand_positions(*tmpl, *tmpl->extraction.position);
    }
  } else {
    runs.push_back(std::move(*tmpl));
  }
  return runs;
}

FactoryForm form_from_run(const RunSpec& run) {
  FactoryForm f;
  f.identifier = run.id.identifier;
  f.aliquot = run.id.aliquot;
  f.step = run.id.step;
  const auto& e = run.extraction;
  f.extract_device = e.device;
  if (e.position) f.position = format_position(*e.position);
  f.value = e.value;
  f.units = e.units;
  f.duration_s = e.duration.count();
  f.cleanup_s = e.cleanup.count();
  f.script = e.script;
  f.plan = run.measurement.plan;
  f.overrides = run.measurement.overrides;
  f.post_equilibration = run.post_equilibration.value_or("");
  f.post_measurement = run.post_measurement.value_or("");
  f.comment = run.comment;
  for (const auto& c : run.conditionals) f.conditionals.push_back(c.name);
  return f;
}

std::optional<FactoryForm> with_lab_defaults(const FactoryForm& form, const IdentifierRules& ids,
                                             const DefaultsTable& defaults) {
  const AnalysisType type = ids.classify(trim(form.identifier));
  const RunDefaults* d = defaults.find(type, trim(form.extract_device));
  if (d == nullptr) return std::nullopt;
  FactoryForm out = form;
  out.plan = d->template_name;
  out.overrides = d->overrides;
  out.script = d->script;
  out.post_equilibration = d->post_equilibration.value_or("");
  out.post_measurement = d->post_measurement.value_or("");
  if (d->units) out.units = *d->units;
  if (d->value) out.value = *d->value;
  if (d->duration) out.duration_s = d->duration->count();
  if (d->cleanup) out.cleanup_s = d->cleanup->count();
  return out;
}

Result<FactoryForm> next_form(const FactoryForm& form, const IncrementOptions& inc, const IdentifierRules& ids) {
  auto runs = build_runs(form, ids);
  if (!runs) return fail(runs.error());
  const RunSpec& last = runs->back();
  FactoryForm next = form;
  if (inc.identifier != 0 && !ids.is_special(last.id.identifier)) {
    auto id = increment_identifier(last.id.identifier, inc.identifier);
    if (!id) return fail(id.error());
    if (*id != form.identifier) {
      next.identifier = std::move(*id);
      next.aliquot.reset();
      next.step.clear();
    }
  }
  if (inc.position != 0 && !trim(form.position).empty()) {
    auto pos = parse_position(trim(form.position));
    if (!pos) return fail(pos.error());
    int last_hole = 0;
    for (const auto& r : *runs)
      if (r.extraction.position)
        for (int h : r.extraction.position->holes) last_hole = std::max(last_hole, h);
    // Shift the whole position so its first hole lands inc.position past the last hole added.
    const int first = *std::min_element(pos->holes.begin(), pos->holes.end());
    auto shifted = increment_position(*pos, last_hole + inc.position - first);
    if (!shifted) return fail(shifted.error());
    next.position = format_position(*shifted);
  }
  return next;
}

}  // namespace pychron::experiment
