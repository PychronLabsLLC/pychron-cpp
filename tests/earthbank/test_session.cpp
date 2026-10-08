#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fake_earthbank.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/session.hpp"
#include "session_support.hpp"

namespace pychron::earthbank {
namespace {

using nlohmann::json;
using testing::FakeEarthBank;
using testing::LoggedRequest;
using testing::Spy;

constexpr std::string_view kUnauthorized = R"({"title":"Unauthorized","status":401,"detail":"Full authentication is required"})";

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// A session signed in to a fake of its own.
struct SignedIn {
  FakeEarthBank bank;
  Session session{bank, bank.base_url()};
  SignedIn() {
    const auto in = session.sign_in(bank.login(), bank.password());
    if (!in) ADD_FAILURE() << to_string(in.error());
  }
};

HttpRequest post(const Session& session, std::string_view path, const json& body) {
  HttpRequest request;
  request.method = "POST";
  request.url = session.url(path);
  request.headers.emplace_back("Content-Type", "application/json");
  request.body = body.dump();
  return request;
}

HttpRequest get(const Session& session, std::string_view path) {
  HttpRequest request;
  request.method = "GET";
  request.url = session.url(path);
  return request;
}

// The sentence for an answer, as a caller gets it: there is no way to it but
// through a session, which keeps its secrets out of it.
std::string said(const Session& session, int status, std::string_view body, std::string_view path,
                 std::string_view record) {
  return session.failure(HttpResponse{status, std::string(body)}, path, record).what;
}

// The sample wrapper as the service takes it (the fake's own tests).
json sample_body(std::int64_t package, const std::string& name) {
  return {{"sampleDTO", {{"name", name}, {"sampleID", name}, {"dataPackageId", package}}},
          {"locationDTO", json::object()},
          {"dataPackageId", package},
          {"shortName", name}};
}

// ---- Signing in ------------------------------------------------------------

TEST(Session, SignInThenAccount) {
  FakeEarthBank bank;
  Session session(bank, bank.base_url());
  EXPECT_FALSE(session.signed_in());
  EXPECT_EQ(session.base_url(), "https://earthbank.test");
  EXPECT_EQ(session.url("/api/account"), "https://earthbank.test/api/account");

  // Without a login nothing that needs one is sent.
  const auto early = session.account();
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error().kind, ErrorKind::NotConnected);
  EXPECT_EQ(early.error().code, kNotSignedIn);
  EXPECT_TRUE(bank.requests().empty());

  ASSERT_TRUE(session.sign_in(bank.login(), bank.password()));
  EXPECT_TRUE(session.signed_in());
  const auto account = session.account();
  ASSERT_TRUE(account) << to_string(account.error());
  EXPECT_EQ(account->id, 4107);
  EXPECT_EQ(account->login, "robin.example");
  EXPECT_EQ(account->name, "Robin Example");

  const std::vector<LoggedRequest> log = bank.requests();
  ASSERT_EQ(testing::lines(log), (std::vector<std::string>{"POST /api/authenticate", "GET /api/account"}));
  // The login as legacy sends it; the fake keeps the password out of its log.
  const json login = json::parse(log[0].body);
  EXPECT_EQ(login.at("username"), bank.login());
  EXPECT_EQ(login.at("rememberMe"), false);
  EXPECT_TRUE(login.contains("password"));
  EXPECT_FALSE(log[0].authenticated);
  EXPECT_TRUE(log[1].authenticated);

  // Signed out, the token and the password are gone: nothing is sent, and
  // nothing is tried with what was kept.
  session.sign_out();
  EXPECT_FALSE(session.signed_in());
  EXPECT_FALSE(session.account());
  EXPECT_EQ(bank.requests().size(), 2u);
}

TEST(Session, ABaseUrlMayEndInASlash) {
  FakeEarthBank bank;
  Session session(bank, bank.base_url() + "/");
  EXPECT_EQ(session.base_url(), "https://earthbank.test");
  EXPECT_EQ(session.url("api/account"), "https://earthbank.test/api/account");
  ASSERT_TRUE(session.sign_in(bank.login(), bank.password()));
  EXPECT_TRUE(session.account());
}

TEST(Session, WrongPasswordIsTheLoginSentence) {
  FakeEarthBank bank;
  Session session(bank, bank.base_url());
  const auto in = session.sign_in(bank.login(), "not-the-password");
  ASSERT_FALSE(in);
  EXPECT_EQ(in.error().what, "EarthBank did not accept the user name and password.");
  EXPECT_EQ(in.error().code, kLoginRejected);
  EXPECT_FALSE(session.signed_in());
  EXPECT_EQ(bank.count("POST", "/api/authenticate"), 1u);

  // A user name or a password that is not there is not sent to be refused.
  for (const auto& [user, password] : {std::pair<std::string, std::string>{"", "x"}, {bank.login(), ""}}) {
    const auto empty = session.sign_in(user, password);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().kind, ErrorKind::Config);
  }
  EXPECT_EQ(bank.count("POST", "/api/authenticate"), 1u);

  // A login that fails ends the one before it.
  ASSERT_TRUE(session.sign_in(bank.login(), bank.password()));
  ASSERT_FALSE(session.sign_in(bank.login(), "not-the-password"));
  EXPECT_FALSE(session.signed_in());

  // An answer without a token is no login.
  bank.fail_next("POST", "/api/authenticate", 200, R"({"id_token":""})");
  const auto hollow = session.sign_in(bank.login(), bank.password());
  ASSERT_FALSE(hollow);
  EXPECT_EQ(hollow.error().kind, ErrorKind::Protocol);
  EXPECT_FALSE(session.signed_in());
  bank.fail_next("POST", "/api/authenticate", 200, "<html>welcome</html>");
  EXPECT_FALSE(session.sign_in(bank.login(), bank.password()));

  // No answer: the kind and the code of the client's error are kept.
  bank.refuse_next("POST", "/api/authenticate");
  const auto refused = session.sign_in(bank.login(), bank.password());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Io);
  EXPECT_EQ(refused.error().code, kHttpNotSent);
  EXPECT_TRUE(has(refused.error().what, "could not be reached")) << refused.error().what;
  bank.lose_next("POST", "/api/authenticate");
  const auto silent = session.sign_in(bank.login(), bank.password());
  ASSERT_FALSE(silent);
  EXPECT_EQ(silent.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(bank.pending().empty());
}

// ---- A token that is no longer good ----------------------------------------

TEST(Session, ReauthenticatesOnceOnA401) {
  SignedIn s;
  s.bank.expire_tokens();
  const auto account = s.session.account();
  ASSERT_TRUE(account) << to_string(account.error());
  EXPECT_EQ(account->login, "robin.example");
  EXPECT_EQ(testing::lines(s.bank.requests()),
            (std::vector<std::string>{"POST /api/authenticate", "GET /api/account", "POST /api/authenticate",
                                      "GET /api/account"}));
  // The new token is the session's from then on.
  EXPECT_TRUE(s.session.account());
  EXPECT_EQ(s.bank.count("POST", "/api/authenticate"), 2u);

  // A write that met a 401 was not carried out, so sending it again makes one
  // record and not two.
  s.bank.expire_tokens();
  const std::size_t before = s.bank.store().packages.size();
  const auto made = s.session.create_package("Made across a login", std::nullopt);
  ASSERT_TRUE(made) << to_string(made.error());
  EXPECT_EQ(s.bank.store().packages.size(), before + 1);
  EXPECT_EQ(s.bank.count("POST", "/api/management/data-packages"), 2u);
  EXPECT_EQ(s.bank.count("POST", "/api/authenticate"), 3u);
}

TEST(Session, ASecond401IsAnError) {
  SignedIn s;
  s.bank.fail_next("GET", "/api/account", 401, std::string(kUnauthorized));
  s.bank.fail_next("GET", "/api/account", 401, std::string(kUnauthorized));
  const auto account = s.session.account();
  ASSERT_FALSE(account);
  EXPECT_EQ(account.error().code, kLoginRejected);
  EXPECT_EQ(account.error().what, said(s.session, 401, kUnauthorized, "/api/account", ""));
  EXPECT_TRUE(s.bank.pending().empty());
  // One new login, one repeat, and no more.
  EXPECT_EQ(s.bank.count("POST", "/api/authenticate"), 2u);
  EXPECT_EQ(s.bank.count("GET", "/api/account"), 2u);
  // The login itself was good: the session goes on.
  EXPECT_TRUE(s.session.signed_in());
  EXPECT_TRUE(s.session.account());
  EXPECT_EQ(s.bank.count("POST", "/api/authenticate"), 2u);
}

TEST(Session, APasswordChangedElsewhereEndsTheSession) {
  SignedIn s;
  s.bank.set_password("changed-somewhere-else");
  const auto account = s.session.account();
  ASSERT_FALSE(account);
  EXPECT_EQ(account.error().code, kLoginRejected);
  // The 401, the login that was refused, and the request is not repeated.
  EXPECT_EQ(testing::lines(s.bank.requests(), 1),
            (std::vector<std::string>{"GET /api/account", "POST /api/authenticate"}));
  // A password that was refused is not tried again: the session is over.
  EXPECT_FALSE(s.session.signed_in());
  const std::size_t sent = s.bank.requests().size();
  const auto after = s.session.packages(std::nullopt);
  ASSERT_FALSE(after);
  EXPECT_EQ(after.error().code, kNotSignedIn);
  EXPECT_EQ(s.bank.requests().size(), sent);
}

TEST(Session, ALoginThatCouldNotBeRenewedLeavesThe401) {
  SignedIn s;
  s.bank.expire_tokens();
  s.bank.lose_next("POST", "/api/authenticate");
  const auto answer = s.session.send(get(s.session, "/api/account"), true);
  // The answer to the request stands: it was refused, and nothing was done.
  ASSERT_TRUE(answer) << to_string(answer.error());
  EXPECT_EQ(answer->status, 401);
  EXPECT_EQ(s.bank.count("GET", "/api/account"), 1u);
  // The password was not refused, so the next call tries once more.
  EXPECT_TRUE(s.session.signed_in());
  EXPECT_TRUE(s.session.account());
}

TEST(Session, SendPassesAnswersAndErrorsThrough) {
  SignedIn s;
  // A status is a value, whatever it is, and is not sent again.
  s.bank.fail_next("GET", "/api/account", 500, R"({"title":"Internal Server Error"})");
  const auto failed = s.session.send(get(s.session, "/api/account"), true);
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->status, 500);
  EXPECT_EQ(s.bank.count("GET", "/api/account"), 1u);

  // An error is the client's own, untouched, and is not sent again either.
  s.bank.lose_next("GET", "/api/account");
  const auto lost = s.session.send(get(s.session, "/api/account"), true);
  ASSERT_FALSE(lost);
  EXPECT_EQ(lost.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(request_may_have_arrived(lost.error()));
  s.bank.drop_next("GET", "/api/account", ErrorKind::Io);
  const auto dropped = s.session.send(get(s.session, "/api/account"), true);
  ASSERT_FALSE(dropped);
  EXPECT_EQ(dropped.error().kind, ErrorKind::Io);
  EXPECT_TRUE(dropped.error().code.empty());
  s.bank.refuse_next("GET", "/api/account");
  const auto refused = s.session.send(get(s.session, "/api/account"), true);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, kHttpNotSent);
  EXPECT_FALSE(request_may_have_arrived(refused.error()));
  EXPECT_EQ(s.bank.count("GET", "/api/account"), 4u);
  EXPECT_EQ(s.bank.count("POST", "/api/authenticate"), 1u);

  // The bearer is the session's: one a caller put there is replaced.
  HttpRequest own = get(s.session, "/api/account");
  own.headers.emplace_back("authorization", "Bearer somebody-elses");
  const auto mine = s.session.send(own, true);
  ASSERT_TRUE(mine);
  EXPECT_EQ(mine->status, 200);
}

TEST(Session, WithoutALoginAnOpenRequestGoesAsItIs) {
  FakeEarthBank bank;
  Session session(bank, bank.base_url());
  const auto open = session.send(get(session, "/api/arar/AirRatio"), false);
  ASSERT_TRUE(open);
  EXPECT_EQ(open->status, 200);
  EXPECT_FALSE(bank.requests().back().authenticated);
  // A 401 is then the answer: there is nothing to sign in again with.
  const auto closed = session.send(get(session, "/api/core/l-sample-kinds"), false);
  ASSERT_TRUE(closed);
  EXPECT_EQ(closed->status, 401);
  EXPECT_EQ(bank.count("POST", "/api/authenticate"), 0u);
  EXPECT_EQ(bank.requests().size(), 2u);

  // Signed in, an open request carries the token all the same.
  ASSERT_TRUE(session.sign_in(bank.login(), bank.password()));
  ASSERT_TRUE(session.send(get(session, "/api/arar/AirRatio"), false));
  EXPECT_TRUE(bank.requests().back().authenticated);
}

// ---- Packages --------------------------------------------------------------

TEST(Session, PackagesByInstitution) {
  SignedIn s;
  const auto mine = s.session.packages(9301);
  ASSERT_TRUE(mine) << to_string(mine.error());
  ASSERT_EQ(mine->size(), 3u);
  // By name, as the service sorts them.
  EXPECT_EQ((*mine)[0].name, "Example Lab Ar-Ar 2026");
  EXPECT_EQ((*mine)[0].id, 610001);
  EXPECT_EQ((*mine)[0].institution_id, 9301);
  EXPECT_EQ((*mine)[0].institution_name, "Example University");
  EXPECT_EQ((*mine)[0].workflow_state, "IN_PROGRESS");
  EXPECT_EQ((*mine)[1].name, "Example Lab archive");
  EXPECT_EQ((*mine)[2].name, "Example Lab shared");

  const LoggedRequest asked = s.bank.requests().back();
  EXPECT_EQ(asked.path, "/api/management/data-packages");
  EXPECT_EQ(asked.param("institutionId.equals"), "9301");
  EXPECT_EQ(asked.param("sort"), "name,ASC");
  EXPECT_EQ(asked.param("page"), "0");

  const auto all = s.session.packages(std::nullopt);
  ASSERT_TRUE(all);
  EXPECT_EQ(all->size(), 4u);
  EXPECT_FALSE(s.bank.requests().back().param("institutionId.equals").has_value());
}

TEST(Session, EveryPackageIsListedHoweverMany) {
  SignedIn s;
  for (int i = 0; i < 1203; ++i) s.bank.add_package("Bulk " + std::to_string(10000 + i), false, 9400);
  const auto many = s.session.packages(9400);
  ASSERT_TRUE(many) << to_string(many.error());
  ASSERT_EQ(many->size(), 1203u);
  EXPECT_EQ(many->front().name, "Bulk 10000");
  EXPECT_EQ(many->back().name, "Bulk 11202");
  // Page after page, until one comes short.
  EXPECT_EQ(s.bank.count("GET", "/api/management/data-packages"), 3u);
  EXPECT_EQ(s.bank.requests().back().param("page"), "2");
}

TEST(Session, APackageRowIsReadWhereverItsFieldsAre) {
  SignedIn s;
  // Legacy reads `dataPackageDTO` first and the row itself second; a row with
  // no id is no package.
  s.bank.fail_next("GET", "/api/management/data-packages", 200,
                   R"([{"dataPackageDTO":{"id":11,"name":"Nested","institutionId":5,"institutionName":"There",
                        "workflowState":"IN_PROGRESS"}},
                       {"id":12,"name":"Flat"},
                       {"dataPackageDTO":{"name":"No id"}},
                       {"dataPackageDTO":{"id":"13","name":null}},
                       "not a row"])");
  const auto rows = s.session.packages(std::nullopt);
  ASSERT_TRUE(rows) << to_string(rows.error());
  ASSERT_EQ(rows->size(), 3u);
  EXPECT_EQ((*rows)[0].id, 11);
  EXPECT_EQ((*rows)[0].institution_name, "There");
  EXPECT_EQ((*rows)[1].id, 12);
  EXPECT_EQ((*rows)[1].name, "Flat");
  EXPECT_EQ((*rows)[2].id, 13);
  EXPECT_EQ((*rows)[2].name, "");

  // An answer that is not a list is an error, not an empty list.
  s.bank.fail_next("GET", "/api/management/data-packages", 200, R"({"content":[]})");
  const auto odd = s.session.packages(std::nullopt);
  ASSERT_FALSE(odd);
  EXPECT_EQ(odd.error().kind, ErrorKind::Protocol);
  s.bank.fail_next("GET", "/api/management/data-packages", 503, "");
  const auto down = s.session.packages(std::nullopt);
  ASSERT_FALSE(down);
  EXPECT_EQ(down.error().code, kRefused);
  EXPECT_EQ(down.error().what, said(s.session, 503, "", "/api/management/data-packages", ""));
}

TEST(Session, CreatePackageKeepsItsName) {
  SignedIn s;
  const std::string name = "Caf\xC3\xA9 & Co <2026> \"x\" 100%+";
  const auto made = s.session.create_package(name, 9301);
  ASSERT_TRUE(made) << to_string(made.error());
  EXPECT_EQ(made->id, 620001);
  // The fake gives a package made from a flat body a name of its own.
  EXPECT_EQ(made->name, name);
  EXPECT_EQ(made->institution_id, 9301);
  EXPECT_EQ(made->institution_name, "Example University");
  EXPECT_EQ(made->workflow_state, "IN_PROGRESS");
  const json kept = s.bank.store().packages.back().at("dataPackageDTO");
  EXPECT_EQ(kept.at("name"), name);
  EXPECT_EQ(kept.at("distribution"), "PRIVATE");
  EXPECT_EQ(kept.at("licenseText"), "Creative Commons");
  EXPECT_TRUE(s.bank.can_write(made->id));

  // The body: the fields nested, and the same beside the nest (legacy).
  const json body = json::parse(s.bank.requests().back().body);
  EXPECT_EQ(body.at("dataPackageDTO").at("name"), name);
  EXPECT_EQ(body.at("name"), name);
  EXPECT_EQ(body.at("institutionId"), 9301);
  EXPECT_EQ(body.at("dataPackageDTO").at("institutionId"), 9301);

  // Without an institution none is sent.
  const auto plain = s.session.create_package("No institution", std::nullopt);
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->institution_id, 0);
  EXPECT_FALSE(json::parse(s.bank.requests().back().body).contains("institutionId"));

  // A name that is none is not sent.
  const std::size_t sent = s.bank.requests().size();
  for (const char* none : {"", "   "}) {
    const auto empty = s.session.create_package(none, 9301);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().kind, ErrorKind::Config);
  }
  EXPECT_EQ(s.bank.requests().size(), sent);
}

TEST(Session, ANewPackagesIdIsReadWhereverItIs) {
  SignedIn s;
  // Any 2xx is a package made.
  s.bank.created_status(200);
  const auto ok = s.session.create_package("Answered 200", std::nullopt);
  ASSERT_TRUE(ok) << to_string(ok.error());
  EXPECT_EQ(ok->name, "Answered 200");
  EXPECT_EQ(ok->id, 620001);

  // The id beside the nest, where there is no nest (legacy's second place).
  s.bank.fail_next("POST", "/api/management/data-packages", 201, R"({"id":77,"name":"Flat answer"})");
  const auto flat = s.session.create_package("Flat answer", std::nullopt);
  ASSERT_TRUE(flat) << to_string(flat.error());
  EXPECT_EQ(flat->id, 77);
  EXPECT_EQ(flat->name, "Flat answer");

  // A 2xx that names no id: something may have been made, and the message
  // must not send the user off to make another.
  for (const char* body : {"", "{}", R"({"dataPackageDTO":{"name":"x"}})", "created"}) {
    s.bank.fail_next("POST", "/api/management/data-packages", 201, body);
    const auto blind = s.session.create_package("No id", std::nullopt);
    ASSERT_FALSE(blind) << body;
    EXPECT_EQ(blind.error().kind, ErrorKind::Protocol) << body;
    EXPECT_TRUE(has(blind.error().what, "may have been made")) << blind.error().what;
  }
}

TEST(Session, APackageThatMayOrMayNotHaveBeenMade) {
  SignedIn s;
  const std::size_t before = s.bank.store().packages.size();
  // Made, and the answer lost.
  s.bank.drop_next("POST", "/api/management/data-packages");
  const auto dropped = s.session.create_package("In doubt", std::nullopt);
  ASSERT_FALSE(dropped);
  EXPECT_EQ(dropped.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(request_may_have_arrived(dropped.error()));
  EXPECT_EQ(dropped.error().what,
            "EarthBank did not answer; the data package 'In doubt' may or may not have been saved. Look for it on "
            "EarthBank before it is sent again.");
  EXPECT_EQ(s.bank.store().packages.size(), before + 1);
  // Not made, and the client cannot tell: the same words.
  s.bank.lose_next("POST", "/api/management/data-packages", ErrorKind::Io);
  const auto lost = s.session.create_package("In doubt", std::nullopt);
  ASSERT_FALSE(lost);
  EXPECT_EQ(lost.error().kind, ErrorKind::Io);
  EXPECT_TRUE(has(lost.error().what, "may or may not have been saved"));
  // Never sent: it was not made.
  s.bank.refuse_next("POST", "/api/management/data-packages");
  const auto refused = s.session.create_package("Not sent", std::nullopt);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, kHttpNotSent);
  EXPECT_EQ(refused.error().what,
            "EarthBank could not be reached, and the data package 'Not sent' was not sent. Check the network and the "
            "base URL of the profile.");
  // Refused with an answer: it was not made either.
  s.bank.fail_next("POST", "/api/management/data-packages", 400, R"({"title":"Bad Request","detail":"name too long"})");
  const auto bad = s.session.create_package("Refused", std::nullopt);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().code, kRefused);
  EXPECT_EQ(bad.error().what, "EarthBank refused the data package 'Refused': name too long");
  EXPECT_EQ(s.bank.store().packages.size(), before + 1);
  // Nothing was sent a second time.
  EXPECT_EQ(s.bank.count("POST", "/api/management/data-packages"), 4u);
}

// ---- Secrets ---------------------------------------------------------------

// Every way a call can fail, with a login and around one: no message, no
// error and no log line may hold the token, the header it travels in, or the
// password.
TEST(Session, NoTokenInAnyErrorOrLog) {
  FakeEarthBank bank;
  Spy spy(bank);
  Session session(spy, bank.base_url());
  std::vector<std::string> said;
  session.set_log([&](std::string_view line) { said.emplace_back(line); });
  const auto note = [&](const Error& error) {
    said.push_back(error.what);
    said.push_back(to_string(error));
    said.push_back(error.device);
    said.push_back(error.code);
  };
  const std::string wrong = "a-wrong-password-9731";

  // A wrong password, and a login that gets no answer or an odd one.
  if (const auto in = session.sign_in(bank.login(), wrong); !in) note(in.error());
  for (const int status : {401, 403, 500, 502}) {
    bank.fail_next("POST", "/api/authenticate", status, R"({"detail":"said of )" + wrong + R"("})");
    if (const auto in = session.sign_in(bank.login(), wrong); !in) note(in.error());
  }
  bank.lose_next("POST", "/api/authenticate");
  if (const auto in = session.sign_in(bank.login(), bank.password()); !in) note(in.error());
  bank.drop_next("POST", "/api/authenticate", ErrorKind::Io);
  if (const auto in = session.sign_in(bank.login(), bank.password()); !in) note(in.error());
  bank.refuse_next("POST", "/api/authenticate");
  if (const auto in = session.sign_in(bank.login(), bank.password()); !in) note(in.error());
  bank.fail_next("POST", "/api/authenticate", 200, R"({"id_token":17})");
  if (const auto in = session.sign_in(bank.login(), bank.password()); !in) note(in.error());

  ASSERT_TRUE(session.sign_in(bank.login(), bank.password()));
  ASSERT_FALSE(spy.token().empty());

  // Each kind of call, failing each way. A body that repeats the token back
  // (no service should) is the hardest case: it goes to the log and, cut
  // down, into the sentence.
  const std::vector<std::pair<const char*, const char*>> routes = {
      {"GET", "/api/account"},
      {"GET", "/api/management/data-packages"},
      {"POST", "/api/management/data-packages"},
      {"GET", "/api/core/l-sample-kinds"},  // a list that wants a login
      {"POST", "/api/arar/ArArMeasurement"},
  };
  const auto call = [&](std::size_t route) {
    switch (route) {
      case 0:
        if (const auto r = session.account(); !r) note(r.error());
        break;
      case 1:
        if (const auto r = session.packages(9301); !r) note(r.error());
        break;
      case 2:
        if (const auto r = session.create_package("A package", 9301); !r) note(r.error());
        break;
      case 3: {
        const auto r = session.send(get(session, "/api/core/l-sample-kinds?name.equals=Ore"), false);
        // What a caller of send() makes of a failure, as the lookup does.
        if (!r) {
          note(r.error());
          note(session.failure(r.error(), "/api/core/l-sample-kinds", "", false));
        } else if (r->status != 200) {
          note(session.failure(*r, "/api/core/l-sample-kinds", ""));
        }
        break;
      }
      default: {
        const auto r = session.send(post(session, "/api/arar/ArArMeasurement", {{"arArDataPointId", 1}}), true);
        // And as the upload will.
        if (!r) {
          note(r.error());
          note(session.failure(r.error(), "/api/arar/ArArMeasurement", "measurement 20001-01A", true));
        } else if (r->status != 200) {
          note(session.failure(*r, "/api/arar/ArArMeasurement", "measurement 20001-01A"));
        }
      }
    }
  };
  for (std::size_t route = 0; route < routes.size(); ++route) {
    const auto [method, path] = routes[route];
    const auto echo = [&](int status) {
      bank.fail_next(method, path, status,
                     json{{"title", "Failed"},
                          {"detail", "token " + spy.token() + " and password " + bank.password()}}
                         .dump());
    };
    // Twice 401: the second, after a good new login, is the error.
    echo(401);
    echo(401);
    call(route);
    echo(403);
    call(route);
    echo(404);
    call(route);
    echo(400);
    call(route);
    echo(500);
    call(route);
    // Not JSON at all.
    bank.fail_next(method, path, 502, "<html>" + spy.token() + "</html>");
    call(route);
    bank.fail_next(method, path, 200, "not json " + spy.token());
    call(route);
    bank.lose_next(method, path);
    call(route);
    bank.drop_next(method, path, ErrorKind::Io);
    call(route);
    bank.refuse_next(method, path);
    call(route);
    // The token is no longer good and the new login fails.
    bank.expire_tokens();
    bank.lose_next("POST", "/api/authenticate");
    call(route);
    bank.expire_tokens();
    bank.fail_next("POST", "/api/authenticate", 500, R"({"detail":")" + bank.password() + R"("})");
    call(route);
    EXPECT_TRUE(bank.pending().empty()) << method << " " << path << ": " << ::testing::PrintToString(bank.pending());
  }
  // And the password changed under the session.
  bank.set_password("changed-elsewhere-5512");
  call(0);
  call(1);

  ASSERT_GT(spy.secrets().size(), 4u);
  ASSERT_GT(said.size(), 100u);
  std::vector<std::string> secrets = spy.secrets();
  secrets.push_back("not-a-real-password");
  secrets.push_back(wrong);
  secrets.push_back("Bearer ");
  secrets.push_back("Authorization");
  for (const std::string& line : said) {
    for (const std::string& secret : secrets) EXPECT_FALSE(has(line, secret)) << line;
  }
  // The fake's own log holds none either (it redacts; this is the check that
  // nothing went into a query or a path).
  for (const LoggedRequest& request : bank.requests()) {
    const std::string printed = ::testing::PrintToString(request);
    for (const std::string& secret : spy.secrets()) EXPECT_FALSE(has(printed, secret)) << printed;
  }
}

// The service's words are scrubbed before they are cut, and only they are:
// a secret that holds the character a reason is cut at, or lies across the
// place it is cut, leaves no part of itself, and one that is a word of the
// sentence does not show where.
TEST(Session, ASecretIsTakenOutBeforeTheReasonIsCut) {
  const auto with_password = [](const std::string& password) {
    FakeEarthBank::Options options;
    options.password = password;
    return options;
  };
  {
    // `;` is where the machine's part of a reason is cut off.
    FakeEarthBank bank(with_password("Zq7pre;Zq7post"));
    Session session(bank, bank.base_url());
    std::vector<std::string> lines;
    session.set_log([&](std::string_view line) { lines.emplace_back(line); });
    ASSERT_TRUE(session.sign_in(bank.login(), "Zq7pre;Zq7post"));
    for (const char* key : {"detail", "message", "title"}) {
      bank.fail_next("GET", "/api/account", 400, json{{key, "no Zq7pre;Zq7post here"}}.dump());
      const auto account = session.account();
      ASSERT_FALSE(account);
      EXPECT_EQ(account.error().what, "EarthBank refused the account: no <redacted> here") << key;
    }
    // Not JSON: the text as it came.
    bank.fail_next("GET", "/api/account", 400, "no Zq7pre;Zq7post here");
    EXPECT_EQ(session.account().error().what, "EarthBank refused the account: no <redacted> here");
    // At the login, a password that is being tried and is not yet the session's.
    bank.fail_next("POST", "/api/authenticate", 400, R"({"detail":"Zq7pre;Zq7post is too short"})");
    const auto in = session.sign_in(bank.login(), "Zq7pre;Zq7post");
    ASSERT_FALSE(in);
    EXPECT_EQ(in.error().what, "EarthBank refused the login: <redacted> is too short");
    ASSERT_EQ(lines.size(), 6u);
    for (const std::string& line : lines) EXPECT_FALSE(has(line, "Zq7")) << line;
  }
  {
    // Across the place a long reason is cut: no first half of it is left.
    const std::string password = "Zq7-straddles-the-cut-Zq7";
    FakeEarthBank bank(with_password(password));
    Session session(bank, bank.base_url());
    std::vector<std::string> lines;
    session.set_log([&](std::string_view line) { lines.emplace_back(line); });
    ASSERT_TRUE(session.sign_in(bank.login(), password));
    for (std::size_t before = 270; before <= 305; ++before) {
      bank.fail_next("GET", "/api/account", 400, json{{"detail", std::string(before, 'x') + " " + password}}.dump());
      const auto account = session.account();
      ASSERT_FALSE(account);
      EXPECT_FALSE(has(account.error().what, "Zq7")) << before << ": " << account.error().what;
      EXPECT_LT(account.error().what.size(), 400u);
    }
    for (const std::string& line : lines) EXPECT_FALSE(has(line, "Zq7")) << line;
  }
  {
    // A password that is a word of the sentence: our own words stand as they
    // are, wherever the service did not say it.
    FakeEarthBank bank(with_password("login"));
    Session session(bank, bank.base_url());
    ASSERT_TRUE(session.sign_in(bank.login(), "login"));
    const Error quiet = session.failure(HttpResponse{401, ""}, "/api/arar/ArArAliquot", "");
    EXPECT_TRUE(has(quiet.what, "login")) << quiet.what;
    EXPECT_FALSE(has(quiet.what, "<redacted>")) << quiet.what;
    // Nor does the caller's word for the record show it.
    const Error named = session.failure(HttpResponse{418, ""}, "/api/x", "the login sheet");
    EXPECT_EQ(named.what, "EarthBank refused the the login sheet (HTTP 418).");
    const Error echoed = session.failure(HttpResponse{400, R"({"detail":"bad login given"})"}, "/api/authenticate", "");
    EXPECT_EQ(echoed.what, "EarthBank refused the login: bad <redacted> given");
  }
  {
    // One that is a part of the mark itself is not found inside a mark.
    FakeEarthBank bank(with_password("red"));
    Spy spy(bank);
    Session session(spy, bank.base_url());
    ASSERT_TRUE(session.sign_in(bank.login(), "red"));
    const std::string body = json{{"detail", "red and " + spy.token() + " and red"}}.dump();
    EXPECT_EQ(session.failure(HttpResponse{400, body}, "/api/x", "").what,
              "EarthBank refused the request: <redacted> and <redacted> and <redacted>");
  }
}

// What a caller of send() makes an Error of: the sentence, the code, and for
// no answer the client's own kind, so that a write in doubt stays one.
TEST(Session, AFailedSendAsAnError) {
  SignedIn s;
  Spy spy(s.bank);
  Session session(spy, s.bank.base_url());
  ASSERT_TRUE(session.sign_in(s.bank.login(), s.bank.password()));
  const std::string body = R"({"detail":"no room for )" + spy.token() + R"("})";

  const Error refused = session.failure(HttpResponse{400, body}, "/api/arar/ArArAliquot", "aliquot 20001-01");
  EXPECT_EQ(refused.kind, ErrorKind::Protocol);
  EXPECT_EQ(refused.code, kRefused);
  EXPECT_EQ(refused.what, "EarthBank refused the aliquot 20001-01: no room for <redacted>");
  const Error login = session.failure(HttpResponse{403, ""}, "/api/arar/ArArAliquot", "");
  EXPECT_EQ(login.kind, ErrorKind::NotConnected);
  EXPECT_EQ(login.code, kLoginRejected);

  Error timeout{ErrorKind::Timeout, "no answer to POST /api/arar/ArArAliquot within 30000 ms", ""};
  const Error doubt = session.failure(timeout, "/api/arar/ArArAliquot", "aliquot 20001-01", true);
  EXPECT_EQ(doubt.kind, ErrorKind::Timeout);
  EXPECT_TRUE(doubt.code.empty());
  EXPECT_TRUE(request_may_have_arrived(doubt));
  EXPECT_EQ(doubt.what, Session::sentence(timeout, "/api/arar/ArArAliquot", "aliquot 20001-01", true));
  Error unsent{ErrorKind::Io, "connection refused", ""};
  unsent.code = std::string(kHttpNotSent);
  const Error never = session.failure(unsent, "/api/arar/ArArAliquot", "", true);
  EXPECT_EQ(never.code, kHttpNotSent);
  EXPECT_FALSE(request_may_have_arrived(never));
  // An Error that is already an answer's keeps its sentence.
  EXPECT_EQ(session.failure(refused, "/api/x", "", true).what, refused.what);
  EXPECT_EQ(session.failure(refused, "/api/x", "", true).code, kRefused);
}

TEST(Session, TheLogSaysMethodPathAndStatus) {
  FakeEarthBank bank;
  Session session(bank, bank.base_url());
  std::vector<std::string> said;
  session.set_log([&](std::string_view line) { said.emplace_back(line); });
  ASSERT_TRUE(session.sign_in(bank.login(), bank.password()));
  ASSERT_TRUE(session.packages(9301));
  bank.fail_next("GET", "/api/account", 500, R"({"title":"Internal Server Error","detail":"the database is away"})");
  EXPECT_FALSE(session.account());
  bank.lose_next("GET", "/api/account");
  EXPECT_FALSE(session.account());
  bank.refuse_next("GET", "/api/account");
  EXPECT_FALSE(session.account());
  EXPECT_EQ(said, (std::vector<std::string>{
                      "POST /api/authenticate -> 200",
                      // Never the query: it holds names.
                      "GET /api/management/data-packages -> 200",
                      // What the service said of a failure is for the log.
                      "GET /api/account -> 500: the database is away",
                      "GET /api/account: no answer (timeout)",
                      "GET /api/account: not sent (io)",
                  }));
}

// ---- Sentences -------------------------------------------------------------

TEST(Session, Sentences) {
  SignedIn s;
  const auto answer_to = [&](const HttpRequest& request) {
    const auto answer = s.session.send(request, true);
    if (!answer) throw std::runtime_error(to_string(answer.error()));
    return *answer;
  };

  // "...does not have write access to data package...": a package that is
  // somebody else's, as the fake answers it (a 500 that is no server fault).
  const HttpResponse unwritable =
      answer_to(post(s.session, "/api/core/sample-with-locations", sample_body(s.bank.read_only_package(), "S-1")));
  ASSERT_EQ(unwritable.status, 500);
  EXPECT_EQ(said(s.session, unwritable.status, unwritable.body, "/api/core/sample-with-locations", "sample S-1"),
            "EarthBank did not take the sample S-1: this account may not write to the data package. Choose a package "
            "the account may write to, or make a new one, and send it again.");
  // Without a word for the record, the path says what it was.
  EXPECT_EQ(said(s.session, unwritable.status, unwritable.body, "/api/core/sample-with-locations", ""),
            "EarthBank did not take the sample: this account may not write to the data package. Choose a package "
            "the account may write to, or make a new one, and send it again.");

  // "...no data package has been selected.": nothing is sent for it.
  EXPECT_EQ(Session::no_package_sentence(),
            "No data package is chosen, and EarthBank keeps every record in one. Choose a package, or make one, "
            "before anything is sent.");

  // "...rejected your login...": at the login, and at anything after it.
  const HttpResponse bad_login = *s.bank.send(post(s.session, "/api/authenticate", {{"username", "x"}, {"password", "y"}}));
  ASSERT_EQ(bad_login.status, 401);
  EXPECT_EQ(said(s.session, bad_login.status, bad_login.body, "/api/authenticate", ""),
            "EarthBank did not accept the user name and password.");
  EXPECT_EQ(said(s.session, 401, kUnauthorized, "/api/arar/ArArAliquot", "aliquot 20001-01"),
            "EarthBank refused the login (aliquot 20001-01). Check the user name and the password, and that the "
            "account has the right to do this.");
  EXPECT_EQ(said(s.session, 403, "", "/api/account", ""),
            "EarthBank refused the login (account). Check the user name and the password, and that the account has "
            "the right to do this.");

  // "...could not find the upload service... check the Base URL...".
  const HttpResponse nowhere = answer_to(post(s.session, "/api/arar/NoSuchRecord", json::object()));
  ASSERT_EQ(nowhere.status, 404);
  EXPECT_EQ(said(s.session, nowhere.status, nowhere.body, "/api/arar/NoSuchRecord", "measurement 1"),
            "EarthBank has nothing at /api/arar/NoSuchRecord: the address is probably wrong. Check the base URL of "
            "the profile.");
  // A whole URL is cut to its path: a query holds names.
  EXPECT_EQ(said(s.session, 404, "", "https://earthbank.test/wrong/api/account?name.equals=A%20Sample", ""),
            "EarthBank has nothing at /wrong/api/account: the address is probably wrong. Check the base URL of the "
            "profile.");

  // "...did not recognise one of the values...": an id that is no row of its
  // list, as the fake answers it, whichever status it comes with.
  const std::int64_t package = s.bank.writable_package();
  const HttpResponse sample = answer_to(post(s.session, "/api/core/sample-with-locations", sample_body(package, "S-2")));
  ASSERT_EQ(sample.status, 201);
  const std::int64_t sample_id = json::parse(sample.body).at("sampleDTO").at("id").get<std::int64_t>();
  const json data_point = {{"dataPointDTO",
                            {{"dataStructure", "ARARDATAPOINT"},
                             {"dataPackageId", package},
                             {"sampleId", sample_id},
                             {"name", "S-2 sanidine"}}},
                           {"extendingDataPointDTO", {{"mineralId", 5}, {"analysisUnits", "fA"}}}};
  for (const int status : {400, 500}) {
    s.bank.constraint_status(status);
    const HttpResponse refused = answer_to(post(s.session, "/api/arar/ArArDataPoint", data_point));
    ASSERT_EQ(refused.status, status) << refused.body;
    EXPECT_EQ(said(s.session, refused.status, refused.body, "/api/arar/ArArDataPoint", ""),
              "EarthBank did not take the data point: it does not know the value sent for 'mineral'. The term is "
              "probably not on EarthBank's list; a pre-flight check names such terms before anything is sent.")
        << status;
  }
  // The field out of the constraint's name, where no key is given; and no
  // field, where neither is.
  EXPECT_EQ(said(s.session, 400, R"({"detail":"violates foreign key constraint \"fk_measurement_plateau_step_id\""})",
                              "/api/arar/ArArMeasurement", "measurement 20001-01A"),
            "EarthBank did not take the measurement 20001-01A: it does not know the value sent for 'plateau step'. "
            "The term is probably not on EarthBank's list; a pre-flight check names such terms before anything is "
            "sent.");
  EXPECT_EQ(said(s.session, 500, "violates a foreign key", "/api/arar/ArArAgeCalc", ""),
            "EarthBank did not take the age calculation: it does not know one of the values sent. The term is "
            "probably not on EarthBank's list; a pre-flight check names such terms before anything is sent.");
  // A constraint on the package's own column is still a value not known, not
  // a package that cannot be written to.
  EXPECT_TRUE(has(said(s.session, 500, R"({"detail":"Key (data_package_id)=(9) is not present in table \"data_package\""})",
                                    "/api/core/sample-with-locations", ""),
                  "the value sent for 'data package'"));

  // "...was partly uploaded...".
  const std::vector<std::string> missing = {"aliquot 20001-02", "measurement 20001-02A", "age summary"};
  EXPECT_EQ(Session::partial_sentence("S-2", missing),
            "S-2 is only partly on EarthBank: its data point was saved and 3 records under it were not (aliquot "
            "20001-02, measurement 20001-02A, age summary). Send it again to add what is missing.");
  EXPECT_EQ(Session::partial_sentence("S-2", std::vector<std::string>{"age summary"}),
            "S-2 is only partly on EarthBank: its data point was saved and 1 record under it was not (age summary). "
            "Send it again to add what is missing.");

  // "Could not reach EarthBank...": the request never left.
  s.bank.refuse_next("GET", "/api/account");
  const auto unreachable = s.session.send(get(s.session, "/api/account"), true);
  ASSERT_FALSE(unreachable);
  for (const bool write : {false, true}) {
    EXPECT_EQ(Session::sentence(unreachable.error(), "/api/account", "", write),
              "EarthBank could not be reached, and the account was not sent. Check the network and the base URL of "
              "the profile.");
  }

  // Beyond the guide's table. No answer to a write: in doubt, and no word
  // that sends the user to try again blind.
  for (const ErrorKind kind : {ErrorKind::Timeout, ErrorKind::Io}) {
    s.bank.drop_next("POST", "/api/arar/ArArMeasurement", kind);
    const auto silent = s.session.send(post(s.session, "/api/arar/ArArMeasurement", json::object()), true);
    ASSERT_FALSE(silent);
    const std::string doubt =
        Session::sentence(silent.error(), "/api/arar/ArArMeasurement", "measurement 20001-01A", true);
    EXPECT_EQ(doubt, "EarthBank did not answer; the measurement 20001-01A may or may not have been saved. Look for it "
                     "on EarthBank before it is sent again.");
    for (const char* blind : {"try again", "Try again", "retry", "Retry", "re-run", "again later"}) {
      EXPECT_FALSE(has(doubt, blind)) << doubt;
    }
    // The same on a question: nothing was at stake.
    EXPECT_EQ(Session::sentence(silent.error(), "/api/core/materials", "", false),
              "EarthBank did not answer (list of terms). Check the network and try again.");
  }
  // A request that could not be formed.
  EXPECT_EQ(Session::sentence(Error{ErrorKind::Config, "not a URL", ""}, "/api/account", "", true),
            "The request for the account could not be made, and nothing was sent: the base URL of the profile is "
            "probably not a web address.");
  // Not signed in.
  const auto early = Session(s.bank, s.bank.base_url()).account();
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error().what, "Not signed in to EarthBank: the account was not sent.");

  // A fault of the service's own; a redirect; and what else it may refuse,
  // with its reason cut where the machine's part begins.
  EXPECT_EQ(said(s.session, 503, "<html>Service Unavailable</html>", "/api/arar/ArArAgeSummary", ""),
            "EarthBank failed on its own side at the age summary (HTTP 503). Try again later; if it goes on, tell "
            "EarthBank's support.");
  EXPECT_EQ(said(s.session, 302, "", "/api/account", ""),
            "EarthBank answered the account with a redirect (HTTP 302): the base URL of the profile is probably not "
            "the service's own address.");
  EXPECT_EQ(said(s.session, 400, R"({"title":"Bad Request","detail":"aliquotSize must be a number; nested exception is x"})",
                              "/api/arar/ArArAliquot", ""),
            "EarthBank refused the aliquot: aliquotSize must be a number");
  EXPECT_EQ(said(s.session, 418, "", "/api/arar/ArArAliquot", ""), "EarthBank refused the aliquot (HTTP 418).");
  // The reason is read from `detail`, then `message`, then `title`, then the
  // text as it came (legacy).
  EXPECT_EQ(said(s.session, 400, R"({"title":"T","message":"M","detail":"D"})", "/api/x", ""),
            "EarthBank refused the request: D");
  EXPECT_EQ(said(s.session, 400, R"({"title":"T","message":"M","detail":""})", "/api/x", ""),
            "EarthBank refused the request: M");
  EXPECT_EQ(said(s.session, 400, R"({"title":"T","detail":null})", "/api/x", ""), "EarthBank refused the request: T");
  EXPECT_EQ(said(s.session, 400, "plainly no", "/api/x", ""), "EarthBank refused the request: plainly no");
  EXPECT_EQ(said(s.session, 400, R"(["a list"])", "/api/x", ""), "EarthBank refused the request (HTTP 400).");
  // "Package is not writable" under any of the three keys, and as plain text.
  for (const char* body : {R"({"title":"Package is not writable by user."})", R"({"detail":"Package is not writable by user."})",
                           R"({"message":"package is NOT WRITABLE"})", "Package is not writable by user."}) {
    EXPECT_TRUE(has(said(s.session, 500, body, "/api/arar/ArArAliquot", ""), "may not write to the data package")) << body;
  }
  // A reason of any length is cut, and holds no line break.
  const std::string endless = said(s.session, 400, std::string(5000, 'x') + "\nmore", "/api/x", "");
  EXPECT_LT(endless.size(), 400u);
  EXPECT_FALSE(has(endless, "\n"));
  // An answer that is no failure has no sentence.
  EXPECT_EQ(said(s.session, 200, "", "/api/account", ""), "");
  EXPECT_EQ(said(s.session, 201, "{}", "/api/account", ""), "");
}

// What the path says a record is, when the caller has no better word.
TEST(Session, ARecordIsNamedByItsPath) {
  SignedIn s;
  const auto what = [&](std::string_view path) {
    const std::string sentence = said(s.session, 418, "", path, "");
    const std::string_view head = "EarthBank refused the ", tail = " (HTTP 418).";
    return sentence.substr(head.size(), sentence.size() - head.size() - tail.size());
  };
  EXPECT_EQ(what("/api/core/sample-with-locations"), "sample");
  EXPECT_EQ(what("/api/arar/ArArDataPoint"), "data point");
  EXPECT_EQ(what("/api/arar/ArArAliquot"), "aliquot");
  EXPECT_EQ(what("/api/arar/ArArMeasurement"), "measurement");
  EXPECT_EQ(what("/api/arar/ArArAgeCalc"), "age calculation");
  EXPECT_EQ(what("/api/arar/ArArAgeSummary"), "age summary");
  EXPECT_EQ(what("/api/management/data-packages"), "data package");
  EXPECT_EQ(what("/api/management/data-packages/610001"), "data package");
  EXPECT_EQ(what("/api/authenticate"), "login");
  EXPECT_EQ(what("/api/account"), "account");
  EXPECT_EQ(what("/api/arar/FluxMonitor"), "list of terms");
  EXPECT_EQ(what("/api/core/l-sample-kinds"), "list of terms");
  EXPECT_EQ(what("https://earthbank.test/api/core/materials?name.equals=x"), "list of terms");
  EXPECT_EQ(what("/api/other"), "request");
  // A service kept under a path of its own.
  EXPECT_EQ(what("https://earthbank.test/geochem/api/account"), "account");
  EXPECT_EQ(what("/geochem/api/arar/AirRatio"), "list of terms");
  EXPECT_EQ(said(s.session, 401, "", "https://earthbank.test/geochem/api/authenticate", ""),
            "EarthBank did not accept the user name and password.");
}

}  // namespace
}  // namespace pychron::earthbank
