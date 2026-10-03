#include "pychron/core/process.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>

#include <cstdint>
#include <vector>
#endif

namespace pychron {

namespace {

constexpr std::size_t kMaxOutput = 64 * 1024;

void keep(std::string& out, const char* data, std::size_t n) {
  if (out.size() < kMaxOutput) out.append(data, std::min(n, kMaxOutput - out.size()));
}

}  // namespace

#ifndef _WIN32

namespace {

std::string errno_text(int e) { return std::strerror(e); }

// The input as an unlinked temporary file: the child reads it as stdin, so
// no pipe can raise SIGPIPE in this process if the child exits early.
Result<int> input_file(const std::string& input) {
  std::string path = (std::filesystem::temp_directory_path() / "pychron-in-XXXXXX").string();
  const int fd = mkstemp(path.data());
  if (fd < 0) return fail(ErrorKind::Io, "cannot create a temporary file: " + errno_text(errno));
  unlink(path.c_str());
  std::size_t done = 0;
  while (done < input.size()) {
    const ssize_t n = write(fd, input.data() + done, input.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      const int e = errno;
      close(fd);
      return fail(ErrorKind::Io, "cannot write a temporary file: " + errno_text(e));
    }
    done += static_cast<std::size_t>(n);
  }
  lseek(fd, 0, SEEK_SET);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

}  // namespace

Result<ProcessResult> run_process(const ProcessSpec& spec) {
  if (spec.argv.empty() || spec.argv.front().empty()) return fail(ErrorKind::Config, "no program to run");
  auto in = input_file(spec.input);
  if (!in) return fail(in.error());
  int out[2];
  if (pipe(out) != 0) {
    const int e = errno;
    close(*in);
    return fail(ErrorKind::Io, "cannot create a pipe: " + errno_text(e));
  }
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  fcntl(out[1], F_SETFD, FD_CLOEXEC);

  std::vector<std::string> env_strings;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    const std::string entry(*e);
    const auto name = entry.substr(0, entry.find('='));
    const bool replaced = std::any_of(spec.env.begin(), spec.env.end(), [&](const auto& kv) { return kv.first == name; });
    if (!replaced) env_strings.push_back(entry);
  }
  for (const auto& [k, v] : spec.env) env_strings.push_back(k + "=" + v);
  std::vector<char*> envp;
  for (auto& s : env_strings) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = spec.argv;
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, *in, 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, out[1], 2);
  // Its own process group, so a timeout kills whatever it started too.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  pid_t pid = 0;
  const int spawned = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  close(*in);
  close(out[1]);
  if (spawned != 0) {
    close(out[0]);
    return fail(ErrorKind::Io, "cannot run " + spec.argv.front() + ": " + errno_text(spawned));
  }

  ProcessResult result;
  bool timed_out = false;
  const auto deadline = std::chrono::steady_clock::now() + spec.timeout;
  char buf[4096];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      timed_out = true;
      break;
    }
    pollfd p{out[0], POLLIN, 0};
    const int r = poll(&p, 1, static_cast<int>(std::min<long long>(left.count(), 1000)));
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) break;
    if (r == 0) continue;
    const ssize_t n = read(out[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // EOF: the child (and anything it started) closed the pipe
    keep(result.output, buf, static_cast<std::size_t>(n));
  }
  close(out[0]);
  if (timed_out) kill(-pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (timed_out)
    return fail(ErrorKind::Timeout, spec.argv.front() + " ran longer than " +
                                        std::to_string(spec.timeout.count()) + " ms and was killed");
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
  return result;
}

#else  // _WIN32

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string last_error_text() {
  const DWORD code = GetLastError();
  char* msg = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 code, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
  std::string text = msg != nullptr ? msg : ("error " + std::to_string(code));
  if (msg != nullptr) LocalFree(msg);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

// One argument quoted for CommandLineToArgvW / the C runtime.
void append_quoted(std::wstring& cmd, const std::wstring& arg) {
  if (!cmd.empty()) cmd += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd += L'"';
  for (auto it = arg.begin();; ++it) {
    std::size_t slashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++slashes;
    }
    if (it == arg.end()) {
      cmd.append(slashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      cmd.append(slashes * 2 + 1, L'\\');
      cmd += L'"';
    } else {
      cmd.append(slashes, L'\\');
      cmd += *it;
    }
  }
  cmd += L'"';
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}

// The inherited environment plus `extra`, as a CreateProcessW block.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& extra) {
  std::vector<std::wstring> entries;
  if (LPWCH env = GetEnvironmentStringsW()) {
    for (const wchar_t* p = env; *p != L'\0'; p += wcslen(p) + 1) entries.emplace_back(p);
    FreeEnvironmentStringsW(env);
  }
  for (const auto& [k, v] : extra) {
    const std::wstring name = lower(widen(k));
    std::erase_if(entries, [&](const std::wstring& e) {
      const auto eq = e.find(L'=', 1);  // names such as "=C:" start with '='
      return lower(e.substr(0, eq)) == name;
    });
    entries.push_back(widen(k) + L"=" + widen(v));
  }
  std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) { return lower(a) < lower(b); });
  std::wstring block;
  for (const auto& e : entries) {
    block += e;
    block += L'\0';
  }
  block += L'\0';
  return block;
}

}  // namespace

Result<ProcessResult> run_process(const ProcessSpec& spec) {
  if (spec.argv.empty() || spec.argv.front().empty()) return fail(ErrorKind::Config, "no program to run");
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

  // The input as a temporary file, deleted when the last handle closes.
  wchar_t dir[MAX_PATH + 1];
  wchar_t path[MAX_PATH + 1];
  if (GetTempPathW(MAX_PATH, dir) == 0 || GetTempFileNameW(dir, L"pyc", 0, path) == 0)
    return fail(ErrorKind::Io, "cannot create a temporary file: " + last_error_text());
  {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    f.write(spec.input.data(), static_cast<std::streamsize>(spec.input.size()));
    if (!f) {
      DeleteFileW(path);
      return fail(ErrorKind::Io, "cannot write a temporary file");
    }
  }
  HANDLE in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, OPEN_EXISTING,
                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (in == INVALID_HANDLE_VALUE) {
    const std::string why = last_error_text();
    DeleteFileW(path);
    return fail(ErrorKind::Io, "cannot open a temporary file: " + why);
  }

  HANDLE out_r = nullptr, out_w = nullptr;
  if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
    const std::string why = last_error_text();
    CloseHandle(in);
    return fail(ErrorKind::Io, "cannot create a pipe: " + why);
  }
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

  std::wstring cmd;
  for (const auto& a : spec.argv) append_quoted(cmd, widen(a));
  std::wstring env = environment_block(spec.env);

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out_w;
  si.hStdError = out_w;
  // A job holds the child and whatever it starts: closing the job kills them
  // all, so nothing keeps the output pipe open after the child is done.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
  }
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data(), nullptr,
                                 &si, &pi);
  const std::string why = ok ? std::string() : last_error_text();
  CloseHandle(in);
  CloseHandle(out_w);
  if (!ok) {
    CloseHandle(out_r);
    if (job != nullptr) CloseHandle(job);
    return fail(ErrorKind::Io, "cannot run " + spec.argv.front() + ": " + why);
  }
  if (job != nullptr) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  ProcessResult result;
  std::thread reader([&] {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(out_r, buf, sizeof buf, &n, nullptr) && n > 0) keep(result.output, buf, n);
  });
  const bool timed_out = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(spec.timeout.count())) == WAIT_TIMEOUT;
  if (timed_out) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  if (job != nullptr) CloseHandle(job);  // kills anything the child left running
  reader.join();
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(out_r);
  if (timed_out)
    return fail(ErrorKind::Timeout, spec.argv.front() + " ran longer than " +
                                        std::to_string(spec.timeout.count()) + " ms and was killed");
  result.exit_code = static_cast<int>(code);
  return result;
}

#endif

std::filesystem::path executable_dir() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
  std::error_code ec;
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size + 1, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  auto p = std::filesystem::weakly_canonical(std::filesystem::path(buf.data()), ec);
  return ec ? std::filesystem::path(buf.data()).parent_path() : p.parent_path();
#else
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : p.parent_path();
#endif
}

}  // namespace pychron
