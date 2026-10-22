#include "pychron/core/config/local_file.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/core/config/loader.hpp"

namespace pychron::config {

namespace {

namespace fs = std::filesystem;

// A TOML basic string.
std::string toml_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

std::map<std::string, LogLevel> by_pattern(const LoggingConfig& l) { return {l.levels.begin(), l.levels.end()}; }

std::string_view trimmed(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool is_comment(std::string_view line) { return !trimmed(line).empty() && trimmed(line).front() == '#'; }

// The name in a table header line (`[a.b]`, `[[a.b]]`, a comment after it
// allowed); empty when the line is not one.
std::string_view header_name(std::string_view line) {
  std::string_view t = trimmed(line);
  if (t.empty() || t.front() != '[') return {};
  const bool array = t.size() > 1 && t[1] == '[';
  const std::string_view close = array ? "]]" : "]";
  const auto end = t.find(close);
  if (end == std::string_view::npos) return {};
  const std::string_view rest = trimmed(t.substr(end + close.size()));
  if (!rest.empty() && rest.front() != '#') return {};  // `[1, 2],` is a row of an array, not a header
  const std::string_view name = trimmed(t.substr(array ? 2 : 1, end - (array ? 2 : 1)));
  for (const char c : name) {
    const bool key = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                     c == '-' || c == '.' || c == '"' || c == '\'' || c == ' ' || c == '*';
    if (!key) return {};
  }
  return name;
}

bool names(std::string_view header, std::string_view table) {
  return header == table ||
         (header.size() > table.size() && header.starts_with(table) && header[table.size()] == '.');
}

// The lines of `text`, each with its line ending.
std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at < text.size()) {
    const auto nl = text.find('\n', at);
    const std::size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
    out.push_back(text.substr(at, end - at));
    at = end;
  }
  return out;
}

// `text` without `[table]`, its subtables, and the comments directly above each.
std::string without_table(std::string_view text, std::string_view table) {
  const std::vector<std::string_view> lines = lines_of(text);
  std::vector<bool> drop(lines.size(), false);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!names(header_name(lines[i]), table)) continue;
    std::size_t end = i + 1;
    while (end < lines.size() && header_name(lines[end]).empty()) ++end;
    // A comment sitting on the next table is that table's.
    if (end < lines.size()) {
      while (end > i + 1 && is_comment(lines[end - 1])) --end;
    }
    std::size_t start = i;
    while (start > 0 && is_comment(lines[start - 1]) && !drop[start - 1]) --start;
    for (std::size_t k = start; k < end; ++k) drop[k] = true;
  }
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!drop[i]) out += lines[i];
  }
  // What followed the table moved up; no blank lines are left dangling at the end.
  const auto last = out.find_last_not_of(" \t\r\n");
  if (last == std::string::npos) return {};
  const auto nl = out.find('\n', last);
  out.erase(nl == std::string::npos ? out.size() : nl + 1);
  return out;
}

bool is_toml(std::string_view text, std::string& why) {
  const toml::parse_result r = toml::parse(text);
  if (r) return true;
  why = std::string(r.error().description());
  return false;
}

Unexpected<Error> failed(ErrorKind kind, const fs::path& file, const std::string& what) {
  return fail(kind, file.filename().string() + ": " + what + " (" + file.string() + ")");
}

}  // namespace

std::string logging_override_toml(const LoggingConfig& wanted, const LoggingConfig& main) {
  std::string keys;
  if (wanted.dir != main.dir) keys += "dir = " + toml_string(wanted.dir.string()) + "\n";
  if (wanted.max_size_mb != main.max_size_mb) keys += "max_size_mb = " + std::to_string(wanted.max_size_mb) + "\n";
  if (wanted.max_files != main.max_files) keys += "max_files = " + std::to_string(wanted.max_files) + "\n";
  if (wanted.default_level != main.default_level) {
    keys += std::string("default_level = \"") + level_name(wanted.default_level) + "\"\n";
  }
  if (wanted.echo_stderr != main.echo_stderr) {
    keys += std::string("echo_stderr = ") + (wanted.echo_stderr ? "true" : "false") + "\n";
  }
  std::string out;
  if (!keys.empty()) out = "[logging]\n" + keys;

  const auto levels = by_pattern(wanted);
  if (levels != by_pattern(main)) {
    if (!out.empty()) out += '\n';
    out += "[logging.levels]\n";
    for (const auto& [pattern, level] : levels) out += toml_string(pattern) + " = \"" + level_name(level) + "\"\n";
  }
  return out;
}

std::string metrics_override_toml(const MetricsConfig& wanted, const MetricsConfig& main) {
  std::string keys;
  if (wanted.enabled != main.enabled) keys += std::string("enabled = ") + (wanted.enabled ? "true" : "false") + "\n";
  if (wanted.bind != main.bind) keys += "bind = " + toml_string(wanted.bind) + "\n";
  if (wanted.port != main.port) keys += "port = " + std::to_string(wanted.port) + "\n";
  return keys.empty() ? std::string() : "[metrics]\n" + keys;
}

namespace {

// The file's text ("" when it is not there), checked to be TOML.
Result<void> read_local_file(const fs::path& file, std::string_view toml, bool& exists, std::string& before) {
  std::error_code ec;
  exists = fs::exists(file, ec);
  before.clear();
  if (exists) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return failed(ErrorKind::Io, file, "cannot be read");
    std::ostringstream text;
    text << in.rdbuf();
    before = text.str();
  }
  std::string why;
  if (!is_toml(before, why)) {
    return failed(ErrorKind::Config, file, "is not valid TOML and was left as it is: " + why);
  }
  if (!is_toml(toml, why)) return failed(ErrorKind::Config, file, "what was to be written is not valid TOML: " + why);
  return {};
}

// Writes `after` over `file` (which held `before`): checked to be TOML, the
// file removed when nothing is left, written beside the file and moved over it.
Result<void> write_local_file(const fs::path& file, bool exists, const std::string& before, const std::string& after) {
  std::error_code ec;
  std::string why;
  if (!is_toml(after, why)) return failed(ErrorKind::Config, file, "could not be changed safely: " + why);

  if (trimmed(after).empty()) {
    if (exists && !fs::remove(file, ec)) return failed(ErrorKind::Io, file, "cannot be removed: " + ec.message());
    return {};
  }
  if (after == before) return {};

  // Beside the file, then over it: a write that fails leaves the file whole.
  fs::path temp = file;
  temp += ".writing";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return failed(ErrorKind::Io, file, "cannot be written (is its folder read-only?)");
    out << after;
    out.flush();
    if (!out) {
      out.close();
      fs::remove(temp, ec);
      return failed(ErrorKind::Io, file, "could not be written in full");
    }
  }
  // The new file is as private as the one it replaces; one made here is its
  // owner's alone, as the setup wizard makes them.
  const fs::perms perms = exists ? fs::status(file, ec).permissions() & fs::perms::mask
                                 : fs::perms::owner_read | fs::perms::owner_write;
  fs::permissions(temp, perms, ec);
  fs::rename(temp, file, ec);
  if (ec) {
    const std::string message = ec.message();
    fs::remove(temp, ec);
    return failed(ErrorKind::Io, file, "could not be replaced: " + message);
  }
  return {};
}

bool is_blank(std::string_view line) { return trimmed(line).empty(); }

}  // namespace

Result<void> replace_local_table(const fs::path& local_file, std::string_view table, std::string_view toml) {
  bool exists = false;
  std::string before;
  if (auto r = read_local_file(local_file, toml, exists, before); !r) return r;

  std::string after = without_table(before, table);
  if (!trimmed(toml).empty()) {
    if (!after.empty()) {
      if (after.back() != '\n') after += '\n';
      after += '\n';
    }
    after += toml;
    if (after.back() != '\n') after += '\n';
  }
  return write_local_file(local_file, exists, before, after);
}

Result<void> set_local_table(const fs::path& file, std::string_view table, std::string_view toml) {
  if (trimmed(toml).empty()) return replace_local_table(file, table, toml);
  bool exists = false;
  std::string before;
  if (auto r = read_local_file(file, toml, exists, before); !r) return r;

  const bool crlf = [&] {
    const auto nl = before.find('\n');
    return nl != std::string::npos && nl > 0 && before[nl - 1] == '\r';
  }();
  const std::string eol = crlf ? "\r\n" : "\n";
  std::string body;  // the new text, in the file's line ending
  for (const char c : toml) {
    if (c == '\n') {
      if (!body.ends_with('\r')) body += eol;
      else body += '\n';
    } else {
      body += c;
    }
  }
  if (!body.ends_with('\n')) body += eol;

  const std::vector<std::string_view> lines = lines_of(before);
  std::size_t first = lines.size();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (names(header_name(lines[i]), table)) {
      first = i;
      break;
    }
  }

  std::string after;
  if (first == lines.size()) {
    after = before;
    if (!after.empty()) {
      if (after.back() != '\n') after += eol;
      after += eol;
    }
    after += body;
  } else {
    // The table runs on through its own subtables; a comment sitting on the
    // next table is that table's, and the blank lines before it stay.
    std::size_t end = first + 1;
    while (end < lines.size() && (header_name(lines[end]).empty() || names(header_name(lines[end]), table))) ++end;
    if (end < lines.size()) {
      while (end > first + 1 && is_comment(lines[end - 1])) --end;
    }
    std::size_t content = end;
    while (content > first + 1 && is_blank(lines[content - 1])) --content;
    for (std::size_t i = 0; i < first; ++i) after += lines[i];
    after += body;
    for (std::size_t i = content; i < end; ++i) after += lines[i];
    std::string rest;
    for (std::size_t i = end; i < lines.size(); ++i) rest += lines[i];
    // A later table of the same name (or its subtables) goes too.
    const bool again = std::ranges::any_of(lines_of(rest), [&](std::string_view l) { return names(header_name(l), table); });
    if (again) {
      rest = without_table(rest, table);
    }
    after += rest;
  }
  return write_local_file(file, exists, before, after);
}


Result<SystemConfig> load_system_config_without_local(const fs::path& main_file) {
  std::ifstream in(main_file, std::ios::binary);
  if (!in) return fail(ErrorKind::Config, "cannot read config file " + main_file.string());
  std::ostringstream text;
  text << in.rdbuf();
  return load_system_config_from_string(text.str(), main_file.string());
}

}  // namespace pychron::config
