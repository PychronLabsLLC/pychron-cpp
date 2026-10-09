#include "pychron/earthbank/journal.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "pychron/core/error.hpp"
#include "pychron/core/path_text.hpp"
#include "pychron/core/sha256.hpp"

namespace pychron::earthbank {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kVersion = 1;
constexpr std::size_t kLongest = 150;  // of an encoded name, under any file system's limit
constexpr std::size_t kKept = 100;     // of a longer one, before its hash

bool plain(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

std::string encoded(std::string_view text) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    if (plain(c) && !(out.empty() && c == '.')) {
      out.push_back(c);
    } else {
      const auto byte = static_cast<unsigned char>(c);
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0xF]);
    }
  }
  return out;
}

std::string hex_of(std::string_view text, std::size_t digits) {
  const Sha256Digest digest = sha256(text);
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; out.size() < digits && i < digest.size(); ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0xF]);
  }
  out.resize(digits);
  return out;
}

// The name of a sample (or a host) as a file name.
std::string file_name_of(std::string_view text) {
  std::string out = encoded(text);
  if (out.empty()) return "%";  // no other name encodes to a lone `%`
  if (out.size() <= kLongest) return out;
  std::size_t cut = kKept;
  while (cut > 0 && (out[cut - 1] == '%' || out[cut - 2] == '%')) --cut;  // not inside a `%XX`
  return out.substr(0, cut) + "~" + hex_of(text, 16);
}

long process_id() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

// A new file `file` holding `text`, on the disk when this returns; the
// owner's alone. What went wrong otherwise.
std::optional<std::string> write_new(const fs::path& file, std::string_view text) {
#ifdef _WIN32
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return "cannot be created";
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out) return "cannot be written";
  return std::nullopt;
#else
  const auto words = [](int number) { return std::error_code(number, std::generic_category()).message(); };
  const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return words(errno);
  std::optional<std::string> wrong;
  while (!text.empty() && !wrong) {
    const ssize_t wrote = ::write(fd, text.data(), text.size());
    if (wrote < 0) {
      if (errno == EINTR) continue;
      wrong = words(errno);
    } else {
      text.remove_prefix(static_cast<std::size_t>(wrote));
    }
  }
  if (!wrong && ::fsync(fd) != 0) wrong = words(errno);
  if (::close(fd) != 0 && !wrong) wrong = words(errno);
  return wrong;
#endif
}

// The rename is on the disk too, as far as the system lets us ask.
void flush_directory(const fs::path& dir) {
#ifndef _WIN32
  const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  (void)::fsync(fd);
  ::close(fd);
#else
  (void)dir;
#endif
}

Error bad(const std::string& where, const std::string& what) {
  return Error{ErrorKind::Config, "earthbank journal " + where + ": " + what +
                                      ". It is the record of what was sent, so it is not read as empty: look at what is "
                                      "on EarthBank, then move the file away to start the sample afresh.",
               ""};
}

// A positive id.
std::optional<std::int64_t> id_in(const json& value) {
  if (!value.is_number_integer()) return std::nullopt;
  const auto id = value.get<std::int64_t>();
  if (id < 1) return std::nullopt;
  return id;
}

json to_json(const Journal& journal) {
  json out = {{"version", kVersion}, {"hash", journal.hash}};
  if (journal.sample_id) out["sampleId"] = *journal.sample_id;
  if (journal.data_point_id) out["arArDataPointId"] = *journal.data_point_id;
  out["aliquots"] = journal.aliquots;
  out["measurements"] = journal.measurements;
  if (journal.age_calc_id) out["ageCalcId"] = *journal.age_calc_id;
  if (journal.age_summary_id) out["ageSummaryId"] = *journal.age_summary_id;
  if (journal.in_doubt) out["inDoubt"] = *journal.in_doubt;
  return out;
}

}  // namespace

fs::path journal_file(const fs::path& dir, std::string_view host, std::int64_t package, std::string_view sample) {
  return dir / file_name_of(host) / std::to_string(package) / (file_name_of(sample) + ".json");
}

Result<std::optional<Journal>> load_journal(const fs::path& file) {
  const std::string where = utf8(file);
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(file, ec);
  if (status.type() == fs::file_type::not_found) return std::optional<Journal>();
  if (ec || !fs::is_regular_file(file, ec)) return fail(ErrorKind::Io, "earthbank journal " + where + ": it is not a file");
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "earthbank journal " + where + ": it cannot be opened");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return fail(ErrorKind::Io, "earthbank journal " + where + ": it cannot be read");

  const json doc = json::parse(text, nullptr, false);
  if (!doc.is_object()) return fail(bad(where, "it is not a JSON object (not a journal, or damaged)"));
  Journal out;
  bool versioned = false;
  for (const auto& [key, value] : doc.items()) {
    if (key == "version") {
      if (!value.is_number_integer() || value.get<int>() != kVersion) {
        return fail(bad(where, "version " + value.dump() + " is not one this program reads"));
      }
      versioned = true;
    } else if (key == "hash") {
      if (!value.is_string() || value.get_ref<const std::string&>().empty()) return fail(bad(where, "'hash' is not text"));
      out.hash = value.get<std::string>();
    } else if (key == "sampleId" || key == "arArDataPointId" || key == "ageCalcId" || key == "ageSummaryId") {
      const auto id = id_in(value);
      if (!id) return fail(bad(where, "'" + key + "' is not an id"));
      std::optional<std::int64_t>& slot = key == "sampleId"          ? out.sample_id
                                          : key == "arArDataPointId" ? out.data_point_id
                                          : key == "ageCalcId"       ? out.age_calc_id
                                                                     : out.age_summary_id;
      slot = id;
    } else if (key == "aliquots" || key == "measurements") {
      if (!value.is_object()) return fail(bad(where, "'" + key + "' is not an object"));
      std::map<std::string, std::int64_t>& into = key == "aliquots" ? out.aliquots : out.measurements;
      for (const auto& [name, id] : value.items()) {
        const auto parsed = id_in(id);
        if (!parsed) return fail(bad(where, "'" + key + "' has no id for '" + name + "'"));
        into.emplace(name, *parsed);
      }
    } else if (key == "inDoubt") {
      if (!value.is_string() || value.get_ref<const std::string&>().empty()) return fail(bad(where, "'inDoubt' is not text"));
      out.in_doubt = value.get<std::string>();
    } else {
      return fail(bad(where, "unknown key '" + key + "'"));
    }
  }
  if (!versioned) return fail(bad(where, "it has no 'version'"));
  if (out.hash.empty()) return fail(bad(where, "it has no 'hash'"));
  return std::optional<Journal>(std::move(out));
}

Result<void> save_journal(const Journal& journal, const fs::path& file) {
  if (file.empty()) return fail(ErrorKind::Config, "earthbank journal: no file was named");
  const std::string where = utf8(file);
  std::error_code ec;
  if (file.has_parent_path()) {
    fs::create_directories(file.parent_path(), ec);
    if (ec) return fail(ErrorKind::Io, "earthbank journal " + where + ": cannot make its directory: " + ec.message());
  }
  static std::atomic<unsigned> serial{0};
  fs::path beside = file;
  beside += ".tmp." + std::to_string(process_id()) + "." + std::to_string(serial.fetch_add(1));
  fs::remove(beside, ec);
  if (const auto wrong = write_new(beside, to_json(journal).dump(2) + "\n")) {
    fs::remove(beside, ec);
    return fail(ErrorKind::Io, "earthbank journal " + where + ": cannot be written (it is as it was): " + *wrong);
  }
  fs::rename(beside, file, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(beside, ec);
    return fail(ErrorKind::Io, "earthbank journal " + where + ": cannot be replaced (it is as it was): " + why);
  }
  if (file.has_parent_path()) flush_directory(file.parent_path());
  return {};
}

}  // namespace pychron::earthbank
