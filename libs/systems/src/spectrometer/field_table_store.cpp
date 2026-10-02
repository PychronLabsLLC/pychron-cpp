#include "pychron/systems/spectrometer/field_table_store.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

namespace pychron::spectrometer {

namespace fs = std::filesystem;

namespace {

// Version names become file names: keep them to a safe alphabet so a
// caller-supplied version can never escape the table directory.
bool valid_version(std::string_view v) {
  if (v.empty()) return false;
  return std::all_of(v.begin(), v.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_';
  });
}

// (base, collision suffix) so "…Z-10" sorts after "…Z-2".
std::pair<std::string, long> sort_key(const std::string& v) {
  auto dash = v.rfind('-');
  if (dash == std::string::npos || dash + 1 == v.size()) return {v, 0};
  const auto suffix = v.substr(dash + 1);
  if (!std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; })) return {v, 0};
  return {v.substr(0, dash), std::stol(suffix)};
}

Result<std::string> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Write-then-rename so readers never see a partial file.
Result<void> write_atomic(const fs::path& p, const std::string& text) {
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail(ErrorKind::Io, "cannot write " + tmp.string());
    out << text;
    out.flush();
    if (!out) return fail(ErrorKind::Io, "write failed: " + tmp.string());
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) return fail(ErrorKind::Io, "cannot rename " + tmp.string() + ": " + ec.message());
  return {};
}

}  // namespace

std::string format_version(FieldTableStore::WallTime when) {
  // Civil-from-days (H. Hinnant), avoiding non-thread-safe gmtime.
  const auto secs = std::chrono::floor<std::chrono::seconds>(when).time_since_epoch().count();
  long long days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
  long long sod = secs - days * 86400;
  days += 719468;
  const long long era = (days >= 0 ? days : days - 146096) / 146097;
  const long long doe = days - era * 146097;
  const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const long long mp = (5 * doy + 2) / 153;
  const long long d = doy - (153 * mp + 2) / 5 + 1;
  const long long m = mp < 10 ? mp + 3 : mp - 9;
  const long long y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  char buf[128];  // room for six full-width long longs: gcc checks the worst case
  std::snprintf(buf, sizeof buf, "%04lld%02lld%02lldT%02lld%02lld%02lldZ", y, m, d, sod / 3600, (sod / 60) % 60,
                sod % 60);
  return buf;
}

FieldTableStore::FieldTableStore(fs::path tables_root, std::string name)
    : root_(std::move(tables_root)), name_(std::move(name)) {}

fs::path FieldTableStore::version_path(const std::string& version) const {
  return directory() / (version + ".toml");
}

Result<std::string> FieldTableStore::save(const FieldTable& table, WallTime when) {
  if (!valid_version(name_)) return fail(ErrorKind::Config, "invalid field table name '" + name_ + "'");
  std::error_code ec;
  fs::create_directories(directory(), ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + directory().string() + ": " + ec.message());

  const std::string base = format_version(when);
  std::string version = base;
  for (int n = 1; fs::exists(version_path(version), ec); ++n) version = base + "-" + std::to_string(n);

  if (auto w = write_atomic(version_path(version), to_toml(table)); !w) return fail(w.error());
  if (auto c = set_current(version); !c) return fail(c.error());
  return version;
}

Result<std::vector<std::string>> FieldTableStore::versions() const {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(directory(), ec)) return out;
  for (fs::directory_iterator it(directory(), ec), end; !ec && it != end; it.increment(ec)) {
    const auto& p = it->path();
    if (p.extension() != ".toml") continue;
    auto stem = p.stem().string();
    if (valid_version(stem)) out.push_back(std::move(stem));
  }
  if (ec) return fail(ErrorKind::Io, "cannot list " + directory().string() + ": " + ec.message());
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return sort_key(a) < sort_key(b); });
  return out;
}

Result<std::string> FieldTableStore::current_version() const {
  auto text = read_file(directory() / "current");
  if (!text) return fail(ErrorKind::Config, "field table '" + name_ + "' has no current version");
  std::string v = *text;
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
  if (!valid_version(v)) return fail(ErrorKind::Config, "field table '" + name_ + "': bad current pointer '" + v + "'");
  return v;
}

Result<FieldTable> FieldTableStore::load() const {
  auto v = current_version();
  if (!v) return fail(v.error());
  return load(*v);
}

Result<FieldTable> FieldTableStore::load(const std::string& version) const {
  if (!valid_version(version)) return fail(ErrorKind::Config, "invalid field table version '" + version + "'");
  std::error_code ec;
  if (!fs::exists(version_path(version), ec)) {
    return fail(ErrorKind::Config, "field table '" + name_ + "' has no version " + version);
  }
  auto text = read_file(version_path(version));
  if (!text) return fail(text.error());
  auto table = parse_field_table(*text);
  if (!table) {
    auto e = table.error();
    e.what = version_path(version).string() + ": " + e.what;
    return fail(std::move(e));
  }
  return table;
}

Result<void> FieldTableStore::restore(const std::string& version) {
  if (auto t = load(version); !t) return fail(t.error());
  return set_current(version);
}

Result<void> FieldTableStore::set_current(const std::string& version) {
  return write_atomic(directory() / "current", version + "\n");
}

}  // namespace pychron::spectrometer
