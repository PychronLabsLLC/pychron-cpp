// A signed-in conversation with EarthBank.
//
// The service gives a token for a user name and a password and wants it on
// most requests. A Session signs in, puts the token on what it is given to
// send, signs in again once when the service says the token is no longer
// good, and reads the three answers every program needs before anything is
// published: who the account is, which data packages there are, and a new
// package. It also has the words for what went wrong: a status and a body
// from the service, or no answer at all, as one plain sentence a
// geochronologist can act on.
//
// It talks through an IHttpClient (http.hpp) and so opens no socket itself;
// the tests give it a whole service in memory.
//
// Secrets. The password is kept for the life of the Session, for the one new
// sign-in a 401 asks for, and for nothing else; sign_out and the destructor
// overwrite the session's copy of it and of the token (the body of a login
// request is overwritten once it is sent; what the client under it copies is
// the client's). No token, password or Authorization value is ever put into
// an Error, a sentence or a log line: what is said of a request is its
// method, its path and the status (request_line), and what the service said
// of a failure has both taken out of it first.
//
// One thread at a time, as the client under it.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"

namespace pychron::earthbank {

// Who is signed in: the service's id for the user, the login, and the name
// as the service has it (given name and family name; the login when it has
// neither).
struct Account {
  std::int64_t id = 0;
  std::string login, name;
};

// A data package: what every record on EarthBank is kept in. Nothing in a
// row says whether the account may write to it; a package the account made
// itself always can be.
struct DataPackage {
  std::int64_t id = 0;
  std::string name, institution_name, workflow_state;
  std::int64_t institution_id = 0;  // 0: none
};

// The `code` of an Error from a Session, beside the client's kHttpNotSent,
// which is passed on as it came.
//   kNotSignedIn    the call needs a login and there is none: nothing was sent.
//   kLoginRejected  the service refused the user name and password, or the
//                   login the call was made with (401, 403).
//   kRefused        the service answered with another status that is not 2xx.
// The last two are answers: whatever was asked for was not done. An Error
// with none of these codes and not kHttpNotSent is no answer (a timeout, a
// lost connection, a 2xx that could not be read), and a write that ended so
// is in doubt (request_may_have_arrived).
inline constexpr std::string_view kNotSignedIn = "earthbank_not_signed_in";
inline constexpr std::string_view kLoginRejected = "earthbank_login_rejected";
inline constexpr std::string_view kRefused = "earthbank_refused";

class Session {
 public:
  // `http` must outlive the session. `base_url` is scheme and host
  // (`https://earthbank.test`); a slash at its end is dropped.
  Session(IHttpClient& http, std::string base_url);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  const std::string& base_url() const { return base_url_; }
  // The URL of a path of the service: `url("/api/account")`.
  std::string url(std::string_view path) const;

  // `POST /api/authenticate`. Keeps the token, and both arguments for one new
  // sign-in later. Whatever login there was is ended first, so a failure
  // leaves the session signed out. Errors: `Config` for an empty user name or
  // password (nothing sent); kLoginRejected with the login sentence for a 401
  // or 403; kRefused for another status; `Protocol` for a 2xx with no token
  // in it; the client's own kind and code when there was no answer.
  Result<void> sign_in(std::string_view username, std::string_view password);
  // Forgets the token and the password, overwriting both.
  void sign_out();
  bool signed_in() const { return !token_.empty(); }

  // `GET /api/account`: the connection test, and who the upload will be made
  // as.
  Result<Account> account();
  // `GET /api/management/data-packages`, by name, every page; of one
  // institution when an id is given. Every package is listed, not only those
  // the account may write to.
  Result<std::vector<DataPackage>> packages(std::optional<std::int64_t> institution_id);
  // `POST` to the same path: a private package in progress, which its maker
  // may write to. The fields go nested under `dataPackageDTO` and again
  // beside it; a flat body is taken by the service and its name thrown away.
  // The answer's package is returned as the service names it. A 2xx with no
  // id in it is a `Protocol` error that says a package may have been made;
  // so, by its sentence, is a call that got no answer.
  Result<DataPackage> create_package(std::string_view name, std::optional<std::int64_t> institution_id);

  // Sends `request` (its url whole: see url()) with the bearer when signed
  // in. `needs_login` and no login: kNotSignedIn, nothing sent. Without
  // `needs_login` a session that is not signed in sends the request as it is
  // (some lists are open), and a 401 is then its answer.
  //
  // What comes back is the client's, untouched: any status is a value, 401
  // and 500 included, and an Error is the client's Error. Nothing is ever
  // sent again after an Error. Only on a 401, and only once, the session
  // signs in anew and sends the request a second time; when the new sign-in
  // fails, the 401 is returned as the answer it was. That is safe for a write
  // too: a 401 is an answer, and it says nothing was written. A sign-in the
  // service refuses outright (the password was changed) ends the session, so
  // that a wrong password is not offered again and again.
  Result<HttpResponse> send(HttpRequest request, bool needs_login);

  // A failed call as an Error, for a caller of send(): the sentence (below)
  // as `what`, with this session's token and password taken out of it should
  // the service have said either back. For an answer that is not 2xx: the
  // code is kLoginRejected (401, 403) or kRefused. For no answer: the
  // client's error, its kind and code kept, so that request_may_have_arrived
  // still tells a write in doubt from one that never left.
  Error failure(const HttpResponse& answer, std::string_view path, std::string_view record) const;
  Error failure(Error error, std::string_view path, std::string_view record, bool write) const;

  // Where a line goes for each request sent: `GET /api/account -> 200`, with
  // what the service said after it when the status is not 2xx, or `: no
  // answer (timeout)`. Never a query, a header or a body sent. No sink: no
  // lines.
  void set_log(std::function<void(std::string_view)> sink) { log_ = std::move(sink); }

  // ---- Sentences ------------------------------------------------------------
  //
  // One plain sentence for a failed call. `path` is the path (or the URL: the
  // query is cut off) the call was made to; `record` is what was being asked
  // for or written, in the caller's words (`measurement 20001-01A`), or empty
  // for the word the path gives (`measurement`, `data package`, `list of
  // terms`).
  //
  // For an answer. Told by the body first, whatever the status: a package the
  // account may not write to ("not writable"), then a value the service does
  // not know (a constraint, with the field named when the body names it; it
  // may come as a 400 or a 500). Then by the status: the login refused (401,
  // 403), a wrong address (404), the service's own fault (5xx), a redirect,
  // and anything else with the service's reason. The reason is read from the
  // body's `detail`, then `message`, then `title`, then the text as it came.
  // A 2xx has no sentence. The service's reason goes in as it came: a caller
  // with a session uses failure(), which keeps the session's secrets out.
  static std::string sentence(int status, std::string_view body, std::string_view path, std::string_view record);
  // For no answer: the request never left (kHttpNotSent, or `Config`); or it
  // may have arrived, and then for a `write` the sentence says the record may
  // or may not have been saved and does not say to send it again.
  static std::string sentence(const Error& error, std::string_view path, std::string_view record, bool write);
  // An upload asked for with no package to put it in.
  static std::string no_package_sentence();
  // A sample whose data point was saved and some of whose other records were
  // not: `missing` names them.
  static std::string partial_sentence(std::string_view sample, std::span<const std::string> missing);

 private:
  Result<void> authenticate(std::string_view username, std::string_view password);
  Result<HttpResponse> exchange(const HttpRequest& request, std::string_view trying = {});
  std::string without_secrets(std::string text, std::string_view trying = {}) const;

  IHttpClient& http_;
  std::string base_url_;
  std::string username_, password_, token_;
  std::vector<std::string> spent_tokens_;  // earlier tokens of this session, only to keep them out of messages
  std::function<void(std::string_view)> log_;
};

}  // namespace pychron::earthbank
