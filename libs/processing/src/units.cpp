#include "pychron/processing/units.hpp"

#include <algorithm>
#include <functional>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/core/sha256.hpp"

namespace pychron::processing {

namespace {

Unexpected<Error> pipe_fail(std::string what) { return fail(ErrorKind::Config, "pipeline: " + std::move(what)); }

struct InputRef {
  std::string node;
  std::size_t port = 0;
};

std::optional<InputRef> parse_input(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) return InputRef{text, 0};
  InputRef r{text.substr(0, colon), 0};
  const std::string port = text.substr(colon + 1);
  if (port.empty() || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  r.port = static_cast<std::size_t>(std::stoul(port));
  return r;
}

}  // namespace

std::string_view to_string(PortType t) noexcept {
  switch (t) {
    case PortType::Dataset:
      return "dataset";
    case PortType::Scene:
      return "scene";
    case PortType::GroupResults:
      return "group_results";
  }
  return "dataset";
}

PortType port_type(const PortValue& v) noexcept {
  switch (v.index()) {
    case 0:
      return PortType::Dataset;
    case 1:
      return PortType::Scene;
    default:
      return PortType::GroupResults;
  }
}

DatasetPtr make_dataset(Dataset dataset) { return std::make_shared<const Dataset>(std::move(dataset)); }

// ---------------------------------------------------------------- registry

void UnitRegistry::add(std::unique_ptr<Unit> unit) {
  const std::string kind(unit->kind());
  units_[kind] = std::move(unit);
}

const Unit* UnitRegistry::find(std::string_view kind) const {
  auto it = units_.find(kind);
  return it == units_.end() ? nullptr : it->second.get();
}

std::vector<std::string> UnitRegistry::kinds() const {
  std::vector<std::string> out;
  for (const auto& [k, _] : units_) out.push_back(k);
  return out;
}

// ---------------------------------------------------------------- pipeline

NodeSpec* Pipeline::find(std::string_view id) {
  for (auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

const NodeSpec* Pipeline::find(std::string_view id) const {
  for (const auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

NodeSpec& Pipeline::add(const UnitRegistry& registry, std::string id, std::string kind, std::vector<std::string> inputs,
                        std::optional<Options> options) {
  NodeSpec n;
  n.id = std::move(id);
  const Unit* u = registry.find(kind);
  n.kind = std::move(kind);
  n.inputs = std::move(inputs);
  n.options = options ? std::move(*options) : Options(u ? u->schema() : nullptr);
  nodes.push_back(std::move(n));
  return nodes.back();
}

Result<void> Pipeline::validate(const UnitRegistry& registry) const {
  std::set<std::string> ids;
  for (const auto& n : nodes) {
    if (n.id.empty()) return pipe_fail("a unit has no id");
    if (!ids.insert(n.id).second) return pipe_fail("duplicate unit id '" + n.id + "'");
  }
  for (const auto& n : nodes) {
    const Unit* u = registry.find(n.kind);
    if (!u) return pipe_fail("'" + n.id + "': unknown unit kind '" + n.kind + "'");
    if (n.options.schema() != u->schema()) return pipe_fail("'" + n.id + "': options are not for '" + n.kind + "'");
    const auto ins = u->inputs();
    if (n.inputs.size() != ins.size())
      return pipe_fail("'" + n.id + "': " + n.kind + " takes " + std::to_string(ins.size()) + " input(s), got " +
                       std::to_string(n.inputs.size()));
    for (std::size_t i = 0; i < ins.size(); ++i) {
      auto ref = parse_input(n.inputs[i]);
      if (!ref) return pipe_fail("'" + n.id + "': bad input '" + n.inputs[i] + "'");
      const NodeSpec* src = find(ref->node);
      if (!src) return pipe_fail("'" + n.id + "': unknown input unit '" + ref->node + "'");
      const Unit* su = registry.find(src->kind);
      if (!su) return pipe_fail("'" + src->id + "': unknown unit kind '" + src->kind + "'");
      const auto outs = su->outputs();
      if (ref->port >= outs.size()) return pipe_fail("'" + n.id + "': '" + ref->node + "' has no output " + std::to_string(ref->port));
      if (outs[ref->port].type != ins[i].type)
        return pipe_fail("'" + n.id + "': input '" + ins[i].name + "' needs " + std::string(to_string(ins[i].type)) +
                         ", '" + ref->node + "' gives " + std::string(to_string(outs[ref->port].type)));
    }
  }
  auto ord = order();
  if (!ord) return fail(ord.error());
  return {};
}

Result<std::vector<std::string>> Pipeline::order() const {
  std::vector<std::string> out;
  std::map<std::string, int> state;  // 0 new, 1 visiting, 2 done
  std::function<Result<void>(const NodeSpec&)> visit = [&](const NodeSpec& n) -> Result<void> {
    int& s = state[n.id];
    if (s == 2) return {};
    if (s == 1) return pipe_fail("cycle through '" + n.id + "'");
    s = 1;
    for (const auto& in : n.inputs) {
      auto ref = parse_input(in);
      if (!ref) return pipe_fail("'" + n.id + "': bad input '" + in + "'");
      const NodeSpec* src = find(ref->node);
      if (!src) return pipe_fail("'" + n.id + "': unknown input unit '" + ref->node + "'");
      if (auto r = visit(*src); !r) return r;
    }
    state[n.id] = 2;
    out.push_back(n.id);
    return {};
  };
  for (const auto& n : nodes)
    if (auto r = visit(n); !r) return fail(r.error());
  return out;
}

// ---------------------------------------------------------------- TOML

Result<Pipeline> pipeline_from_toml(const UnitRegistry& registry, std::string_view text,
                                    std::vector<std::string>* warnings) {
  auto parsed = toml::parse(text);
  if (!parsed) return pipe_fail(std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();
  if (const auto* s = root.get("schema"); s && s->value_exact<std::string>().value_or("") != "pipeline")
    return pipe_fail("not a pipeline document");
  Pipeline p;
  if (const auto* n = root.get("name")) p.name = n->value_exact<std::string>().value_or("");
  const auto* units = root.get("units") ? root.get("units")->as_array() : nullptr;
  if (!units) return pipe_fail("no [[units]]");
  for (const auto& e : *units) {
    const auto* t = e.as_table();
    if (!t) return pipe_fail("each [[units]] entry must be a table");
    NodeSpec n;
    n.id = t->get("id") ? t->get("id")->value_exact<std::string>().value_or("") : "";
    n.kind = t->get("kind") ? t->get("kind")->value_exact<std::string>().value_or("") : "";
    if (n.id.empty() || n.kind.empty()) return pipe_fail("a unit needs an id and a kind");
    const Unit* u = registry.find(n.kind);
    if (!u) return pipe_fail("'" + n.id + "': unknown unit kind '" + n.kind + "'");
    if (const auto* ins = t->get("inputs") ? t->get("inputs")->as_array() : nullptr) {
      for (const auto& i : *ins) n.inputs.push_back(i.value_exact<std::string>().value_or(""));
    }
    if (const auto* pr = t->get("preset")) n.preset = pr->value_exact<std::string>().value_or("");
    n.options = Options(u->schema());
    if (const auto* o = t->get("options") ? t->get("options")->as_table() : nullptr) {
      std::ostringstream os;
      os << *o;
      auto loaded = options_from_toml(u->schema(), os.str());
      if (!loaded) return pipe_fail("'" + n.id + "': " + loaded.error().what);
      if (warnings)
        for (auto& w : loaded->warnings) warnings->push_back("'" + n.id + "': " + w);
      n.options = std::move(loaded->options);
    }
    p.nodes.push_back(std::move(n));
  }
  if (auto ok = p.validate(registry); !ok) return fail(ok.error());
  return p;
}

std::string pipeline_to_toml(const Pipeline& p) {
  toml::table root;
  root.insert("schema", "pipeline");
  root.insert("version", 1);
  if (!p.name.empty()) root.insert("name", p.name);
  toml::array units;
  for (const auto& n : p.nodes) {
    toml::table t;
    t.insert("id", n.id);
    t.insert("kind", n.kind);
    toml::array ins;
    for (const auto& i : n.inputs) ins.push_back(i);
    t.insert("inputs", std::move(ins));
    if (!n.preset.empty()) t.insert("preset", n.preset);
    auto body = toml::parse(options_to_toml(n.options));
    if (body) {
      toml::table opts = std::move(body).table();
      opts.erase("schema");
      opts.erase("version");
      if (!opts.empty()) t.insert("options", std::move(opts));
    }
    units.push_back(std::move(t));
  }
  root.insert("units", std::move(units));
  std::ostringstream os;
  os << root << "\n";
  return os.str();
}

// ---------------------------------------------------------------- runner

Result<std::vector<PortValue>> Runner::run(const Pipeline& pipeline, std::string_view target,
                                           const std::atomic<bool>* cancel) {
  last_.clear();
  diagnostics_.clear();
  if (auto ok = pipeline.validate(registry_); !ok) return fail(ok.error());
  if (!pipeline.find(target)) return pipe_fail("unknown target '" + std::string(target) + "'");

  // Only the target's ancestors.
  std::set<std::string> needed;
  std::function<void(const NodeSpec&)> mark = [&](const NodeSpec& n) {
    if (!needed.insert(n.id).second) return;
    for (const auto& in : n.inputs)
      if (auto ref = parse_input(in))
        if (const auto* src = pipeline.find(ref->node)) mark(*src);
  };
  mark(*pipeline.find(target));

  auto ord = pipeline.order();
  if (!ord) return fail(ord.error());
  std::map<std::string, std::string> fingerprints;
  std::map<std::string, Error> errors;

  for (const auto& id : *ord) {
    if (!needed.count(id)) continue;
    const NodeSpec& n = *pipeline.find(id);
    const Unit& u = *registry_.find(n.kind);
    NodeRun nr{id, false, std::nullopt};

    std::string text = "unit/1\n" + n.kind + "\n" + n.options.canonical() + "\n";
    std::vector<PortValue> inputs;
    std::optional<Error> upstream;
    for (const auto& in : n.inputs) {
      const auto ref = *parse_input(in);
      text += "in " + fingerprints[ref.node] + ":" + std::to_string(ref.port) + "\n";
      if (auto e = errors.find(ref.node); e != errors.end()) {
        upstream = e->second;
        continue;
      }
      inputs.push_back(cache_.at(ref.node).outputs.at(ref.port));
    }
    if (u.reads_source() && source_) text += "source " + source_->name() + " " + std::to_string(source_->generation()) + "\n";
    const std::string fp = pychron::to_hex(sha256(text));
    fingerprints[id] = fp;

    if (upstream) {
      errors[id] = *upstream;
      nr.error = *upstream;
      last_.push_back(std::move(nr));
      continue;
    }
    if (cancel && cancel->load()) return fail(ErrorKind::Cancelled, "pipeline: cancelled");

    auto cached = cache_.find(id);
    if (cached != cache_.end() && cached->second.fingerprint == fp) {
      last_.push_back(std::move(nr));
      continue;
    }
    RunContext ctx{source_, cancel, {}};
    auto out = u.execute(inputs, n.options, ctx);
    ++executions_;
    nr.executed = true;
    for (auto& d : ctx.diagnostics) diagnostics_.push_back(id + ": " + d);
    if (!out) {
      Error e = out.error();
      if (e.device.empty()) e.device = id;
      errors[id] = e;
      nr.error = e;
      cache_.erase(id);
      last_.push_back(std::move(nr));
      continue;
    }
    const auto outs = u.outputs();
    if (out->size() != outs.size()) return pipe_fail("'" + id + "' returned the wrong number of outputs");
    cache_[id] = Entry{fp, std::move(*out)};
    last_.push_back(std::move(nr));
  }

  if (auto e = errors.find(std::string(target)); e != errors.end()) return fail(e->second);
  return cache_.at(std::string(target)).outputs;
}

}  // namespace pychron::processing
