#include "pychron/earthbank/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/vocab.hpp"

namespace pychron::earthbank {
namespace {

using nlohmann::json;

constexpr std::string_view kAuthenticate = "/api/authenticate";
constexpr std::string_view kAccount = "/api/account";
constexpr std::string_view kPackages = "/api/management/data-packages";
constexpr std::string_view kRedacted = "<redacted>";
// Legacy asks for 500 packages and takes what comes; here every page is read.
constexpr std::size_t kPackagePage = 500;
constexpr std::size_t kMostPackagePages = 200;
// The most of the service's own words that go into a sentence or a log line.
constexpr std::size_t kMostReason = 300;

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

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

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

bool is_success(int status) { return status >= 200 && status < 300; }

// Overwrites a secret before it is let go. The write is through a volatile
// pointer so that it is not optimised away with the string.
void wipe(std::string& secret) {
  volatile char* p = secret.data();
  for (std::size_t i = 0; i < secret.size(); ++i) p[i] = '\0';
  secret.clear();
}

// `secret` as it may stand in what a service says back: as it is, as it is
// written inside a JSON string, and as it is written in a URL.
void add_forms(std::vector<std::string>& out, std::string_view secret) {
  if (secret.empty()) return;
  out.emplace_back(secret);
  std::string quoted = json(secret).dump(-1, ' ', false, json::error_handler_t::replace);
  if (quoted.size() >= 2) quoted = quoted.substr(1, quoted.size() - 2);
  if (quoted != secret) out.push_back(std::move(quoted));
  if (std::string encoded = url_encode(secret); encoded != secret) out.push_back(std::move(encoded));
}

// `text` with every occurrence of each of `secrets` replaced by the mark.
// The places are found in the text as it came and then replaced together, so
// that a secret which is a part of the mark, or of another secret, is not
// found again in what was put in.
std::string redacted(std::string_view text, const std::vector<std::string>& secrets) {
  std::vector<bool> secret_at(text.size(), false);
  for (const std::string& secret : secrets) {
    for (std::size_t at = text.find(secret); at != std::string_view::npos; at = text.find(secret, at + 1)) {
      std::fill_n(secret_at.begin() + static_cast<std::ptrdiff_t>(at), secret.size(), true);
    }
  }
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (!secret_at[i]) {
      out.push_back(text[i]);
    } else if (i == 0 || !secret_at[i - 1]) {
      out += kRedacted;
    }
  }
  return out;
}

// What the service said of a failure: the body's `detail`, else `message`,
// else `title` (legacy's order), else the body itself when it is not JSON.
std::string reason_of(std::string_view body) {
  const json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded()) return std::string(trimmed(body));
  if (!parsed.is_object()) return {};
  for (const char* key : {"detail", "message", "title"}) {
    const auto it = parsed.find(key);
    if (it == parsed.end() || !it->is_string()) continue;
    if (const std::string_view text = trimmed(it->get_ref<const std::string&>()); !text.empty()) return std::string(text);
  }
  return {};
}

// A reason as it may stand in a sentence: cut where the machine's part
// begins (`;`, `nested exception`: legacy), on one line, and not endless.
std::string short_reason(std::string_view reason) {
  reason = reason.substr(0, reason.find(';'));
  reason = reason.substr(0, reason.find("nested exception"));
  std::string out(trimmed(reason));
  for (char& c : out) {
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  }
  if (out.size() > kMostReason) {
    std::size_t cut = kMostReason;
    // Not in the middle of a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
    out += "...";
  }
  return out;
}

bool is_column(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

// The field a constraint failure is about, as words: out of PostgreSQL's
// `Key (mineral_id)=(5)`, else out of the constraint's name,
// `fk_<table>_<column>_id`, the table taken to be its first word (legacy's
// two patterns). Empty when the body gives neither.
std::string constraint_field(std::string_view reason) {
  std::string column;
  constexpr std::string_view kKey = "Key (";
  if (const std::size_t key = reason.find(kKey); key != std::string_view::npos) {
    const std::size_t from = key + kKey.size();
    const std::size_t close = reason.find(')', from);
    if (close != std::string_view::npos && is_column(reason.substr(from, close - from))) {
      column = reason.substr(from, close - from);
      if (column.ends_with("_id")) column.resize(column.size() - 3);
    }
  }
  if (column.empty()) {
    constexpr std::string_view kNamed = "constraint \"fk_";
    if (const std::size_t named = reason.find(kNamed); named != std::string_view::npos) {
      std::string_view rest = reason.substr(named + kNamed.size());
      rest = rest.substr(0, rest.find('"'));
      const std::size_t table = rest.find('_');
      const std::size_t id = table == std::string_view::npos ? table : rest.find("_id", table + 1);
      if (id != std::string_view::npos && is_column(rest.substr(table + 1, id - table - 1))) {
        column = rest.substr(table + 1, id - table - 1);
      }
    }
  }
  std::replace(column.begin(), column.end(), '_', ' ');
  return std::string(trimmed(column));
}

// What a path's record is called when the caller has no better word.
std::string_view kind_of(std::string_view path) {
  constexpr std::pair<std::string_view, std::string_view> kRecords[] = {
      {"sample-with-locations", "sample"}, {"ArArDataPoint", "data point"},
      {"ArArAliquot", "aliquot"},          {"ArArMeasurement", "measurement"},
      {"ArArAgeCalc", "age calculation"},  {"ArArAgeSummary", "age summary"},
      {"data-packages", "data package"},
  };
  for (const auto& [part, name] : kRecords) {
    if (has(path, part)) return name;
  }
  // By its end: a base URL may have a path of its own before `/api`.
  if (path.ends_with(kAuthenticate)) return "login";
  if (path.ends_with(kAccount)) return "account";
  if (has(path, "/api/core/l-")) return "list of terms";
  for (const LookupSpec& spec : lookup_specs()) {
    if (!spec.endpoint.empty() && path.ends_with(spec.endpoint)) return "list of terms";
  }
  return "request";
}

std::string what_of(std::string_view path, std::string_view record) {
  return record.empty() ? std::string(kind_of(path)) : std::string(record);
}

// An id as the service writes one: a number, or (it has been seen to) a
// string of digits.
std::optional<std::int64_t> id_of(const json& object, const char* key) {
  if (!object.is_object()) return std::nullopt;
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  if (it->is_number_integer()) return it->get<std::int64_t>();
  if (it->is_string()) {
    const std::string& text = it->get_ref<const std::string&>();
    if (!text.empty() && text.size() <= 18 && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return std::stoll(text);
    }
  }
  return std::nullopt;
}

std::string text_of(const json& object, const char* key) {
  if (!object.is_object()) return {};
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A row of the package list, or the answer to a new package: the fields under
// `dataPackageDTO`, and where that is missing on the row itself (legacy reads
// both). No id: no package.
std::optional<DataPackage> package_of(const json& row) {
  if (!row.is_object()) return std::nullopt;
  const auto nested = row.find("dataPackageDTO");
  const json& dto = nested != row.end() && nested->is_object() ? *nested : row;
  DataPackage out;
  std::optional<std::int64_t> id = id_of(dto, "id");
  if (!id) id = id_of(row, "id");
  if (!id) return std::nullopt;
  out.id = *id;
  out.name = text_of(dto, "name");
  if (out.name.empty()) out.name = text_of(row, "name");
  out.institution_name = text_of(dto, "institutionName");
  out.workflow_state = text_of(dto, "workflowState");
  out.institution_id = id_of(dto, "institutionId").value_or(0);
  return out;
}

HttpRequest json_post(std::string url, const json& body) {
  HttpRequest request;
  request.method = "POST";
  request.url = std::move(url);
  request.headers.emplace_back("Content-Type", "application/json");
  request.body = body.dump();
  return request;
}

HttpRequest plain_get(std::string url) {
  HttpRequest request;
  request.method = "GET";
  request.url = std::move(url);
  return request;
}

// The sentence for an answer that is not 2xx. `reason` is what the service
// said (reason_of), the session's secrets already out of it: nothing else in
// the sentence is the service's.
std::string answer_sentence(int status, const std::string& reason, std::string_view path, std::string_view record) {
  if (is_success(status)) return {};
  const std::string where = url_path(path);
  const std::string what = what_of(where, record);
  const std::string low = lowered(reason);

  if (has(low, "not writable")) {
    return "EarthBank did not take the " + what +
           ": this account may not write to the data package. Choose a package the account may write to, or make a "
           "new one, and send it again.";
  }
  // Before the status: the service has been seen to send it as a 400 and may
  // as well send a 500.
  if (has(low, "constraint") || has(low, "not present in table") || has(low, "foreign key")) {
    const std::string field = constraint_field(reason);
    return "EarthBank did not take the " + what + ": it does not know " +
           (field.empty() ? std::string("one of the values sent") : "the value sent for '" + field + "'") +
           ". The term is probably not on EarthBank's list; a pre-flight check names such terms before anything is "
           "sent.";
  }
  if (status == 401 || status == 403) {
    if (where.ends_with(kAuthenticate)) return "EarthBank did not accept the user name and password.";
    return "EarthBank refused the login (" + what +
           "). Check the user name and the password, and that the account has the right to do this.";
  }
  if (status == 404) {
    return "EarthBank has nothing at " + where + ": the address is probably wrong. Check the base URL of the profile.";
  }
  // Legacy: a 500 that speaks of the package is the package's.
  if (status == 500 && has(low, "package")) {
    return "EarthBank did not take the " + what +
           ": this account may not write to the data package. Choose a package the account may write to, or make a "
           "new one, and send it again.";
  }
  if (status >= 500) {
    return "EarthBank failed on its own side at the " + what + " (HTTP " + std::to_string(status) +
           "). Try again later; if it goes on, tell EarthBank's support.";
  }
  if (status >= 300 && status < 400) {
    return "EarthBank answered the " + what + " with a redirect (HTTP " + std::to_string(status) +
           "): the base URL of the profile is probably not the service's own address.";
  }
  if (const std::string said = short_reason(reason); !said.empty()) return "EarthBank refused the " + what + ": " + said;
  return "EarthBank refused the " + what + " (HTTP " + std::to_string(status) + ").";
}

}  // namespace

Session::Session(IHttpClient& http, std::string base_url) : http_(http), base_url_(std::move(base_url)) {
  while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

Session::~Session() { sign_out(); }

std::string Session::url(std::string_view path) const {
  std::string out = base_url_;
  if (!path.starts_with('/')) out.push_back('/');
  out.append(path);
  return out;
}

void Session::sign_out() {
  wipe(token_);
  for (std::string& token : spent_tokens_) wipe(token);
  spent_tokens_.clear();
  wipe(password_);
  username_.clear();
}

// What the service said of a failure, with nothing of this session's in it:
// its tokens, its password, and `trying`, a password on its way in. Scrubbed
// whole, before any of it is cut or read: a cut could leave the first part
// of a secret standing.
std::string Session::said_in(std::string_view body, std::string_view trying) const {
  std::vector<std::string> secrets;
  add_forms(secrets, token_);
  for (const std::string& token : spent_tokens_) add_forms(secrets, token);
  add_forms(secrets, password_);
  add_forms(secrets, trying);
  std::string out = redacted(reason_of(body), secrets);
  for (std::string& secret : secrets) wipe(secret);
  return out;
}

// One request on the wire, and its line in the log.
Result<HttpResponse> Session::exchange(const HttpRequest& request, std::string_view trying) {
  Result<HttpResponse> answer = http_.send(request);
  if (!log_) return answer;
  std::string line = request_line(request);
  if (!answer) {
    line += answer.error().code == kHttpNotSent ? ": not sent (" : ": no answer (";
    line += to_string(answer.error().kind);
    line += ")";
  } else {
    line += " -> " + std::to_string(answer->status);
    if (!is_success(answer->status)) {
      if (const std::string reason = short_reason(said_in(answer->body, trying)); !reason.empty()) line += ": " + reason;
    }
  }
  log_(line);
  return answer;
}

Error Session::answer_error(const HttpResponse& answer, std::string_view path, std::string_view record,
                            std::string_view trying) const {
  const bool login = answer.status == 401 || answer.status == 403;
  Error error{login ? ErrorKind::NotConnected : ErrorKind::Protocol,
              answer_sentence(answer.status, said_in(answer.body, trying), path, record), ""};
  error.code = std::string(login ? kLoginRejected : kRefused);
  return error;
}

Error Session::failure(const HttpResponse& answer, std::string_view path, std::string_view record) const {
  return answer_error(answer, path, record, {});
}

// The client's error with the sentence for `what`: its kind and code stay.
// The sentence is of our own words and the caller's, and is not scrubbed.
Error Session::failure(Error error, std::string_view path, std::string_view record, bool write) const {
  error.what = sentence(error, path, record, write);
  return error;
}

Result<void> Session::authenticate(std::string_view username, std::string_view password) {
  // Legacy's body. The password is in it and nowhere else.
  HttpRequest request = json_post(url(kAuthenticate), {{"username", username}, {"password", password}, {"rememberMe", false}});
  Result<HttpResponse> answer = exchange(request, password);
  wipe(request.body);
  if (!answer) return fail(failure(std::move(answer.error()), kAuthenticate, "", false));
  if (!is_success(answer->status)) return fail(answer_error(*answer, kAuthenticate, "", password));
  const json body = json::parse(answer->body, nullptr, false);
  std::string token = text_of(body, "id_token");
  wipe(answer->body);
  if (token.empty()) return fail(ErrorKind::Protocol, "EarthBank answered the login without a token.");
  // The token before this one may still be said back by the service in the
  // answer to a request that carried it: it is kept, to be kept out of what
  // is said, until the session ends.
  if (!token_.empty()) spent_tokens_.push_back(std::move(token_));
  token_ = std::move(token);
  return {};
}

Result<void> Session::sign_in(std::string_view username, std::string_view password) {
  sign_out();
  if (trimmed(username).empty() || password.empty()) {
    return fail(ErrorKind::Config, "earthbank: a login needs a user name and a password");
  }
  if (auto in = authenticate(username, password); !in) return in;
  username_ = username;
  password_ = password;
  return {};
}

Result<HttpResponse> Session::send(HttpRequest request, bool needs_login) {
  if (!signed_in()) {
    if (needs_login) {
      Error error{ErrorKind::NotConnected, {}, ""};
      error.code = std::string(kNotSignedIn);
      error.what = sentence(error, request.url, "", false);
      return fail(std::move(error));
    }
    return exchange(request);
  }
  const auto with_bearer = [this](HttpRequest& out) {
    std::erase_if(out.headers, [](const auto& header) {
      return lowered(header.first) == "authorization";
    });
    out.headers.emplace_back("Authorization", "Bearer " + token_);
  };
  with_bearer(request);
  Result<HttpResponse> answer = exchange(request);
  if (!answer || answer->status != 401) return answer;

  // The token is no longer good. One new sign-in, and the request once more;
  // when the sign-in fails, the 401 stands.
  const std::string username = username_;
  std::string password = password_;
  const Result<void> again = authenticate(username, password);
  wipe(password);
  if (!again) {
    if (again.error().code == kLoginRejected) sign_out();
    return answer;
  }
  with_bearer(request);
  return exchange(request);
}

Result<Account> Session::account() {
  Result<HttpResponse> answer = send(plain_get(url(kAccount)), true);
  if (!answer) return fail(failure(std::move(answer.error()), kAccount, "", false));
  if (!is_success(answer->status)) return fail(failure(*answer, kAccount, ""));
  const json body = json::parse(answer->body, nullptr, false);
  const std::optional<std::int64_t> id = id_of(body, "id");
  if (!id) return fail(ErrorKind::Protocol, "EarthBank's answer about the account could not be read.");
  Account out;
  out.id = *id;
  out.login = text_of(body, "login");
  out.name = std::string(trimmed(text_of(body, "firstName") + " " + text_of(body, "lastName")));
  if (out.name.empty()) out.name = out.login;
  return out;
}

Result<std::vector<DataPackage>> Session::packages(std::optional<std::int64_t> institution_id) {
  std::vector<DataPackage> out;
  for (std::size_t page = 0; page < kMostPackagePages; ++page) {
    std::string address = with_query(
        url(kPackages), {{"page", std::to_string(page)}, {"size", std::to_string(kPackagePage)}, {"sort", "name,ASC"}});
    if (institution_id) address = with_query(address, {{"institutionId.equals", std::to_string(*institution_id)}});
    Result<HttpResponse> answer = send(plain_get(std::move(address)), true);
    if (!answer) return fail(failure(std::move(answer.error()), kPackages, "", false));
    if (!is_success(answer->status)) return fail(failure(*answer, kPackages, ""));
    const json rows = json::parse(answer->body, nullptr, false);
    if (!rows.is_array()) return fail(ErrorKind::Protocol, "EarthBank's list of data packages could not be read.");
    for (const json& row : rows) {
      if (std::optional<DataPackage> package = package_of(row)) out.push_back(std::move(*package));
    }
    if (rows.size() < kPackagePage) break;
  }
  return out;
}

Result<DataPackage> Session::create_package(std::string_view name, std::optional<std::int64_t> institution_id) {
  name = trimmed(name);
  if (name.empty()) return fail(ErrorKind::Config, "earthbank: a data package needs a name");
  const std::string record = "data package '" + std::string(name) + "'";

  // Legacy's fields and defaults: private, in progress.
  json dto = {{"name", name},
              {"distribution", "PRIVATE"},
              {"workflowState", "IN_PROGRESS"},
              {"licenseText", "Creative Commons"}};
  if (institution_id) dto["institutionId"] = *institution_id;
  // Nested, and mirrored beside the nest: a flat body is accepted and its
  // name silently dropped.
  json body = dto;
  body["dataPackageDTO"] = std::move(dto);

  Result<HttpResponse> answer = send(json_post(url(kPackages), body), true);
  if (!answer) return fail(failure(std::move(answer.error()), kPackages, record, true));
  if (!is_success(answer->status)) return fail(failure(*answer, kPackages, record));
  std::optional<DataPackage> made = package_of(json::parse(answer->body, nullptr, false));
  if (!made) {
    return fail(ErrorKind::Protocol, "EarthBank answered, but named no id for the " + record +
                                         ": it may have been made. Look for it on EarthBank before making another.");
  }
  return std::move(*made);
}

// ---- Sentences ---------------------------------------------------------------

std::string Session::sentence(const Error& error, std::string_view path, std::string_view record, bool write) {
  // An answer already has its sentence.
  if (error.code == kLoginRejected || error.code == kRefused) return error.what;
  const std::string what = what_of(url_path(path), record);
  if (error.code == kNotSignedIn) return "Not signed in to EarthBank: the " + what + " was not sent.";
  if (error.code == kHttpNotSent) {
    return "EarthBank could not be reached, and the " + what +
           " was not sent. Check the network and the base URL of the profile.";
  }
  if (error.kind == ErrorKind::Config) {
    return "The request for the " + what +
           " could not be made, and nothing was sent: the base URL of the profile is probably not a web address.";
  }
  if (write) {
    return "EarthBank did not answer; the " + what +
           " may or may not have been saved. Look for it on EarthBank before it is sent again.";
  }
  return "EarthBank did not answer (" + what + "). Check the network and try again.";
}

std::string Session::no_package_sentence() {
  return "No data package is chosen, and EarthBank keeps every record in one. Choose a package, or make one, before "
         "anything is sent.";
}

std::string Session::partial_sentence(std::string_view sample, std::span<const std::string> missing) {
  std::string out(sample);
  out += " is only partly on EarthBank: its data point was saved and ";
  if (missing.empty()) {
    out += "some records under it were not";
  } else {
    out += std::to_string(missing.size()) + (missing.size() == 1 ? " record under it was not (" : " records under it were not (");
    for (std::size_t i = 0; i < missing.size(); ++i) out += (i == 0 ? "" : ", ") + missing[i];
    out += ")";
  }
  out += ". Send it again to add what is missing.";
  return out;
}

}  // namespace pychron::earthbank
