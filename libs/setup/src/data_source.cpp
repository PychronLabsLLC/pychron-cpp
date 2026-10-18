#include "pychron/setup/data_source.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/template.hpp"

#include "file_io.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pychron::setup {
namespace {

constexpr std::string_view kSqlitePrefix = "sqlite:";
constexpr std::string_view kPostgresPrefix = "postgresql://";

// Byte-equal to profiles/store-common/credentials.toml (a test holds them
// together): a change writes the file a reconfigure would render, so the
// reconfigure finds it as its own.
constexpr std::string_view kCredentialsTemplate =
    "# This computer only (written by pychron setup, owner-readable).\n"
    "[database]\n"
    "password = {{ db_password | toml }}\n";
constexpr std::string_view kCredentialsFile = ".pychron/credentials.toml";

fs::path resolve(const fs::path& path, const fs::path& root) { return path.is_absolute() ? path : root / path; }

std::optional<int> parse_port(std::string_view text) {
  int port = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, port);
  if (ec != std::errc{} || ptr != end) return std::nullopt;
  return port;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Undoes percent_encode. A '%' not followed by two hex digits is kept as it is.
std::string percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = hex_value(text[i + 1]);
      const int lo = hex_value(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += text[i];
  }
  return out;
}

// postgresql://user[:password]@host:port/db; the password is not taken. The
// user is percent-decoded, as database_url_for encodes it; host and db name
// are written unencoded there, so they are taken as they stand.
Result<ServerDatabase> parse_server_url(const std::string& url) {
  auto bad = [&] { return fail(ErrorKind::Config, "database URL is not postgresql://user@host:port/db: " + url); };
  std::string_view rest = std::string_view(url).substr(kPostgresPrefix.size());
  const auto at = rest.rfind('@');
  if (at == std::string_view::npos) return bad();
  const std::string_view userinfo = rest.substr(0, at);
  rest.remove_prefix(at + 1);
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos) return bad();
  const std::string_view hostport = rest.substr(0, slash);
  ServerDatabase server;
  server.name = std::string(rest.substr(slash + 1));
  server.user = percent_decode(userinfo.substr(0, userinfo.find(':')));
  if (const auto colon = hostport.rfind(':'); colon != std::string_view::npos) {
    const auto port = parse_port(hostport.substr(colon + 1));
    if (!port) return bad();
    server.port = *port;
    server.host = std::string(hostport.substr(0, colon));
  } else {
    server.host = std::string(hostport);
  }
  return server;
}

Result<DataSource> from_answers(const Answers& answers) {
  auto text = [&](const char* id) {
    auto it = answers.find(id);
    return it == answers.end() ? std::string{} : to_text(it->second);
  };
  DataSource source;
  if (text("data_source") == "server") {
    ServerDatabase server;
    server.host = text("db_host");
    server.name = text("db_name");
    server.user = text("db_user");
    if (const std::string port = text("db_port"); !port.empty()) {
      const auto parsed = parse_port(port);
      if (!parsed) return fail(ErrorKind::Config, "install record: db_port is not a port: " + port);
      server.port = *parsed;
    }
    source.database = std::move(server);
  } else {
    LocalDatabase local;
    if (std::string file = text("db_file"); !file.empty()) local.file = std::move(file);
    source.database = std::move(local);
  }
  if (std::string data = text("data_folder"); !data.empty()) source.data = std::move(data);
  return source;
}

Result<DataSource> from_site_entry(const SiteInstall& install) {
  DataSource source;
  const std::string& url = install.database;
  if (url.starts_with(kPostgresPrefix)) {
    auto server = parse_server_url(url);
    if (!server) return fail(std::move(server).error());
    source.database = std::move(*server);
  } else if (url.starts_with(kSqlitePrefix)) {
    source.database = LocalDatabase{fs::path(url.substr(kSqlitePrefix.size()))};
  } else if (url.empty()) {
    source.database = LocalDatabase{};
  } else {
    return fail(ErrorKind::Config, "database URL is neither sqlite: nor postgresql://: " + url);
  }
  if (!install.data.empty()) source.data = install.data;
  return source;
}

Result<void> refuse_bad(const SiteInstall& install, const DataSource& to) {
  std::error_code ec;
  if (install.root.empty() || !fs::is_directory(install.root, ec))
    return fail(ErrorKind::Config, "install root " + install.root.generic_string() + " is not on disk");
  if (to.data.empty()) return fail(ErrorKind::Config, "data folder is empty");
  if (const auto* local = std::get_if<LocalDatabase>(&to.database)) {
    if (local->file.empty()) return fail(ErrorKind::Config, "database file is empty");
    const fs::path file = resolve(local->file, install.root);
    if (fs::is_directory(file, ec))
      return fail(ErrorKind::Config, "database file " + file.generic_string() + " is a directory");
    return {};
  }
  const auto& server = std::get<ServerDatabase>(to.database);
  if (server.host.empty()) return fail(ErrorKind::Config, "database server host is empty");
  if (server.name.empty()) return fail(ErrorKind::Config, "database name is empty");
  if (server.user.empty()) return fail(ErrorKind::Config, "database user is empty");
  if (server.port < 1 || server.port > 65535)
    return fail(ErrorKind::Config, "database port " + std::to_string(server.port) + " is not 1 to 65535");
  return {};
}

bool holds_records(const fs::path& data_folder) {
  std::error_code ec;
  const fs::path records = data_folder / "records";
  if (!fs::is_directory(records, ec)) return false;
  return fs::directory_iterator(records, ec) != fs::directory_iterator();
}

// The answers of the install record `to` stands for: the server's keys or
// the file, never both; the password is not an answer that is recorded.
void set_answers(Answers& answers, const DataSource& to) {
  if (const auto* local = std::get_if<LocalDatabase>(&to.database)) {
    answers["data_source"] = Value{std::string("local")};
    answers["db_file"] = Value{local->file.generic_string()};
    for (const char* id : {"db_host", "db_port", "db_name", "db_user"}) answers.erase(id);
  } else {
    const auto& server = std::get<ServerDatabase>(to.database);
    answers["data_source"] = Value{std::string("server")};
    answers["db_host"] = Value{server.host};
    answers["db_port"] = Value{std::int64_t{server.port}};
    answers["db_name"] = Value{server.name};
    answers["db_user"] = Value{server.user};
    answers.erase("db_file");
  }
  answers.erase("db_password");
  answers["data_folder"] = Value{to.data};
}

std::string first_line(const std::string& text) { return text.substr(0, text.find('\n')); }

// `text` with every occurrence of `secret` put out of sight: a driver's
// message may quote the URL it was given.
std::string redact(std::string text, const std::optional<std::string>& secret) {
  if (!secret || secret->empty()) return text;
  for (auto at = text.find(*secret); at != std::string::npos; at = text.find(*secret, at + 3))
    text.replace(at, secret->size(), "***");
  return text;
}

// Whether `dir`, which exists, can be written to, found without writing.
bool writable_without_writing(const fs::path& dir) {
#ifdef _WIN32
  std::error_code ec;
  const auto status = fs::status(dir, ec);
  return !ec && (status.permissions() & fs::perms::owner_write) != fs::perms::none;
#else
  return ::access(dir.c_str(), W_OK) == 0;
#endif
}

// Whether a file can be made in `dir` (as doctor's check_writable): one is
// made and removed.
bool writable_by_probe(const fs::path& dir) {
  const fs::path probe = dir / ".pychron-data-source-probe";
  {
    std::ofstream f(probe, std::ios::binary);
    f << "x";
    if (!f) return false;
  }
  std::error_code ec;
  fs::remove(probe, ec);
  return true;
}

// The data folder: made with `create`, then probed; without it nothing is
// written and a missing folder is said as what will happen.
Result<void> check_data_folder(const fs::path& folder, bool create, std::vector<std::string>& said) {
  const std::string name = folder.generic_string();
  auto refused = [&] { return fail(ErrorKind::Config, "data folder " + name + " is not writable"); };
  std::error_code ec;
  if (!fs::exists(folder, ec)) {
    if (!create) {
      said.push_back("data folder " + name + " will be made");
      return {};
    }
    fs::create_directories(folder, ec);
    if (ec) return refused();
  }
  if (!fs::is_directory(folder, ec)) return refused();
  if (create ? !writable_by_probe(folder) : !writable_without_writing(folder)) return refused();
  said.push_back("data folder " + name);
  return {};
}

Result<void> check_local(const fs::path& file, const std::string& url, const DataSourceOpeners& openers, bool create,
                         DataSourceCheck& check) {
  std::error_code ec;
  if (fs::exists(file, ec)) {
    if (fs::file_size(file, ec) == 0 && !ec)
      return fail(ErrorKind::Config, file.generic_string() + " is empty: not a pychron store");
    // Never migrated from here: the file may be a copy an older build reads.
    auto opened = openers.open_database(url, false);
    if (!opened) {
      std::string why = url + ": " + first_line(opened.error().what);
      if (opened.error().kind == ErrorKind::Config) why += "; run `elctl db migrate --db " + url + "` first";
      return fail(ErrorKind::Config, std::move(why));
    }
    check.opened = true;
    check.said.push_back("database " + url + " (" + *opened + ")");
    return {};
  }
  if (!create) {
    check.said.push_back("database " + file.generic_string() + " will be made");
    return {};
  }
  // SQLite makes the file, not the folder it is in.
  fs::create_directories(file.parent_path(), ec);
  if (ec) return fail(ErrorKind::Config, "cannot make " + file.parent_path().generic_string() + ": " + ec.message());
  auto opened = openers.open_database(url, true);
  if (!opened) return fail(ErrorKind::Config, url + ": " + first_line(opened.error().what));
  check.made_file = true;
  check.opened = true;
  check.said.push_back("database " + url + " made (" + *opened + ")");
  return {};
}

Result<void> check_server(const ServerDatabase& server, const DataSource& to, const fs::path& root,
                          const DataSourceOpeners& openers, DataSourceCheck& check) {
  const std::string shown = database_url(to, root);
  auto url = database_url_with_password(to, root);
  if (!url) return fail(ErrorKind::Config, shown + ": " + first_line(url.error().what));
  auto opened = openers.open_database(*url, false);
  if (!opened) {
    // The password may be the kept one: whichever was used is kept out of sight.
    std::optional<std::string> password = server.password;
    if (!password) {
      auto kept = read_kept_password(root);
      if (kept) password = *kept;
    }
    return fail(ErrorKind::Config, shown + ": " + redact(first_line(opened.error().what), password) +
                                       "; check the server, the user and the password");
  }
  check.opened = true;
  check.said.push_back("database " + shown + " (" + *opened + ")");
  return {};
}

}  // namespace

Result<DataSource> current_data_source(const SiteInstall& install) {
  std::error_code ec;
  if (fs::exists(record_path(install.root), ec)) {
    auto record = read_install_record(install.root);
    if (!record) return fail(std::move(record).error());
    return from_answers(record->answers);
  }
  return from_site_entry(install);
}

std::string database_url(const DataSource& source, const fs::path& root) {
  if (const auto* local = std::get_if<LocalDatabase>(&source.database))
    return std::string(kSqlitePrefix) + resolve(local->file, root).generic_string();
  const auto& server = std::get<ServerDatabase>(source.database);
  return std::string(kPostgresPrefix) + percent_encode(server.user) + "@" + server.host + ":" +
         std::to_string(server.port) + "/" + server.name;
}

Result<std::string> database_url_with_password(const DataSource& source, const fs::path& root) {
  const auto* server = std::get_if<ServerDatabase>(&source.database);
  if (server == nullptr) return database_url(source, root);
  std::optional<std::string> password = server->password;
  if (!password) {
    auto kept = read_kept_password(root);
    if (!kept) return fail(std::move(kept).error());
    password = std::move(*kept);
  }
  std::string user = percent_encode(server->user);
  if (password && !password->empty()) user += ":" + percent_encode(*password);
  return std::string(kPostgresPrefix) + user + "@" + server->host + ":" + std::to_string(server->port) + "/" +
         server->name;
}

std::string describe(const DataSource& source, const fs::path& root) {
  return "database " + database_url(source, root) + "\ndata folder " + source.data + " (" +
         resolve(fs::path(source.data), root).generic_string() + ")";
}

Result<DataSourceChange> plan_data_source(const SiteInstall& install, const DataSource& to) {
  if (auto refused = refuse_bad(install, to); !refused) return fail(std::move(refused).error());
  auto current = current_data_source(install);
  if (!current) return fail(std::move(current).error());

  DataSourceChange change{install, {}};
  if (*current == to) {
    change.notes.emplace_back("nothing to change");
    return change;
  }
  change.install.database = database_url(to, install.root);
  change.install.data = to.data;

  const fs::path old_folder = resolve(fs::path(current->data), install.root);
  const fs::path new_folder = resolve(fs::path(to.data), install.root);
  if (old_folder.lexically_normal() != new_folder.lexically_normal() && holds_records(old_folder)) {
    change.notes.push_back("records in " + old_folder.generic_string() + "/records are not moved; " +
                           new_folder.generic_string() + " starts without them");
  }
  std::error_code ec;
  if (!fs::exists(record_path(install.root), ec)) {
    change.notes.push_back("no install record in " + (install.root / ".pychron").generic_string() +
                           ": a later elctl init --reconfigure would not know the new data source");
  }
  return change;
}

Result<void> apply_data_source(const DataSourceChange& change, const DataSource& to, const fs::path& site_path) {
  const fs::path& root = change.install.root;
  // Read first: a site file or a record that does not load stops the change
  // before anything is written.
  auto site = load_site(site_path);
  if (!site) return fail(std::move(site).error());
  std::optional<InstallRecord> record;
  std::error_code ec;
  if (fs::exists(record_path(root), ec)) {
    auto read = read_install_record(root);
    if (!read) return fail(std::move(read).error());
    record = std::move(*read);
  }

  // 1. The credentials: only a server given a password.
  std::optional<std::string> credentials;
  if (const auto* server = std::get_if<ServerDatabase>(&to.database); server != nullptr && server->password) {
    auto rendered = render(kCredentialsTemplate, {{"db_password", Value{*server->password}}}, kCredentialsFile);
    if (!rendered) return fail(std::move(rendered).error());
    if (auto written = detail::write_file(root / kCredentialsFile, *rendered, true); !written)
      return fail(std::move(written).error());
    credentials = std::move(*rendered);
  }

  // 2. The install record, when there is one.
  if (record) {
    set_answers(record->answers, to);
    if (credentials) record->files[std::string(kCredentialsFile)] = sha256_hex(*credentials);
    if (auto written = write_install_record(root, *record); !written) return fail(std::move(written).error());
  }

  // 3. The site entry: database and data only.
  SiteInstall entry = change.install;
  if (const SiteInstall* found = site->find(change.install.name)) {
    entry = *found;
    entry.database = change.install.database;
    entry.data = change.install.data;
  }
  site->upsert(std::move(entry));
  return save_site(*site, site_path);
}

Result<DataSourceCheck> check_data_source(const DataSourceChange& change, const DataSource& to,
                                          const DataSourceOpeners& openers, bool create) {
  const fs::path& root = change.install.root;
  DataSourceCheck check;
  if (auto folder = check_data_folder(resolve(fs::path(to.data), root), create, check.said); !folder)
    return fail(std::move(folder).error());
  if (!openers.open_database) {
    check.said.emplace_back("skip  check: built without the DVC store");
    return check;
  }
  if (const auto* local = std::get_if<LocalDatabase>(&to.database)) {
    if (auto done = check_local(resolve(local->file, root), database_url(to, root), openers, create, check); !done)
      return fail(std::move(done).error());
    return check;
  }
  if (auto done = check_server(std::get<ServerDatabase>(to.database), to, root, openers, check); !done)
    return fail(std::move(done).error());
  return check;
}

std::vector<std::string> seed_data_source(const DataSourceChange& change, const DataSource& to,
                                          const DataSourceOpeners& openers, const DataSourceCheck& check) {
  const SiteInstall& install = change.install;
  if (install.kind != "instrument") return {};
  // An install from before there was a seed file has none: nothing is said.
  const fs::path seed = install.root / "seed.toml";
  std::error_code ec;
  if (!fs::exists(seed, ec)) return {};
  if (!openers.seed_database) return {"skip  seed: built without the DVC store"};
  if (!check.opened) return {"seed skipped: the database was not checked; doctor says the rest"};
  const auto* local = std::get_if<LocalDatabase>(&to.database);
  if (local != nullptr && !check.made_file)
    return {"seed skipped: " + resolve(local->file, install.root).generic_string() +
            " existed; an existing store is not seeded"};
  auto url = database_url_with_password(to, install.root);
  if (!url) return {"seed skipped: " + first_line(url.error().what)};
  // Only a file this change made is migrated; a server never is from here.
  auto done = openers.seed_database(*url, seed, local != nullptr);
  if (!done) return {"seed skipped: " + first_line(done.error().what)};
  return {"database " + database_url(to, install.root) + ": " + *done};
}

}  // namespace pychron::setup
