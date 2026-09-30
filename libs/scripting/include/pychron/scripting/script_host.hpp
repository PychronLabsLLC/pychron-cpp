#pragma once

// The extraction scripting host (spec section 6). One host serves
// extraction, post-equilibration, post-measurement scripts and measurement
// hooks.
//
// Each execution gets a fresh, isolated module namespace (spec 11 allows a
// fresh namespace instead of a sub-interpreter) holding restricted builtins,
// the vocabulary bound to this run's typed services, the read-only context
// globals and `opt`. The GIL is held only while Python runs; hardware calls
// and waits release it.
//
// check()     compile + AST walk against the vocabulary and the run's
//             capabilities: unknown commands/names, arity, unavailable
//             features, unresolvable valve names and gosubs, disallowed
//             imports, writes to context names. Warnings flag while loops.
// estimate()  executes the script with a DurationAccumulator in place of
//             hardware; gosubs recurse; unbounded loops are flagged.
// run()       check(), then execute main(). Uncaught hardware errors come back
//             with their original kind; cancel/abort as ErrorKind::Cancelled
//             ("cancelled: ..." / "aborted: ..."); script bugs as Config.
//
// With PYCHRON_SCRIPTING off, make_script_host() returns a stub whose every
// call is a "not supported: scripting" error.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/services.hpp"
#include "pychron/scripting/cancel_token.hpp"
#include "pychron/scripting/duration_accumulator.hpp"
#include "pychron/scripting/script.hpp"
#include "pychron/scripting/services.hpp"

namespace pychron::scripting {

struct Diagnostic {
  enum class Severity { Error, Warning };
  Severity severity = Severity::Error;
  std::string script;  // script (or gosub) name
  int line = 0;
  std::string code;  // e.g. "unknown-command", "arity", "not-supported"
  std::string message;
};

struct CheckReport {
  std::vector<Diagnostic> diagnostics;

  bool ok() const noexcept;
  std::vector<Diagnostic> errors() const;
  std::vector<Diagnostic> warnings() const;
  bool has(std::string_view code) const noexcept;
};

struct Estimate {
  Duration total{};
  std::vector<DurationEntry> entries;
  // Why the total is a lower bound (while loops, open-ended waits, pauses).
  std::vector<std::string> unbounded;
  bool bounded() const noexcept { return unbounded.empty(); }
};

struct ScriptLimits {
  std::size_t max_lines = 0;  // executed-line budget; 0 = unlimited
  Duration max_wall_time{};   // Python execution wall time; 0 = unlimited
  int max_gosub_depth = 8;
  std::size_t estimate_max_lines = 2'000'000;  // budget for estimate()
};

using LogSink = std::function<void(std::string_view)>;

// Everything one execution may use. Pointers may be null; a command whose
// service is missing is a NotSupported error (and a static-check error when
// it maps to a capability).
struct ScriptEnvironment {
  extraction::ExtractionServices line;
  IResourceService* resources = nullptr;
  IIntensitySource* intensity = nullptr;  // post_measurement
  std::function<void()> on_pump_time_start;  // post_measurement
  IMeasurementApi* measurement = nullptr;  // measurement hooks
  const IScriptResolver* resolver = nullptr;  // gosub
  const Clock* clock = nullptr;  // default: a SteadyClock
  LogSink log;  // info()/print()
  ScriptContext context;
  ScriptLimits limits;
  std::vector<std::string> allowed_imports;  // empty: default_import_allowlist()

  // For check()/estimate() without live hardware (e.g. `elctl exp
  // validate`): override what capabilities(line) and line.valves report.
  std::optional<extraction::CapabilitySet> capabilities;
  std::optional<std::vector<std::string>> valve_names;
};

struct ScriptResult {
  std::string sha;
  ScriptHeader header;
  std::vector<std::string> messages;  // info() lines in order
};

class IScriptHost {
 public:
  virtual ~IScriptHost() = default;

  // false for the stub host.
  virtual bool available() const noexcept = 0;
  // Config error only if the host cannot check at all (stub).
  virtual Result<CheckReport> check(const Script& script, const ScriptEnvironment& env) = 0;
  virtual Result<Estimate> estimate(const Script& script, const ScriptEnvironment& env) = 0;
  // Executes main().
  virtual Result<ScriptResult> run(const Script& script, const ScriptEnvironment& env,
                                   CancelToken& token) = 0;
  // Measurement hooks: calls `entry(api)` or `entry(api, args)` when args is
  // non-empty. A hook that does not define `entry` is a no-op.
  virtual Result<ScriptResult> call_hook(const Script& script, std::string_view entry,
                                         const ValueMap& args, const ScriptEnvironment& env,
                                         CancelToken& token) = 0;
};

// Python host when built with PYCHRON_SCRIPTING, else the stub.
std::unique_ptr<IScriptHost> make_script_host();
bool scripting_enabled() noexcept;

// Command names the host actually binds (empty for the stub). Must equal the
// names in vocabulary().
std::vector<std::string> bound_commands();

// capabilities(env.line) or the override.
extraction::CapabilitySet effective_capabilities(const ScriptEnvironment& env);

}  // namespace pychron::scripting
