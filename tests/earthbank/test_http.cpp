#include <chrono>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "pychron/core/error.hpp"
#include "pychron/core/http.hpp"
#include "pychron/earthbank/http.hpp"

namespace pychron::earthbank {
namespace {

using namespace std::string_view_literals;

static_assert(std::is_same_v<IHttpClient, pychron::http::IHttpClient>);
static_assert(std::is_same_v<HttpRequest, pychron::http::HttpRequest>);
static_assert(std::is_same_v<HttpResponse, pychron::http::HttpResponse>);

TEST(Http, UrlEncode) {
  EXPECT_EQ(url_encode(""), "");
  EXPECT_EQ(url_encode("ABCXYZabcxyz0189-._~"), "ABCXYZabcxyz0189-._~");
  EXPECT_EQ(url_encode("Fish Canyon"), "Fish%20Canyon");
  EXPECT_EQ(url_encode("Spurr & Washington"), "Spurr%20%26%20Washington");
  EXPECT_EQ(url_encode("(Fe2+)"), "%28Fe2%2B%29");
  EXPECT_EQ(url_encode("95% confidence"), "95%25%20confidence");
  EXPECT_EQ(url_encode("Borehole/well"), "Borehole%2Fwell");
  EXPECT_EQ(url_encode("a=b?c#d"), "a%3Db%3Fc%23d");
  EXPECT_EQ(url_encode("\"plutonic\" <rock>"), "%22plutonic%22%20%3Crock%3E");
}

// The bytes of the UTF-8, one escape each: a byte above 0x7F is not a
// negative number on the way.
TEST(Http, UrlEncodeIsOfBytes) {
  EXPECT_EQ(url_encode("\xC3\xA9"), "%C3%A9");                             // é
  EXPECT_EQ(url_encode("Chabourn\xC3\xA9ite"), "Chabourn%C3%A9ite");
  EXPECT_EQ(url_encode("\xE2\x81\xB4\xE2\x81\xB0" "Ar"), "%E2%81%B4%E2%81%B0" "Ar");  // ⁴⁰Ar
  EXPECT_EQ(url_encode("\xF0\x9F\x8C\x8B"), "%F0%9F%8C%8B");               // four bytes
  EXPECT_EQ(url_encode("\xFF\x80"), "%FF%80");                             // not UTF-8 at all
  EXPECT_EQ(url_encode("a\0b"sv), "a%00b");
  EXPECT_EQ(url_encode("\t\r\n"), "%09%0D%0A");
}

TEST(Http, WithQuery) {
  EXPECT_EQ(with_query("https://earthbank.test/api/arar/FluxMonitor", {}),
            "https://earthbank.test/api/arar/FluxMonitor");
  EXPECT_EQ(with_query("https://earthbank.test/api/core/materials", {{"name.equals", "Sanidine"}, {"size", "1"}}),
            "https://earthbank.test/api/core/materials?name.equals=Sanidine&size=1");
  EXPECT_EQ(with_query("/api/core/materials", {{"name.contains", "Spurr & Washington"}}),
            "/api/core/materials?name.contains=Spurr%20%26%20Washington");
  // A value that is empty is still a parameter.
  EXPECT_EQ(with_query("/x", {{"a", ""}, {"b", "1"}}), "/x?a=&b=1");
  // A key is encoded as a value is.
  EXPECT_EQ(with_query("/x", {{"a b", "c+d"}}), "/x?a%20b=c%2Bd");
  // A url that has a query already gets the rest of one.
  EXPECT_EQ(with_query("/x?page=0", {{"size", "20"}}), "/x?page=0&size=20");
  EXPECT_EQ(with_query("/x?", {{"size", "20"}}), "/x?size=20");
  EXPECT_EQ(with_query("/x?page=0", {}), "/x?page=0");
  // A fragment stays last: the query goes in before it.
  EXPECT_EQ(with_query("/x#top", {{"size", "20"}}), "/x?size=20#top");
  EXPECT_EQ(with_query("/x?page=0#top", {{"size", "20"}, {"a", "b"}}), "/x?page=0&size=20&a=b#top");
  EXPECT_EQ(with_query("https://earthbank.test/x#a?b", {{"size", "20"}}), "https://earthbank.test/x?size=20#a?b");
  EXPECT_EQ(with_query("/x#top", {}), "/x#top");
}

TEST(Http, UrlPath) {
  EXPECT_EQ(url_path("https://earthbank.test/api/account"), "/api/account");
  EXPECT_EQ(url_path("https://earthbank.test/api/core/materials?name.equals=a/b"), "/api/core/materials");
  EXPECT_EQ(url_path("http://127.0.0.1:8080/api/account#top"), "/api/account");
  EXPECT_EQ(url_path("https://earthbank.test"), "/");
  EXPECT_EQ(url_path("https://earthbank.test?x=1"), "/");
  EXPECT_EQ(url_path("https://earthbank.test/"), "/");
  EXPECT_EQ(url_path("/api/account?x=1"), "/api/account");
  EXPECT_EQ(url_path(""), "/");
  // `://` is a scheme's only before the path, the query and the fragment: a
  // URL inside a query is not the URL.
  EXPECT_EQ(url_path("/x?u=http://h/secret"), "/x");
  EXPECT_EQ(url_path("/a/b://c/d"), "/a/b://c/d");
  EXPECT_EQ(url_path("x#see://h/secret"), "x");
  EXPECT_EQ(url_path("https://earthbank.test/x?u=http://h/secret"), "/x");
}

TEST(Http, FindHeaderIgnoresCase) {
  HttpRequest request;
  request.headers = {{"Content-Type", "application/json"}, {"authorization", "Bearer abc"}};
  ASSERT_TRUE(find_header(request, "content-type").has_value());
  EXPECT_EQ(*find_header(request, "content-type"), "application/json");
  EXPECT_EQ(*find_header(request, "Authorization"), "Bearer abc");
  EXPECT_FALSE(find_header(request, "Accept").has_value());
  EXPECT_FALSE(find_header(request, "Content").has_value());
}

// What goes into a log: the method and the path. Not the host, not the query
// (it holds names), and above all nothing of the headers or the body.
TEST(Http, TheRequestLineHoldsNoSecret) {
  HttpRequest request;
  request.method = "POST";
  request.url = "https://earthbank.test/api/authenticate?token=QUERYSECRET";
  request.headers = {{"Authorization", "Bearer HEADERSECRET"}};
  request.body = R"({"username":"someone","password":"BODYSECRET"})";
  const std::string line = request_line(request);
  EXPECT_EQ(line, "POST /api/authenticate");
  for (const std::string_view secret : {"QUERYSECRET"sv, "HEADERSECRET"sv, "BODYSECRET"sv, "someone"sv}) {
    EXPECT_EQ(line.find(secret), std::string::npos) << secret;
  }
}

TEST(Http, ARequestWaitsThirtySecondsUnlessTold) {
  EXPECT_EQ(HttpRequest{}.timeout, std::chrono::milliseconds(30000));
  EXPECT_EQ(HttpResponse{}.status, 0);
}

// An answer that never came is not a refusal: the request may have been
// carried out. Only a client that knows nothing left says otherwise.
TEST(Http, AnErrorSaysWhetherTheRequestMayHaveArrived) {
  EXPECT_TRUE(request_may_have_arrived(Error{ErrorKind::Timeout, "no answer in 30000 ms", ""}));
  EXPECT_TRUE(request_may_have_arrived(Error{ErrorKind::Io, "connection closed", ""}));
  EXPECT_TRUE(request_may_have_arrived(Error{ErrorKind::Protocol, "half a reply", ""}));
  Error refused{ErrorKind::Io, "connection refused", ""};
  refused.code = std::string(kHttpNotSent);
  EXPECT_FALSE(request_may_have_arrived(refused));
  // The connection was never made within the time: a timeout, and not sent.
  Error connect{ErrorKind::Timeout, "no connection in 30000 ms", ""};
  connect.code = std::string(kHttpNotSent);
  EXPECT_FALSE(request_may_have_arrived(connect));
  // A request that could not be formed went nowhere.
  EXPECT_FALSE(request_may_have_arrived(Error{ErrorKind::Config, "not a URL", ""}));
  EXPECT_TRUE(request_may_have_arrived(Error{ErrorKind::Cancelled, "stopped while waiting", ""}));
}

}  // namespace
}  // namespace pychron::earthbank
