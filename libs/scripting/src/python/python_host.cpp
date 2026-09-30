// IScriptHost over embedded CPython: static check, estimate, run, hooks.

#include <set>

#include "internal.hpp"
#include "pychron/scripting/vocabulary.hpp"

namespace pychron::scripting {
namespace {

using python::HostState;
namespace py = pybind11;

std::string format(const Diagnostic& d) {
  return d.script + ":" + std::to_string(d.line) + ": " + d.code + ": " + d.message;
}

Error check_failed(const CheckReport& report) {
  auto errors = report.errors();
  std::string what = "script check failed: " + format(errors.front());
  if (errors.size() > 1) what += " (+" + std::to_string(errors.size() - 1) + " more)";
  return Error{ErrorKind::Config, std::move(what), {}};
}

void check_script(const Script& script, const ScriptEnvironment& env, CheckReport& report,
                  std::set<std::string>& visited, int depth) {
  auto add = [&](Diagnostic::Severity sev, int line, std::string code, std::string message) {
    report.diagnostics.push_back({sev, script.name, line, std::move(code), std::move(message)});
  };
  if (auto header = parse_header(script.text); !header)
    add(Diagnostic::Severity::Error, 0, "header", header.error().what);

  auto& rt = python::runtime();
  auto caps = effective_capabilities(env);

  py::dict vocab;
  for (const auto& cmd : vocabulary()) {
    bool available = !cmd.capability || caps.has(*cmd.capability);
    std::string cap = cmd.capability ? std::string(extraction::to_string(*cmd.capability)) : "";
    vocab[py::str(std::string(cmd.name))] =
        py::make_tuple(available, command_allowed(cmd, script.kind), cmd.valve_argument, cap);
  }

  py::list known;
  for (auto name : rt.module.attr("SAFE_BUILTINS")) known.append(name);
  for (auto name : {"print", "__import__", "__name__", "opt", "ScriptCancelled", "HardwareError",
                    "NotSupportedError"})
    known.append(py::str(name));
  py::list readonly;
  readonly.append(py::str("opt"));
  for (const auto& [k, v] : env.context.globals) {
    known.append(py::str(k));
    readonly.append(py::str(k));
  }

  py::object valves = py::none();
  if (env.valve_names) valves = py::cast(*env.valve_names);
  else if (env.line.valves && caps.has(extraction::Capability::Valves))
    valves = py::cast(env.line.valves->names());

  py::list imports;
  if (env.allowed_imports.empty())
    for (auto m : default_import_allowlist()) imports.append(py::str(std::string(m)));
  else
    for (const auto& m : env.allowed_imports) imports.append(py::str(m));

  auto result = rt.module.attr("check_source")(script.text, script.name, vocab, rt.reference,
                                                known, valves, imports, readonly,
                                                script.kind != ScriptKind::MeasurementHook);
  auto tuple = result.cast<py::tuple>();
  for (auto item : tuple[0].cast<py::list>()) {
    auto d = item.cast<py::tuple>();
    add(d[0].cast<std::string>() == "error" ? Diagnostic::Severity::Error
                                             : Diagnostic::Severity::Warning,
        d[1].cast<int>(), d[2].cast<std::string>(), d[3].cast<std::string>());
  }

  for (auto item : tuple[1].cast<py::list>()) {
    auto g = item.cast<py::tuple>();
    int line = g[0].cast<int>();
    auto name = g[1].cast<std::string>();
    if (!env.resolver) {
      add(Diagnostic::Severity::Error, line, "unknown-gosub", "gosub '" + name + "': no resolver");
      continue;
    }
    auto sub = env.resolver->resolve(name, script.kind);
    if (!sub) {
      add(Diagnostic::Severity::Error, line, "unknown-gosub", sub.error().what);
      continue;
    }
    if (depth + 1 > env.limits.max_gosub_depth) {
      add(Diagnostic::Severity::Error, line, "gosub-depth", "gosub nesting too deep at '" + name + "'");
      continue;
    }
    if (!visited.insert(sub->name).second) continue;
    check_script(*sub, env, report, visited, depth + 1);
  }
}

class PythonScriptHost final : public IScriptHost {
 public:
  PythonScriptHost() { python::ensure_interpreter(); }

  bool available() const noexcept override { return true; }

  Result<CheckReport> check(const Script& script, const ScriptEnvironment& env) override {
    py::gil_scoped_acquire gil;
    CheckReport report;
    std::set<std::string> visited{script.name};
    try {
      check_script(script, env, report, visited, 0);
    } catch (const std::exception& e) {
      return fail(ErrorKind::Config, std::string("static check failed: ") + e.what());
    }
    return report;
  }

  Result<Estimate> estimate(const Script& script, const ScriptEnvironment& env) override {
    auto report = check(script, env);
    if (!report) return fail(report.error());
    if (!report->ok()) return fail(check_failed(*report));

    DurationAccumulator acc;
    for (const auto& w : report->warnings())
      if (w.code == "unbounded-loop")
        acc.flag_unbounded("while loop at " + w.script + ":" + std::to_string(w.line));

    CancelToken token;
    py::gil_scoped_acquire gil;
    HostState state(env, token, HostState::Mode::Estimate, script.kind, &acc);
    auto r = state.guarded([&] {
      if (script.kind == ScriptKind::MeasurementHook) state.load(script);
      else state.run_main(script);
    });
    if (!r) {
      if (!r.error().what.starts_with("estimate budget")) return fail(r.error());
      acc.flag_unbounded("line budget exhausted (a loop the estimate cannot bound)");
    }
    return Estimate{acc.total(), acc.entries(), acc.unbounded()};
  }

  Result<ScriptResult> run(const Script& script, const ScriptEnvironment& env,
                           CancelToken& token) override {
    return execute(script, env, token, [&](HostState& state) { state.run_main(script); });
  }

  Result<ScriptResult> call_hook(const Script& script, std::string_view entry,
                                 const ValueMap& args, const ScriptEnvironment& env,
                                 CancelToken& token) override {
    std::string name(entry);
    return execute(script, env, token, [&](HostState& state) {
      py::dict ns = state.load(script);
      if (!ns.contains(name)) return;
      py::object fn = ns[py::str(name)];
      if (args.empty()) {
        fn(state.hook_api());
        return;
      }
      py::dict kwargs;
      for (const auto& [k, v] : args)
        kwargs[py::str(k)] = std::visit(
            [](const auto& x) -> py::object {
              using T = std::decay_t<decltype(x)>;
              if constexpr (std::is_same_v<T, std::monostate>) return py::none();
              else return py::cast(x);
            },
            v);
      fn(state.hook_api(), kwargs);
    });
  }

 private:
  Result<ScriptResult> execute(const Script& script, const ScriptEnvironment& env,
                               CancelToken& token, const std::function<void(HostState&)>& body) {
    auto header = parse_header(script.text);
    if (!header) return fail(header.error());
    auto report = check(script, env);
    if (!report) return fail(report.error());
    if (!report->ok()) return fail(check_failed(*report));

    ScriptResult out{sha256_hex(script.text), std::move(*header), {}};
    Result<void> r;
    {
      py::gil_scoped_acquire gil;
      HostState state(env, token, HostState::Mode::Run, script.kind, nullptr);
      r = state.guarded([&] { body(state); });
      out.messages = state.messages();
    }
    if (!r) return fail(r.error());
    if (token.mode() == CancelMode::Abort) return fail(ErrorKind::Cancelled, "aborted: " + script.name);
    if (token.mode() == CancelMode::Cancel)
      return fail(ErrorKind::Cancelled, "cancelled: " + script.name);
    return out;
  }
};

}  // namespace

std::unique_ptr<IScriptHost> make_script_host() { return std::make_unique<PythonScriptHost>(); }

bool scripting_enabled() noexcept { return true; }

std::vector<std::string> bound_commands() {
  python::ensure_interpreter();
  py::gil_scoped_acquire gil;
  std::vector<std::string> out;
  auto& reference = python::runtime().reference;
  for (auto item : reference) {
    auto name = py::str(item.first).cast<std::string>();
    if (name.starts_with("_") || name == "HardwareError") continue;
    out.push_back(name);
  }
  return out;
}

}  // namespace pychron::scripting
