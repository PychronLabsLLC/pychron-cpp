#pragma once

// Conditionals: the model, TOML loading, level merging and the per-reading
// evaluation engine (conditionals spec sections 2, 4, 6).

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/experiment/conditionals/evaluator.hpp"

namespace pychron::experiment {

enum class ConditionalKind { Truncation, Termination, Cancelation, Action, Modification, Equilibration, PreRun, PostRun };

std::string_view to_string(ConditionalKind k) noexcept;
std::optional<ConditionalKind> parse_conditional_kind(std::string_view table_name);  // "truncations", ...

// Where a conditional came from; later levels override earlier ones.
enum class ConditionalLevel { System, Queue, Plan, Run, Hook };
std::string_view to_string(ConditionalLevel l) noexcept;

// Enum action; there are no script snippets.
struct ActionSpec {
  enum class Type {
    None,
    Truncate, Terminate, Cancel, SetParam, RunHook, Notify,     // in-run actions
    SkipNext, SkipN, SkipAliquot, SkipToLastInAliquot,          // queue actions (modifications, post_run)
    SetExtract, Repeat, RunBlank,
  };
  Type type = Type::None;
  bool quick = false;           // truncate:quick
  std::string name;             // set_param / run_hook name
  double value = 0;             // set_param value
  int count = 1;                // skip_n N
  std::vector<double> steps;    // set_extract: successive increments
  bool percent = false;         // set_extract: steps are percentages

  friend bool operator==(const ActionSpec&, const ActionSpec&) = default;
};

// "truncate:quick", "set_param NAME=1.5", "run_hook h", "skip_n 3",
// "set_extract 1,2,3", "set_extract 10%,20%" ...
Result<ActionSpec> parse_action(std::string_view text);
std::string to_string(const ActionSpec& a);
bool is_queue_action(ActionSpec::Type t) noexcept;

struct Conditional {
  std::string name;  // unique within a merged set; defaults to "<kind>:<check>"
  ConditionalKind kind = ConditionalKind::Truncation;
  std::string check;                 // as authored
  std::shared_ptr<const Expr> expr;  // parsed once, after the window/mapper transforms
  int start = 0;      // readings ignored before the first evaluation
  int frequency = 1;  // evaluated every Nth reading after start
  int ntrips = 1;     // consecutive true evaluations required to trip
  std::optional<int> window;               // legacy window (already applied to expr)
  std::string mapper;                      // legacy mapper (already applied to expr)
  std::vector<std::string> analysis_types; // lowercased; empty = all; "blank" = every blank_*
  double abbreviated_count_ratio = 1.0;    // count scale after a truncation it causes
  ActionSpec action;
  bool resume = false;          // action: keep measuring and re-arm
  bool truncate = false;        // modification: also truncate the run
  bool terminate = false;       // modification: also terminate the run
  ConditionalLevel level = ConditionalLevel::Run;
  std::string location;         // source file or plan, for provenance

  // Canonical check after transforms.
  std::string effective_check() const;
  // sha256 hex of the canonical definition (stable across processes).
  std::string id() const;
  // Does it apply to a run of this analysis type?
  bool applies_to(std::string_view analysis_type) const;
};

// Parses `check`, then applies window and mapper. Errors name the check.
Result<std::shared_ptr<const Expr>> compile_check(const std::string& check, std::optional<int> window,
                                                  const std::string& mapper);

// One level's conditionals plus the upstream names it disables.
struct ConditionalSet {
  std::vector<Conditional> items;
  std::vector<std::string> disable;

  // Sets level and location on every item.
  ConditionalSet& stamp(ConditionalLevel level, const std::string& location);
};

// Level merge, in order system -> queue -> plan -> run. Later levels add; a
// level's `disable` removes named conditionals from all earlier levels. A
// later conditional of the same name replaces the earlier one.
ConditionalSet merge_levels(const std::vector<ConditionalSet>& levels_in_order);

// A metric value a check was evaluated against.
struct MetricValue {
  std::string metric;  // to_string(MetricRef)
  double value = 0;
  friend bool operator==(const MetricValue&, const MetricValue&) = default;
};

// Trip event, recorded by the engine.
struct Trip {
  std::string name;
  ConditionalKind kind = ConditionalKind::Truncation;
  double value = 0;  // left operand of a top-level comparison, else the check's value
  int count = 0;     // consecutive true evaluations at the time of firing
  double ts = 0;     // seconds on the measurement axis
  ActionSpec action;
  int reading = 0;   // reading count at which it fired
  std::string id, check;
  ConditionalLevel level = ConditionalLevel::Run;
  std::vector<MetricValue> context;
  // Copied from the conditional so the run layer can act on the trip alone.
  double abbreviated_count_ratio = 1.0;
  bool resume = false, truncate = false, terminate = false;
};

// A check that could not be evaluated; recorded once per conditional.
struct ConditionalError {
  std::string name, message;
  int count = 0;  // evaluations that failed
};

// Measurement order (spec L4); equilibration is evaluated on sniff readings.
inline constexpr ConditionalKind kMeasurementOrder[] = {ConditionalKind::Modification, ConditionalKind::Truncation,
                                                        ConditionalKind::Action, ConditionalKind::Termination,
                                                        ConditionalKind::Cancelation};

// Evaluates a merged set.
//
// Gating (spec L2): after reading n, a conditional is evaluated when
// n > start and (n - start) % frequency == 0. It trips after `ntrips`
// consecutive true evaluations; a false or unevaluable check resets the
// count. It then fires once per run, except an action with resume = true,
// which re-arms. Per call the kinds are visited in the given order and the
// first trip ends the call (spec L4).
class ConditionalEngine {
 public:
  explicit ConditionalEngine(ConditionalSet set, std::string analysis_type = {});

  // Pass/fail reading counts are per call; use the kinds in order.
  std::optional<Trip> evaluate(std::span<const ConditionalKind> kinds, const MetricContext& ctx, const Variables& vars,
                               int reading, double ts);
  std::optional<Trip> evaluate(ConditionalKind kind, const MetricContext& ctx, const Variables& vars, int reading,
                               double ts) {
    return evaluate(std::span<const ConditionalKind>(&kind, 1), ctx, vars, reading, ts);
  }
  // Ungated evaluation (pre-run, post-run, whiff): every applicable
  // conditional of `kind` is evaluated once; ntrips still counts calls.
  // Conditionals that do not apply to `analysis_type` are skipped and keep
  // their trip count.
  std::optional<Trip> check_now(ConditionalKind kind, const MetricContext& ctx, const Variables& vars, double ts = 0,
                                std::string_view analysis_type = {});

  const ConditionalSet& set() const noexcept { return set_; }
  std::vector<const Conditional*> installed() const;  // applicable to the analysis type
  const std::vector<Trip>& trips() const noexcept { return trips_; }
  const std::vector<ConditionalError>& errors() const noexcept { return errors_; }
  void reset();

 private:
  struct State {
    int consecutive = 0;
    bool fired = false;
    bool applicable = true;
  };
  std::optional<Trip> step(std::size_t i, const MetricContext& ctx, const Variables& vars, int reading, double ts);
  void record_error(const Conditional& c, const std::string& message);

  ConditionalSet set_;
  std::vector<State> states_;
  std::vector<Trip> trips_;
  std::vector<ConditionalError> errors_;
};

// Values of every metric `e` reads that `ctx` can answer, for provenance.
std::vector<MetricValue> metric_context(const Expr& e, const MetricContext& ctx);

// Whiff plan block: N readings, then an ordered list of check -> action.
struct WhiffCheck {
  std::string check;
  std::shared_ptr<const Expr> expr;
  enum class Action { RunRemainder, Pump, Abort } action = Action::RunRemainder;
};
struct Whiff {
  int sniff = 0;
  std::vector<WhiffCheck> checks;
};
std::string_view to_string(WhiffCheck::Action a) noexcept;
std::optional<WhiffCheck::Action> parse_whiff_action(std::string_view s) noexcept;
// First matching check's action, if any.
std::optional<WhiffCheck::Action> evaluate_whiff(const Whiff& w, const MetricContext& ctx, const Variables& vars);

// TOML: [[truncations]] ... [[post_run]] tables plus optional `disable = [...]`.
Result<ConditionalSet> parse_conditionals(std::string_view toml_text, std::string_view file = "conditionals.toml");
// [whiff] sniff = N ; [[whiff.checks]] check = "...", action = "pump"
Result<Whiff> parse_whiff(std::string_view toml_text, std::string_view file = "whiff.toml");

}  // namespace pychron::experiment
