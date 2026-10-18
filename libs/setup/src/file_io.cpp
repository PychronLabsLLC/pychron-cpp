#include "file_io.hpp"

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace pychron::setup::detail {

Result<void> write_file(const fs::path& p, const std::string& content, bool secret) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + p.parent_path().string() + ": " + ec.message());
  if (secret) {
    // Created empty and restricted before the secret goes in.
    { std::ofstream create(p, std::ios::binary | std::ios::trunc); }
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
  }
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
  out.close();
  if (!out) return fail(ErrorKind::Io, "cannot write " + p.string());
  return {};
}

}  // namespace pychron::setup::detail
