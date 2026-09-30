#include "pychron/experiment/conditionals/conditional.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

#include <toml++/toml.hpp>

namespace pychron::experiment {
namespace {

struct KindName {
  ConditionalKind kind;
  std::string_view singular, table;
};
constexpr KindName kKinds[] = {
    {ConditionalKind::Modification, "modification", "modifications"},
    {ConditionalKind::Truncation, "truncation", "truncations"},
    {ConditionalKind::Action, "action", "actions"},
    {ConditionalKind::Termination, "termination", "terminations"},
    {ConditionalKind::Cancelation, "cancelation", "cancelations"},
    {ConditionalKind::Equilibration, "equilibration", "equilibrations"},
    {ConditionalKind::PreRun, "pre_run", "pre_run"},
    {ConditionalKind::PostRun, "post_run", "post_run"},
};
// pychron evaluation order: modification, truncation, action, termination, cancelation, equilibration.
constexpr ConditionalKind kOrder[] = {ConditionalKind::Modification, ConditionalKind::Truncation,
                                      ConditionalKind::Action,       ConditionalKind::Termination,
                                      ConditionalKind::Cancelation,  ConditionalKind::Equilibration};

std::string trim(std::string_view s) {
  size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
  return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}

Unexpected<Error> cfg(const std::string& m) { return fail(ErrorKind::Config, m); }

bool parse_double(const std::string& s, double& out) {
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return !s.empty() && end == s.c_str() + s.size();
}

// "NAME=v"
Result<ActionSpec> name_value(ActionSpec a, const std::string& arg, const std::string& what) {
  auto eq = arg.find('=');
  if (eq == std::string::npos || eq == 0 || !parse_double(trim(arg.substr(eq + 1)), a.value))
    return cfg(what + " needs NAME=value, got '" + arg + "'");
  a.name = trim(arg.substr(0, eq));
  return a;
}

}  // namespace

std::string_view to_string(ConditionalKind k) noexcept {
  for (const auto& e : kKinds)
    if (e.kind == k) return e.singular;
  return "";
}

std::optional<ConditionalKind> parse_conditional_kind(std::string_view table_name) {
  for (const auto& e : kKinds)
    if (e.table == table_name) return e.kind;
  return std::nullopt;
}

Result<ActionSpec> parse_action(std::string_view text_in) {
  const std::string text = trim(text_in);
  ActionSpec a;
  auto word_end = text.find_first_of(" =:");
  const std::string word = text.substr(0, word_end);
  const std::string rest = word_end == std::string::npos ? "" : trim(text.substr(word_end + (text[word_end] == ' ' ? 0 : 1)));
  using T = ActionSpec::Type;
  auto no_arg = [&](T t) -> Result<ActionSpec> {
    if (!rest.empty()) return cfg("action '" + text + "' takes no argument");
    a.type = t;
    return a;
  };
  if (word == "truncate") {
    if (!rest.empty() && rest != "quick") return cfg("unknown truncate mode '" + rest + "'");
    a.type = T::Truncate;
    a.quick = rest == "quick";
    return a;
  }
  if (word == "terminate") return no_arg(T::Terminate);
  if (word == "cancel") return no_arg(T::Cancel);
  if (word == "notify") return no_arg(T::Notify);
  if (word == "skip_n") return no_arg(T::SkipN);
  if (word == "skip_aliquot") return no_arg(T::SkipAliquot);
  if (word == "repeat") return no_arg(T::Repeat);
  if (word == "run_blank") return no_arg(T::RunBlank);
  if (word == "set_param") {
    a.type = T::SetParam;
    return name_value(a, rest, "set_param");
  }
  if (word == "run_hook") {
    if (rest.empty() || rest.find_first_of(" =") != std::string::npos) return cfg("run_hook needs a single NAME");
    a.type = T::RunHook;
    a.name = rest;
    return a;
  }
  if (word == "set_extract") {
    a.type = T::SetExtract;
    if (!parse_double(rest, a.value)) return cfg("set_extract needs a numeric value, got '" + rest + "'");
    return a;
  }
  return cfg("unknown action '" + text + "'");
}

std::string to_string(const ActionSpec& a) {
  using T = ActionSpec::Type;
  auto num = [](double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.15g", v);
    return std::string(b);
  };
  switch (a.type) {
    case T::None: return "";
    case T::Truncate: return a.quick ? "truncate:quick" : "truncate";
    case T::Terminate: return "terminate";
    case T::Cancel: return "cancel";
    case T::SetParam: return "set_param " + a.name + "=" + num(a.value);
    case T::RunHook: return "run_hook " + a.name;
    case T::Notify: return "notify";
    case T::SkipN: return "skip_n";
    case T::SkipAliquot: return "skip_aliquot";
    case T::Repeat: return "repeat";
    case T::RunBlank: return "run_blank";
    case T::SetExtract: return "set_extract=" + num(a.value);
  }
  return "";
}

ConditionalSet merge_levels(const std::vector<ConditionalSet>& levels) {
  ConditionalSet out;
  for (const auto& level : levels) {
    for (const auto& name : level.disable)
      out.items.erase(std::remove_if(out.items.begin(), out.items.end(), [&](const Conditional& c) { return c.name == name; }),
                      out.items.end());
    for (const auto& c : level.items) {
      auto it = std::find_if(out.items.begin(), out.items.end(), [&](const Conditional& x) { return x.name == c.name; });
      if (it != out.items.end()) *it = c;
      else out.items.push_back(c);
    }
  }
  return out;
}

void ConditionalEngine::reset() {
  for (auto& s : states_) s = State{};
  trips_.clear();
  errors_.clear();
}

std::vector<Trip> ConditionalEngine::evaluate(const MetricContext& ctx, const Variables& vars, int reading, double ts) {
  std::vector<Trip> out;
  for (auto kind : kOrder) {
    auto t = evaluate_kind(kind, ctx, vars, reading, ts);
    out.insert(out.end(), t.begin(), t.end());
  }
  return out;
}

std::vector<Trip> ConditionalEngine::evaluate_kind(ConditionalKind kind, const MetricContext& ctx, const Variables& vars,
                                                   int reading, double ts) {
  std::vector<Trip> out;
  for (size_t i = 0; i < set_.items.size(); ++i) {
    const Conditional& c = set_.items[i];
    State& st = states_[i];
    if (c.kind != kind || !c.expr || st.fired) continue;
    if (reading < c.start) continue;
    if (c.frequency > 1 && (reading - c.start) % c.frequency != 0) continue;
    auto r = evaluate_check(*c.expr, ctx, vars);
    if (!r) {
      errors_.push_back(c.name + ": " + r.error().what);
      st.consecutive = 0;
      continue;
    }
    if (!r->tripped) {
      st.consecutive = 0;
      continue;
    }
    if (++st.consecutive < c.ntrips) continue;
    st.fired = true;
    Trip t{c.name, c.kind, r->value, st.consecutive, ts, c.action};
    trips_.push_back(t);
    out.push_back(std::move(t));
  }
  return out;
}

std::optional<WhiffCheck::Action> evaluate_whiff(const Whiff& w, const MetricContext& ctx, const Variables& vars) {
  for (const auto& c : w.checks) {
    if (!c.expr) continue;
    auto r = evaluate_check(*c.expr, ctx, vars);
    if (r && r->tripped) return c.action;
  }
  return std::nullopt;
}

namespace {

Result<int> int_key(const toml::table& t, std::string_view key, int def, int min) {
  auto n = t.get(key);
  if (!n) return def;
  auto v = n->value<int64_t>();
  if (!v || *v < min) return cfg("'" + std::string(key) + "' must be an integer >= " + std::to_string(min));
  return static_cast<int>(*v);
}

Result<std::string> str_key(const toml::table& t, std::string_view key) {
  auto n = t.get(key);
  if (!n) return std::string();
  auto v = n->value<std::string>();
  if (!v) return cfg("'" + std::string(key) + "' must be a string");
  return *v;
}

Result<Conditional> parse_item(const toml::table& t, ConditionalKind kind, size_t index) {
  static const std::set<std::string_view> known{"check", "name", "start", "frequency", "ntrips", "action", "resume"};
  for (const auto& [k, v] : t)
    if (!known.count(k.str())) return cfg("unknown key '" + std::string(k.str()) + "'");
  Conditional c;
  c.kind = kind;
  auto check = str_key(t, "check");
  if (!check) return fail(check.error());
  if (check->empty()) return cfg("missing 'check'");
  c.check = *check;
  auto expr = parse_expression(c.check);
  if (!expr) return fail(expr.error());
  c.expr = std::shared_ptr<const Expr>(std::move(*expr));
  auto name = str_key(t, "name");
  if (!name) return fail(name.error());
  c.name = name->empty() ? std::string(to_string(kind)) + ":" + c.check : *name;
  (void)index;
  auto start = int_key(t, "start", 0, 0), freq = int_key(t, "frequency", 1, 1), nt = int_key(t, "ntrips", 1, 1);
  if (!start) return fail(start.error());
  if (!freq) return fail(freq.error());
  if (!nt) return fail(nt.error());
  c.start = *start;
  c.frequency = *freq;
  c.ntrips = *nt;
  auto act = str_key(t, "action");
  if (!act) return fail(act.error());
  using T = ActionSpec::Type;
  switch (kind) {
    case ConditionalKind::Truncation: c.action.type = T::Truncate; break;
    case ConditionalKind::Termination: c.action.type = T::Terminate; break;
    case ConditionalKind::Cancelation: c.action.type = T::Cancel; break;
    default: break;
  }
  if (!act->empty()) {
    auto a = parse_action(*act);
    if (!a) return fail(a.error());
    c.action = *a;
  }
  if (auto r = t.get("resume")) {
    auto b = r->value<bool>();
    if (!b) return cfg("'resume' must be a boolean");
    c.resume = *b;
  }
  return c;
}

Result<toml::table> load_table(std::string_view text, std::string_view file) {
  auto res = toml::parse(text, file);
  if (!res) return cfg(std::string(res.error().description()));
  return std::move(res).table();
}

}  // namespace

Result<ConditionalSet> parse_conditionals(std::string_view text, std::string_view file) {
  auto root = load_table(text, file);
  if (!root) return fail(root.error());
  ConditionalSet set;
  std::set<std::string> names;
  for (const auto& [key, node] : *root) {
    const std::string k(key.str());
    if (k == "disable") {
      auto* arr = node.as_array();
      if (!arr) return cfg(std::string(file) + ": 'disable' must be an array of names");
      for (const auto& e : *arr) {
        auto s = e.value<std::string>();
        if (!s) return cfg(std::string(file) + ": 'disable' must be an array of names");
        set.disable.push_back(*s);
      }
      continue;
    }
    auto kind = parse_conditional_kind(k);
    if (!kind) return cfg(std::string(file) + ": unknown table '" + k + "'");
    auto* arr = node.as_array();
    if (!arr) return cfg(std::string(file) + ": '" + k + "' must be an array of tables ([[" + k + "]])");
    size_t idx = 0;
    for (const auto& e : *arr) {
      auto* t = e.as_table();
      if (!t) return cfg(std::string(file) + ": '" + k + "' entries must be tables");
      auto c = parse_item(*t, *kind, idx++);
      if (!c) return cfg(std::string(file) + ": [[" + k + "]] #" + std::to_string(idx) + ": " + c.error().what);
      if (!names.insert(c->name).second) return cfg(std::string(file) + ": duplicate conditional name '" + c->name + "'");
      set.items.push_back(std::move(*c));
    }
  }
  return set;
}

Result<Whiff> parse_whiff(std::string_view text, std::string_view file) {
  auto root = load_table(text, file);
  if (!root) return fail(root.error());
  auto* w = (*root)["whiff"].as_table();
  if (!w) return cfg(std::string(file) + ": missing [whiff] table");
  Whiff out;
  auto sn = int_key(*w, "sniff", 0, 0);
  if (!sn) return cfg(std::string(file) + ": " + sn.error().what);
  out.sniff = *sn;
  if (auto* arr = (*w)["checks"].as_array()) {
    for (const auto& e : *arr) {
      auto* t = e.as_table();
      if (!t) return cfg(std::string(file) + ": whiff.checks entries must be tables");
      auto check = str_key(*t, "check");
      auto act = str_key(*t, "action");
      if (!check || check->empty()) return cfg(std::string(file) + ": whiff check missing 'check'");
      if (!act) return fail(act.error());
      WhiffCheck wc;
      wc.check = *check;
      if (*act == "run_remainder") wc.action = WhiffCheck::Action::RunRemainder;
      else if (*act == "pump") wc.action = WhiffCheck::Action::Pump;
      else if (*act == "abort") wc.action = WhiffCheck::Action::Abort;
      else return cfg(std::string(file) + ": whiff action must be run_remainder|pump|abort, got '" + *act + "'");
      auto expr = parse_expression(wc.check);
      if (!expr) return fail(expr.error());
      wc.expr = std::shared_ptr<const Expr>(std::move(*expr));
      out.checks.push_back(std::move(wc));
    }
  }
  return out;
}

}  // namespace pychron::experiment
