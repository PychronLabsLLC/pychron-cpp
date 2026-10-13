#include "pychron/core/host_name.hpp"

#ifdef _WIN32
#include "pychron/core/env.hpp"
#else
#include <array>

#include <unistd.h>
#endif

namespace pychron {

std::string host_name() {
#ifdef _WIN32
  // Winsock's gethostname needs the socket library started; the system
  // gives every process this variable.
  std::string name = env_var("COMPUTERNAME").value_or("");
#else
  std::array<char, 256> buffer{};
  // One short of the buffer: a name that fills it need not be terminated.
  std::string name;
  if (::gethostname(buffer.data(), buffer.size() - 1) == 0) name = buffer.data();
#endif
  return name.empty() ? "localhost" : name;
}

}  // namespace pychron
