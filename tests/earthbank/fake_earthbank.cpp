#include "fake_earthbank.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/vocab.hpp"

// Where each behaviour was learned. "legacy" is the University of Melbourne
// fork's `pychron/ausgeochem`: `earthbank_service.py` (the client), its
// `README.md` and `UPLOAD_GUIDE.md`, and the probe scripts under `tests/`.
// What legacy does not say is marked by a comment that begins `assumed (n)`,
// n being its item in the list of fixtures/README.md. A test holds the marks
// and the list to each other (FakeEarthBank.EveryAssumptionIsMarkedAndListed).

namespace pychron::earthbank::testing {
namespace {

using nlohmann::json;
using namespace std::string_view_literals;

constexpr std::string_view kRedacted = "<redacted>";
constexpr std::string_view kAuthenticate = "/api/authenticate";
constexpr std::string_view kAccount = "/api/account";
constexpr std::string_view kPackages = "/api/management/data-packages";
constexpr std::string_view kSamples = "/api/core/sample-with-locations";
constexpr std::string_view kDataPoint = "/api/arar/ArArDataPoint";
// Legacy's comment on LOOKUP_ENDPOINTS: this list has a `1 sigma`, and its id
// is not one a record takes. It is served so that a client that asks it gets
// what the real one would: an id the data point then refuses.
// assumed (15): what else the list holds (the fixture's rows).
constexpr std::string_view kErrorTypes = "/api/core/l-error-types";

// assumed (1): a page is 20 rows unless `size` says otherwise and never more than
// 2000 (Spring Data's defaults; legacy asks for 2000 and gets a whole list).
constexpr std::size_t kDefaultPageSize = 20;
constexpr std::size_t kMaxPageSize = 2000;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = lower(c);
  return out;
}

bool starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

bool contains_without_case(std::string_view text, std::string_view part) {
  return lowered(text).find(lowered(part)) != std::string::npos;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A component of a query: `%XX` is a byte.
// assumed (6): `+` is a space, as a servlet container reads a query, so a client
// that does not encode a plus loses it.
std::string url_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 && hex_value(text[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2])));
      i += 2;
    } else {
      out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query) {
  std::vector<std::pair<std::string, std::string>> out;
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view one = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
    if (one.empty()) continue;
    const std::size_t eq = one.find('=');
    out.emplace_back(url_decode(one.substr(0, eq)),
                     eq == std::string_view::npos ? std::string{} : url_decode(one.substr(eq + 1)));
  }
  return out;
}

// What may stand in a request's target as it is: a space, a quote, a byte of
// UTF-8 must have been encoded.
bool is_url_text(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return c > 0x20 && c < 0x7F && c != '"' && c != '<' && c != '>' && c != '\\' && c != '^' && c != '`' &&
           c != '{' && c != '|' && c != '}';
  });
}

std::optional<std::int64_t> parse_int(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
  return value;
}

// An id: a whole number. Null and absent are no id.
// assumed (7): the same in a string is taken too, as Jackson reads a number.
std::optional<std::int64_t> as_id(const json& value) {
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_string()) return parse_int(value.get_ref<const std::string&>());
  return std::nullopt;
}

bool is_set(const json& object, std::string_view key) {
  const auto it = object.find(key);
  return it != object.end() && !it->is_null();
}

std::optional<std::int64_t> id_of(const json& object, std::string_view key) {
  if (!object.is_object()) return std::nullopt;
  const auto it = object.find(key);
  return it == object.end() ? std::nullopt : as_id(*it);
}

// `mineralId` -> `mineral_id`, `LArArMethod` -> `l_ar_ar_method`,
// `l-sample-kinds` -> `l_sample_kinds`: the service's names as its database
// has them, as far as legacy shows (`ar_ar_data_point`, `l_uncertainty`).
std::string snake(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c == '-') {
      out.push_back('_');
    } else if (c >= 'A' && c <= 'Z') {
      if (!out.empty() && out.back() != '_') out.push_back('_');
      out.push_back(lower(c));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string_view last_segment(std::string_view path) { return path.substr(path.rfind('/') + 1); }

// ---- Answers ---------------------------------------------------------------

HttpResponse answer(int status, const json& body) { return HttpResponse{status, body.dump()}; }

// assumed (3): an error that legacy does not quote has the body JHipster gives
// every error, an RFC 7807 problem. Legacy reads `detail`, then `message`,
// then `title` out of whatever comes (_humanize_error).
HttpResponse problem(int status, std::string_view title, std::string_view path, std::string_view detail = {}) {
  json body = {{"type", "https://www.jhipster.tech/problem/problem-with-message"},
               {"title", title},
               {"status", status},
               {"path", path},
               {"message", "error.http." + std::to_string(status)}};
  if (!detail.empty()) body["detail"] = detail;
  return answer(status, body);
}

// Legacy, wherever a write has no package the account may write to (README:
// "500 'Package is not writable by user.'"): no package given, a package that
// is somebody else's, a data point without the wrapper that carries it.
// assumed (20): the sentence is the `title` and nothing else is in the body.
HttpResponse not_writable() { return HttpResponse{500, R"({"title":"Package is not writable by user."})"}; }

HttpResponse unauthorized(std::string_view path, std::string_view detail) {
  return problem(401, "Unauthorized", path, detail);
}

// A foreign key the database refuses. Legacy's _constraint_field reads the
// column out of `Key (<column>)=` or out of `constraint "fk_..._<column>"`,
// and _humanize_error cuts the text at `;` and at `nested exception`: so the
// service passes on Spring's sentence with PostgreSQL's inside it.
// assumed (21): the status, 400 unless constraint_status says 500 (legacy
// looks for the word "constraint" before it looks at the status), and every
// word of the text beyond those marks.
HttpResponse constraint(int status, std::string_view path, std::string_view table, std::string_view field,
                        std::int64_t id, std::string_view referenced) {
  const std::string column = snake(field);
  const std::string key = "fk_" + std::string(table) + "_" + column;
  std::string detail = "could not execute statement; SQL [n/a]; constraint [" + key +
                       "]; nested exception is org.hibernate.exception.ConstraintViolationException: could not "
                       "execute statement: ERROR: insert or update on table \"" +
                       std::string(table) + "\" violates foreign key constraint \"" + key + "\"\n  Detail: Key (" +
                       column + ")=(" + std::to_string(id) + ") is not present in table \"" + std::string(referenced) +
                       "\".";
  return problem(status, status == 400 ? "Bad Request" : "Internal Server Error", path, detail);
}

// ---- Rows ------------------------------------------------------------------

std::string text_of(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_null()) return {};
  return value.dump();
}

// `sort=name,ASC` over the field of each row that `field_of` finds. A stable
// sort: rows that compare equal keep the order they are stored in.
// assumed (33): `sort` works on a lookup as on the package list, the one
// place legacy sends it.
template <class FieldOf>
void sort_rows(std::vector<json>& rows, const std::optional<std::string>& sort, FieldOf field_of) {
  if (!sort || sort->empty()) return;
  const std::size_t comma = sort->find(',');
  const std::string field = sort->substr(0, comma);
  const bool descending = comma != std::string::npos && lowered(sort->substr(comma + 1)) == "desc";
  std::stable_sort(rows.begin(), rows.end(), [&](const json& a, const json& b) {
    const json& x = field_of(a, field);
    const json& y = field_of(b, field);
    if (x.is_number() && y.is_number()) return descending ? y < x : x < y;
    return descending ? text_of(y) < text_of(x) : text_of(x) < text_of(y);
  });
}

// A key of a JSON body or of a query whose value is a credential.
bool is_credential_key(std::string_view key) {
  const std::string name = lowered(key);
  for (const std::string_view mark : {"password"sv, "token"sv, "secret"sv, "api_key"sv, "apikey"sv}) {
    if (name.find(mark) != std::string::npos) return true;
  }
  return false;
}

void redact(json& value) {
  if (value.is_object()) {
    for (auto& [key, inner] : value.items()) {
      if (is_credential_key(key)) {
        inner = kRedacted;
      } else {
        redact(inner);
      }
    }
  } else if (value.is_array()) {
    for (json& inner : value) redact(inner);
  }
}

bool is_credential_header(std::string_view name) {
  const std::string key = lowered(name);
  for (const std::string_view mark : {"auth"sv, "cookie"sv, "token"sv, "secret"sv, "password"sv, "key"sv}) {
    if (key.find(mark) != std::string::npos) return true;
  }
  return false;
}

json read_json_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("EarthBank fixture not found: " + path.string());
  std::stringstream text;
  text << in.rdbuf();
  json out = json::parse(text.str(), nullptr, false);
  if (out.is_discarded()) throw std::runtime_error("EarthBank fixture is not JSON: " + path.string());
  return out;
}

void require(bool ok, const std::filesystem::path& path, const char* what) {
  if (!ok) throw std::runtime_error("EarthBank fixture " + path.string() + ": " + what);
}

const json kNull;

}  // namespace

// ---- The log ---------------------------------------------------------------

std::optional<std::string> LoggedRequest::param(std::string_view key) const {
  for (const auto& [name, value] : query) {
    if (name == key) return value;
  }
  return std::nullopt;
}

std::ostream& operator<<(std::ostream& out, const LoggedRequest& request) {
  out << request.method << ' ' << request.path;
  char separator = '?';
  for (const auto& [name, value] : request.query) {
    out << separator << name << '=' << value;
    separator = '&';
  }
  switch (request.outcome) {
    case LoggedRequest::Outcome::Answered: out << " -> " << request.status; break;
    case LoggedRequest::Outcome::Dropped: out << " -> no answer (carried out: " << request.status << ")"; break;
    case LoggedRequest::Outcome::Lost: out << " -> no answer (not carried out)"; break;
    case LoggedRequest::Outcome::NotSent: out << " -> never arrived"; break;
  }
  for (const auto& [name, value] : request.headers) out << "\n    " << name << ": " << value;
  if (!request.body.empty()) {
    constexpr std::size_t kShown = 400;
    out << "\n    " << request.body.substr(0, kShown);
    if (request.body.size() > kShown) out << "... (" << request.body.size() << " bytes)";
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const PendingScript& script) {
  out << script.kind << ' ' << script.method << ' ' << script.path_prefix << '*';
  if (script.skip > 0) out << " after " << script.skip << " more";
  return out;
}

// ---- One request -----------------------------------------------------------

struct FakeEarthBank::Call {
  std::string method;
  std::string path;
  std::vector<std::pair<std::string, std::string>> query;
  const HttpRequest* request = nullptr;
  bool authenticated = false;

  std::optional<std::string> param(std::string_view key) const {
    for (const auto& [name, value] : query) {
      if (name == key) return value;
    }
    return std::nullopt;
  }

  // The rows of a list that the page asked for.
  json page(const std::vector<json>& rows) const {
    std::size_t size = kDefaultPageSize, number = 0;
    if (const auto asked = param("size")) {
      if (const auto n = parse_int(*asked); n && *n >= 0) size = static_cast<std::size_t>(*n);
    }
    if (const auto asked = param("page")) {
      if (const auto n = parse_int(*asked); n && *n >= 0) number = static_cast<std::size_t>(*n);
    }
    size = std::min(size, kMaxPageSize);
    json out = json::array();
    for (std::size_t i = number * size; i < rows.size() && out.size() < size; ++i) out.push_back(rows[i]);
    return out;
  }

  // The body as the JSON object every POST of the service takes; or what the
  // service says of one that is not.
  // assumed (4): a body with no JSON content type is 415 and one that is not a
  // JSON object is 400, as Spring answers a @RequestBody. Legacy sets the
  // content type on every POST and never saw either.
  std::optional<HttpResponse> read(json& into) const {
    const auto type = find_header(*request, "Content-Type");
    if (!type || lowered(*type).find("application/json") == std::string::npos) {
      return problem(415, "Unsupported Media Type", path);
    }
    into = json::parse(request->body, nullptr, false);
    if (into.is_discarded() || !into.is_object()) return problem(400, "Bad Request", path, "JSON parse error");
    return std::nullopt;
  }
};

// ---- Construction ----------------------------------------------------------

FakeEarthBank::FakeEarthBank() : FakeEarthBank(Options{}) {}

FakeEarthBank::FakeEarthBank(Options options) : options_(std::move(options)) {
  while (!options_.base_url.empty() && options_.base_url.back() == '/') options_.base_url.pop_back();
  const std::filesystem::path& dir = options_.fixtures;

  const std::filesystem::path account = dir / "account.json";
  account_ = read_json_file(account);
  require(account_.is_object() && account_.contains("login") && account_["login"].is_string(), account,
          "needs a `login`");
  require(id_of(account_, "id").has_value(), account, "needs an `id`");
  login_ = account_["login"].get<std::string>();

  const std::filesystem::path packages = dir / "packages.json";
  const json rows = read_json_file(packages);
  require(rows.is_array(), packages, "is to be the list the service gives");
  for (const json& row : rows) {
    require(row.is_object() && row.contains("dataPackageDTO") && id_of(row["dataPackageDTO"], "id").has_value() &&
                row["dataPackageDTO"].contains("name"),
            packages, "each row needs a `dataPackageDTO` with `id` and `name`");
    store_.packages.push_back(row);
  }

  // What no answer of the service says and the fake must know: who the
  // account is as the maker of a package, and which packages it may write to.
  const std::filesystem::path permissions = dir / "permissions.json";
  const json rights = read_json_file(permissions);
  require(rights.is_object() && id_of(rights, "createdById").has_value() && rights.contains("writablePackageIds") &&
              rights["writablePackageIds"].is_array(),
          permissions, "needs `createdById` and `writablePackageIds`");
  person_id_ = *id_of(rights, "createdById");
  for (const json& id : rights["writablePackageIds"]) {
    require(as_id(id).has_value() && find_package(*as_id(id)) != nullptr, permissions,
            "names a package that packages.json has not");
    writable_.insert(*as_id(id));
  }

  auto load = [&](std::string_view endpoint) {
    if (endpoint.empty() || lookups_.contains(endpoint)) return;
    const std::filesystem::path file = dir / "lookups" / (std::string(last_segment(endpoint)) + ".json");
    const json list = read_json_file(file);
    require(list.is_array(), file, "is to be the list the service gives");
    // assumed (12): a row is `id` and `name`, and a fixture's rows have no more.
    // assumed (13): the names are the vendor template's lists, to the letter.
    std::vector<json>& into = lookups_[std::string(endpoint)];
    for (const json& row : list) {
      require(row.is_object() && row.contains("id") && row["id"].is_number_integer() && row.contains("name") &&
                  row["name"].is_string(),
              file, "each row needs an `id` and a `name`");
      into.push_back(row);
    }
  };
  for (const LookupSpec& spec : lookup_specs()) load(spec.endpoint);
  load(kErrorTypes);
}

// ---- Arranging and asking --------------------------------------------------

std::string FakeEarthBank::password() const {
  const std::scoped_lock lock(mutex_);
  return options_.password;
}

std::string FakeEarthBank::issue_token() {
  const std::scoped_lock lock(mutex_);
  return new_token();
}

// The real token is a JWT (legacy's packages_probe decodes one).
// assumed (10): nothing a client needs is read out of it: this one has the
// three parts and no meaning. Numbered, so that a run gives the same again.
std::string FakeEarthBank::new_token() {
  std::string number = std::to_string(++tokens_issued_);
  number.insert(0, 4 - std::min<std::size_t>(4, number.size()), '0');
  std::string token = "fakeheader.fakeclaims" + number + ".fakesignature";
  tokens_.insert(token);
  return token;
}

std::optional<std::int64_t> FakeEarthBank::lookup_id(std::string_view endpoint, std::string_view name) const {
  const std::scoped_lock lock(mutex_);
  const auto list = lookups_.find(endpoint);
  if (list == lookups_.end()) return std::nullopt;
  for (const json& row : list->second) {
    if (row["name"].get_ref<const std::string&>() == name) return row["id"].get<std::int64_t>();
  }
  return std::nullopt;
}

std::int64_t FakeEarthBank::writable_package() const {
  const std::scoped_lock lock(mutex_);
  for (const json& row : store_.packages) {
    if (const auto id = id_of(row["dataPackageDTO"], "id"); id && writable_.contains(*id)) return *id;
  }
  throw std::runtime_error("the EarthBank fixtures have no package the account can write");
}

std::int64_t FakeEarthBank::read_only_package() const {
  const std::scoped_lock lock(mutex_);
  for (const json& row : store_.packages) {
    if (const auto id = id_of(row["dataPackageDTO"], "id"); id && !writable_.contains(*id)) return *id;
  }
  throw std::runtime_error("the EarthBank fixtures have no package the account cannot write");
}

bool FakeEarthBank::can_write(std::int64_t package_id) const {
  const std::scoped_lock lock(mutex_);
  return can_write_locked(package_id);
}

bool FakeEarthBank::can_write_locked(std::int64_t package_id) const {
  return writable_.contains(package_id) && find_package(package_id) != nullptr;
}

const json* FakeEarthBank::find_package(std::int64_t id) const {
  for (const json& row : store_.packages) {
    if (id_of(row["dataPackageDTO"], "id") == id) return &row;
  }
  return nullptr;
}

std::int64_t FakeEarthBank::add_package(std::string_view name, bool writable, std::int64_t institution_id) {
  const std::scoped_lock lock(mutex_);
  // The institution asked for, with its name if a package here has it; none
  // asked for: that of the first package.
  json institution = nullptr, institution_name = nullptr;
  for (const json& row : store_.packages) {
    const json& other = row["dataPackageDTO"];
    if (institution_id == 0 || id_of(other, "institutionId") == institution_id) {
      institution = other.value("institutionId", json());
      institution_name = other.value("institutionName", json());
      break;
    }
  }
  if (institution_id != 0) institution = institution_id;
  const std::int64_t id = next_package_++;
  json dto = {{"id", id},
              {"name", name},
              {"description", nullptr},
              {"distribution", "PRIVATE"},
              {"workflowState", "IN_PROGRESS"},
              {"licenseText", "Creative Commons"},
              {"institutionId", institution},
              {"institutionName", institution_name},
              {"createdById", person_id_ + 1}};  // somebody else
  store_.packages.push_back({{"dataPackageDTO", std::move(dto)}, {"countEditors", 0}, {"countSupervisors", 1}});
  if (writable) writable_.insert(id);
  return id;
}

void FakeEarthBank::set_writable(std::int64_t package_id, bool writable) {
  const std::scoped_lock lock(mutex_);
  if (writable) {
    writable_.insert(package_id);
  } else {
    writable_.erase(package_id);
  }
}

void FakeEarthBank::add_lookup_row(std::string_view endpoint, json row) {
  const std::scoped_lock lock(mutex_);
  const auto list = lookups_.find(endpoint);
  if (list == lookups_.end()) throw std::runtime_error("no such EarthBank vocabulary: " + std::string(endpoint));
  if (!row.is_object() || !row.contains("id") || !row["id"].is_number_integer() || !row.contains("name") ||
      !row["name"].is_string()) {
    throw std::runtime_error("a vocabulary row needs an `id` and a `name`");
  }
  list->second.push_back(std::move(row));
}

void FakeEarthBank::set_password(std::string password) {
  const std::scoped_lock lock(mutex_);
  options_.password = std::move(password);
  tokens_.clear();
}

void FakeEarthBank::expire_tokens() {
  const std::scoped_lock lock(mutex_);
  tokens_.clear();
}

void FakeEarthBank::ignore_sample_package_filter(bool ignore) {
  const std::scoped_lock lock(mutex_);
  ignore_sample_package_filter_ = ignore;
}

void FakeEarthBank::constraint_status(int status) {
  if (status != 400 && status != 500) throw std::invalid_argument("a constraint failure is 400 or 500");
  const std::scoped_lock lock(mutex_);
  constraint_status_ = status;
}

void FakeEarthBank::created_status(int status) {
  if (status != 200 && status != 201) throw std::invalid_argument("a record made is 200 or 201");
  const std::scoped_lock lock(mutex_);
  created_status_ = status;
}

void FakeEarthBank::sample_id_place(SampleIdPlace place) {
  const std::scoped_lock lock(mutex_);
  sample_id_place_ = place;
}

void FakeEarthBank::script(Scripted scripted) {
  if (scripted.error != ErrorKind::Timeout && scripted.error != ErrorKind::Io) {
    throw std::invalid_argument("an answer is lost as a Timeout or as an Io");
  }
  const std::scoped_lock lock(mutex_);
  scripted_.push_back(std::move(scripted));
}

void FakeEarthBank::fail_next(std::string_view method, std::string_view path_prefix, int status, std::string body,
                              std::size_t skip) {
  script({Scripted::Kind::Fail, std::string(method), std::string(path_prefix), skip, status, std::move(body),
          ErrorKind::Io});
}

void FakeEarthBank::drop_next(std::string_view method, std::string_view path_prefix, ErrorKind error,
                              std::size_t skip) {
  script({Scripted::Kind::Drop, std::string(method), std::string(path_prefix), skip, 0, {}, error});
}

void FakeEarthBank::lose_next(std::string_view method, std::string_view path_prefix, ErrorKind error,
                              std::size_t skip) {
  script({Scripted::Kind::Lose, std::string(method), std::string(path_prefix), skip, 0, {}, error});
}

void FakeEarthBank::refuse_next(std::string_view method, std::string_view path_prefix, std::size_t skip) {
  script({Scripted::Kind::Refuse, std::string(method), std::string(path_prefix), skip, 0, {}, ErrorKind::Io});
}

std::vector<PendingScript> FakeEarthBank::pending() const {
  const std::scoped_lock lock(mutex_);
  std::vector<PendingScript> out;
  for (const Scripted& one : scripted_) {
    const char* kind = one.kind == Scripted::Kind::Fail   ? "fail"
                       : one.kind == Scripted::Kind::Drop ? "drop"
                       : one.kind == Scripted::Kind::Lose ? "lose"
                                                          : "refuse";
    out.push_back({kind, one.method, one.path_prefix, one.skip});
  }
  return out;
}

std::vector<LoggedRequest> FakeEarthBank::requests() const {
  const std::scoped_lock lock(mutex_);
  return log_;
}

std::size_t FakeEarthBank::count(std::string_view method, std::string_view path_prefix) const {
  const std::scoped_lock lock(mutex_);
  return static_cast<std::size_t>(std::count_if(log_.begin(), log_.end(), [&](const LoggedRequest& one) {
    return one.method == method && starts_with(one.path, path_prefix);
  }));
}

Store FakeEarthBank::store() const {
  const std::scoped_lock lock(mutex_);
  Store out = store_;
  for (json& row : out.packages) row = package_row(row);
  return out;
}

// The script this request sets off, if any. Every script the request fits
// counts it: one that still has requests to let by lets this one by, and the
// oldest that has none left fires. So `skip` is "after that many requests
// that fit", whatever other scripts did with them.
std::optional<FakeEarthBank::Scripted> FakeEarthBank::take_scripted(std::string_view method, std::string_view path) {
  std::optional<Scripted> fired;
  for (auto it = scripted_.begin(); it != scripted_.end();) {
    const bool fits = it->method == method && starts_with(path, it->path_prefix);
    if (fits && it->skip > 0) {
      --it->skip;
      ++it;
    } else if (fits && !fired) {
      fired = std::move(*it);
      it = scripted_.erase(it);
    } else {
      ++it;
    }
  }
  return fired;
}

// ---- send ------------------------------------------------------------------

Result<HttpResponse> FakeEarthBank::send(const HttpRequest& request) {
  const std::scoped_lock lock(mutex_);

  // Another scheme, another host, no host: not this service, and nothing of
  // the request goes anywhere.
  const std::string_view url = request.url;
  const std::string_view base = options_.base_url;
  const std::string_view rest = starts_with(url, base) ? url.substr(base.size()) : std::string_view{};
  if (!starts_with(url, base) || (!rest.empty() && rest.front() != '/' && rest.front() != '?')) {
    Error error{ErrorKind::Io, "host not found: " + request_line(request) + " is not addressed to " +
                                   options_.base_url, ""};
    error.code = std::string(kHttpNotSent);
    return fail(std::move(error));
  }

  Call call;
  call.method = request.method;
  call.path = url_path(rest);
  call.request = &request;
  const std::size_t question = rest.find('?');
  if (question != std::string_view::npos) {
    call.query = parse_query(rest.substr(question + 1, rest.find('#', question) - question - 1));
  }
  if (const auto header = find_header(request, "Authorization"); header && starts_with(*header, "Bearer ")) {
    call.authenticated = tokens_.contains(std::string(header->substr(7)));
  }

  LoggedRequest logged;
  logged.method = call.method;
  logged.path = call.path;
  for (const auto& [name, value] : call.query) {
    logged.query.emplace_back(name, is_credential_key(name) ? std::string(kRedacted) : value);
  }
  logged.authenticated = call.authenticated;
  for (const auto& [name, value] : request.headers) {
    logged.headers.emplace_back(name, is_credential_header(name) ? std::string(kRedacted) : value);
  }
  if (json body = json::parse(request.body, nullptr, false); !body.is_discarded()) {
    redact(body);
    logged.body = body.dump();
  } else if (call.path == kAuthenticate) {
    logged.body = kRedacted;  // a password in some other form
  } else {
    logged.body = request.body;
  }

  const std::optional<Scripted> scripted = take_scripted(call.method, call.path);
  if (scripted && scripted->kind == Scripted::Kind::Refuse) {
    logged.outcome = LoggedRequest::Outcome::NotSent;
    log_.push_back(std::move(logged));
    Error error{ErrorKind::Io, "connection refused: " + request_line(request), ""};
    error.code = std::string(kHttpNotSent);
    return fail(std::move(error));
  }
  // What a client makes of an answer that does not come: the time ran out,
  // or the connection went. Neither says the request never left.
  auto no_answer = [&](ErrorKind kind) {
    return kind == ErrorKind::Timeout
               ? fail(ErrorKind::Timeout, "no answer to " + request_line(request) + " within " +
                                              std::to_string(request.timeout.count()) + " ms")
               : fail(ErrorKind::Io, "connection closed before the answer to " + request_line(request));
  };
  if (scripted && scripted->kind == Scripted::Kind::Lose) {
    logged.outcome = LoggedRequest::Outcome::Lost;
    log_.push_back(std::move(logged));
    return no_answer(scripted->error);
  }
  if (scripted && scripted->kind == Scripted::Kind::Fail) {
    logged.status = scripted->status;
    log_.push_back(std::move(logged));
    return HttpResponse{scripted->status, scripted->body};
  }

  // assumed (6): a request target with a character that had to be encoded is
  // 400 before anything looks at it (Tomcat's rule).
  HttpResponse response = is_url_text(rest) ? handle(call) : problem(400, "Bad Request", call.path);
  logged.status = response.status;
  if (scripted) {  // Drop: done, and the client never hears.
    logged.outcome = LoggedRequest::Outcome::Dropped;
    log_.push_back(std::move(logged));
    return no_answer(scripted->error);
  }
  log_.push_back(std::move(logged));
  return response;
}

HttpResponse FakeEarthBank::handle(const Call& call) {
  const std::string_view path = call.path;
  const bool get = call.method == "GET", post = call.method == "POST", del = call.method == "DELETE";
  // assumed (5): a path the service has, asked with a method it has not, is 405.
  auto not_allowed = [&] { return problem(405, "Method Not Allowed", path); };
  // assumed (8): the sentence of a 401. Legacy acts on the status alone (one new
  // login, then the request again).
  auto login_needed = [&] { return unauthorized(path, "Full authentication is required to access this resource"); };

  if (path == kAuthenticate) return post ? authenticate(call) : not_allowed();

  // Legacy's test_connection and get_account: the account of the token.
  // assumed (9): its fields (fixtures/account.json), beyond the `id` legacy reads.
  if (path == kAccount) {
    if (!get) return not_allowed();
    return call.authenticated ? answer(200, account_) : login_needed();
  }

  if (const auto list = lookups_.find(path); list != lookups_.end()) {
    if (!get) return not_allowed();
    // Legacy (_load_lookup): "ArAr lookups are public, but /api/core/L*
    // lookups need auth".
    // assumed (11): that is the `l-` lists under /api/core and not its other
    // lists (materials, archives, stratigraphic units, fundings, literature),
    // and a token that is no good is as no token.
    if (starts_with(path, "/api/core/l-") && !call.authenticated) return login_needed();
    return lookup(call, list->second);
  }

  // Legacy: packages are under /api/management, not /api/core.
  if (path == kPackages) {
    if (!get && !post) return not_allowed();
    if (!call.authenticated) return login_needed();
    return get ? list_packages(call) : create_package(call);
  }
  if (starts_with(path, kPackages) && path.size() > kPackages.size() && path[kPackages.size()] == '/') {
    const auto id = parse_int(path.substr(kPackages.size() + 1));
    if (!id) return problem(404, "Not Found", path);
    if (!get && !del) return not_allowed();
    if (!call.authenticated) return login_needed();
    return one_package(call, *id);
  }

  if (path == kSamples) {
    if (!get && !post) return not_allowed();
    if (!call.authenticated) return login_needed();
    return get ? find_samples(call) : create_sample(call);
  }

  if (path == kDataPoint) {
    if (!post) return not_allowed();
    if (!call.authenticated) return login_needed();
    return create_data_point(call);
  }
  struct SubRecord {
    std::string_view path, table;
    std::vector<Store::Record>* into;
    std::int64_t* next_id;
  };
  // assumed (21): the tables' names, after the one legacy names (ar_ar_data_point).
  for (const SubRecord& kind :
       {SubRecord{"/api/arar/ArArAliquot", "ar_ar_aliquot", &store_.aliquots, &next_aliquot_},
        SubRecord{"/api/arar/ArArMeasurement", "ar_ar_measurement", &store_.measurements, &next_measurement_},
        SubRecord{"/api/arar/ArArAgeCalc", "ar_ar_age_calc", &store_.age_calcs, &next_age_calc_},
        SubRecord{"/api/arar/ArArAgeSummary", "ar_ar_age_summary", &store_.age_summaries, &next_age_summary_}}) {
    if (path != kind.path) continue;
    if (!post) return not_allowed();
    if (!call.authenticated) return login_needed();
    return create_sub_record(call, kind.table, *kind.into, *kind.next_id);
  }

  // Legacy (_humanize_error): 404 is what a wrong address gives.
  return problem(404, "Not Found", path);
}

// ---- Session ---------------------------------------------------------------

// Legacy (_ensure_token): POST {username, password, rememberMe} gives
// {"id_token": ...}; anything but 2xx is a failed login.
// assumed (2): 200 for a login, and 201 for everything created unless
// created_status says 200 (legacy takes any 2xx).
// assumed (8): 401 for wrong credentials, and its sentence.
HttpResponse FakeEarthBank::authenticate(const Call& call) {
  json body;
  if (auto fault = call.read(body)) return *fault;
  const auto user = body.find("username");
  const auto pass = body.find("password");
  if (user == body.end() || pass == body.end() || !user->is_string() || !pass->is_string() ||
      user->get_ref<const std::string&>() != login_ || pass->get_ref<const std::string&>() != options_.password) {
    return unauthorized(call.path, "Bad credentials");
  }
  return answer(200, {{"id_token", new_token()}});
}

// ---- Lookups ---------------------------------------------------------------

// Legacy (_exact_lookup, _load_lookup): a list is a JSON array of rows with
// `id` and `name`; `name.equals` is exact and minds the case ("server is
// case-sensitive but exact"); `name.contains` finds a part; `size` and `page`
// cut the answer.
// assumed (14): `contains` does not mind the case (JHipster's criteria upper-case
// both sides; legacy filters the answer without case, which fits), and a
// parameter the list does not know is ignored.
HttpResponse FakeEarthBank::lookup(const Call& call, const std::vector<json>& rows) const {
  std::vector<json> out;
  const auto equals = call.param("name.equals");
  const auto contains = call.param("name.contains");
  for (const json& row : rows) {
    const auto& name = row["name"].get_ref<const std::string&>();
    if (equals && name != *equals) continue;
    if (contains && !contains_without_case(name, *contains)) continue;
    out.push_back(row);
  }
  sort_rows(out, call.param("sort"), [](const json& row, const std::string& field) -> const json& {
    const auto it = row.find(field);
    return it == row.end() ? kNull : *it;
  });
  return answer(200, call.page(out));
}

// ---- Packages --------------------------------------------------------------

// A package as it is listed, its counts taken from what is in it now.
// Legacy: "Rows come back wrapped as {"dataPackageDTO": {...}, counts}"; the
// counts it reads are countSamples and countArArDataPoints (live_upload_test)
// and it knows of countEditors and countSupervisors. Nothing in a row says
// whether the account may write (README, known limitations).
// assumed (17): a row is that and no more: no copy of `id` or `name` beside
// `dataPackageDTO`, and a field that is not set is null.
json FakeEarthBank::package_row(const json& row) const {
  json out = row;
  const auto id = id_of(row["dataPackageDTO"], "id");
  auto in_package = [&](const std::vector<Store::Record>& records) {
    return std::count_if(records.begin(), records.end(), [&](const Store::Record& r) { return r.package_id == id; });
  };
  out["countSamples"] = in_package(store_.samples);
  out["countArArDataPoints"] = in_package(store_.data_points);
  return out;
}

// Legacy (list_data_packages): page, size, `sort=name,ASC`,
// `institutionId.equals`, `name.contains`.
// assumed (16): every package is listed to every account, and `name.equals` works
// here as on a lookup.
HttpResponse FakeEarthBank::list_packages(const Call& call) const {
  std::vector<json> out;
  const auto institution = call.param("institutionId.equals");
  const auto equals = call.param("name.equals");
  const auto contains = call.param("name.contains");
  for (const json& row : store_.packages) {
    const json& dto = row["dataPackageDTO"];
    const std::string name = text_of(dto.value("name", json()));
    if (institution && text_of(dto.value("institutionId", json())) != *institution) continue;
    if (equals && name != *equals) continue;
    if (contains && !contains_without_case(name, *contains)) continue;
    out.push_back(package_row(row));
  }
  sort_rows(out, call.param("sort"), [](const json& row, const std::string& field) -> const json& {
    const json& dto = row["dataPackageDTO"];
    const auto it = dto.find(field);
    return it == dto.end() ? kNull : *it;
  });
  return answer(200, call.page(out));
}

// Legacy (create_data_package): the fields go under `dataPackageDTO`. "A flat
// body is accepted but SILENTLY IGNORES the name and creates a default-named
// package." The maker becomes the package's supervisor, so it is writable;
// and `createdById` is not the `id` of /api/account (README).
// assumed (18): a flat body loses every field, not the name alone; the
// default name; the answer is the row as it is listed; two packages may have
// one name.
HttpResponse FakeEarthBank::create_package(const Call& call) {
  json body;
  if (auto fault = call.read(body)) return *fault;
  const auto nested = body.find("dataPackageDTO");
  const json given = nested != body.end() && nested->is_object() ? *nested : json::object();

  json dto = {{"id", next_package_++},
              {"name", "Unnamed data package"},
              {"description", nullptr},
              {"distribution", nullptr},
              {"workflowState", nullptr},
              {"licenseText", nullptr},
              {"institutionId", nullptr},
              {"institutionName", nullptr},
              {"createdById", person_id_}};
  for (const char* field : {"name", "description", "distribution", "workflowState", "licenseText", "institutionId"}) {
    if (is_set(given, field)) dto[field] = given[field];
  }
  for (const json& row : store_.packages) {
    const json& other = row["dataPackageDTO"];
    if (is_set(dto, "institutionId") && other.value("institutionId", json()) == dto["institutionId"]) {
      dto["institutionName"] = other.value("institutionName", json());
    }
  }
  writable_.insert(dto["id"].get<std::int64_t>());
  store_.packages.push_back({{"dataPackageDTO", std::move(dto)}, {"countEditors", 0}, {"countSupervisors", 1}});
  return answer(created_status_, package_row(store_.packages.back()));
}

// Legacy reads one package at `<list>/<id>` (packages_probe, live_upload_test)
// and its README lists DELETE.
// assumed (19): 404 for an id that is not there; DELETE answers 204, takes
// what is in the package with it, and is refused as any write to a package of
// somebody else's.
// assumed (35): DELETE is of `<list>/<id>`, as the GET of one package is
// (legacy's README gives the verb for the list's path and no more).
HttpResponse FakeEarthBank::one_package(const Call& call, std::int64_t id) {
  const json* row = find_package(id);
  if (row == nullptr) return problem(404, "Not Found", call.path);
  if (call.method == "GET") return answer(200, package_row(*row));
  if (!can_write_locked(id)) return not_writable();

  auto drop = [&](std::vector<Store::Record>& records) {
    std::erase_if(records, [&](const Store::Record& r) { return r.package_id == id; });
  };
  for (auto* records : {&store_.samples, &store_.data_points, &store_.aliquots, &store_.measurements,
                        &store_.age_calcs, &store_.age_summaries}) {
    drop(*records);
  }
  std::erase_if(store_.packages, [&](const json& r) { return id_of(r["dataPackageDTO"], "id") == id; });
  writable_.erase(id);
  return HttpResponse{204, {}};
}

// ---- Vocabulary ------------------------------------------------------------

// What the service makes of the controlled terms of one record.
//   - A `*Id` of a term that has a list must be a row of that list: the lists
//     are the fixtures', so a pre-flight and an upload agree on what is known.
//   - A sigma level (`*UncertaintyTypeId`) is 1 or 2: legacy's
//     UNCERTAINTY_TYPE_VOCAB, "stable small ids" of a table with no endpoint.
//   - A term's name with no id beside it is refused: legacy drops a name it
//     could not resolve "so the server doesn't 400 on the row" (README).
// assumed (22): no other id of a sigma level is good (the table has more
// rows than legacy knew).
// assumed (23): the body of that 400; that a name beside its id is ignored;
// and that the names with no list (the yes/no steps, the sigma levels,
// `lithologyName`) are taken as they come.
std::optional<HttpResponse> FakeEarthBank::vocabulary_fault(std::string_view path, const json& dto,
                                                            std::string_view table) const {
  for (const LookupSpec& spec : lookup_specs()) {
    if (spec.endpoint.empty()) continue;
    if (is_set(dto, spec.id_field)) {
      const auto id = as_id(dto[std::string(spec.id_field)]);
      if (!id) return problem(400, "Bad Request", path, "JSON parse error: " + std::string(spec.id_field));
      const std::vector<json>& rows = lookups_.find(spec.endpoint)->second;
      const bool known = std::any_of(rows.begin(), rows.end(), [&](const json& row) { return row["id"] == *id; });
      if (!known) {
        return constraint(constraint_status_, path, table, spec.id_field, *id,
                          snake(last_segment(spec.endpoint)));
      }
    } else if (is_set(dto, spec.name_field) && !text_of(dto[std::string(spec.name_field)]).empty()) {
      return problem(400, "Bad Request", path,
                     std::string(spec.name_field) + " has no " + std::string(spec.id_field));
    }
  }
  for (const LookupSpec& spec : uncertainty_type_specs()) {
    if (!is_set(dto, spec.id_field)) continue;
    const auto id = as_id(dto[std::string(spec.id_field)]);
    if (!id) return problem(400, "Bad Request", path, "JSON parse error: " + std::string(spec.id_field));
    if (*id != 1 && *id != 2) return constraint(constraint_status_, path, table, spec.id_field, *id, "l_uncertainty");
  }
  return std::nullopt;
}

// ---- Samples ---------------------------------------------------------------

// Legacy (find_sample_by_name): `name.equals`, `size`, and
// `dataPackageId.equals`; rows are `{"sampleDTO": {id, name, dataPackageId,
// ...}}`. Without the package filter the search is of every package, which is
// how a sample of another package came to be reused. Legacy does not trust
// the filter: it checks each row's `sampleDTO.dataPackageId` itself.
// assumed (30): the service does keep to the filter; with
// ignore_sample_package_filter it does not, and rows of every package come.
HttpResponse FakeEarthBank::find_samples(const Call& call) const {
  std::vector<json> out;
  const auto name = call.param("name.equals");
  const auto package = call.param("dataPackageId.equals");
  for (const Store::Record& record : store_.samples) {
    const json& dto = record.body["sampleDTO"];
    if (name && text_of(dto.value("name", json())) != *name) continue;
    if (package && !ignore_sample_package_filter_ && std::to_string(record.package_id) != *package) continue;
    out.push_back(record.body);
  }
  return answer(200, call.page(out));
}

// Legacy (create_sample): the body is `{sampleDTO, locationDTO, dataPackageId,
// shortName}`; without a package the account may write it is "not writable".
// The package "is stamped on both the wrapper and the inner sampleDTO since
// the accepting field is not documented", and the new id is read from
// `sampleDTO.id` of the answer (_extract_sample_id).
// assumed (24): which of the two the service reads is not known, so only
// what legacy sent is taken: both, and the same.
// assumed (25): the answer is the body as it was sent with the ids in
// `sampleDTO` (`id`, `locationId`) and `locationDTO` (`id`), none on the
// wrapper, unless sample_id_place puts the sample's elsewhere; an empty
// `locationDTO` makes no location; a second sample of a name in a package is
// a second sample.
// assumed (34): nothing in `locationDTO` is checked against a list.
HttpResponse FakeEarthBank::create_sample(const Call& call) {
  json body;
  if (auto fault = call.read(body)) return *fault;
  const auto dto = body.find("sampleDTO");
  const auto outer = id_of(body, "dataPackageId");
  const auto inner = dto != body.end() ? id_of(*dto, "dataPackageId") : std::nullopt;
  if (!outer || !inner || *outer != *inner || !can_write_locked(*outer)) return not_writable();
  if (auto fault = vocabulary_fault(call.path, *dto, "sample")) return *fault;

  const std::int64_t id = next_sample_++;
  body["sampleDTO"]["id"] = id;
  if (auto location = body.find("locationDTO");
      location != body.end() && location->is_object() && !location->empty()) {
    (*location)["id"] = next_location_++;
    body["sampleDTO"]["locationId"] = (*location)["id"];
  }
  store_.samples.push_back({id, *outer, body});
  // Legacy's _extract_sample_id looks under `sampleDTO`, then under `sample`.
  if (sample_id_place_ == SampleIdPlace::Sample) {
    body["sample"] = body["sampleDTO"];
    body.erase("sampleDTO");
  } else if (sample_id_place_ == SampleIdPlace::Top) {
    body["sampleDTO"].erase("id");
    body["id"] = id;
  }
  return answer(created_status_, body);
}

// ---- The data point --------------------------------------------------------

// Legacy (create_data_point, README, arar_post_probe): the body is
// `{dataPointDTO: {dataStructure: "ARARDATAPOINT", dataPackageId, sampleId,
// name}, extendingDataPointDTO: {the Ar-Ar fields}}`, with `arArSetUpName` and
// `arArIrradiationName` beside them. The Ar-Ar fields alone are "not
// writable" wherever the package is put among them or in the query: the
// package is read from the umbrella. The `id` on top of the answer is the
// arArDataPointId. A sample of another package is linked without complaint.
// assumed (26): the rest of the answer (the body as sent, the umbrella with
// its own `id` and `arArDataPointId`, the Ar-Ar record with the id on top).
// assumed (27): a `sampleId` that is no sample is a constraint error and
// none at all is accepted; another `dataStructure` is 400.
// assumed (28): the two names beside the records are kept and not checked
// (legacy could send them and its upload never did).
// assumed (31): a second data point of the same name for the same sample is
// a second data point.
HttpResponse FakeEarthBank::create_data_point(const Call& call) {
  json body;
  if (auto fault = call.read(body)) return *fault;
  const auto core = body.find("dataPointDTO");
  if (core == body.end() || !core->is_object()) return not_writable();
  const auto package = id_of(*core, "dataPackageId");
  if (!package || !can_write_locked(*package)) return not_writable();
  if (text_of(core->value("dataStructure", json())) != "ARARDATAPOINT") {
    return problem(400, "Bad Request", call.path, "dataStructure");
  }
  if (is_set(*core, "sampleId")) {
    const auto sample = id_of(*core, "sampleId");
    const bool known = sample && std::any_of(store_.samples.begin(), store_.samples.end(),
                                             [&](const Store::Record& r) { return r.id == *sample; });
    if (!known) return constraint(constraint_status_, call.path, "data_point", "sampleId", sample.value_or(0), "sample");
  }
  const auto extending = body.find("extendingDataPointDTO");
  if (extending == body.end() || !extending->is_object()) {
    return problem(400, "Bad Request", call.path, "extendingDataPointDTO");
  }
  if (auto fault = vocabulary_fault(call.path, *extending, "ar_ar_data_point")) return *fault;

  const std::int64_t id = next_data_point_++;
  body["id"] = id;
  body["dataPointDTO"]["id"] = next_core_data_point_++;
  body["dataPointDTO"]["arArDataPointId"] = id;
  body["extendingDataPointDTO"]["id"] = id;
  store_.data_points.push_back({id, *package, body});
  return answer(created_status_, body);
}

// Legacy (create_aliquot and the like, _post_json): a flat record with the
// `arArDataPointId` it hangs from; the answer's `id` is the new record's.
// There is no update, and a re-run of legacy's upload posts every one again.
// assumed (29): the answer is the record as sent with its `id`; a record
// posted twice is there twice; one whose data point is missing or unknown has
// no package and is "not writable", as a data point without its umbrella is;
// a measurement's `aliquotName` need not be an aliquot's.
// assumed (32): no field of any record is required and none but an id is
// held to a type; a field the record does not have is kept and does no harm.
HttpResponse FakeEarthBank::create_sub_record(const Call& call, std::string_view table,
                                              std::vector<Store::Record>& into, std::int64_t& next_id) {
  json body;
  if (auto fault = call.read(body)) return *fault;
  const auto point = id_of(body, "arArDataPointId");
  const auto parent = std::find_if(store_.data_points.begin(), store_.data_points.end(),
                                   [&](const Store::Record& r) { return point && r.id == *point; });
  if (parent == store_.data_points.end() || !can_write_locked(parent->package_id)) return not_writable();
  if (auto fault = vocabulary_fault(call.path, body, table)) return *fault;

  const std::int64_t id = next_id++;
  body["id"] = id;
  into.push_back({id, parent->package_id, body});
  return answer(created_status_, body);
}

}  // namespace pychron::earthbank::testing
