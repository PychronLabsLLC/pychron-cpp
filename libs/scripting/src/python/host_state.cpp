// HostState: the vocabulary's C++ side for one execution. Every hardware call
// checks for Abort, releases the GIL, and turns a failed Result into a Python
// exception; estimate mode skips hardware and books time instead.


#include "internal.hpp"
#include "pychron/scripting/vocabulary.hpp"

namespace pychron::scripting::python {
namespace {

using extraction::Capability;
using extraction::not_supported;

constexpr auto kPoll = std::chrono::milliseconds(50);
constexpr auto kResourcePoll = std::chrono::milliseconds(250);

Duration seconds(double s) {
  if (!(s > 0)) return Duration::zero();
  return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(s));
}

template <class F>
auto nogil(F&& f) {
  py::gil_scoped_release release;
  return f();
}

int trace(PyObject* obj, PyFrameObject* frame, int what, PyObject*) {
  if (what != PyTrace_LINE) return 0;
  return static_cast<HostState*>(PyCapsule_GetPointer(obj, nullptr))->on_line(frame);
}

py::object to_python(const Value& v) {
  return std::visit(
      [](const auto& x) -> py::object {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return py::none();
        else return py::cast(x);
      },
      v);
}

std::optional<ErrorKind> kind_from_string(std::string_view s) {
  for (auto k : {ErrorKind::Timeout, ErrorKind::Io, ErrorKind::Protocol, ErrorKind::Config,
                 ErrorKind::NotConnected, ErrorKind::Interlock, ErrorKind::Cancelled})
    if (to_string(k) == s) return k;
  return std::nullopt;
}

}  // namespace

HostState::HostState(const ScriptEnvironment& env, CancelToken& token, Mode mode, ScriptKind kind,
                     DurationAccumulator* accumulator)
    : env_(env), token_(token), mode_(mode), kind_(kind), accumulator_(accumulator) {
  line_budget_ = mode == Mode::Estimate ? env.limits.estimate_max_lines : env.limits.max_lines;
}

HostState::~HostState() {
  for (auto& ns : namespaces_) ns.clear();
}

// ---- execution ---------------------------------------------------------

py::dict HostState::load(const Script& script, const ValueMap& overrides) {
  auto& rt = runtime();
  py::object self = py::cast(this, py::return_value_policy::reference);

  py::dict context;
  for (const auto& [k, v] : env_.context.globals) context[py::str(k)] = to_python(v);
  for (const auto& [k, v] : overrides) context[py::str(k)] = to_python(v);

  py::dict prelude;
  prelude["__builtins__"] = py::module_::import("builtins");
  prelude["_h"] = self;
  prelude["_context"] = context;
  prelude["HardwareError"] = rt.hardware;
  namespaces_.push_back(prelude);
  PyObject* done = PyEval_EvalCode(rt.prelude_code.ptr(), prelude.ptr(), prelude.ptr());
  if (!done) throw py::error_already_set();
  Py_DECREF(done);

  py::list imports;
  if (env_.allowed_imports.empty())
    for (auto m : default_import_allowlist()) imports.append(py::str(std::string(m)));
  else
    for (const auto& m : env_.allowed_imports) imports.append(py::str(m));

  py::dict ns;
  ns["__builtins__"] = rt.module.attr("make_builtins")(imports, self.attr("info"));
  ns["__name__"] = "__pychron_script__";
  for (auto item : context) ns[item.first] = item.second;
  py::dict options;
  for (const auto& [k, v] : env_.context.options) options[py::str(k)] = to_python(v);
  ns["opt"] = rt.module.attr("Options")(options);
  ns["ScriptCancelled"] = rt.cancelled;
  ns["HardwareError"] = rt.hardware;
  ns["NotSupportedError"] = rt.not_supported;
  for (const auto& cmd : vocabulary()) {
    if (!command_allowed(cmd, kind_)) continue;
    auto name = py::str(std::string(cmd.name));
    ns[name] = prelude[name];
  }
  namespaces_.push_back(ns);

  PyObject* code = Py_CompileString(script.text.c_str(), script.name.c_str(), Py_file_input);
  if (!code) throw py::error_already_set();
  auto code_obj = py::reinterpret_steal<py::object>(code);
  script_stack_.push_back(script.name);
  PyObject* result = PyEval_EvalCode(code, ns.ptr(), ns.ptr());
  script_stack_.pop_back();
  if (!result) throw py::error_already_set();
  Py_DECREF(result);
  return ns;
}

void HostState::run_main(const Script& script, const ValueMap& overrides) {
  py::dict ns = load(script, overrides);
  if (!ns.contains("main"))
    raise(Error{ErrorKind::Config, script.name + " defines no main()", {}});
  script_stack_.push_back(script.name);
  try {
    ns["main"]();
  } catch (...) {
    script_stack_.pop_back();
    throw;
  }
  script_stack_.pop_back();
}

Result<void> HostState::guarded(const std::function<void()>& body) {
  started_ = std::chrono::steady_clock::now();
  auto capsule = py::capsule(static_cast<void*>(this));
  PyEval_SetTrace(trace, capsule.ptr());
  Result<void> out;
  try {
    body();
  } catch (py::error_already_set& e) {
    out = fail(convert(e));
  } catch (const std::exception& e) {
    out = fail(ErrorKind::Config, std::string("script host: ") + e.what());
  }
  PyEval_SetTrace(nullptr, nullptr);
  return out;
}

Error HostState::convert(py::error_already_set& e) {
  auto& rt = runtime();
  auto message = [&] { return py::str(e.value()).cast<std::string>(); };
  if (e.matches(rt.aborted)) return Error{ErrorKind::Cancelled, "aborted: " + message(), {}};
  if (e.matches(rt.cancelled)) return Error{ErrorKind::Cancelled, "cancelled: " + message(), {}};
  if (e.matches(rt.limit)) return Error{ErrorKind::Config, "script limit: " + message(), {}};
  if (e.matches(rt.budget)) return Error{ErrorKind::Config, "estimate budget: " + message(), {}};
  if (e.matches(rt.hardware)) {
    if (last_error_ && last_exception_ && e.value().is(last_exception_)) return *last_error_;
    auto kind = kind_from_string(py::str(e.value().attr("kind")).cast<std::string>());
    return Error{kind.value_or(ErrorKind::Io), message(),
                 py::str(e.value().attr("device")).cast<std::string>()};
  }
  auto text = rt.module.attr("describe")(e.value(), py::make_tuple(kPreludeFilename, kRuntimeFilename)).cast<std::string>();
  return Error{ErrorKind::Config, text, {}};
}

int HostState::on_line(PyFrameObject* frame) {
  ++lines_;
  PyCodeObject* code = PyFrame_GetCode(frame);
  if (code->co_filename != last_file_) {
    last_file_ = code->co_filename;
    auto name = py::reinterpret_borrow<py::str>(code->co_filename).cast<std::string>();
    in_host_code_ = name == kPreludeFilename || name == kRuntimeFilename;
    if (!in_host_code_) current_script_ = name;
  }
  Py_DECREF(code);
  if (!in_host_code_) current_line_ = PyFrame_GetLineNumber(frame);

  auto& rt = runtime();
  if (token_.mode() == CancelMode::Abort) {
    PyErr_SetString(rt.aborted.ptr(), "abort requested");
    return -1;
  }
  if (line_budget_ && lines_ > line_budget_) {
    PyErr_SetString(estimating() ? rt.budget.ptr() : rt.limit.ptr(),
                    ("executed-line limit " + std::to_string(line_budget_) + " reached").c_str());
    return -1;
  }
  if (env_.limits.max_wall_time > Duration::zero() && lines_ % 256 == 0 &&
      std::chrono::steady_clock::now() - started_ > env_.limits.max_wall_time) {
    PyErr_SetString(rt.limit.ptr(), "wall-time limit reached");
    return -1;
  }
  return 0;
}

// ---- errors and services ----------------------------------------------

void HostState::raise(const Error& error) {
  auto& rt = runtime();
  py::object exc;
  if (error.kind == ErrorKind::Cancelled)
    exc = (token_.mode() == CancelMode::Abort ? rt.aborted : rt.cancelled)(error.what);
  else if (extraction::is_not_supported(error))
    exc = rt.not_supported(error.what, error.device);
  else
    exc = rt.hardware(std::string(to_string(error.kind)), error.what, error.device);
  last_error_ = error;
  last_exception_ = exc;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): CPython's own idiom: a type object is an object
  PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(exc.ptr())), exc.ptr());
  throw py::error_already_set();
}

void HostState::guard() {
  if (token_.mode() == CancelMode::Abort) {
    PyErr_SetString(runtime().aborted.ptr(), "hardware refused after abort");
    throw py::error_already_set();
  }
}

void HostState::check_requested(const std::string& command) {
  auto mode = token_.mode();
  if (mode == CancelMode::None) return;
  auto& rt = runtime();
  PyErr_SetString(mode == CancelMode::Abort ? rt.aborted.ptr() : rt.cancelled.ptr(),
                  (command + " interrupted").c_str());
  throw py::error_already_set();
}

bool HostState::wait_period(Duration d, const std::string& command) {
  check_requested(command);
  auto deadline = clock().now() + d;
  auto r = nogil([&] { return token_.wait_until(clock(), deadline); });
  if (r == WaitResult::Cancelled) check_requested(command);
  return r == WaitResult::Woken;
}

void HostState::wait_while(const std::function<Result<bool>()>& busy, const std::string& command,
                           const std::function<void()>& stop) {
  while (true) {
    auto r = nogil([&] { return busy(); });
    if (!unwrap(std::move(r))) return;
    try {
      wait_period(kPoll, command);
    } catch (py::error_already_set&) {
      if (stop) {
        py::error_scope keep;  // stop() must not clobber the pending exception
        stop();
      }
      throw;
    }
  }
}

// cppcheck-suppress returnTempReference ; the environment's clock or a member: neither is a temporary
const Clock& HostState::clock() const { return env_.clock ? *env_.clock : steady_; }

extraction::IValveService& HostState::valves() {
  if (!env_.line.valves) raise(not_supported(Capability::Valves));
  return *env_.line.valves;
}

extraction::IPressureService& HostState::pressure() {
  if (!env_.line.pressure) raise(not_supported(Capability::Pressure));
  return *env_.line.pressure;
}

extraction::IExtractionDevice& HostState::device() {
  if (!env_.line.device) raise(not_supported("extraction device"));
  return *env_.line.device;
}

IResourceService& HostState::resources() {
  if (!env_.resources) raise(not_supported("shared resources"));
  return *env_.resources;
}

IMeasurementApi& HostState::measurement() {
  if (!env_.measurement) raise(not_supported("measurement api"));
  return *env_.measurement;
}

py::object HostState::hook_api() { return py::cast(HookApi{this}); }

void HostState::add_estimate(Duration d, const std::string& command) {
  if (accumulator_) accumulator_->add(d, command, current_script_, current_line_);
}

// ---- vocabulary --------------------------------------------------------

double HostState::now() {
  auto t = estimating() && accumulator_ ? accumulator_->now() : clock().now();
  return std::chrono::duration<double>(t.time_since_epoch()).count();
}

void HostState::info(const std::string& message) {
  messages_.push_back(message);
  if (env_.log) env_.log(message);
}

void HostState::require(const std::string& capability) {
  if (estimating()) return;
  auto cap = extraction::capability_from_string(capability);
  if (cap && !effective_capabilities(env_).has(*cap)) raise(not_supported(*cap));
}

// Resolve the service with the GIL held (a missing one raises), then call it
// without.
#define PYCHRON_HW(fallback, service, call) \
  if (estimating()) return fallback; \
  guard(); \
  auto& svc = service; \
  return unwrap(nogil([&] { return svc.call; }))

void HostState::open(const std::string& n) { PYCHRON_HW(, valves(), open(n)); }
void HostState::close(const std::string& n) { PYCHRON_HW(, valves(), close(n)); }
void HostState::lock(const std::string& n) { PYCHRON_HW(, valves(), lock(n)); }
void HostState::unlock(const std::string& n) { PYCHRON_HW(, valves(), unlock(n)); }
bool HostState::is_open(const std::string& n) { PYCHRON_HW(false, valves(), is_open(n)); }
bool HostState::is_closed(const std::string& n) { PYCHRON_HW(false, valves(), is_closed(n)); }

void HostState::extract(double value, const std::string& units) {
  if (estimating()) return;
  guard();
  auto u = extraction::extract_units_from_string(units);
  if (!u) raise(Error{ErrorKind::Config, "unknown extract units '" + units + "'", {}});
  auto& dev = device();
  unwrap(nogil([&] { return dev.extract(value, *u); }));
}

void HostState::end_extract() { PYCHRON_HW(, device(), end_extract()); }
void HostState::enable() { PYCHRON_HW(, device(), enable()); }
void HostState::disable() { PYCHRON_HW(, device(), disable()); }
void HostState::prepare() { PYCHRON_HW(, device(), prepare()); }

void HostState::fire_laser() {
  if (estimating()) return;
  guard();
  auto* laser = device().laser();
  if (!laser) raise(not_supported(Capability::Laser, device().device_name()));
  unwrap(nogil([&] { return laser->fire_laser(); }));
}

void HostState::warmup() {
  if (estimating()) return;
  guard();
  auto* laser = device().laser();
  if (!laser) raise(not_supported(Capability::Laser, device().device_name()));
  unwrap(nogil([&] { return laser->warmup(); }));
}

DeviceHandle HostState::get_device(const std::string& name) {
  if (!estimating() && !name.empty() && device().device_name() != name)
    raise(Error{ErrorKind::Config, "no device '" + name + "' in this run", {}});
  return DeviceHandle{this};
}

namespace {
template <class T>
T* feature(HostState& st, T* (extraction::IExtractionDevice::*get)(), Capability cap) {
  auto& dev = st.device();
  T* f = (dev.*get)();
  if (!f) st.raise(not_supported(cap, dev.device_name()));
  return f;
}
}  // namespace

// What a cancelled wait on the stage does: stop it where it is. The beam may
// be on, and a stage that goes on to its target heats whatever it passes. A
// stage that cannot stop, or fails to, does not keep the script from ending.
std::function<void()> HostState::stop_stage(extraction::IStage* stage) {
  return [stage] { (void)nogil([&] { return stage->stop(); }); };
}

void HostState::move_to_position(const std::string& position, bool autocenter, bool block) {
  if (estimating()) return;
  guard();
  check_requested("move_to_position");
  auto* stage = feature(*this, &extraction::IExtractionDevice::stage, Capability::Stage);
  if (!block && autocenter && stage->autocenter_needs_polling()) {
    // This stage centers the hole after it arrives, and only while the
    // script waits: unwaited, the hole would be left uncentered and a failure
    // to center it never seen.
    unwrap(Result<void>(fail(ErrorKind::Config,
                             "move_to_position(block=False) cannot center the hole: wait for the move, or say "
                             "autocenter=False",
                             device().device_name())));
  }
  unwrap(nogil([&] { return stage->move_to_position(position, autocenter); }));
  if (block) {
    wait_while([&] { return stage->moving(); }, "move_to_position", stop_stage(stage));
    // Centered, and by how much; or not, and why: into the run's log.
    if (const std::string note = stage->last_move_note(); !note.empty() && env_.log) env_.log(note);
  }
}

void HostState::set_axis(const std::string& axis, double value, bool block) {
  if (estimating()) return;
  guard();
  check_requested("set_" + axis);
  auto* stage = feature(*this, &extraction::IExtractionDevice::stage, Capability::Stage);
  auto a = axis == "x" ? extraction::IStage::Axis::X
                       : axis == "y" ? extraction::IStage::Axis::Y : extraction::IStage::Axis::Z;
  unwrap(nogil([&] { return stage->set_axis(a, value); }));
  if (block) wait_while([&] { return stage->moving(); }, "set_" + axis, stop_stage(stage));
}

void HostState::set_xy(double x, double y, bool block) {
  if (estimating()) return;
  guard();
  check_requested("set_xy");
  auto* stage = feature(*this, &extraction::IExtractionDevice::stage, Capability::Stage);
  unwrap(nogil([&] { return stage->set_xy(x, y); }));
  if (block) wait_while([&] { return stage->moving(); }, "set_xy", stop_stage(stage));
}

void HostState::set_tray(const std::string& tray) {
  if (estimating()) return;
  guard();
  auto* stage = feature(*this, &extraction::IExtractionDevice::stage, Capability::Stage);
  unwrap(nogil([&] { return stage->set_tray(tray); }));
}

void HostState::execute_pattern(const std::string& pattern, bool block, double duration_s) {
  if (estimating()) return;
  guard();
  check_requested("execute_pattern");
  auto* runner =
      feature(*this, &extraction::IExtractionDevice::pattern_runner, Capability::Pattern);
  if (!block && runner->needs_polling()) {
    // This device's patterns advance only while the script waits for them:
    // unwaited, the beam would sit on the first point for the whole heating.
    unwrap(Result<void>(fail(ErrorKind::Config,
                             "execute_pattern(block=False) cannot be used with this device: its patterns run only "
                             "while the script waits for them",
                             device().device_name())));
  }
  // With the run's duration: a pattern that runs for a time runs for that.
  unwrap(nogil([&] { return runner->execute_pattern_for(pattern, duration_s); }));
  if (block) {
    wait_while([&] { return runner->running(); }, "execute_pattern",
               [&] { (void)nogil([&] { return runner->stop_pattern(); }); });
    if (const std::string note = runner->last_note(); !note.empty() && env_.log) env_.log(note);
  }
}

void HostState::dump_sample() {
  if (estimating()) return;
  guard();
  auto* f = feature(*this, &extraction::IExtractionDevice::furnace, Capability::Furnace);
  unwrap(nogil([&] { return f->dump_sample(); }));
}

void HostState::drop_sample(const std::string& position) {
  if (estimating()) return;
  guard();
  auto* f = feature(*this, &extraction::IExtractionDevice::furnace, Capability::Furnace);
  unwrap(nogil([&] { return f->drop_sample(position); }));
}

void HostState::set_pid_parameters(double value) {
  if (estimating()) return;
  guard();
  auto* f = feature(*this, &extraction::IExtractionDevice::furnace, Capability::Furnace);
  unwrap(nogil([&] { return f->set_pid_parameters(value); }));
}

void HostState::load_pipette(const std::string& name) {
  if (estimating()) return;
  guard();
  auto* p = feature(*this, &extraction::IExtractionDevice::pipettes, Capability::Pipette);
  unwrap(nogil([&] { return p->load_pipette(name); }));
}

void HostState::extract_pipette(const std::string& name) {
  if (estimating()) return;
  guard();
  auto* p = feature(*this, &extraction::IExtractionDevice::pipettes, Capability::Pipette);
  unwrap(nogil([&] { return p->extract_pipette(name); }));
}

void HostState::set_motor(const std::string& name, double value, bool block) {
  if (estimating()) return;
  guard();
  auto* m = feature(*this, &extraction::IExtractionDevice::motors, Capability::Motor);
  unwrap(nogil([&] { return m->set_motor(name, value); }));
  if (block) wait_while([&] { return m->motor_moving(name); }, "set_motor");
}

double HostState::get_value(const std::string& name) {
  if (estimating()) return 0.0;
  guard();
  auto* m = feature(*this, &extraction::IExtractionDevice::motors, Capability::Motor);
  return unwrap(nogil([&] { return m->get_value(name); }));
}

extraction::ICryo& HostState::cryo() {
  if (env_.line.cryo) return *env_.line.cryo;
  if (!env_.line.device) raise(not_supported(Capability::Cryo));
  return *feature(*this, &extraction::IExtractionDevice::cryo, Capability::Cryo);
}

void HostState::set_cryo(double value, bool block) {
  if (estimating()) return;
  guard();
  auto& c = cryo();
  unwrap(nogil([&] { return c.set_cryo(value); }));
  if (block) wait_while([&] { return c.cryo_settling(); }, "set_cryo");
}

void HostState::set_cryo_named(const std::string& name, bool block) {
  if (estimating()) return;
  guard();
  auto& c = cryo();
  unwrap(nogil([&] { return c.set_cryo_named(name); }));
  if (block) wait_while([&] { return c.cryo_settling(); }, "set_cryo");
}

double HostState::get_cryo_temp(int channel) {
  if (estimating()) return 0.0;
  guard();
  auto& c = cryo();
  return unwrap(nogil([&] { return c.get_cryo_temp(channel); }));
}

std::string HostState::snapshot(const std::string& name) {
  if (estimating()) return {};
  guard();
  auto* im = feature(*this, &extraction::IExtractionDevice::imaging, Capability::Imaging);
  return unwrap(nogil([&] { return im->snapshot(name); }));
}

void HostState::video_start(const std::string& name) {
  if (estimating()) return;
  guard();
  auto* im = feature(*this, &extraction::IExtractionDevice::imaging, Capability::Imaging);
  unwrap(nogil([&] { return im->start_video_recording(name); }));
}

void HostState::video_stop() {
  if (estimating()) return;
  guard();
  auto* im = feature(*this, &extraction::IExtractionDevice::imaging, Capability::Imaging);
  unwrap(nogil([&] { return im->stop_video_recording(); }));
}

double HostState::get_pressure(const std::string& controller, const std::string& gauge) {
  PYCHRON_HW(0.0, pressure(), get_pressure(controller, gauge));
}

double HostState::get_manometer_pressure(const std::string& name) {
  PYCHRON_HW(0.0, pressure(), get_manometer_pressure(name));
}

#undef PYCHRON_HW

void HostState::sleep(double s, const std::string& command) {
  if (estimating()) return add_estimate(seconds(s), command);
  wait_period(seconds(s), command);
}

void HostState::pause(double s) {
  if (estimating()) {
    if (s > 0) return add_estimate(seconds(s), "pause");
    if (accumulator_) accumulator_->flag_unbounded("pause() without a duration");
    return;
  }
  if (s > 0) {
    wait_period(seconds(s), "pause");
    return;
  }
  // Until wake().
  while (!wait_period(std::chrono::hours(1), "pause")) {
  }
}

void HostState::wake() {
  if (!estimating()) token_.wake();
}

void HostState::estimate_wait(const std::string& command, double timeout) {
  if (timeout > 0) return add_estimate(seconds(timeout), command);
  if (accumulator_)
    accumulator_->flag_unbounded(command + " without a timeout (" + current_script_ + ":" +
                                 std::to_string(current_line_) + ")");
}

void HostState::acquire(const std::string& name) {
  if (estimating()) return;
  guard();
  auto& res = resources();
  while (!unwrap(nogil([&] { return res.try_acquire(name); }))) wait_period(kResourcePoll, "acquire");
}

void HostState::wait_resource(const std::string& name, double value) {
  if (estimating()) return;
  guard();
  auto& res = resources();
  while (unwrap(nogil([&] { return res.get_value(name); })) != value)
    wait_period(kResourcePoll, "wait");
}

void HostState::release(const std::string& name) {
  if (estimating()) return;
  guard();
  unwrap(resources().release(name));
}

void HostState::set_resource(const std::string& name, double value) {
  if (estimating()) return;
  guard();
  unwrap(resources().set_value(name, value));
}

double HostState::get_resource_value(const std::string& name) {
  if (estimating()) return 0.0;
  guard();
  return unwrap(resources().get_value(name));
}

void HostState::gosub(const std::string& name, py::dict kwargs) {
  if (gosub_depth_ >= env_.limits.max_gosub_depth)
    raise(Error{ErrorKind::Config, "gosub depth limit reached at '" + name + "'", {}});
  if (!env_.resolver) raise(Error{ErrorKind::Config, "gosub '" + name + "': no resolver", {}});
  auto sub = unwrap(env_.resolver->resolve(name, kind_));
  ValueMap overrides;
  for (auto item : kwargs) {
    auto key = py::str(item.first).cast<std::string>();
    auto v = item.second;
    if (v.is_none()) overrides[key] = std::monostate{};
    else if (py::isinstance<py::bool_>(v)) overrides[key] = v.cast<bool>();
    else if (py::isinstance<py::int_>(v)) overrides[key] = v.cast<std::int64_t>();
    else if (py::isinstance<py::float_>(v)) overrides[key] = v.cast<double>();
    else overrides[key] = py::str(v).cast<std::string>();
  }
  ++gosub_depth_;
  try {
    run_main(sub, overrides);
  } catch (...) {
    --gosub_depth_;
    throw;
  }
  --gosub_depth_;
}

double HostState::get_intensity(const std::string& key) {
  if (estimating()) return 0.0;
  guard();
  if (!env_.intensity) raise(not_supported("get_intensity"));
  return unwrap(nogil([&] { return env_.intensity->intensity(key); }));
}

void HostState::signal_pump_time_start() {
  if (estimating()) return;
  if (env_.on_pump_time_start) env_.on_pump_time_start();
}

}  // namespace pychron::scripting::python
