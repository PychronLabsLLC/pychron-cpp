#include "pychron/experiment/conditionals/conditional.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

#include <toml++/toml.hpp>

#include "number.hpp"
#include "pychron/experiment/record/sha256.hpp"

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

std::string trim(std::string_view s) {
  size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
  return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}

Unexpected<Error> cfg(const std::string& m) { return fail(ErrorKind::Config, m); }

bool parse_double(const std::string& s, double& out) { return detail::parse_number(s, out); }

std::string num(double v) { return detail::shortest(v); }

// "NAME=v"
Result<ActionSpec> name_value(ActionSpec a, const std::string& arg, const std::string& what) {
  auto eq = arg.find('=');
  if (eq == std::string::npos || eq == 0 || !parse_double(trim(arg.substr(eq + 1)), a.value))
    return cfg(what + " needs NAME=value, got '" + arg + "'");
  a.name = trim(arg.substr(0, eq));
  return a;
}

// "1,2,3" or "10%,20%"; all steps the same flavour.
Result<ActionSpec> extract_steps(ActionSpec a, const std::string& arg) {
  if (arg.empty()) return cfg("set_extract needs steps, e.g. '1,2' or '10%,20%'");
  size_t b = 0;
  int percents = 0, plain = 0;
  while (b <= arg.size()) {
    size_t e = arg.find(',', b);
    if (e == std::string::npos) e = arg.size();
    std::string item = trim(arg.substr(b, e - b));
    bool pct = !item.empty() && item.back() == '%';
    if (pct) item.pop_back();
    double v = 0;
    if (!parse_double(trim(item), v)) return cfg("set_extract step '" + item + "' is not a number");
    (pct ? percents : plain) += 1;
    a.steps.push_back(v);
    b = e + 1;
  }
  if (percents > 0 && plain > 0) return cfg("set_extract steps must be all absolute or all percentages");
  a.percent = percents > 0;
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

std::string_view table_name(ConditionalKind k) noexcept {
  for (const auto& e : kKinds)
    if (e.kind == k) return e.table;
  return "";
}

std::string default_name(ConditionalKind k, std::string_view check) {
  return std::string(to_string(k)) + ":" + std::string(check);
}

const KindFields& fields_of(ConditionalKind k) {
  using K = ConditionalKind;
  using T = ActionSpec::Type;
  static const std::vector<T> queue{T::SkipNext, T::SkipN,   T::SkipAliquot, T::SkipToLastInAliquot,
                                    T::SetExtract, T::Repeat, T::RunBlank};
  static const std::vector<T> post = [] {
    std::vector<T> v{T::Cancel};
    v.insert(v.end(), queue.begin(), queue.end());
    return v;
  }();
  //                                       gating ratio  resume flags  actions  default
  static const KindFields truncation{true, true, false, false, {T::Truncate}, T::Truncate};
  static const KindFields termination{true, false, false, false, {}, T::Terminate};
  static const KindFields cancelation{true, false, false, false, {}, T::Cancel};
  static const KindFields action{
      true, false, true, false, {T::Truncate, T::Terminate, T::Cancel, T::SetParam, T::RunHook, T::Notify}, T::None};
  static const KindFields modification{true, true, false, true, queue, T::SkipNext};
  static const KindFields equilibration{true, true, false, false, {}, T::None};
  static const KindFields pre_run{false, false, false, false, {T::Cancel}, T::Cancel};
  static const KindFields post_run{false, false, false, false, post, T::Cancel};
  switch (k) {
    case K::Truncation: return truncation;
    case K::Termination: return termination;
    case K::Cancelation: return cancelation;
    case K::Action: return action;
    case K::Modification: return modification;
    case K::Equilibration: return equilibration;
    case K::PreRun: return pre_run;
    case K::PostRun: return post_run;
  }
  return truncation;
}

std::string_view to_string(ConditionalLevel l) noexcept {
  switch (l) {
    case ConditionalLevel::System: return "system";
    case ConditionalLevel::Queue: return "queue";
    case ConditionalLevel::Plan: return "plan";
    case ConditionalLevel::Run: return "run";
    case ConditionalLevel::Hook: return "hook";
  }
  return "run";
}

bool is_queue_action(ActionSpec::Type t) noexcept {
  using T = ActionSpec::Type;
  return t == T::SkipNext || t == T::SkipN || t == T::SkipAliquot || t == T::SkipToLastInAliquot ||
         t == T::SetExtract || t == T::Repeat || t == T::RunBlank;
}

Result<ActionSpec> parse_action(std::string_view text_in) {
  const std::string text = trim(text_in);
  ActionSpec a;
  auto word_end = text.find_first_of(" =:");
  const std::string word = text.substr(0, word_end);
  const std::string rest =
      word_end == std::string::npos ? "" : trim(text.substr(word_end + (text[word_end] == ' ' ? 0 : 1)));
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
  if (word == "skip_next") return no_arg(T::SkipNext);
  if (word == "skip_aliquot") return no_arg(T::SkipAliquot);
  if (word == "skip_to_last_in_aliquot") return no_arg(T::SkipToLastInAliquot);
  if (word == "repeat") return no_arg(T::Repeat);
  if (word == "run_blank") return no_arg(T::RunBlank);
  if (word == "skip_n") {
    a.type = T::SkipN;
    if (rest.empty()) return a;
    double n = 0;
    if (!parse_double(rest, n) || n < 1 || n != static_cast<int>(n)) return cfg("skip_n needs a count >= 1");
    a.count = static_cast<int>(n);
    return a;
  }
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
    return extract_steps(a, rest);
  }
  return cfg("unknown action '" + text + "'");
}

std::string to_string(const ActionSpec& a) {
  using T = ActionSpec::Type;
  switch (a.type) {
    case T::None: return "";
    case T::Truncate: return a.quick ? "truncate:quick" : "truncate";
    case T::Terminate: return "terminate";
    case T::Cancel: return "cancel";
    case T::SetParam: return "set_param " + a.name + "=" + num(a.value);
    case T::RunHook: return "run_hook " + a.name;
    case T::Notify: return "notify";
    case T::SkipNext: return "skip_next";
    case T::SkipN: return "skip_n " + std::to_string(a.count);
    case T::SkipAliquot: return "skip_aliquot";
    case T::SkipToLastInAliquot: return "skip_to_last_in_aliquot";
    case T::SetExtract: {
      std::string s = "set_extract ";
      for (size_t i = 0; i < a.steps.size(); ++i) s += (i ? "," : "") + num(a.steps[i]) + (a.percent ? "%" : "");
      return s;
    }
    case T::Repeat: return "repeat";
    case T::RunBlank: return "run_blank";
  }
  return "";
}

Result<std::shared_ptr<const Expr>> compile_check(const std::string& check, std::optional<int> window,
                                                  const std::string& mapper) {
  auto expr = parse_expression(check);
  if (!expr) return fail(expr.error());
  ExprPtr e = std::move(*expr);
  if (window) e = apply_window(*e, *window);
  if (!mapper.empty()) {
    auto m = apply_mapper(*e, mapper);
    if (!m) return fail(m.error());
    e = std::move(*m);
  }
  return std::shared_ptr<const Expr>(std::move(e));
}

std::string Conditional::effective_check() const { return expr ? to_string(*expr) : check; }

std::string Conditional::id() const {
  std::string d = std::string(to_string(kind)) + "|" + effective_check() + "|" + std::to_string(start) + "|" +
                  std::to_string(frequency) + "|" + std::to_string(ntrips) + "|" + num(abbreviated_count_ratio) +
                  "|" + to_string(action) + "|" + (resume ? "r" : "") + (truncate ? "t" : "") +
                  (terminate ? "T" : "") + "|";
  for (const auto& t : analysis_types) d += t + ",";
  return record::sha256_hex(d);
}

bool Conditional::applies_to(std::string_view analysis_type) const {
  if (analysis_types.empty() || analysis_type.empty()) return true;
  std::string at(analysis_type);
  std::transform(at.begin(), at.end(), at.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const auto& t : analysis_types) {
    if (t == at) return true;
    if (t == "blank" && at.starts_with("blank")) return true;
  }
  return false;
}

ConditionalSet& ConditionalSet::stamp(ConditionalLevel level, const std::string& location) {
  for (auto& c : items) {
    c.level = level;
    c.location = location;
  }
  return *this;
}

ConditionalSet merge_levels(const std::vector<ConditionalSet>& levels) {
  ConditionalSet out;
  for (const auto& level : levels) {
    for (const auto& name : level.disable)
      out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                     [&](const Conditional& c) { return c.name == name; }),
                      out.items.end());
    for (const auto& c : level.items) {
      auto it = std::find_if(out.items.begin(), out.items.end(), [&](const Conditional& x) { return x.name == c.name; });
      if (it != out.items.end()) *it = c;
      else out.items.push_back(c);
    }
  }
  return out;
}

std::vector<MetricValue> metric_context(const Expr& e, const MetricContext& ctx) {
  std::vector<MetricValue> out;
  for (const auto& m : metrics_of(e)) {
    if (auto v = ctx.scalar(m)) {
      out.push_back({to_string(m), *v});
    } else if (auto s = ctx.series(m); s && !s->empty()) {
      out.push_back({to_string(m), s->back()});
    }
  }
  return out;
}

// ---- engine -----------------------------------------------------------------

ConditionalEngine::ConditionalEngine(ConditionalSet set, std::string analysis_type) : set_(std::move(set)) {
  states_.resize(set_.items.size());
  for (size_t i = 0; i < set_.items.size(); ++i) states_[i].applicable = set_.items[i].applies_to(analysis_type);
}

void ConditionalEngine::reset() {
  for (auto& s : states_) {
    s.consecutive = 0;
    s.fired = false;
  }
  trips_.clear();
  errors_.clear();
}

std::vector<const Conditional*> ConditionalEngine::installed() const {
  std::vector<const Conditional*> out;
  for (size_t i = 0; i < set_.items.size(); ++i)
    if (states_[i].applicable) out.push_back(&set_.items[i]);
  return out;
}

void ConditionalEngine::record_error(const Conditional& c, const std::string& message) {
  for (auto& e : errors_) {
    if (e.name == c.name) {
      ++e.count;
      return;
    }
  }
  errors_.push_back({c.name, message, 1});
}

std::optional<Trip> ConditionalEngine::step(size_t i, const MetricContext& ctx, const Variables& vars, int reading,
                                            double ts) {
  const Conditional& c = set_.items[i];
  State& st = states_[i];
  auto r = evaluate_check(*c.expr, ctx, vars);
  if (!r) {
    record_error(c, r.error().what);
    st.consecutive = 0;
    return std::nullopt;
  }
  if (!r->tripped) {
    st.consecutive = 0;
    return std::nullopt;
  }
  if (++st.consecutive < c.ntrips) return std::nullopt;
  Trip t;
  t.name = c.name;
  t.kind = c.kind;
  t.value = r->value;
  t.count = st.consecutive;
  t.ts = ts;
  t.action = c.action;
  t.reading = reading;
  t.id = c.id();
  t.check = c.effective_check();
  t.level = c.level;
  t.context = metric_context(*c.expr, ctx);
  t.abbreviated_count_ratio = c.abbreviated_count_ratio;
  t.resume = c.resume;
  t.truncate = c.truncate;
  t.terminate = c.terminate;
  // Fires once, except a resuming action, which re-arms.
  st.consecutive = 0;
  st.fired = !(c.kind == ConditionalKind::Action && c.resume);
  trips_.push_back(t);
  return t;
}

std::optional<Trip> ConditionalEngine::evaluate(std::span<const ConditionalKind> kinds, const MetricContext& ctx,
                                                const Variables& vars, int reading, double ts) {
  for (auto kind : kinds) {
    for (size_t i = 0; i < set_.items.size(); ++i) {
      const Conditional& c = set_.items[i];
      const State& st = states_[i];
      if (c.kind != kind || !c.expr || st.fired || !st.applicable) continue;
      const int after = reading - c.start;
      if (after <= 0 || after % std::max(c.frequency, 1) != 0) continue;
      if (auto t = step(i, ctx, vars, reading, ts)) return t;
    }
  }
  return std::nullopt;
}

std::optional<Trip> ConditionalEngine::check_now(ConditionalKind kind, const MetricContext& ctx, const Variables& vars,
                                                 double ts, std::string_view analysis_type) {
  for (size_t i = 0; i < set_.items.size(); ++i) {
    const Conditional& c = set_.items[i];
    if (c.kind != kind || !c.expr || !states_[i].applicable || !c.applies_to(analysis_type)) continue;
    states_[i].fired = false;  // between-run checks may fire on every run
    if (auto t = step(i, ctx, vars, 0, ts)) return t;
  }
  return std::nullopt;
}

// ---- whiff --------------------------------------------------------------------

std::string_view to_string(WhiffCheck::Action a) noexcept {
  switch (a) {
    case WhiffCheck::Action::RunRemainder: return "run_remainder";
    case WhiffCheck::Action::Pump: return "pump";
    case WhiffCheck::Action::Abort: return "abort";
  }
  return "run_remainder";
}

std::optional<WhiffCheck::Action> parse_whiff_action(std::string_view s) noexcept {
  if (s == "run_remainder") return WhiffCheck::Action::RunRemainder;
  if (s == "pump") return WhiffCheck::Action::Pump;
  if (s == "abort") return WhiffCheck::Action::Abort;
  return std::nullopt;
}

std::optional<WhiffCheck::Action> evaluate_whiff(const Whiff& w, const MetricContext& ctx, const Variables& vars) {
  for (const auto& c : w.checks) {
    if (!c.expr) continue;
    auto r = evaluate_check(*c.expr, ctx, vars);
    if (r && r->tripped) return c.action;
  }
  return std::nullopt;
}

Result<Conditional> finalize(Conditional c) {
  using K = ConditionalKind;
  using T = ActionSpec::Type;
  const K kind = c.kind;
  const KindFields& f = fields_of(kind);
  auto at_least = [](const char* key, int v, int min) -> Result<void> {
    if (v < min) return cfg("'" + std::string(key) + "' must be an integer >= " + std::to_string(min));
    return {};
  };
  if (trim(c.check).empty()) return cfg("missing 'check'");
  if (auto r = at_least("start", c.start, 0); !r) return fail(r.error());
  if (auto r = at_least("frequency", c.frequency, 1); !r) return fail(r.error());
  if (auto r = at_least("ntrips", c.ntrips, 1); !r) return fail(r.error());
  if (c.window)
    if (auto r = at_least("window", *c.window, 1); !r) return fail(r.error());
  auto expr = compile_check(c.check, c.window, c.mapper);
  if (!expr) return fail(expr.error());
  c.expr = *expr;
  if (c.name.empty()) c.name = default_name(kind, c.check);

  if (!(c.abbreviated_count_ratio > 0 && c.abbreviated_count_ratio <= 1))
    return cfg("'abbreviated_count_ratio' must be a number in (0, 1]");
  if (c.abbreviated_count_ratio != 1.0 && !f.ratio)
    return cfg("'abbreviated_count_ratio' applies to truncations, modifications and equilibrations");
  if (c.resume && !f.resume) return cfg("'resume' applies to actions only");
  if ((c.truncate || c.terminate) && !f.run_flags) return cfg("'truncate'/'terminate' apply to modifications only");
  if (c.truncate && c.terminate) return cfg("a modification may truncate or terminate, not both");

  // Which actions each kind may take.
  const bool given = c.action.type != T::None;
  if (!given) c.action.type = f.default_action;
  const T at = c.action.type;
  const bool allowed = std::find(f.actions.begin(), f.actions.end(), at) != f.actions.end();
  switch (kind) {
    case K::Truncation:
      if (!allowed) return cfg("a truncation's action must be truncate or truncate:quick");
      break;
    case K::Termination:
    case K::Cancelation:
    case K::Equilibration:
      if (given && c.action != ActionSpec{.type = f.default_action})
        return cfg("'action' is not allowed on " + std::string(to_string(kind)) + "s");
      break;
    case K::Action:
      if (at == T::None) return cfg("an action conditional needs 'action'");
      if (is_queue_action(at)) return cfg("queue actions belong in [[modifications]] or [[post_run]]");
      break;
    case K::Modification:
      if (!allowed) return cfg("a modification's action must be a queue action (skip_next, run_blank, ...)");
      break;
    case K::PreRun:
      if (!allowed) return cfg("a pre_run conditional's action must be cancel");
      break;
    case K::PostRun:
      if (!allowed) return cfg("a post_run action must be cancel or a queue action");
      break;
  }
  // The action must be one its text form can carry (a name, steps, a finite value).
  if (c.action.type != T::None) {
    const std::string text = to_string(c.action);
    auto back = parse_action(text);
    if (!back) return fail(back.error());
    if (*back != c.action) return cfg("action '" + text + "' has parameters it cannot be written with");
  }
  return c;
}

// ---- TOML ---------------------------------------------------------------------

namespace {

Result<int> int_key(const toml::table& t, std::string_view key, int def, int min) {
  auto n = t.get(key);
  if (!n) return def;
  auto v = n->value<int64_t>();
  if (!v || !n->is_integer() || *v < min)
    return cfg("'" + std::string(key) + "' must be an integer >= " + std::to_string(min));
  return static_cast<int>(*v);
}

Result<std::string> str_key(const toml::table& t, std::string_view key) {
  auto n = t.get(key);
  if (!n) return std::string();
  auto v = n->value<std::string>();
  if (!v || !n->is_string()) return cfg("'" + std::string(key) + "' must be a string");
  return *v;
}

Result<bool> bool_key(const toml::table& t, std::string_view key) {
  auto n = t.get(key);
  if (!n) return false;
  if (!n->is_boolean()) return cfg("'" + std::string(key) + "' must be a boolean");
  return *n->value<bool>();
}

Result<Conditional> parse_item(const toml::table& t, ConditionalKind kind) {
  static const std::set<std::string_view> known{"check",  "name",  "start",  "frequency", "ntrips",
                                                "action", "resume", "window", "mapper",    "analysis_types",
                                                "abbreviated_count_ratio", "truncate", "terminate"};
  for (const auto& [k, v] : t)
    if (!known.contains(k.str())) return cfg("unknown key '" + std::string(k.str()) + "'");
  Conditional c;
  c.kind = kind;
  auto check = str_key(t, "check");
  if (!check) return fail(check.error());
  if (check->empty()) return cfg("missing 'check'");
  c.check = *check;

  auto start = int_key(t, "start", 0, 0), freq = int_key(t, "frequency", 1, 1), nt = int_key(t, "ntrips", 1, 1);
  if (!start) return fail(start.error());
  if (!freq) return fail(freq.error());
  if (!nt) return fail(nt.error());
  c.start = *start;
  c.frequency = *freq;
  c.ntrips = *nt;
  if (t.contains("window")) {
    auto w = int_key(t, "window", 0, 1);
    if (!w) return fail(w.error());
    c.window = *w;
  }
  auto mapper = str_key(t, "mapper");
  if (!mapper) return fail(mapper.error());
  c.mapper = *mapper;
  // Compiled here as well so a bad check is reported before the keys after it.
  if (auto expr = compile_check(c.check, c.window, c.mapper); !expr) return fail(expr.error());

  auto name = str_key(t, "name");
  if (!name) return fail(name.error());
  c.name = *name;

  if (auto* n = t.get("analysis_types")) {
    auto* arr = n->as_array();
    if (!arr) return cfg("'analysis_types' must be an array of strings");
    for (const auto& e : *arr) {
      auto s = e.value<std::string>();
      if (!s) return cfg("'analysis_types' must be an array of strings");
      std::string v = trim(*s);
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return ch == ' ' ? '_' : std::tolower(ch); });
      c.analysis_types.push_back(v);
    }
  }
  if (auto* n = t.get("abbreviated_count_ratio")) {
    auto v = n->value<double>();
    if (!v || *v <= 0 || *v > 1) return cfg("'abbreviated_count_ratio' must be a number in (0, 1]");
    // The key itself is refused, even at its default value.
    if (!fields_of(kind).ratio)
      return cfg("'abbreviated_count_ratio' applies to truncations, modifications and equilibrations");
    c.abbreviated_count_ratio = *v;
  }

  auto resume = bool_key(t, "resume"), trunc = bool_key(t, "truncate"), term = bool_key(t, "terminate");
  if (!resume) return fail(resume.error());
  if (!trunc) return fail(trunc.error());
  if (!term) return fail(term.error());
  c.resume = *resume;
  c.truncate = *trunc;
  c.terminate = *term;
  if (c.resume && !fields_of(kind).resume) return cfg("'resume' applies to actions only");
  if ((c.truncate || c.terminate) && !fields_of(kind).run_flags)
    return cfg("'truncate'/'terminate' apply to modifications only");
  if (c.truncate && c.terminate) return cfg("a modification may truncate or terminate, not both");

  auto act = str_key(t, "action");
  if (!act) return fail(act.error());
  if (!act->empty()) {
    auto a = parse_action(*act);
    if (!a) return fail(a.error());
    c.action = *a;
  }
  // The key itself is refused where the kind has no action.
  if (t.contains("action") && fields_of(kind).actions.empty())
    return cfg("'action' is not allowed on " + std::string(to_string(kind)) + "s");
  return finalize(std::move(c));
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
      ++idx;
      if (!t) return cfg(std::string(file) + ": '" + k + "' entries must be tables");
      auto c = parse_item(*t, *kind);
      if (!c) return cfg(std::string(file) + ": [[" + k + "]] #" + std::to_string(idx) + ": " + c.error().what);
      if (!names.insert(c->name).second) return cfg(std::string(file) + ": duplicate conditional name '" + c->name + "'");
      c->location = std::string(file);
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
      auto a = parse_whiff_action(*act);
      if (!a) return cfg(std::string(file) + ": whiff action must be run_remainder|pump|abort, got '" + *act + "'");
      wc.action = *a;
      auto expr = parse_expression(wc.check);
      if (!expr) return fail(expr.error());
      wc.expr = std::shared_ptr<const Expr>(std::move(*expr));
      out.checks.push_back(std::move(wc));
    }
  }
  return out;
}

}  // namespace pychron::experiment
