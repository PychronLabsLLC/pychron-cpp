// Interpreter start-up and the pybind11 bindings of HostState (`_h`), the
// hook `api` and get_device() handles.

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>

#include "internal.hpp"
#include "pychron/core/env.hpp"
#include "pychron/core/process.hpp"

namespace pychron::scripting::python {
namespace {

Runtime* g_runtime = nullptr;

void bind_classes(py::module_ scope) {
  py::class_<HostState>(scope, "_Host")
      .def_property_readonly("estimating", &HostState::estimating)
      .def("now", &HostState::now)
      .def("info", &HostState::info)
      .def("require", &HostState::require)
      .def("open", &HostState::open)
      .def("close", &HostState::close)
      .def("lock", &HostState::lock)
      .def("unlock", &HostState::unlock)
      .def("is_open", &HostState::is_open)
      .def("is_closed", &HostState::is_closed)
      .def("extract", &HostState::extract)
      .def("end_extract", &HostState::end_extract)
      .def("enable", &HostState::enable)
      .def("disable", &HostState::disable)
      .def("prepare", &HostState::prepare)
      .def("fire_laser", &HostState::fire_laser)
      .def("warmup", &HostState::warmup)
      .def("get_device", &HostState::get_device)
      .def("move_to_position", &HostState::move_to_position)
      .def("set_axis", &HostState::set_axis)
      .def("set_xy", &HostState::set_xy)
      .def("set_tray", &HostState::set_tray)
      .def("execute_pattern", &HostState::execute_pattern)
      .def("dump_sample", &HostState::dump_sample)
      .def("drop_sample", &HostState::drop_sample)
      .def("set_pid_parameters", &HostState::set_pid_parameters)
      .def("load_pipette", &HostState::load_pipette)
      .def("extract_pipette", &HostState::extract_pipette)
      .def("set_motor", &HostState::set_motor)
      .def("get_value", &HostState::get_value)
      .def("set_cryo", &HostState::set_cryo)
      .def("get_cryo_temp", &HostState::get_cryo_temp)
      .def("snapshot", &HostState::snapshot)
      .def("video_start", &HostState::video_start)
      .def("video_stop", &HostState::video_stop)
      .def("get_pressure", &HostState::get_pressure)
      .def("get_manometer_pressure", &HostState::get_manometer_pressure)
      .def("sleep", &HostState::sleep)
      .def("pause", &HostState::pause)
      .def("wake", &HostState::wake)
      .def("estimate_wait", &HostState::estimate_wait)
      .def("acquire", &HostState::acquire)
      .def("wait_resource", &HostState::wait_resource)
      .def("release", &HostState::release)
      .def("set_resource", &HostState::set_resource)
      .def("get_resource_value", &HostState::get_resource_value)
      .def("gosub", &HostState::gosub)
      .def("get_intensity", &HostState::get_intensity)
      .def("signal_pump_time_start", &HostState::signal_pump_time_start);

  // Hook API: the hardware calls release the GIL like the vocabulary does.
  auto nogil = [](auto&& f) {
    py::gil_scoped_release release;
    return f();
  };
  py::class_<HookApi>(scope, "MeasurementAPI")
      .def("position",
           [nogil](HookApi& a, const std::string& iso, const std::string& det) {
             a.state->guard();
             auto& api = a.state->measurement();
             a.state->unwrap(nogil([&] { return api.position(iso, det); }));
           })
      .def(
          "acquire",
          [nogil](HookApi& a, int counts, double integration_time) {
            a.state->guard();
            auto& api = a.state->measurement();
            a.state->unwrap(nogil([&] { return api.acquire(counts, integration_time); }));
          },
          py::arg("counts"), py::arg("integration_time") = 1.0)
      .def("open",
           [nogil](HookApi& a, const std::string& v) {
             a.state->guard();
             auto& api = a.state->measurement();
             a.state->unwrap(nogil([&] { return api.open(v); }));
           })
      .def("close",
           [nogil](HookApi& a, const std::string& v) {
             a.state->guard();
             auto& api = a.state->measurement();
             a.state->unwrap(nogil([&] { return api.close(v); }));
           })
      .def("add_conditional",
           [](HookApi& a, const std::string& spec) {
             a.state->unwrap(a.state->measurement().add_conditional(spec));
           })
      .def(
          "truncate",
          [](HookApi& a, bool quick) { a.state->unwrap(a.state->measurement().truncate(quick)); },
          py::arg("quick") = false)
      .def("log", [](HookApi& a, const std::string& m) {
        a.state->measurement().log(m);
        a.state->info(m);
      });

  py::class_<DeviceHandle>(scope, "DeviceHandle")
      .def_property_readonly("name",
                             [](DeviceHandle& d) {
                               if (d.state->estimating()) return std::string{};
                               return d.state->device().device_name();
                             })
      .def("output",
           [](DeviceHandle& d) {
             if (d.state->estimating()) return 0.0;
             return d.state->unwrap(d.state->device().output());
           })
      .def("is_enabled", [](DeviceHandle& d) {
        if (d.state->estimating()) return false;
        return d.state->unwrap(d.state->device().is_enabled());
      });
}

void create_runtime() {
  auto* rt = new Runtime();  // lives for the process, like the interpreter
  auto types = py::module_::import("types");
  rt->module = types.attr("ModuleType")("_pychron_runtime");
  PyObject* runtime_code = Py_CompileString(kRuntimeSource, kRuntimeFilename, Py_file_input);
  if (!runtime_code) throw py::error_already_set();
  auto runtime_code_obj = py::reinterpret_steal<py::object>(runtime_code);
  auto runtime_dict = rt->module.attr("__dict__");
  PyObject* ran = PyEval_EvalCode(runtime_code, runtime_dict.ptr(), runtime_dict.ptr());
  if (!ran) throw py::error_already_set();
  Py_DECREF(ran);
  py::module_::import("sys").attr("modules")["_pychron_runtime"] = rt->module;
  bind_classes(py::reinterpret_borrow<py::module_>(rt->module));

  PyObject* code = Py_CompileString(kPreludeSource, kPreludeFilename, Py_file_input);
  if (!code) throw py::error_already_set();
  rt->prelude_code = py::reinterpret_steal<py::object>(code);

  rt->reference["__builtins__"] = py::module_::import("builtins");
  rt->reference["_h"] = py::none();
  rt->reference["_context"] = py::dict();
  rt->reference["HardwareError"] = rt->module.attr("HardwareError");
  PyObject* done = PyEval_EvalCode(code, rt->reference.ptr(), rt->reference.ptr());
  if (!done) throw py::error_already_set();
  Py_DECREF(done);

  rt->cancelled = rt->module.attr("ScriptCancelled");
  rt->aborted = rt->module.attr("ScriptAborted");
  rt->limit = rt->module.attr("ScriptLimitExceeded");
  rt->budget = rt->module.attr("EstimateBudget");
  rt->hardware = rt->module.attr("HardwareError");
  rt->not_supported = rt->module.attr("NotSupportedError");
  g_runtime = rt;
}

}  // namespace

namespace {

// An installed pychron carries its own CPython (python-build-standalone)
// beside the programs: <prefix>/share/pychron/python, or a macOS bundle's
// Contents/Resources/python. Point the interpreter at it unless PYTHONHOME is
// already set; a build-tree run uses the Python it was built against.
void use_bundled_python() {
  if (auto home = env_var("PYTHONHOME"); home && !home->empty()) return;
  const std::filesystem::path exe = executable_dir();
  if (exe.empty()) return;
  for (const auto& dir : {exe / ".." / "share" / "pychron" / "python", exe / ".." / "Resources" / "python"}) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir / "lib", ec) && !std::filesystem::is_directory(dir / "Lib", ec)) continue;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
    const std::string value = (ec ? dir : canonical).string();
#ifdef _WIN32
    _putenv_s("PYTHONHOME", value.c_str());
#else
    setenv("PYTHONHOME", value.c_str(), 1);
#endif
    return;
  }
}

}  // namespace

void ensure_interpreter() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!Py_IsInitialized()) {
      use_bundled_python();
      py::initialize_interpreter(/*init_signal_handlers=*/false);
      create_runtime();
      // Scripts take the GIL only while they run.
      PyEval_SaveThread();
    } else {
      py::gil_scoped_acquire gil;
      create_runtime();
    }
  });
}

Runtime& runtime() { return *g_runtime; }

}  // namespace pychron::scripting::python
