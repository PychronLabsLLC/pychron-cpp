#include "pychron/earthbank/profiles.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
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
#include <vector>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "pychron/core/env.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/path_text.hpp"

namespace pychron::earthbank {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view kLegacySection = "pychron.ausgeochem";

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool is_alnum(char c) {
  const auto byte = static_cast<unsigned char>(c);
  return byte < 0x80 && std::isalnum(byte) != 0;
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// A key that would hold what is never kept in a file, in any spelling of
// case and with whatever is around the word (`Password`, `api_token`).
bool names_a_secret(std::string_view key) {
  const std::string low = lowered(key);
  return low.find("password") != std::string::npos || low.find("token") != std::string::npos ||
         low.find("secret") != std::string::npos;
}

// What a file that is there holds, or an `Io` error naming it: never nothing
// for a file that could not be read.
Result<std::string> read_file(const fs::path& file) {
  const std::string where = utf8(file);
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    return fail(ErrorKind::Io, where + ": cannot be read: " + (ec ? ec.message() : std::string("it is not a file")));
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, where + ": cannot be read");
  std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) return fail(ErrorKind::Io, where + ": cannot be read to its end");
  return text;
}

// --- the address ---

// A host name or an IPv4 address: labels of letters, digits and `-`.
bool is_host_name(std::string_view host) {
  if (host.empty() || host.size() > 253) return false;
  if (host.front() == '.' || host.back() == '.' || host.front() == '-' || host.back() == '-') return false;
  if (host.find("..") != std::string_view::npos) return false;
  return std::all_of(host.begin(), host.end(), [](char c) { return is_alnum(c) || c == '-' || c == '.'; });
}

// An IPv6 address in its brackets, as far as telling it from anything else.
bool is_bracketed_address(std::string_view host) {
  if (host.size() < 4 || host.front() != '[' || host.back() != ']') return false;
  const std::string_view inner = host.substr(1, host.size() - 2);
  return inner.find(':') != std::string_view::npos && std::all_of(inner.begin(), inner.end(), [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c)) != 0 || c == ':' || c == '.';
         });
}

bool is_port(std::string_view port) {
  if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), is_digit)) return false;
  int value = 0;
  std::from_chars(port.data(), port.data() + port.size(), value);
  return value >= 1 && value <= 65535;
}

// What is wrong with `url` as a profile's address, or nothing and the
// address in `out`. The words never repeat the address: one that is refused
// for holding a password would put it into a message.
std::optional<std::string> base_url_fault(std::string_view url, std::string& out) {
  constexpr std::string_view kSecure = "https://", kPlain = "http://";
  const bool secure = url.starts_with(kSecure);
  if (!secure && !url.starts_with(kPlain)) return "it must begin with https://";
  const std::string_view scheme = secure ? kSecure : kPlain;
  const std::string_view rest = url.substr(scheme.size());
  if (rest.find('?') != std::string_view::npos) return "it has a query (a `?`): the address ends at the host";
  if (rest.find('#') != std::string_view::npos) return "it has a fragment (a `#`): the address ends at the host";
  const auto slash = rest.find('/');
  const std::string_view authority = rest.substr(0, slash);
  const std::string_view path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
  if (authority.find('@') != std::string_view::npos)
    return "it holds a user name or a password: the address is https:// and the host, and the login is `username`";
  if (!path.empty() && path != "/") return "it has a path: the address ends at the host (https://host)";

  std::string_view host = authority, port;
  bool has_port = false;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) return "its host is not an address ([::1] is one)";
    host = authority.substr(0, close + 1);
    const std::string_view after = authority.substr(close + 1);
    if (!after.empty() && after.front() != ':') return "its host is not an address ([::1] is one)";
    has_port = !after.empty();
    if (has_port) port = after.substr(1);
    if (!is_bracketed_address(host)) return "its host is not an address ([::1] is one)";
  } else {
    if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
      has_port = true;
    }
    if (host.empty()) return "it names no host";
    if (!is_host_name(host)) return "its host is not a host's name (letters, digits, `-` and `.`)";
  }
  if (has_port && !is_port(port)) return "its port is not a number from 1 to 65535";

  if (!secure) {
    const std::string low = lowered(host);
    if (low != "localhost" && low != "127.0.0.1" && low != "[::1]")
      return "http:// is taken only for this machine (localhost, 127.0.0.1, [::1]): a server is reached by https://";
  }
  out = std::string(scheme) + std::string(authority);
  return std::nullopt;
}

// --- what a file may hold ---

// What is wrong with `profiles` as a whole, with the key it is wrong at, or
// nothing. What load_profiles refuses and save_profiles will not write.
std::optional<std::string> fault(const Profiles& profiles) {
  std::map<std::string, std::string> seen;  // lower case -> as spelled
  for (const Profile& p : profiles.profiles) {
    if (!valid_profile_name(p.name))
      return "profiles: '" + p.name + "' is not a profile's name (letters, digits, `-` and `_`)";
    const auto [first, fresh] = seen.emplace(lowered(p.name), p.name);
    if (!fresh) {
      if (first->second == p.name) return "profiles: there are two profiles named '" + p.name + "'";
      return "profiles: '" + first->second + "' and '" + p.name +
             "' differ only in case, and their passwords would be kept under one name";
    }
    const std::string key = "profiles." + p.name + ".";
    std::string url;
    if (const auto wrong = base_url_fault(p.base_url, url)) return key + "base_url: " + *wrong;
    if (p.institution_id && *p.institution_id < 1) return key + "institution_id: an id is a whole number above 0";
    if (p.data_package_id && *p.data_package_id < 1) return key + "data_package_id: an id is a whole number above 0";
  }
  if (!profiles.active.empty() && profiles.find(profiles.active) == nullptr)
    return "active: there is no profile named '" + profiles.active + "'";
  return std::nullopt;
}

Unexpected<Error> refused(const std::string& where, const std::string& what) {
  return fail(ErrorKind::Config, where + ": " + what);
}

Unexpected<Error> refused_secret(const std::string& where, const std::string& key) {
  return refused(where, key + ": a password is never kept in this file. Remove the key: the password is kept by the "
                              "system's password store, and is asked for when it is not there");
}

// `text`, the contents of the file named `where`, as profiles.
Result<Profiles> parse_profiles(std::string_view text, const std::string& where) {
  const auto parsed = toml::parse(text, where);
  if (!parsed) {
    const auto& error = parsed.error();
    return refused(where, "line " + std::to_string(error.source().begin.line) + ": " +
                              std::string(error.description()));
  }
  const toml::table& top = parsed.table();

  Profiles out;
  // A table's keys come in the order of their names; the file's order is in
  // where each was read.
  std::vector<std::pair<toml::source_index, Profile>> in_order;
  for (const auto& [top_key, top_node] : top) {
    const std::string key(top_key.str());
    if (names_a_secret(key)) return refused_secret(where, key);
    if (key == "active") {
      const auto name = top_node.value<std::string>();
      if (!top_node.is_string() || !name) return refused(where, "active: it is a profile's name, in quotes");
      out.active = *name;
    } else if (key == "profiles") {
      const toml::table* table = top_node.as_table();
      if (table == nullptr) return refused(where, "profiles: it is a table of profiles ([profiles.<name>])");
      for (const auto& [name_key, node] : *table) {
        Profile p;
        p.name = std::string(name_key.str());
        const std::string prefix = "profiles." + p.name;
        if (names_a_secret(p.name) && !node.is_table()) return refused_secret(where, prefix);
        const toml::table* fields = node.as_table();
        if (fields == nullptr) return refused(where, prefix + ": a profile is a table ([" + prefix + "])");
        if (!valid_profile_name(p.name))
          return refused(where, "profiles: '" + p.name + "' is not a profile's name (letters, digits, `-` and `_`)");
        for (const auto& [field_key, value] : *fields) {
          const std::string field(field_key.str());
          const std::string at = prefix + "." + field;
          if (names_a_secret(field)) return refused_secret(where, at);
          if (field == "base_url" || field == "username") {
            const auto said = value.value<std::string>();
            if (!value.is_string() || !said) return refused(where, at + ": it is text, in quotes");
            if (field == "username") {
              p.username = *said;
            } else if (const auto wrong = base_url_fault(*said, p.base_url)) {
              return refused(where, at + ": " + *wrong);
            }
          } else if (field == "institution_id" || field == "data_package_id") {
            const auto id = value.value<std::int64_t>();
            if (!value.is_integer() || !id || *id < 1) return refused(where, at + ": an id is a whole number above 0");
            (field == "institution_id" ? p.institution_id : p.data_package_id) = *id;
          } else {
            return refused(where, at + ": not a key of a profile (those are base_url, username, institution_id, "
                                       "data_package_id)");
          }
        }
        in_order.emplace_back(node.source().begin.line, std::move(p));
      }
    } else {
      return refused(where, key + ": not a key of this file (those are active and profiles)");
    }
  }
  std::stable_sort(in_order.begin(), in_order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [line, p] : in_order) out.profiles.push_back(std::move(p));
  if (const auto wrong = fault(out)) return refused(where, *wrong);
  return out;
}

// --- writing ---

// `text` as a TOML string in double quotes.
std::string toml_string(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (byte < 0x20 || byte == 0x7F) {
      out += "\\u00";
      out += kHex[byte >> 4];
      out += kHex[byte & 0xF];
    } else {
      out += c;
    }
  }
  return out + "\"";
}

std::string to_toml(const Profiles& profiles) {
  std::string out;
  if (!profiles.active.empty()) out += "active = " + toml_string(profiles.active) + "\n";
  for (const Profile& p : profiles.profiles) {
    if (!out.empty()) out += "\n";
    out += "[profiles." + p.name + "]\n";
    out += "base_url = " + toml_string(p.base_url) + "\n";
    out += "username = " + toml_string(p.username) + "\n";
    if (p.institution_id) out += "institution_id = " + std::to_string(*p.institution_id) + "\n";
    if (p.data_package_id) out += "data_package_id = " + std::to_string(*p.data_package_id) + "\n";
  }
  return out;
}

long process_id() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

#ifndef _WIN32
std::string words_for(int error_number) { return std::error_code(error_number, std::generic_category()).message(); }
#endif

// A new file `file` holding `text`, the owner's alone, and on the disk when
// this returns. What went wrong otherwise; the file may then be there in
// part, for the caller to remove.
std::optional<std::string> write_new(const fs::path& file, std::string_view text) {
#ifdef _WIN32
  // No mode to give here: what keeps the file the owner's is the access
  // list it inherits from %APPDATA%, which is the user's own.
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return "cannot be created";
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out) return "cannot be written";
  return std::nullopt;
#else
  // Never through a link somebody left under the name, and never open to
  // anybody else, not even while it is empty.
  const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return words_for(errno);
  std::optional<std::string> wrong;
  while (!text.empty() && !wrong) {
    const ssize_t wrote = ::write(fd, text.data(), text.size());
    if (wrote < 0) {
      if (errno == EINTR) continue;
      wrong = words_for(errno);
    } else {
      text.remove_prefix(static_cast<std::size_t>(wrote));
    }
  }
  if (!wrong && ::fsync(fd) != 0) wrong = words_for(errno);
  if (::close(fd) != 0 && !wrong) wrong = words_for(errno);
  return wrong;
#endif
}

// --- legacy pychron's preferences ---

// The value of a line of the file as its writer (ConfigObj) quoted it: in
// one kind of quote, in three when the text had both kinds in it, or bare,
// where a `#` begins a comment.
std::string unquoted(std::string_view value) {
  value = trimmed(value);
  for (const std::string_view triple : {std::string_view("'''"), std::string_view("\"\"\"")}) {
    if (!value.starts_with(triple)) continue;
    value.remove_prefix(3);
    return std::string(value.substr(0, value.rfind(triple)));
  }
  if (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
    const char quote = value.front();
    value.remove_prefix(1);
    return std::string(value.substr(0, value.find(quote)));
  }
  return std::string(trimmed(value.substr(0, value.find('#'))));
}

// A legacy name as a profile's name here: what is not allowed becomes `_`.
std::string respelled(std::string_view name) {
  std::string out(trimmed(name));
  for (char& c : out)
    if (!is_alnum(c) && c != '-' && c != '_') c = '_';
  return out;
}

// An id as legacy kept it: a number, or a number as text, or blank for none.
// `said` is what the entry held when that was something else.
std::optional<std::int64_t> legacy_id(const json& entry, const char* key, std::string& said) {
  const auto found = entry.find(key);
  if (found == entry.end() || found->is_null()) return std::nullopt;
  if (found->is_number_integer() || found->is_number_unsigned()) {
    const auto id = found->get<std::int64_t>();
    if (id >= 1) return id;
    said = std::to_string(id);
    return std::nullopt;
  }
  if (!found->is_string()) {
    said = found->dump();
    return std::nullopt;
  }
  const std::string text(trimmed(found->get<std::string>()));
  if (text.empty()) return std::nullopt;
  std::int64_t id = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (error == std::errc{} && end == text.data() + text.size() && id >= 1) return id;
  said = text;
  return std::nullopt;
}

std::string text_of(const json& entry, const char* key) {
  const auto found = entry.find(key);
  if (found == entry.end() || !found->is_string()) return {};
  return found->get<std::string>();
}

// An error when `file` is a symbolic link to something that is not there:
// not a file that is missing, for the link is the user's own (a dotfiles
// directory that is not mounted), and a save would replace it.
std::optional<Error> dangling_link(const fs::path& file) {
  std::error_code ec;
  if (!fs::is_symlink(file, ec) || ec) return std::nullopt;
  if (fs::exists(file, ec) || ec) return std::nullopt;
  return Error{ErrorKind::Io, utf8(file) + ": is a symbolic link to a file that is not there", ""};
}

}  // namespace

const Profile* Profiles::find(std::string_view name) const {
  const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& p) { return p.name == name; });
  return found == profiles.end() ? nullptr : &*found;
}

const Profile* Profiles::active_profile() const { return active.empty() ? nullptr : find(active); }

bool valid_profile_name(std::string_view name) {
  return !name.empty() &&
         std::all_of(name.begin(), name.end(), [](char c) { return is_alnum(c) || c == '-' || c == '_'; });
}

Result<std::string> checked_base_url(std::string_view url) {
  std::string out;
  if (const auto wrong = base_url_fault(url, out)) return fail(ErrorKind::Config, "the address is not one: " + *wrong);
  return out;
}

Result<Profiles> load_profiles(const fs::path& file) {
  if (file.empty()) return fail(ErrorKind::Config, "no profiles file was named (the user has no home directory?)");
  // Only a file that is not there is no profiles. One that is there and
  // cannot be read, or cannot be asked about, is an error: taken for empty,
  // the next save would replace the user's profiles with nothing.
  std::error_code ec;
  const bool there = fs::exists(file, ec);
  if (ec) return fail(ErrorKind::Io, utf8(file) + ": cannot be looked for: " + ec.message());
  if (!there) {
    if (auto link = dangling_link(file)) return fail(std::move(*link));
    return Profiles{};
  }
  const auto text = read_file(file);
  if (!text) return fail(text.error());
  return parse_profiles(*text, utf8(file));
}

Result<void> save_profiles(const Profiles& profiles, const fs::path& file) {
  if (file.empty()) return fail(ErrorKind::Config, "no profiles file was named (the user has no home directory?)");
  const std::string where = utf8(file);
  if (const auto wrong = fault(profiles)) return refused(where, *wrong);
  // An address is written as it would be read (no slash at its end), and
  // whatever else the reader would not take back (a login that is not text)
  // is found by reading what is about to be written.
  Profiles tidy = profiles;
  for (Profile& p : tidy.profiles) (void)base_url_fault(std::string(p.base_url), p.base_url);
  const std::string text = to_toml(tidy);
  const auto back = parse_profiles(text, where);
  if (!back) return fail(back.error());
  if (back->active != tidy.active || back->profiles != tidy.profiles)
    return refused(where, "these profiles would not be read back as they are");

  if (auto link = dangling_link(file)) return fail(std::move(*link));
  std::error_code ec;
  if (file.has_parent_path()) {
    fs::create_directories(file.parent_path(), ec);
    if (ec) return fail(ErrorKind::Io, where + ": cannot make its directory: " + ec.message());
  }
  // Beside the file, so that the rename is within one file system; a name
  // of this process's and this call's own.
  static std::atomic<unsigned> serial{0};
  fs::path beside = file;
  beside += ".tmp." + std::to_string(process_id()) + "." + std::to_string(serial.fetch_add(1));
  fs::remove(beside, ec);
  if (const auto wrong = write_new(beside, text)) {
    fs::remove(beside, ec);
    return fail(ErrorKind::Io, where + ": cannot write it (the old file, if any, is as it was): " + *wrong);
  }
  fs::rename(beside, file, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(beside, ec);
    return fail(ErrorKind::Io, where + ": cannot replace it (the old file, if any, is as it was): " + why);
  }
  return {};
}

fs::path default_profiles_file() {
  // The directory libs/setup keeps the site file in (default_site_path).
#if defined(_WIN32) || defined(__APPLE__)
  const fs::path tail = fs::path("Pychron") / "earthbank" / "profiles.toml";
#else
  const fs::path tail = fs::path("pychron") / "earthbank" / "profiles.toml";
#endif
  const auto set = [](const char* name) -> std::optional<fs::path> {
    auto value = env_var(name);
    if (!value || value->empty()) return std::nullopt;
    return fs::path(*value);
  };
#if defined(_WIN32)
  if (const auto appdata = set("APPDATA")) return *appdata / tail;
  if (const auto profile = set("USERPROFILE")) return *profile / "AppData" / "Roaming" / tail;
  return {};
#elif defined(__APPLE__)
  if (const auto home = set("HOME")) return *home / "Library" / "Application Support" / tail;
  return {};
#else
  // A value that is not a whole path is to be ignored (the XDG base
  // directory rules).
  if (const auto xdg = set("XDG_CONFIG_HOME"); xdg && xdg->is_absolute()) return *xdg / tail;
  if (const auto home = set("HOME")) return *home / ".config" / tail;
  return {};
#endif
}

Result<Profiles> import_legacy_ini(const fs::path& ini) {
  const std::string where = utf8(ini);
  std::error_code ec;
  if (!fs::is_regular_file(ini, ec)) return fail(ErrorKind::Io, where + ": there is no such file");
  std::ifstream in(ini, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, where + ": cannot be read");
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

  // The keys of the plugin's section, or of a file that has no sections.
  std::map<std::string, std::string> keys, bare;
  bool in_section = false, any_section = false, has_section = false;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::string_view line = trimmed(std::string_view(text).substr(at, end - at));
    at = end + 1;
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    if (line.front() == '[') {
      const auto close = line.find(']');
      const std::string_view name = trimmed(line.substr(1, close == std::string_view::npos ? close : close - 1));
      any_section = true;
      in_section = name == kLegacySection;
      has_section = has_section || in_section;
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) continue;
    const std::string key(trimmed(line.substr(0, equals)));
    if (in_section) keys[key] = unquoted(line.substr(equals + 1));
    else if (!any_section) bare[key] = unquoted(line.substr(equals + 1));
  }
  if (!has_section) {
    if (!bare.contains("profiles_json"))
      return refused(where, "it has no [" + std::string(kLegacySection) + "] section: not the preferences of legacy "
                            "pychron's EarthBank plugin");
    keys = std::move(bare);
  }

  Profiles out;
  for (const auto& [key, value] : keys)
    if (names_a_secret(key) && !value.empty())
      out.notes.push_back("a password in the file (the key '" + key + "') was ignored: passwords are not kept in "
                          "files. Sign in once to store it");

  const std::string blob = keys.contains("profiles_json") ? keys.at("profiles_json") : std::string{};
  if (trimmed(blob).empty()) {
    out.notes.push_back("the file has no EarthBank profiles in it");
    return out;
  }
  const json list = json::parse(blob, nullptr, false);
  if (!list.is_array())
    return refused(where, "profiles_json: not the list of profiles legacy pychron wrote (a JSON list)");

  // Legacy's names, as they come out here: the active one is among them.
  std::map<std::string, std::string> renamed;
  for (const json& entry : list) {
    if (!entry.is_object()) continue;
    const std::string legacy_name = text_of(entry, "name");
    const std::string shown = "'" + legacy_name + "'";

    Profile p;
    p.name = respelled(legacy_name);
    if (p.name.empty()) {
      out.notes.push_back("a profile with no name was left out");
      continue;
    }
    const bool taken = std::any_of(out.profiles.begin(), out.profiles.end(),
                                   [&](const Profile& q) { return lowered(q.name) == lowered(p.name); });
    if (taken) {
      out.notes.push_back("the profile " + shown + " was left out: there is one named '" + p.name +
                          "' already (names differ by more than case here)");
      continue;
    }
    // Blank is the service, as it was for legacy.
    if (const std::string url(trimmed(text_of(entry, "base_url"))); !url.empty()) {
      if (const auto wrong = base_url_fault(url, p.base_url)) {
        out.notes.push_back("the profile " + shown + " was left out for its address: " + *wrong);
        continue;
      }
    }
    if (p.name != legacy_name)
      out.notes.push_back("the profile " + shown + " is named '" + p.name +
                          "' here: a name is letters, digits, `-` and `_`");
    p.username = text_of(entry, "username");
    for (const auto& [key, id] : {std::pair{"institution_id", &p.institution_id},
                                  std::pair{"data_package_id", &p.data_package_id}}) {
      std::string said;
      *id = legacy_id(entry, key, said);
      if (!said.empty())
        out.notes.push_back("the profile " + shown + ": its " + key + ", '" + said +
                            "', is not an id (a whole number above 0) and was left out");
    }
    for (const auto& [key, value] : entry.items())
      if (names_a_secret(key) && !(value.is_string() && value.get<std::string>().empty()) && !value.is_null())
        out.notes.push_back("the profile " + shown + ": a password in the file (the key '" + key +
                            "') was ignored: passwords are not kept in files. Sign in once to store it");
    renamed.emplace(legacy_name, p.name);
    out.profiles.push_back(std::move(p));
  }

  if (out.profiles.empty()) {
    out.notes.push_back("no profile of the file could be taken");
    return out;
  }
  const std::string active = keys.contains("active_profile") ? keys.at("active_profile") : std::string{};
  if (const auto found = renamed.find(active); found != renamed.end()) {
    out.active = found->second;
  } else {
    // Legacy took the first when the one it had chosen was gone.
    out.active = out.profiles.front().name;
    if (!active.empty())
      out.notes.push_back("the active profile, '" + active + "', is not among those taken: '" + out.active +
                          "' is the active one");
  }
  return out;
}

}  // namespace pychron::earthbank
