#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/experiment/conditionals/evaluator.hpp"

namespace pychron::experiment {

enum class ConditionalKind { Truncation, Termination, Cancelation, Action, Modification, Equilibration, PreRun, PostRun };

std::string_view to_string(ConditionalKind k) noexcept;
std::optional<ConditionalKind> parse_conditional_kind(std::string_view table_name);  // "truncations", ...

// Enum action; there are no script snippets.
struct ActionSpec {
  enum class Type {
    None, Truncate, Terminate, Cancel, SetParam, RunHook, Notify,  // general
    SkipN, SkipAliquot, Repeat, RunBlank, SetExtract               // modifications / post_run
  };
  Type type = Type::None;
  bool quick = false;   // truncate:quick
  std::string name;     // set_param / run_hook name
  double value = 0;     // set_param / set_extract value

  friend bool operator==(const ActionSpec&, const ActionSpec&) = default;
};

// "truncate:quick", "set_param NAME=1.5", "run_hook h", "skip_n", "set_extract=3" ...
Result<ActionSpec> parse_action(std::string_view text);
std::string to_string(const ActionSpec& a);

struct Conditional {
  std::string name;  // unique within a merged set; defaults to "<kind>:<check>"
  ConditionalKind kind = ConditionalKind::Truncation;
  std::string check;
  std::shared_ptr<const Expr> expr;  // parsed once
  int start = 0;      // first reading (1-based count) at which the check is evaluated
  int frequency = 1;  // evaluate every Nth reading
  int ntrips = 1;     // consecutive true evaluations required to trip
  ActionSpec action;
  bool resume = false;
};

// One level's conditionals plus the upstream names it disables.
struct ConditionalSet {
  std::vector<Conditional> items;
  std::vector<std::string> disable;
};

// Level merge, in order system -> queue -> plan -> run. Later levels add; a
// level's `disable` removes named conditionals from all earlier levels. A
// later conditional of the same name replaces the earlier one.
ConditionalSet merge_levels(const std::vector<ConditionalSet>& levels_in_order);

// Trip event, recorded by the engine.
struct Trip {
  std::string name;
  ConditionalKind kind = ConditionalKind::Truncation;
  double value = 0;
  int count = 0;   // consecutive trips at the time of firing
  double ts = 0;
  ActionSpec action;
};

// Evaluates a merged set after every reading in pychron's order:
// modification -> truncation -> action -> termination -> cancelation -> equilibration.
// pre_run / post_run are evaluated on demand with evaluate_kind().
class ConditionalEngine {
 public:
  explicit ConditionalEngine(ConditionalSet set) : set_(std::move(set)) { states_.resize(set_.items.size()); }

  // `reading` is the 1-based reading count. Returns the trips fired by this call.
  std::vector<Trip> evaluate(const MetricContext& ctx, const Variables& vars, int reading, double ts);
  std::vector<Trip> evaluate_kind(ConditionalKind kind, const MetricContext& ctx, const Variables& vars, int reading,
                                  double ts);

  const std::vector<Trip>& trips() const noexcept { return trips_; }
  const std::vector<std::string>& errors() const noexcept { return errors_; }  // unavailable metrics etc.
  void reset();

 private:
  struct State {
    int consecutive = 0;
    bool fired = false;
  };
  ConditionalSet set_;
  std::vector<State> states_;
  std::vector<Trip> trips_;
  std::vector<std::string> errors_;
};

// Whiff plan block: sniff N counts, then an ordered list of check -> action.
struct WhiffCheck {
  std::string check;
  std::shared_ptr<const Expr> expr;
  enum class Action { RunRemainder, Pump, Abort } action = Action::RunRemainder;
};
struct Whiff {
  int sniff = 0;
  std::vector<WhiffCheck> checks;
};
// First matching check's action, if any.
std::optional<WhiffCheck::Action> evaluate_whiff(const Whiff& w, const MetricContext& ctx, const Variables& vars);

// TOML: [[truncations]] ... [[post_run]] tables plus optional `disable = [...]`.
Result<ConditionalSet> parse_conditionals(std::string_view toml_text, std::string_view file = "conditionals.toml");
// [whiff] sniff = N ; [[whiff.checks]] check = "...", action = "pump"
Result<Whiff> parse_whiff(std::string_view toml_text, std::string_view file = "whiff.toml");

}  // namespace pychron::experiment
