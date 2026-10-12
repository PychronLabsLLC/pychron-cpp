// A password asked for at a terminal, without echo.

#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "earthbank_impl.hpp"

namespace elctl::earthbank_cli {

// cppcheck-suppress passedByValue ; Io is three references, and every command takes it by value
bool at_terminal(Io io) {
  // Only the program's own input, never a stream a caller gave: that one is
  // not a person.
  if (&io.in != &std::cin) return false;
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(STDIN_FILENO) != 0;
#endif
}

std::optional<std::string> read_password(Io io, std::string_view prompt) {
  io.err << prompt << std::flush;
  std::string password;
#ifdef _WIN32
  HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  const bool changed = GetConsoleMode(console, &mode) && SetConsoleMode(console, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
  const bool read = static_cast<bool>(std::getline(io.in, password));
  if (changed) SetConsoleMode(console, mode);
#else
  termios saved{};
  const bool changed = tcgetattr(STDIN_FILENO, &saved) == 0;
  if (changed) {
    termios quiet = saved;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
  }
  const bool read = static_cast<bool>(std::getline(io.in, password));
  if (changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
#endif
  io.err << '\n';
  if (!read) return std::nullopt;
  if (!password.empty() && password.back() == '\r') password.pop_back();
  return password;
}

}  // namespace elctl::earthbank_cli
