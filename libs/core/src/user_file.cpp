#include "pychron/core/user_file.hpp"

#include <system_error>

#if defined(__APPLE__)
#include <cerrno>
#include <sys/xattr.h>
#endif

namespace pychron {

bool mark_as_user_file(const std::filesystem::path& path) noexcept {
  std::error_code code;
  if (!std::filesystem::exists(path, code) || code) return false;
#if defined(__APPLE__)
  if (removexattr(path.c_str(), "com.apple.quarantine", 0) == 0) return true;
  return errno == ENOATTR;  // never quarantined: nothing to clear
#else
  return true;
#endif
}

}  // namespace pychron
