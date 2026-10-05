// Python halves of the host. The runtime module is shared by every
// execution; the prelude runs once per execution against that execution's
// `_h` and supplies the vocabulary with pychron's signatures (which the
// static check binds calls against).

#include "internal.hpp"

namespace pychron::scripting::python {

const char* const kRuntimeSource = R"PY(
import ast
import builtins as _builtins
import inspect
import traceback


class ScriptCancelled(BaseException):
    """A Cancel request reached a blocking command; finally: blocks still run."""


class ScriptAborted(BaseException):
    """An Abort request; the host refuses further hardware calls."""


class ScriptLimitExceeded(BaseException):
    """A watchdog limit (executed lines, wall time) was hit."""


class EstimateBudget(BaseException):
    """estimate() ran out of its line budget (a loop it cannot bound)."""


class HardwareError(Exception):
    def __init__(self, kind, message, device=''):
        super().__init__(message)
        self.kind = kind
        self.device = device


class NotSupportedError(HardwareError):
    def __init__(self, message, device=''):
        super().__init__('config', message, device)


class Options:
    """script_options as opt.<key>; read-only."""
    __slots__ = ('_values',)

    def __init__(self, values):
        object.__setattr__(self, '_values', dict(values))

    def __getattr__(self, key):
        try:
            return object.__getattribute__(self, '_values')[key]
        except KeyError:
            raise AttributeError(f"no script option '{key}'") from None

    def __setattr__(self, key, value):
        raise AttributeError('opt is read-only')

    def __contains__(self, key):
        return key in self._values

    def get(self, key, default=None):
        return self._values.get(key, default)


SAFE_BUILTINS = (
    'abs', 'all', 'any', 'bool', 'callable', 'chr', 'dict', 'divmod', 'enumerate',
    'filter', 'float', 'format', 'frozenset', 'hasattr', 'hash', 'int', 'isinstance',
    'issubclass', 'iter', 'len', 'list', 'map', 'max', 'min', 'next', 'ord', 'pow',
    'range', 'repr', 'reversed', 'round', 'set', 'slice', 'sorted', 'str', 'sum',
    'tuple', 'zip', 'True', 'False', 'None', '__build_class__', 'object', 'staticmethod',
    'classmethod', 'property', 'super',
    'Exception', 'BaseException', 'ArithmeticError', 'AssertionError', 'AttributeError',
    'ImportError', 'IndexError', 'KeyError', 'LookupError', 'NameError',
    'NotImplementedError', 'RuntimeError', 'StopIteration', 'TypeError', 'ValueError',
    'ZeroDivisionError',
)


def make_builtins(allowed_imports, printer):
    allowed = frozenset(allowed_imports)

    def guarded_import(name, globals=None, locals=None, fromlist=(), level=0):
        if level != 0 or name.split('.')[0] not in allowed:
            raise ImportError(f"import of '{name}' is not allowed in scripts")
        return _builtins.__import__(name, globals, locals, fromlist, level)

    def script_print(*args, sep=' ', **kw):
        printer(sep.join(str(a) for a in args))

    table = {k: getattr(_builtins, k) for k in SAFE_BUILTINS}
    table['__import__'] = guarded_import
    table['print'] = script_print
    return table


def describe(exc, host_files):
    """'<script>:<line>: <Type>: <message>' at the innermost script frame."""
    where = ''
    for frame in traceback.extract_tb(exc.__traceback__):
        if frame.filename not in host_files:
            where = f'{frame.filename}:{frame.lineno}: '
    if isinstance(exc, SyntaxError):
        where = f'{exc.filename}:{exc.lineno}: '
    return f'{where}{type(exc).__name__}: {exc}'


class _Placeholder:
    pass


def check_source(source, filename, vocab, signatures, known, valves, allowed_imports,
                 readonly, needs_main):
    """Static check. vocab: name -> (available, allowed, valve_argument, capability).
    Returns (diagnostics[(severity, line, code, message)], gosubs[(line, name)])."""
    diags = []
    gosubs = []

    def report(severity, node, code, message):
        diags.append((severity, getattr(node, 'lineno', 0) or 0, code, message))

    try:
        tree = ast.parse(source, filename)
    except SyntaxError as e:
        return [('error', e.lineno or 0, 'syntax', e.msg)], []

    defined = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            defined.add(node.name)
        elif isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del)):
            defined.add(node.id)
        elif isinstance(node, ast.arg):
            defined.add(node.arg)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                defined.add((alias.asname or alias.name).split('.')[0])
        elif isinstance(node, ast.ExceptHandler) and node.name:
            defined.add(node.name)
        elif isinstance(node, (ast.Global, ast.Nonlocal)):
            defined.update(node.names)

    allowed = set(allowed_imports)
    called = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name.split('.')[0] not in allowed:
                    report('error', node, 'import', f"import of '{alias.name}' is not allowed")
        elif isinstance(node, ast.ImportFrom):
            if node.level or (node.module or '').split('.')[0] not in allowed:
                report('error', node, 'import', f"import from '{node.module}' is not allowed")
        elif isinstance(node, ast.Attribute):
            if node.attr.startswith('__') and node.attr.endswith('__'):
                report('error', node, 'restricted', f"access to '{node.attr}' is not allowed")
        elif isinstance(node, ast.While):
            report('warning', node, 'unbounded-loop', 'while loop: duration cannot be bounded')
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            name = node.func.id
            called.add(id(node.func))
            if name in vocab and name not in defined:
                available, ok_here, valve_arg, capability = vocab[name]
                if not ok_here:
                    report('error', node, 'not-allowed',
                           f"'{name}' is not available in this kind of script")
                if not available:
                    report('error', node, 'not-supported',
                           f"not supported: {capability} ({name})")
                starred = any(isinstance(a, ast.Starred) for a in node.args) or \
                    any(k.arg is None for k in node.keywords)
                if not starred and name in signatures:
                    try:
                        inspect.signature(signatures[name]).bind(
                            *[_Placeholder()] * len(node.args),
                            **{k.arg: _Placeholder() for k in node.keywords})
                    except TypeError as e:
                        report('error', node, 'arity', f'{name}(): {e}')
                first = node.args[0] if node.args else next(
                    (k.value for k in node.keywords if k.arg == 'name'), None)
                literal = first.value if isinstance(first, ast.Constant) and \
                    isinstance(first.value, str) else None
                if valve_arg and valves is not None and literal is not None and \
                        literal not in valves:
                    report('error', node, 'unknown-valve', f"unknown valve '{literal}'")
                if name == 'gosub':
                    if literal is not None:
                        gosubs.append((node.lineno, literal))
                    else:
                        report('warning', node, 'dynamic-gosub',
                               'gosub target is not a literal; not checked')
            elif name not in defined and name not in known:
                report('error', node, 'unknown-command', f"unknown command '{name}'")

    for node in ast.walk(tree):
        if isinstance(node, ast.Name):
            if isinstance(node.ctx, (ast.Store, ast.Del)) and node.id in readonly:
                report('error', node, 'read-only', f"'{node.id}' is a read-only context name")
            elif isinstance(node.ctx, ast.Load) and id(node) not in called and \
                    node.id not in defined and node.id not in known and node.id not in vocab:
                report('error', node, 'unknown-name', f"unknown name '{node.id}'")

    if needs_main and not any(isinstance(n, ast.FunctionDef) and n.name == 'main'
                              for n in tree.body):
        diags.append(('error', 0, 'no-main', 'script defines no main()'))
    return diags, gosubs
)PY";

const char* const kPreludeSource = R"PY(
def _ctx(name, value):
    return _context.get(name) if value is None or value == '' else value


def _valve(name, description):
    v = name if name is not None else description
    if v is None:
        raise TypeError('a valve name is required')
    return str(v)


def open(name=None, description=None):
    _h.open(_valve(name, description))


def close(name=None, description=None):
    _h.close(_valve(name, description))


def lock(name=None, description=None):
    _h.lock(_valve(name, description))


def unlock(name=None, description=None):
    _h.unlock(_valve(name, description))


def is_open(name=None, description=None):
    return _h.is_open(_valve(name, description))


def is_closed(name=None, description=None):
    return _h.is_closed(_valve(name, description))


def extract(value=None, units=None, block=None):
    _h.extract(float(_ctx('extract_value', value) or 0), str(_ctx('extract_units', units)))


def end_extract():
    _h.end_extract()


def ramp(start=0, end=0, rate=0, duration=0, period=1):
    if not duration and rate:
        duration = abs(end - start) / float(rate)
    steps = max(1, int(round(duration / period))) if duration and period > 0 else 1
    units = str(_context.get('extract_units', 'percent'))
    for i in range(1, steps + 1):
        _h.extract(float(start + (end - start) * i / steps), units)
        if duration:
            _h.sleep(duration / steps, 'ramp')


def enable():
    _h.enable()


def disable():
    _h.disable()


def prepare():
    _h.prepare()


def get_device(name=None):
    return _h.get_device('' if name is None else str(name))


def fire_laser():
    _h.fire_laser()


def warmup(block=False):
    _h.warmup()


def move_to_position(position=None, autocenter=True, block=True):
    _h.move_to_position(str(_ctx('position', position)), bool(autocenter), bool(block))


def set_x(value, velocity=None, block=True):
    _h.set_axis('x', float(value), bool(block))


def set_y(value, velocity=None, block=True):
    _h.set_axis('y', float(value), bool(block))


def set_z(value, velocity=None, block=True):
    _h.set_axis('z', float(value), bool(block))


def set_xy(x, y, velocity=None, block=True):
    _h.set_xy(float(x), float(y), bool(block))


def set_tray(tray=None):
    _h.set_tray(str(_ctx('tray', tray)))


def execute_pattern(pattern=None, block=True, duration=None):
    _h.execute_pattern(str(_ctx('pattern', pattern)), bool(block), float(_ctx('duration', duration) or 0))


def dump_sample():
    _h.dump_sample()


def drop_sample(position=None):
    _h.drop_sample(str(_ctx('position', position)))


def set_pid_parameters(value):
    _h.set_pid_parameters(float(value))


def begin_heating_interval(duration, min_rise_rate=None, check_time=60, check_delay=60,
                           check_period=1, temperature=None, timeout=300, name='interval'):
    _h.require('furnace')
    begin_interval(duration, name)


def load_pipette(identifier, timeout=None):
    _h.load_pipette(str(identifier))


def extract_pipette(identifier, timeout=None):
    _h.extract_pipette(str(identifier))


def set_motor(name, value, block=False):
    _h.set_motor(str(name), float(value), bool(block))


def get_value(name):
    return _h.get_value(str(name))


def set_cryo(value, block=False):
    _h.set_cryo(float(value))


def get_cryo_temp(channel=1):
    return _h.get_cryo_temp(int(channel))


def snapshot(name='', note='', pic_format='.jpg'):
    return _h.snapshot(str(name))


def video_start(name=''):
    _h.video_start(str(name))


def video_stop():
    _h.video_stop()


class video_recording:
    def __init__(self, name=''):
        self._name = str(name)

    def __enter__(self):
        video_start(self._name)
        return self

    def __exit__(self, *exc):
        video_stop()
        return False


def get_pressure(controller, gauge):
    return _h.get_pressure(str(controller), str(gauge))


def get_manometer_pressure(name='manometer'):
    return _h.get_manometer_pressure(str(name))


def waitfor(func, timeout=0, check_period=1, start_message='', end_message=''):
    if _h.estimating:
        _h.estimate_wait('waitfor', float(timeout))
        return
    start = _h.now()
    while not func():
        if timeout and _h.now() - start >= timeout:
            raise HardwareError('timeout', f'waitfor timed out after {timeout} s')
        _h.sleep(float(check_period), 'waitfor')


def wake():
    _h.wake()


def pause(duration=0, message=None):
    _h.pause(float(duration))


def sleep(duration=0, message=None):
    _h.sleep(float(duration), 'sleep')


def delay(duration=0.5, message=None):
    _h.sleep(float(duration), 'delay')


_intervals = {}


def begin_interval(duration=0, name='interval'):
    _intervals[name] = (_h.now(), float(duration))


def complete_interval(name='interval'):
    try:
        start, duration = _intervals.pop(name)
    except KeyError:
        raise RuntimeError(f"complete_interval: interval '{name}' was not begun") from None
    remaining = duration - (_h.now() - start)
    if remaining > 0:
        _h.sleep(remaining, 'complete_interval')


def acquire(name, clear=False):
    _h.acquire(str(name))


def wait(name, criterion=0):
    _h.wait_resource(str(name), float(criterion))


def release(name):
    _h.release(str(name))


def set_resource(name, value):
    _h.set_resource(str(name), float(value))


def get_resource_value(name):
    return _h.get_resource_value(str(name))


def info(message, *args, **kw):
    _h.info(str(message))


def gosub(name=None, root=None, klass=None, argv=None, **kw):
    _h.gosub(str(name), kw)


def get_intensity(key):
    return _h.get_intensity(str(key))


def signal_pump_time_start():
    _h.signal_pump_time_start()
)PY";

}  // namespace pychron::scripting::python
