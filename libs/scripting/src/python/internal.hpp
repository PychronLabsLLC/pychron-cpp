#pragma once

// Internals of the embedded-CPython host. Nothing here crosses the library
// boundary; Python exceptions are converted to Error in python_host.cpp.

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "pychron/scripting/script_host.hpp"

namespace pychron::scripting::python {

namespace py = pybind11;

// Python source of the runtime module (exceptions, Options, restricted
// builtins, static checker) and of the per-execution vocabulary prelude.
extern const char* const kRuntimeSource;
extern const char* const kPreludeSource;
inline constexpr const char* kPreludeFilename = "<pychron-prelude>";
inline constexpr const char* kRuntimeFilename = "<pychron-runtime>";

// Process-wide interpreter state, created on first use and never finalized.
// Access with the GIL held.
struct Runtime {
  py::object module;        // _pychron_runtime
  py::object prelude_code;  // compiled kPreludeSource
  py::dict reference;       // a prelude namespace for signatures (no host)
  py::object cancelled, aborted, limit, budget, hardware, not_supported;
};

// Initializes the interpreter (once) and releases the GIL. Call without the
// GIL held.
void ensure_interpreter();
// With the GIL held.
Runtime& runtime();

class HostState;

// Measurement-hook `api` and get_device() handles; both hold the state that
// created them.
struct HookApi {
  HostState* state;
};
struct DeviceHandle {
  HostState* state;
};

// One execution (run, estimate or hook) and its gosubs: the object behind
// `_h` in the prelude. Construct and use with the GIL held.
class HostState {
 public:
  // Rehearse: every hardware call and wait is a step of `rehearsal`.
  enum class Mode { Run, Estimate, Rehearse };

  HostState(const ScriptEnvironment& env, CancelToken& token, Mode mode, ScriptKind kind,
            DurationAccumulator* accumulator, IRehearsal* rehearsal = nullptr);
  ~HostState();
  HostState(const HostState&) = delete;
  HostState& operator=(const HostState&) = delete;

  // Compiles and executes `script` in a fresh namespace; returns it.
  py::dict load(const Script& script, const ValueMap& overrides = {});
  // load() then main().
  void run_main(const Script& script, const ValueMap& overrides = {});
  // Runs `body` under the line tracer; converts Python exceptions.
  Result<void> guarded(const std::function<void()>& body);

  const std::vector<std::string>& messages() const noexcept { return messages_; }
  py::object hook_api();

  // ---- bound to `_h` --------------------------------------------------
  bool estimating() const noexcept { return mode_ == Mode::Estimate; }
  bool rehearsing() const noexcept { return mode_ == Mode::Rehearse; }
  // Rehearse, for the prelude's own multi-step commands (ramp, waitfor,
  // begin_heating_interval): one step; `seconds_held` is the time it stands
  // for. Returns the step's value.
  py::object rehearse_step(const std::string& command, const py::list& args, const py::dict& kwargs,
                           double seconds_held);
  double now();
  void info(const std::string& message);
  void require(const std::string& capability);

  void open(const std::string& name);
  void close(const std::string& name);
  void lock(const std::string& name);
  void unlock(const std::string& name);
  bool is_open(const std::string& name);
  bool is_closed(const std::string& name);

  void extract(double value, const std::string& units);
  void end_extract();
  void enable();
  void disable();
  void prepare();
  void fire_laser();
  void warmup();
  DeviceHandle get_device(const std::string& name);
  // DeviceHandle reads; a step in Rehearse.
  bool device_is_enabled();
  double device_output();

  void move_to_position(const std::string& position, bool autocenter, bool block);
  void set_axis(const std::string& axis, double value, bool block);
  void set_xy(double x, double y, bool block);
  void set_tray(const std::string& tray);
  void execute_pattern(const std::string& pattern, bool block, double duration_s);

  void dump_sample();
  void drop_sample(const std::string& position);
  void set_pid_parameters(double value);
  void load_pipette(const std::string& name);
  void extract_pipette(const std::string& name);
  void set_motor(const std::string& name, double value, bool block);
  double get_value(const std::string& name);
  void set_cryo(double value, bool block);
  void set_cryo_named(const std::string& name, bool block);
  double get_cryo_temp(int channel);
  std::string snapshot(const std::string& name);
  void video_start(const std::string& name);
  void video_stop();
  double get_pressure(const std::string& controller, const std::string& gauge);
  double get_manometer_pressure(const std::string& name);

  void sleep(double seconds, const std::string& command);
  void pause(double seconds);
  void wake();
  void estimate_wait(const std::string& command, double timeout);

  void acquire(const std::string& name);
  void wait_resource(const std::string& name, double value);
  void release(const std::string& name);
  void set_resource(const std::string& name, double value);
  double get_resource_value(const std::string& name);

  void gosub(const std::string& name, py::dict kwargs);
  double get_intensity(const std::string& key);
  void signal_pump_time_start();

  // hook api / device handle
  IMeasurementApi& measurement();
  extraction::IExtractionDevice& device();
  // Raises the Python exception for `error`.
  [[noreturn]] void raise(const Error& error);
  // Raises ScriptAborted after an Abort request.
  void guard();
  template <class T>
  T unwrap(Result<T> r) {
    if (!r) raise(r.error());
    if constexpr (!std::is_void_v<T>) return std::move(*r);
  }

  // Line tracer callback.
  int on_line(PyFrameObject* frame);

 private:
  extraction::IValveService& valves();
  extraction::IPressureService& pressure();
  // The line's cryostat when the run has one, else the extract device's.
  extraction::ICryo& cryo();
  IResourceService& resources();
  const Clock& clock() const;
  // Raises ScriptCancelled/ScriptAborted if a request is pending.
  void check_requested(const std::string& command);
  // Waits `d` on the token; raises on a request. true if woken.
  bool wait_period(Duration d, const std::string& command);
  // Polls `busy` every poll period; on a request calls `stop` then raises.
  std::function<void()> stop_stage(extraction::IStage* stage);
  void wait_while(const std::function<Result<bool>()>& busy, const std::string& command,
                  const std::function<void()>& stop = {});
  Error convert(py::error_already_set& e);
  void add_estimate(Duration d, const std::string& command);
  // Rehearse: hands the call to the rehearsal without the GIL, at the line
  // the trace last saw; its error is raised as the real call's would be.
  Value step(std::string command, std::vector<Value> args, ValueMap kwargs = {});
  // Rehearse: a step of a call that answers nothing / a double / a string
  // (guard() first). A double read accepts an integer; any other type is a
  // Config error naming the command.
  void rehearse_void(const std::string& command, std::vector<Value> args, ValueMap kwargs = {});
  double rehearse_double(const std::string& command, std::vector<Value> args);
  bool rehearse_bool(const std::string& command, std::vector<Value> args);
  std::string rehearse_string(const std::string& command, std::vector<Value> args);
  // Rehearse: a read's answer as a bool; Config error for any other type.
  bool step_bool(const std::string& command, const std::string& name);

  const ScriptEnvironment& env_;
  CancelToken& token_;
  Mode mode_;
  ScriptKind kind_;
  DurationAccumulator* accumulator_;
  IRehearsal* rehearsal_;
  Duration rehearsed_{};  // Rehearse: stepped waits so far (what now() reports)
  SteadyClock steady_;

  std::vector<std::string> messages_;
  std::vector<py::dict> namespaces_;  // cleared on destruction to drop `_h`
  std::vector<std::string> script_stack_;
  int gosub_depth_ = 0;

  // Line tracer state.
  std::size_t lines_ = 0;
  std::size_t line_budget_ = 0;
  std::chrono::steady_clock::time_point started_;
  PyObject* last_file_ = nullptr;
  bool in_host_code_ = false;  // prelude or runtime frames
  std::string current_script_;
  int current_line_ = 0;

  // The last hardware error raised into Python and its exception object, so
  // an uncaught one comes back with its original kind and device.
  std::optional<Error> last_error_;
  py::object last_exception_;
};

}  // namespace pychron::scripting::python
