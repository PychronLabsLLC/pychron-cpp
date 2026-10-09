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
// of a failure has both taken out of it first: out of the whole of it, before
// it is cut to length or read for what went wrong, and out of nothing but it.
// The words around it are this library's and the caller's and stand as they
// are (a password that is a word of a sentence would otherwise show where it
// was struck). There is no way to the service's words but through a session:
// failure() is the only thing that turns an answer into a sentence.
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
// which is passed on as it came. What a caller that writes records (and the
// journal it keeps of them) may take each to mean:
//
//   Nothing left this machine:
//     kNotSignedIn      the call needs a login and there is none.
//     (kHttpNotSent, and any `Config` error: http.hpp. A request that is not
//      addressed to the service of the profile is a `Config` error.)
//   The service answered, and what was asked for was not done:
//     kLoginRejected    401: the user name and password were refused, or the
//                       login the call was made with is no longer taken.
//     kNotRenewed       401, and the new sign-in it asked for was not refused
//                       but got no answer, or a fault: nobody said the
//                       password is wrong, and the session keeps it. A blip of
//                       the network and not a reason to ask for the password;
//                       the same call may be made again.
//     kForbidden        403: the login is good and the account may not do
//                       this.
//     kRefused          any other status that is not 2xx, but for the next.
//   Something answered, and it does not say whether it was done:
//     kInDoubt          a write (POST, PUT, DELETE: is_write) answered with a
//                       5xx that is neither of the two the service means
//                       something by, a package that cannot be written to and
//                       a value it does not know (both kRefused). So 502, 503,
//                       504 and any 500 not recognised (one that only speaks
//                       of a package among them): a gateway says these
//                       of a service that may have gone on and saved the
//                       record. The record is in doubt, exactly as after no
//                       answer, and is looked for before it is sent again. To
//                       a question (GET) the same answer is kRefused, and it
//                       may be asked again.
//   It was done, and not as asked:
//     kPackageMisnamed  create_package: a package was made under another
//                       name than the one sent.
//
// An Error with none of these codes and not kHttpNotSent is no answer (a
// timeout, a lost connection, a 2xx that could not be read), and a write that
// ended so is in doubt too (request_may_have_arrived, which is also true of
// every code of the last three groups: ask it only of an Error that has no
// code of an answer).
inline constexpr std::string_view kNotSignedIn = "earthbank_not_signed_in";
inline constexpr std::string_view kLoginRejected = "earthbank_login_rejected";
inline constexpr std::string_view kNotRenewed = "earthbank_not_renewed";
inline constexpr std::string_view kForbidden = "earthbank_forbidden";
inline constexpr std::string_view kRefused = "earthbank_refused";
inline constexpr std::string_view kInDoubt = "earthbank_in_doubt";
inline constexpr std::string_view kPackageMisnamed = "earthbank_package_misnamed";

// Whether a request of this method asks the service to change something:
// every method but GET, HEAD and OPTIONS, whatever its case.
bool is_write(std::string_view method);

class Session {
 public:
  // `http` must outlive the session. `base_url` is scheme and host
  // (`https://earthbank.test`), with a path where the service is kept under
  // one; a slash at its end is dropped. One that is no such address is found
  // out at the first call, which sends nothing.
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
  // password (nothing sent); kLoginRejected (401) or kForbidden (403) with
  // the login sentence; kRefused for another status (a login writes nothing:
  // never kInDoubt); `Protocol` for a 2xx with no token in it; the client's
  // own kind and code when there was no answer.
  Result<void> sign_in(std::string_view username, std::string_view password);
  // Forgets the token and the password, overwriting both.
  void sign_out();
  bool signed_in() const { return !token_.empty(); }

  // `GET /api/account`: the connection test, and who the upload will be made
  // as.
  Result<Account> account();
  // `GET /api/management/data-packages`, by name, every page; of one
  // institution when an id is given. Every package is listed, not only those
  // the account may write to. The pages are read until one comes empty (a
  // short page is not the last: the service may give fewer rows a page than
  // were asked for), and no more than 200 of them: a list that has not ended
  // by then is a `Protocol` error, and nothing of it is returned.
  Result<std::vector<DataPackage>> packages(std::optional<std::int64_t> institution_id);
  // `POST` to the same path: a private package in progress, which its maker
  // may write to. The fields go nested under `dataPackageDTO` and again
  // beside it; a flat body is taken by the service and its name thrown away.
  // The answer's package is returned as the service names it. When that is
  // not the name sent (but for the spaces around it), a package was made and
  // misnamed: kPackageMisnamed, with both names and the id in the sentence,
  // and no package returned. An answer that names no name is taken as it is.
  // A 2xx with no id in it is a `Protocol` error that says a package may have
  // been made; so, by its sentence, is a call that got no answer, and one
  // answered with a fault (kInDoubt).
  Result<DataPackage> create_package(std::string_view name, std::optional<std::int64_t> institution_id);

  // Sends `request` (its url whole: see url()) with the bearer when signed
  // in. A url that is not of this session's service (the base URL's scheme,
  // host and port, compared as such and not as a beginning of the text, and
  // under its path) is a `Config` error and is not sent, signed in or not:
  // the bearer goes nowhere else. `needs_login` and no login: kNotSignedIn,
  // nothing sent. Without
  // `needs_login` a session that is not signed in sends the request as it is
  // (some lists are open), and a 401 is then its answer.
  //
  // What comes back is the client's, untouched: any status is a value, 401
  // and 500 included, and an Error is the client's Error. Nothing is ever
  // sent again after an Error. Only on a 401, and only once, the session
  // signs in anew and sends the request a second time; when the new sign-in
  // fails, the 401 is returned as the answer it was. That is safe for a write
  // too: a 401 is an answer, and it says nothing was written. A sign-in the
  // service refuses outright (401 or 403: the password was changed) ends the
  // session, so that a wrong password is not offered again and again. One
  // that got no answer, or a fault, does not, and failure() of that 401 says
  // that the session could not be renewed, not that the password is wrong.
  Result<HttpResponse> send(HttpRequest request, bool needs_login);

  // A failed call as an Error, for a caller of send(). `method` is the
  // request's: whether it was a write decides what a fault leaves in doubt
  // (is_write, and the codes above).
  //
  // For an answer that is not 2xx, `what` is one plain sentence. `path` is
  // the path (or the URL: the query is cut off) the call was made to;
  // `record` is what was being asked for or written, in the caller's words
  // (`measurement 20001-01A`), or empty for the word the path gives
  // (`measurement`, `data package`, `list of terms`). The sentence is told by
  // the body first, whatever the status: a package the account may not write
  // to ("not writable"), then a value the service does not know (a
  // constraint, with the field named when the body names it; it may come as a
  // 400 or a 500). Then by the status: the login refused (401), the account
  // not allowed (403), a wrong address (404), the service's own fault (5xx:
  // for a write, that the record may or may not have been saved, and never
  // to try again), a redirect, and anything else with the service's reason.
  // The reason is read from the body's `detail`, then `message`, then
  // `title`, then the text as it came, with this session's tokens and
  // password taken out of it (above). The code is by the status: 401
  // kLoginRejected (`NotConnected`), 403 kForbidden, else kInDoubt or
  // kRefused (403 and these two are `Protocol`). A 2xx has no sentence.
  //
  // A 401 that send() returned because the new sign-in got no answer, or a
  // fault, is said to be that, and its code is kNotRenewed (`NotConnected`)
  // and not kLoginRejected. This is known of the last send() only: make the
  // Error of an answer before sending anything else.
  //
  // For no answer: the client's error with sentence() for `what`, its kind
  // and code kept, so that request_may_have_arrived still tells a write in
  // doubt from one that never left.
  Error failure(const HttpResponse& answer, std::string_view method, std::string_view path,
                std::string_view record) const;
  Error failure(Error error, std::string_view method, std::string_view path, std::string_view record) const;

  // Where a line goes for each request sent: `GET /api/account -> 200`, with
  // what the service said after it when the status is not 2xx (scrubbed, cut
  // and on one line; never the body), or `: no answer (timeout): ` and the
  // client's own words for it, which go here and into no sentence. Never a
  // query, a header or a body sent. No sink: no lines.
  void set_log(std::function<void(std::string_view)> sink) { log_ = std::move(sink); }

  // ---- Sentences ------------------------------------------------------------
  //
  // Of this library's own words only: none of these is given anything the
  // service said. `path` and `record` as for failure().
  //
  // For no answer: the request never left (kHttpNotSent, or `Config`); or it
  // may have arrived, and then for a `write` the sentence says the record may
  // or may not have been saved and does not say to send it again. An Error
  // that is already an answer's (from failure()) keeps the sentence it has.
  static std::string sentence(const Error& error, std::string_view path, std::string_view record, bool write);
  // An upload asked for with no package to put it in.
  static std::string no_package_sentence();
  // A sample whose data point was saved and some of whose other records were
  // not: `missing` names those that were refused or never sent, which may be
  // sent again, and `in_doubt` those that may or may not have been saved (no
  // answer, kInDoubt). With any in doubt the sentence does not say to send
  // the sample again: it says to look first.
  static std::string partial_sentence(std::string_view sample, std::span<const std::string> missing,
                                      std::span<const std::string> in_doubt = {});

 private:
  Result<void> authenticate(std::string_view username, std::string_view password);
  Result<HttpResponse> exchange(const HttpRequest& request, std::string_view trying = {});
  std::string scrubbed(std::string_view text, std::string_view trying) const;
  std::string said_in(std::string_view body, std::string_view trying) const;
  std::optional<Error> misaddressed(std::string_view url) const;
  Error answer_error(const HttpResponse& answer, bool write, std::string_view path, std::string_view record,
                     std::string_view trying) const;

  IHttpClient& http_;
  std::string base_url_;
  std::string username_, password_, token_;
  std::vector<std::string> spent_tokens_;  // earlier tokens of this session, only to keep them out of messages
  std::function<void(std::string_view)> log_;
  // Of the last send(): its 401 stands because the new sign-in failed without
  // being refused, and whether that was for want of an answer.
  bool renewal_failed_ = false, renewal_unanswered_ = false;
};

}  // namespace pychron::earthbank
