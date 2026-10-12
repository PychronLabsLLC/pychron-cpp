#include "pychron/earthbank/session.hpp"

#include <algorithm>
#include <array>
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
#include "text_detail.hpp"

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

using detail::one_line;
using detail::trimmed;

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

// A URL-encoded text with the hex digits of its escapes in lower case.
std::string lower_hex_escapes(std::string text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') continue;
    for (std::size_t k = i + 1; k < text.size() && k <= i + 2; ++k)
      text[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[k])));
  }
  return text;
}

// The shortest dot-separated part of a token that is kept as a secret of its
// own: a token is three parts, and a service that quotes one back may quote
// only a part. Short parts are not secrets, and would blot out the words
// around them.
constexpr std::size_t kLeastTokenPart = 16;

// `secret` as it may stand in what a service says back: as it is, as it is
// written inside a JSON string, and as it is written in a URL (escapes in
// capitals or in lower case, a space as `%20` or as `+`); and, of a long
// token, each of its dot-separated parts as it is.
void add_forms(std::vector<std::string>& out, std::string_view secret) {
  if (secret.empty()) return;
  const auto add = [&out](std::string form) {
    if (std::find(out.begin(), out.end(), form) == out.end()) out.push_back(std::move(form));
  };
  add(std::string(secret));
  std::string quoted = json(secret).dump(-1, ' ', false, json::error_handler_t::replace);
  if (quoted.size() >= 2) add(quoted.substr(1, quoted.size() - 2));
  const std::string encoded = url_encode(secret);
  add(encoded);
  add(lower_hex_escapes(encoded));
  if (secret.find(' ') != std::string_view::npos) {
    std::string form;
    for (std::size_t from = 0;;) {
      const std::size_t space = secret.find(' ', from);
      form += url_encode(secret.substr(from, space == std::string_view::npos ? space : space - from));
      if (space == std::string_view::npos) break;
      form += '+';
      from = space + 1;
    }
    add(lower_hex_escapes(form));
    add(std::move(form));
  }
  if (secret.size() >= 3 * kLeastTokenPart) {
    for (std::size_t from = 0; from <= secret.size();) {
      std::size_t dot = secret.find('.', from);
      if (dot == std::string_view::npos) dot = secret.size();
      if (dot - from >= kLeastTokenPart && dot - from < secret.size()) add(std::string(secret.substr(from, dot - from)));
      from = dot + 1;
    }
  }
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
// As it was said, blanks and all: a secret that begins or ends in one is
// found only in the untrimmed text, so it is taken out before it is trimmed.
std::string reason_of(std::string_view body) {
  const json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded()) return std::string(body);
  if (!parsed.is_object()) return {};
  for (const char* key : {"detail", "message", "title"}) {
    const auto it = parsed.find(key);
    if (it == parsed.end() || !it->is_string()) continue;
    const auto& text = it->get_ref<const std::string&>();
    if (!trimmed(text).empty()) return text;
  }
  return {};
}

// A reason as it may stand in a sentence: cut where the machine's part
// begins (`;`, `nested exception`: legacy), on one line, and not endless.
std::string short_reason(std::string_view reason) {
  reason = reason.substr(0, reason.find(';'));
  reason = reason.substr(0, reason.find("nested exception"));
  return one_line(reason);
}

// Where a URL points: the scheme and the host in lower case, the port (the
// scheme's own when none is written), and the path. Nothing for a text that
// is no such URL, or whose authority holds what one client may read one way
// and another another (a user, a backslash, an escape, a space): such a
// request is not sent at all.
struct Address {
  std::string scheme, host, port, path;
};

std::optional<Address> address_of(std::string_view url) {
  const std::size_t colon = url.find("://");
  if (colon == std::string_view::npos || colon == 0) return std::nullopt;
  Address out;
  out.scheme = lowered(url.substr(0, colon));
  if (!std::all_of(out.scheme.begin(), out.scheme.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return std::nullopt;
  std::string_view authority = url.substr(colon + 3);
  authority = authority.substr(0, authority.find_first_of("/?#"));
  const auto plain = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':' ||
           c == '[' || c == ']' || c == '_';
  };
  if (authority.empty() || !std::all_of(authority.begin(), authority.end(), plain)) return std::nullopt;
  // The port is after the last colon that is not inside an IPv6 address.
  const std::size_t bracket = authority.rfind(']');
  const std::size_t port = authority.rfind(':');
  std::string_view host = authority;
  if (port != std::string_view::npos && (bracket == std::string_view::npos || port > bracket)) {
    std::string_view digits = authority.substr(port + 1);
    host = authority.substr(0, port);
    if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return std::nullopt;
    }
    while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
    out.port = digits;
  } else {
    out.port = out.scheme == "https" ? "443" : out.scheme == "http" ? "80" : "";
  }
  if (host.empty()) return std::nullopt;
  out.host = lowered(host);
  out.path = url_path(url);
  return out;
}

// `url` is of the service at `base`: the same scheme, host and port, and
// under the base's path where it has one. Compared part by part: a host that
// only begins as the base's does (`earthbank.test.evil.example`) is another.
bool is_under(const Address& base, const Address& url) {
  if (url.scheme != base.scheme || url.host != base.host || url.port != base.port) return false;
  std::string_view root = base.path;
  while (!root.empty() && root.back() == '/') root.remove_suffix(1);
  if (root.empty()) return true;
  // cppcheck-suppress mismatchingContainers ; the two paths are compared as text; no iterator of one meets the other's
  const std::string_view path = url.path;
  return path == root || (path.starts_with(root) && path[root.size()] == '/');
}

std::string not_writable_sentence(const std::string& what) {
  return "EarthBank did not take the " + what +
         ": this account may not write to the data package. Choose a package the account may write to, or make a "
         "new one, and send it again.";
}

// What an answer that is not 2xx is, as far as it can be told. By the body
// first, whatever the status: a package the account may not write to, then a
// value the service does not know (it has been seen to send that as a 400 and
// may as well send a 500). Then by the status.
enum class Refusal { NotWritable, Constraint, Login, Forbidden, NoSuchAddress, Fault, Redirect, Other };

// `reason` is what the service said (reason_of), secrets out.
Refusal refusal_of(int status, std::string_view reason) {
  const std::string low = lowered(reason);
  // The service's sentence is "Package is not writable by user.". Legacy
  // also took any 500 that speaks of a package for it; that is not done
  // here: such a 500 may be a fault at a record that was saved all the same.
  if (has(low, "not writable")) return Refusal::NotWritable;
  if (has(low, "constraint") || has(low, "not present in table") || has(low, "foreign key")) return Refusal::Constraint;
  if (status == 401) return Refusal::Login;
  if (status == 403) return Refusal::Forbidden;
  if (status == 404) return Refusal::NoSuchAddress;
  if (status >= 500) return Refusal::Fault;
  if (status >= 300 && status < 400) return Refusal::Redirect;
  return Refusal::Other;
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
  constexpr auto kRecords = std::to_array<std::pair<std::string_view, std::string_view>>({
      {"sample-with-locations", "sample"}, {"ArArDataPoint", "data point"},
      {"ArArAliquot", "aliquot"},          {"ArArMeasurement", "measurement"},
      {"ArArAgeCalc", "age calculation"},  {"ArArAgeSummary", "age summary"},
      {"data-packages", "data package"},
  });
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
    const auto& text = it->get_ref<const std::string&>();
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

// How a new sign-in that a 401 asked for ended, when it did not give a token.
enum class Renewal { NotTried, NoAnswer, Fault };

// The sentence for an answer that is not 2xx. `reason` is what the service
// said (reason_of), the session's secrets already out of it: nothing else in
// the sentence is the service's. `in_doubt`: a write the answer does not say
// was not carried out.
std::string answer_sentence(int status, const std::string& reason, std::string_view path, std::string_view record,
                            bool in_doubt, Renewal renewal) {
  if (is_success(status)) return {};
  const std::string where = url_path(path);
  const std::string what = what_of(where, record);
  switch (refusal_of(status, reason)) {
    case Refusal::NotWritable:
      return not_writable_sentence(what);
    case Refusal::Constraint: {
      const std::string field = constraint_field(reason);
      return "EarthBank did not take the " + what + ": it does not know " +
             (field.empty() ? std::string("one of the values sent") : "the value sent for '" + field + "'") +
             ". The term is probably not on EarthBank's list; a pre-flight check names such terms before anything is "
             "sent.";
    }
    case Refusal::Login:
      if (where.ends_with(kAuthenticate)) return "EarthBank did not accept the user name and password.";
      // Nobody refused the password: the token ran out and no new one was had.
      if (renewal != Renewal::NotTried) {
        return std::string("EarthBank no longer takes this session's login, and it could not be renewed: the service ") +
               (renewal == Renewal::NoAnswer ? "did not answer" : "failed at") + " the new sign-in. Nothing was done (" +
               what + "); try again when EarthBank answers.";
      }
      return "EarthBank refused the login (" + what +
             "). Check the user name and the password, and that the account has the right to do this.";
    case Refusal::Forbidden:
      if (where.ends_with(kAuthenticate)) return "EarthBank did not accept the user name and password.";
      return "EarthBank does not let this account do this (" + what +
             "). The login was taken; what the account lacks is the right to it.";
    case Refusal::NoSuchAddress:
      return "EarthBank has nothing at " + where + ": the address is probably wrong. Check the base URL of the profile.";
    case Refusal::Fault:
      // Never "try again" of a write: a gateway may say this of a record the
      // service went on to save.
      if (in_doubt) {
        return "EarthBank failed on its own side at the " + what + " (HTTP " + std::to_string(status) + "); the " + what +
               " may or may not have been saved. Look for it on EarthBank before it is sent again.";
      }
      return "EarthBank failed on its own side at the " + what + " (HTTP " + std::to_string(status) +
             "). Try again later; if it goes on, tell EarthBank's support.";
    case Refusal::Redirect:
      return "EarthBank answered the " + what + " with a redirect (HTTP " + std::to_string(status) +
             "): the base URL of the profile is probably not the service's own address.";
    case Refusal::Other:
      break;
  }
  if (const std::string said = short_reason(reason); !said.empty()) return "EarthBank refused the " + what + ": " + said;
  return "EarthBank refused the " + what + " (HTTP " + std::to_string(status) + ").";
}

// The codes of an Error that is an answer's: it has its sentence.
bool is_answered(const Error& error) {
  for (const std::string_view code : {kLoginRejected, kNotRenewed, kForbidden, kRefused, kInDoubt, kPackageMisnamed}) {
    if (error.code == code) return true;
  }
  return false;
}

}  // namespace

bool is_write(std::string_view method) {
  const std::string low = lowered(method);
  return low != "get" && low != "head" && low != "options";
}

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

// `text` with nothing of this session's in it: its tokens, its password, and
// `trying`, a password on its way in.
std::string Session::scrubbed(std::string_view text, std::string_view trying) const {
  std::vector<std::string> secrets;
  add_forms(secrets, token_);
  for (const std::string& token : spent_tokens_) add_forms(secrets, token);
  add_forms(secrets, password_);
  add_forms(secrets, trying);
  std::string out = redacted(text, secrets);
  for (std::string& secret : secrets) wipe(secret);
  return out;
}

// What the service said of a failure. Scrubbed whole, before any of it is
// cut or read: a cut could leave the first part of a secret standing.
std::string Session::said_in(std::string_view body, std::string_view trying) const {
  return std::string(trimmed(scrubbed(reason_of(body), trying)));
}

// Why `url` may not be sent, when it may not: the token and the password go
// to the service of the profile and to nothing else.
std::optional<Error> Session::misaddressed(std::string_view url) const {
  const std::optional<Address> base = address_of(base_url_);
  if (!base) {
    return Error{ErrorKind::Config, "earthbank: the base URL of the profile is not a web address; nothing was sent", ""};
  }
  const std::optional<Address> to = address_of(url);
  if (!to || !is_under(*base, *to)) {
    // The address is not said back: it is somebody else's, or no address.
    return Error{ErrorKind::Config,
                 "earthbank: a request that is not addressed to the service of the profile was not sent", ""};
  }
  return std::nullopt;
}

// One request on the wire, and its line in the log.
Result<HttpResponse> Session::exchange(const HttpRequest& request, std::string_view trying) {
  if (std::optional<Error> elsewhere = misaddressed(request.url)) return fail(std::move(*elsewhere));
  Result<HttpResponse> answer = http_.send(request);
  if (!log_) return answer;
  std::string line = request_line(request);
  if (!answer) {
    line += answer.error().code == kHttpNotSent ? ": not sent (" : ": no answer (";
    line += to_string(answer.error().kind);
    line += ')';
    // The client's own words are for the log and not for the user. It
    // promises no secret in them (http.hpp); they are scrubbed all the same.
    if (const std::string said = one_line(scrubbed(answer.error().what, trying)); !said.empty()) line += ": " + said;
  } else {
    line += " -> " + std::to_string(answer->status);
    if (!is_success(answer->status)) {
      if (const std::string reason = short_reason(said_in(answer->body, trying)); !reason.empty()) line += ": " + reason;
    }
  }
  log_(line);
  return answer;
}

Error Session::answer_error(const HttpResponse& answer, bool write, std::string_view path, std::string_view record,
                            std::string_view trying) const {
  const std::string reason = said_in(answer.body, trying);
  // A fault to a write is no answer to whether it was written.
  const bool in_doubt = write && refusal_of(answer.status, reason) == Refusal::Fault;
  const Renewal renewal = !renewal_failed_ ? Renewal::NotTried : renewal_unanswered_ ? Renewal::NoAnswer : Renewal::Fault;
  Error error{answer.status == 401 ? ErrorKind::NotConnected : ErrorKind::Protocol,
              answer_sentence(answer.status, reason, path, record, in_doubt, renewal), ""};
  // A 401 that stands because the new sign-in failed unrefused is not the
  // password's: by the same test as its sentence.
  const bool unrenewed = renewal != Renewal::NotTried && !url_path(path).ends_with(kAuthenticate);
  error.code = std::string(answer.status == 401 ? (unrenewed ? kNotRenewed : kLoginRejected)
                           : answer.status == 403 ? kForbidden
                           : in_doubt ? kInDoubt
                                      : kRefused);
  return error;
}

Error Session::failure(const HttpResponse& answer, std::string_view method, std::string_view path,
                       std::string_view record) const {
  return answer_error(answer, is_write(method), path, record, {});
}

// The client's error with the sentence for `what`: its kind and code stay.
// The sentence is of our own words and the caller's, and is not scrubbed.
Error Session::failure(Error error, std::string_view method, std::string_view path, std::string_view record) const {
  error.what = sentence(error, path, record, is_write(method));
  return error;
}

Result<void> Session::authenticate(std::string_view username, std::string_view password) {
  // Legacy's body. The password is in it and nowhere else.
  HttpRequest request = json_post(url(kAuthenticate), {{"username", username}, {"password", password}, {"rememberMe", false}});
  Result<HttpResponse> answer = exchange(request, password);
  wipe(request.body);
  // A login is a POST and writes nothing: no answer to it leaves nothing in
  // doubt.
  if (!answer) return fail(failure(std::move(answer.error()), "GET", kAuthenticate, ""));
  if (!is_success(answer->status)) return fail(answer_error(*answer, false, kAuthenticate, "", password));
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
  renewal_failed_ = renewal_unanswered_ = false;
  if (std::optional<Error> elsewhere = misaddressed(request.url)) return fail(std::move(*elsewhere));
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
    if (again.error().code == kLoginRejected || again.error().code == kForbidden) {
      sign_out();
    } else {
      // Nobody refused the password: failure() says so of this 401.
      renewal_failed_ = true;
      renewal_unanswered_ = again.error().kind == ErrorKind::Timeout || again.error().kind == ErrorKind::Io;
    }
    return answer;
  }
  with_bearer(request);
  return exchange(request);
}

Result<Account> Session::account() {
  Result<HttpResponse> answer = send(plain_get(url(kAccount)), true);
  if (!answer) return fail(failure(std::move(answer.error()), "GET", kAccount, ""));
  if (!is_success(answer->status)) return fail(failure(*answer, "GET", kAccount, ""));
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
    if (!answer) return fail(failure(std::move(answer.error()), "GET", kPackages, ""));
    if (!is_success(answer->status)) return fail(failure(*answer, "GET", kPackages, ""));
    const json rows = json::parse(answer->body, nullptr, false);
    if (!rows.is_array()) return fail(ErrorKind::Protocol, "EarthBank's list of data packages could not be read.");
    // An empty page is the end. A short one is not: the service may give
    // fewer rows a page than it was asked for.
    if (rows.empty()) return out;
    for (const json& row : rows) {
      if (std::optional<DataPackage> package = package_of(row)) out.push_back(std::move(*package));
    }
  }
  // A service that gives a page whatever page is asked for: what was read is
  // not the list.
  return fail(ErrorKind::Protocol, "EarthBank's list of data packages did not end after " +
                                       std::to_string(kMostPackagePages) + " pages; it was not read.");
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
  if (!answer) return fail(failure(std::move(answer.error()), "POST", kPackages, record));
  if (!is_success(answer->status)) return fail(failure(*answer, "POST", kPackages, record));
  std::optional<DataPackage> made = package_of(json::parse(answer->body, nullptr, false));
  if (!made) {
    return fail(ErrorKind::Protocol, "EarthBank answered, but named no id for the " + record +
                                         ": it may have been made. Look for it on EarthBank before making another.");
  }
  // The service has been seen to take a body it does not like and make the
  // package under a name of its own. An answer with no name does not say.
  if (const std::string_view named = trimmed(made->name); !named.empty() && named != name) {
    Error error{ErrorKind::Protocol,
                "EarthBank made a data package, but named it '" + one_line(scrubbed(named, {})) + "' and not '" + std::string(name) +
                    "' (its id is " + std::to_string(made->id) +
                    "). Rename it on EarthBank or use it as it is; do not make another.",
                ""};
    error.code = std::string(kPackageMisnamed);
    return fail(std::move(error));
  }
  return std::move(*made);
}

// ---- Sentences ---------------------------------------------------------------

std::string Session::sentence(const Error& error, std::string_view path, std::string_view record, bool write) {
  // An answer already has its sentence.
  if (is_answered(error)) return error.what;
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

// NOLINTNEXTLINE(modernize-use-string-view): see the declaration
std::string Session::no_package_sentence() {
  return "No data package is chosen, and EarthBank keeps every record in one. Choose a package, or make one, before "
         "anything is sent.";
}

std::string Session::partial_sentence(std::string_view sample, std::span<const std::string> missing,
                                      std::span<const std::string> in_doubt) {
  const auto listed = [](std::span<const std::string> records) {
    std::string out = " (";
    for (std::size_t i = 0; i < records.size(); ++i) out += (i == 0 ? "" : ", ") + records[i];
    return out + ")";
  };
  std::string out(sample);
  out += in_doubt.empty() || !missing.empty() ? " is only partly on EarthBank" : " may be only partly on EarthBank";
  out += ": its data point was saved and ";
  if (missing.empty() && in_doubt.empty()) {
    out += "some records under it were not";
  } else if (!missing.empty()) {
    out += std::to_string(missing.size()) + (missing.size() == 1 ? " record under it was not" : " records under it were not");
    out += listed(missing);
  }
  if (in_doubt.empty()) return out + ". Send it again to add what is missing.";
  // A record that may be there is looked for, not sent again: nothing can
  // be taken back.
  if (missing.empty()) {
    out += std::to_string(in_doubt.size()) + (in_doubt.size() == 1 ? " record under it" : " records under it");
  } else {
    out += "; " + std::to_string(in_doubt.size()) + " more";
  }
  out += " may or may not have been saved" + listed(in_doubt);
  out += std::string(". Look for ") + (in_doubt.size() == 1 ? "it" : "them") + " on EarthBank before anything of " +
         std::string(sample) + " is sent again.";
  return out;
}

}  // namespace pychron::earthbank
