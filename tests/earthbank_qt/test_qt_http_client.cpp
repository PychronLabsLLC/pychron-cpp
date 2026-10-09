// QtHttpClient against a server in this process: a QTcpServer bound to
// 127.0.0.1, port 0. The client runs its own event loop inside `send`, and the
// server lives in the same thread, so it is served while the client waits;
// the test thread runs no loop of its own. Plain HTTP only: the TLS test goes
// to a port that speaks no TLS.

#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QtGlobal>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QTcpServer>
#include <QTcpSocket>
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QSslServer>
#define PYCHRON_TEST_HAS_SSL_SERVER 1
#endif

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/qt_http_client.hpp"

namespace pychron::earthbank {
namespace {

using namespace std::chrono_literals;

// One request as the server read it.
struct Seen {
  std::string method, target, version, body;
  std::vector<std::pair<std::string, std::string>> headers;

  std::string header(const std::string& name) const {
    for (const auto& [key, value] : headers) {
      if (key.size() != name.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < key.size(); ++i)
        same = same && std::tolower(static_cast<unsigned char>(key[i])) == std::tolower(static_cast<unsigned char>(name[i]));
      if (same) return value;
    }
    return {};
  }
};

// What the server does with a request: the whole reply as bytes, or empty to
// say nothing and keep the connection open, or `close` to hang up.
struct Reply {
  std::string bytes;
  bool close = false;
};

std::string response(int status, const std::string& reason, const std::string& body,
                     const std::string& more_headers = {}) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n" + more_headers + "\r\n" + body;
}

// The process's QCoreApplication, made once for the server's sake (a QTcpServer
// wants one). The client does not: ClientMakesItsOwnApplication runs before
// any of this.
void ensure_application() {
  static int argc = 1;
  static char name[] = "test_qt_http_client";
  static char* argv[] = {name, nullptr};
  static std::unique_ptr<QCoreApplication> app;
  if (QCoreApplication::instance() == nullptr) app = std::make_unique<QCoreApplication>(argc, argv);
}

class Server {
 public:
  // `hang_up`: closes every connection on its first bytes, as a server that
  // does not speak what it is spoken to.
  explicit Server(std::function<Reply(const Seen&)> handler, bool hang_up = false)
      : handler_(std::move(handler)), hang_up_(hang_up) {
    ensure_application();
    QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
      while (QTcpSocket* socket = server_.nextPendingConnection()) {
        ++connections_;
        sockets_.push_back(socket);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { read(socket); });
      }
    });
    if (!server_.listen(QHostAddress::LocalHost, 0)) ADD_FAILURE() << server_.errorString().toStdString();
  }
  ~Server() {
    for (QTcpSocket* socket : sockets_) socket->abort();
  }

  std::string url(const std::string& target) const {
    return "http://127.0.0.1:" + std::to_string(server_.serverPort()) + target;
  }
  unsigned short port() const { return server_.serverPort(); }
  // Stops listening; the port is then one that refuses.
  void stop() { server_.close(); }
  int connections() const { return connections_; }
  const std::vector<Seen>& seen() const { return seen_; }

 private:
  void read(QTcpSocket* socket) {
    if (hang_up_) {
      socket->abort();
      return;
    }
    QByteArray& buffer = buffers_[socket];
    buffer += socket->readAll();
    const int end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return;
    Seen request;
    const QList<QByteArray> lines = buffer.left(end).split('\n');
    const QList<QByteArray> start = lines.at(0).trimmed().split(' ');
    if (start.size() >= 3) {
      request.method = start.at(0).toStdString();
      request.target = start.at(1).toStdString();
      request.version = start.at(2).toStdString();
    }
    qsizetype length = 0;
    for (qsizetype i = 1; i < lines.size(); ++i) {
      const QByteArray line = lines.at(i).trimmed();
      const qsizetype colon = line.indexOf(':');
      if (colon < 0) continue;
      request.headers.emplace_back(line.left(colon).toStdString(), line.mid(colon + 1).trimmed().toStdString());
      if (line.left(colon).toLower() == "content-length") length = line.mid(colon + 1).trimmed().toLongLong();
    }
    if (buffer.size() < end + 4 + length) return;
    request.body = buffer.mid(end + 4, length).toStdString();
    buffer.clear();
    seen_.push_back(request);
    const Reply reply = handler_(request);
    if (!reply.bytes.empty()) socket->write(reply.bytes.data(), static_cast<qint64>(reply.bytes.size()));
    if (reply.close) socket->disconnectFromHost();
  }

  std::function<Reply(const Seen&)> handler_;
  bool hang_up_;
  QTcpServer server_;
  std::vector<QTcpSocket*> sockets_;
  std::map<QTcpSocket*, QByteArray> buffers_;
  std::vector<Seen> seen_;
  int connections_ = 0;
};

HttpRequest request(std::string method, std::string url) {
  HttpRequest r;
  r.method = std::move(method);
  r.url = std::move(url);
  r.timeout = 10000ms;
  return r;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

// A program with no QCoreApplication (a command-line one, or a worker
// thread's first call) gets one at the first send. Port 1 on this machine
// refuses.
TEST(QtHttpClient, MakesItsOwnApplicationWhenThereIsNone) {
  if (QCoreApplication::instance() != nullptr) GTEST_SKIP() << "another test made the application first; run alone";
  QtHttpClient client;
  const auto answer = client.send(request("GET", "http://127.0.0.1:1/x"));
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, ErrorKind::Io);
  EXPECT_EQ(answer.error().code, kHttpNotSent);
  EXPECT_NE(QCoreApplication::instance(), nullptr);
  // And a second call, with the application there, goes the same way.
  EXPECT_EQ(client.send(request("GET", "http://127.0.0.1:1/x")).error().code, kHttpNotSent);
}

TEST(QtHttpClient, GetPostAndHeaders) {
  Server server([](const Seen& seen) {
    if (seen.method == "POST") return Reply{response(201, "Created", R"({"echo":)" + seen.body + "}")};
    if (seen.target.starts_with("/empty")) return Reply{response(204, "No Content", "")};
    return Reply{response(200, "OK", "hello")};
  });
  QtHttpClient client;

  HttpRequest get = request("GET", server.url("/api/account?size=2&name.equals=Fish%20Canyon"));
  get.headers = {{"Authorization", "Bearer not-a-real-token"}, {"Accept", "application/json"}};
  const auto got = client.send(get);
  ASSERT_TRUE(got) << got.error().what;
  EXPECT_EQ(got->status, 200);
  EXPECT_EQ(got->body, "hello");
  ASSERT_EQ(server.seen().size(), 1u);
  const Seen& first = server.seen()[0];
  EXPECT_EQ(first.method, "GET");
  EXPECT_EQ(first.target, "/api/account?size=2&name.equals=Fish%20Canyon");
  EXPECT_EQ(first.header("Authorization"), "Bearer not-a-real-token");
  EXPECT_EQ(first.header("Accept"), "application/json");
  EXPECT_TRUE(first.header("User-Agent").starts_with("pychron-cpp/")) << first.header("User-Agent");
  EXPECT_GT(first.header("User-Agent").size(), std::string("pychron-cpp/").size());

  HttpRequest post = request("POST", server.url("/api/authenticate"));
  post.headers = {{"Content-Type", "application/json"}};
  post.body = R"({"a":1})";
  const auto posted = client.send(post);
  ASSERT_TRUE(posted) << posted.error().what;
  EXPECT_EQ(posted->status, 201);
  EXPECT_EQ(posted->body, R"({"echo":{"a":1}})");
  ASSERT_EQ(server.seen().size(), 2u);
  EXPECT_EQ(server.seen()[1].method, "POST");
  EXPECT_EQ(server.seen()[1].body, R"({"a":1})");
  EXPECT_EQ(server.seen()[1].header("Content-Type"), "application/json");
  EXPECT_EQ(server.seen()[1].header("Content-Length"), "7");

  // A User-Agent of the request's own stands.
  HttpRequest own = request("GET", server.url("/x"));
  own.headers = {{"User-Agent", "somebody/1"}};
  ASSERT_TRUE(client.send(own));
  EXPECT_EQ(server.seen()[2].header("User-Agent"), "somebody/1");

  // No body in the answer is an answer; no body in a POST is allowed.
  const auto empty = client.send(request("GET", server.url("/empty")));
  ASSERT_TRUE(empty) << empty.error().what;
  EXPECT_EQ(empty->status, 204);
  EXPECT_EQ(empty->body, "");
  HttpRequest bare = request("POST", server.url("/api/nothing"));
  const auto no_body = client.send(bare);
  ASSERT_TRUE(no_body) << no_body.error().what;
  EXPECT_EQ(server.seen().back().body, "");
  EXPECT_EQ(server.seen().back().method, "POST");

  // Each request has a connection of its own.
  EXPECT_EQ(server.connections(), 5);
}

TEST(QtHttpClient, StatusAndBodyOfAnErrorReply) {
  Server server([](const Seen& seen) {
    if (seen.target == "/401") return Reply{response(401, "Unauthorized", R"({"detail":"Full authentication"})")};
    if (seen.target == "/404") return Reply{response(404, "Not Found", "")};
    if (seen.target == "/500") return Reply{response(500, "Internal Server Error", "boom")};
    return Reply{response(503, "Service Unavailable", "try later")};
  });
  QtHttpClient client;
  struct Case {
    const char* target;
    int status;
    const char* body;
  };
  for (const Case& c : {Case{"/401", 401, R"({"detail":"Full authentication"})"}, Case{"/404", 404, ""},
                        Case{"/500", 500, "boom"}, Case{"/other", 503, "try later"}}) {
    for (const char* method : {"GET", "POST", "DELETE"}) {
      const auto answer = client.send(request(method, server.url(c.target)));
      ASSERT_TRUE(answer) << c.target << " " << method << ": " << answer.error().what;
      EXPECT_EQ(answer->status, c.status) << c.target << " " << method;
      EXPECT_EQ(answer->body, c.body) << c.target << " " << method;
    }
  }
}

TEST(QtHttpClient, Timeout) {
  Server server([](const Seen&) { return Reply{}; });
  QtHttpClient client;
  HttpRequest slow = request("GET", server.url("/never?token=not-a-real-secret"));
  slow.headers = {{"Authorization", "Bearer not-a-real-token"}};
  slow.timeout = 300ms;
  const auto start = std::chrono::steady_clock::now();
  const auto answer = client.send(slow);
  const auto took = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, ErrorKind::Timeout) << answer.error().what;
  EXPECT_GE(took, 250ms);
  EXPECT_LT(took, 5000ms);
  // The server had the request, so it may have acted on it.
  EXPECT_EQ(server.seen().size(), 1u);
  EXPECT_TRUE(request_may_have_arrived(answer.error()));
  EXPECT_FALSE(has(answer.error().what, "not-a-real-secret")) << answer.error().what;
  EXPECT_FALSE(has(answer.error().what, "not-a-real-token")) << answer.error().what;
  EXPECT_FALSE(has(answer.error().what, "token=")) << answer.error().what;
}

TEST(QtHttpClient, DoesNotFollowARedirectToAnotherHost) {
  Server server([](const Seen& seen) {
    if (seen.target == "/elsewhere")
      return Reply{response(302, "Found", "", "Location: http://redirected.invalid/stolen?bearer=x\r\n")};
    if (seen.target == "/same") return Reply{response(301, "Moved Permanently", "", "Location: /landed\r\n")};
    return Reply{response(200, "OK", "landed")};
  });
  QtHttpClient client;
  HttpRequest away = request("GET", server.url("/elsewhere"));
  away.headers = {{"Authorization", "Bearer not-a-real-token"}};
  const auto elsewhere = client.send(away);
  ASSERT_TRUE(elsewhere) << elsewhere.error().what;
  EXPECT_EQ(elsewhere->status, 302);
  EXPECT_EQ(server.seen().size(), 1u);
  // Nor one to this host: a 3xx is the answer, whoever it points to.
  const auto same = client.send(request("POST", server.url("/same")));
  ASSERT_TRUE(same) << same.error().what;
  EXPECT_EQ(same->status, 301);
  ASSERT_EQ(server.seen().size(), 2u);
  EXPECT_EQ(server.seen()[1].method, "POST");
}

TEST(QtHttpClient, ARefusedConnectionWasNeverSent) {
  std::string base;
  {
    Server server([](const Seen&) { return Reply{}; });
    base = server.url("");
    server.stop();
  }
  QtHttpClient client;
  HttpRequest r = request("POST", base + "/api/arar/ArArAliquot?name.equals=Secret%20Sample");
  r.headers = {{"Authorization", "Bearer not-a-real-token"}};
  r.body = R"({"password":"not-a-real-password"})";
  const auto answer = client.send(r);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, ErrorKind::Io);
  EXPECT_EQ(answer.error().code, kHttpNotSent);
  EXPECT_FALSE(request_may_have_arrived(answer.error()));
  EXPECT_FALSE(answer.error().what.empty());
  for (const char* secret : {"Secret", "name.equals", "not-a-real-token", "not-a-real-password", "ArArAliquot?"})
    EXPECT_FALSE(has(answer.error().what, secret)) << secret << ": " << answer.error().what;
}

// A connection that is closed on the request, with no answer: the request is
// not sent again, though a client library of the usual kind would.
TEST(QtHttpClient, ARequestThatGetsNoAnswerIsNotSentAgain) {
  Server server([](const Seen&) { return Reply{"", true}; });
  QtHttpClient client;
  for (const char* method : {"POST", "GET"}) {
    const int before = server.connections();
    const auto answer = client.send(request(method, server.url("/api/x")));
    ASSERT_FALSE(answer) << method;
    EXPECT_EQ(answer.error().kind, ErrorKind::Io) << method << ": " << answer.error().what;
    // The server had it: it is not "never sent".
    EXPECT_NE(answer.error().code, kHttpNotSent) << method;
    EXPECT_EQ(server.connections() - before, 1) << method;
  }
  EXPECT_EQ(server.seen().size(), 2u);
}

TEST(QtHttpClient, AnAddressThatIsNoneIsAConfigErrorAndNothingIsSent) {
  Server server([](const Seen&) { return Reply{response(200, "OK", "")}; });
  QtHttpClient client;
  for (const char* url : {"", "not a url", "ftp://127.0.0.1/x", "/api/account", "http://", "file:///etc/passwd"}) {
    const auto answer = client.send(request("GET", url));
    ASSERT_FALSE(answer) << url;
    EXPECT_EQ(answer.error().kind, ErrorKind::Config) << url;
    EXPECT_FALSE(request_may_have_arrived(answer.error())) << url;
  }
  HttpRequest no_method = request("", server.url("/x"));
  EXPECT_EQ(client.send(no_method).error().kind, ErrorKind::Config);
  EXPECT_TRUE(server.seen().empty());
}

// https to a port that speaks no TLS: the handshake fails, and nothing of the
// request left.
TEST(QtHttpClient, AFailedTlsHandshakeWasNeverSent) {
  Server server([](const Seen&) { return Reply{}; }, true);
  QtHttpClient client;
  HttpRequest r = request("GET", "https://127.0.0.1:" + std::to_string(server.port()) + "/api/account?x=y");
  r.timeout = 5000ms;
  const auto answer = client.send(r);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, ErrorKind::Io) << answer.error().what;
  EXPECT_EQ(answer.error().code, kHttpNotSent) << answer.error().what;
  EXPECT_TRUE(server.seen().empty());
  EXPECT_FALSE(has(answer.error().what, "x=y")) << answer.error().what;
}

// The body is read however the server frames it: by chunks, by length, to the
// end of the connection; and an interim answer is not the answer.
TEST(QtHttpClient, ReadsTheBodyHoweverTheServerFramesIt) {
  Server server([](const Seen& seen) {
    const std::string head = "HTTP/1.1 200 OK\r\nConnection: close\r\n";
    if (seen.target == "/chunked")
      return Reply{head + "Transfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n7\r\n, world\r\n0\r\nTrailer: x\r\n\r\n", true};
    if (seen.target == "/to-the-end") return Reply{head + "\r\nuntil the end", true};
    if (seen.target == "/interim")
      return Reply{"HTTP/1.1 100 Continue\r\n\r\n" + response(200, "OK", "after the interim"), true};
    if (seen.target == "/cut") return Reply{head + "Content-Length: 50\r\n\r\nonly this", true};
    if (seen.target == "/not-http") return Reply{"SSH-2.0-nothing\r\n\r\n", true};
    if (seen.target == "/two-lengths")
      return Reply{head + "Content-Length: 1\r\nContent-Length: 2\r\n\r\nab", true};
    return Reply{response(200, "OK", seen.method == "HEAD" ? "" : "x", "X-Length: 1\r\n"), true};
  });
  QtHttpClient client;
  const auto body_of = [&](const char* target) {
    const auto answer = client.send(request("GET", server.url(target)));
    return answer ? std::to_string(answer->status) + " " + answer->body : "error: " + answer.error().what;
  };
  EXPECT_EQ(body_of("/chunked"), "200 hello, world");
  EXPECT_EQ(body_of("/to-the-end"), "200 until the end");
  EXPECT_EQ(body_of("/interim"), "200 after the interim");
  EXPECT_TRUE(body_of("/cut").starts_with("error: the connection was lost before the answer was complete")) << body_of("/cut");
  EXPECT_TRUE(body_of("/not-http").starts_with("error: the answer could not be read")) << body_of("/not-http");
  EXPECT_TRUE(body_of("/two-lengths").starts_with("error: the answer could not be read")) << body_of("/two-lengths");
  const auto head = client.send(request("HEAD", server.url("/head")));
  ASSERT_TRUE(head) << head.error().what;
  EXPECT_EQ(head->status, 200);
  EXPECT_EQ(head->body, "");
}

// A header that is not one line is not written: it would be a header of the
// server's own to add, or a request of its own.
TEST(QtHttpClient, AHeaderThatIsNotOneLineIsNotSent) {
  Server server([](const Seen&) { return Reply{response(200, "OK", ""), true}; });
  QtHttpClient client;
  HttpRequest injected = request("GET", server.url("/x"));
  injected.headers = {{"X-Note", "a\r\nX-Evil: 1"}};
  EXPECT_EQ(client.send(injected).error().kind, ErrorKind::Config);
  injected.headers = {{"X Note", "a"}};
  EXPECT_EQ(client.send(injected).error().kind, ErrorKind::Config);
  // The ones that frame the request are the client's own.
  HttpRequest framed = request("POST", server.url("/x"));
  framed.body = "abc";
  framed.headers = {{"Content-Length", "99"}, {"Host", "elsewhere.test"}, {"Connection", "keep-alive"}};
  ASSERT_TRUE(client.send(framed));
  EXPECT_EQ(server.seen().back().header("Content-Length"), "3");
  EXPECT_EQ(server.seen().back().header("Host"), "127.0.0.1:" + std::to_string(server.port()));
  EXPECT_EQ(server.seen().back().header("Connection"), "close");
}

// From a thread that is not a QThread (a worker the program made itself): the
// exchange is the same, and Qt has nothing to say of a timer used there. The
// server is served by this thread, which waits for the worker.
namespace {
std::vector<std::string>* g_messages = nullptr;
void collect(QtMsgType, const QMessageLogContext&, const QString& text) {
  if (g_messages != nullptr) g_messages->push_back(text.toStdString());
}
}  // namespace

TEST(QtHttpClient, WorksFromAThreadThatIsNotAQThread) {
  Server server([](const Seen&) { return Reply{response(200, "OK", "from a worker")}; });
  std::vector<std::string> messages;
  g_messages = &messages;
  const QtMessageHandler before = qInstallMessageHandler(collect);
  QtHttpClient client;
  const std::string url = server.url("/x");
  auto answer = std::async(std::launch::async, [&] { return client.send(request("GET", url)); });
  while (answer.wait_for(1ms) != std::future_status::ready) QCoreApplication::processEvents();
  const auto got = answer.get();
  qInstallMessageHandler(before);
  g_messages = nullptr;
  ASSERT_TRUE(got) << got.error().what;
  EXPECT_EQ(got->body, "from a worker");
  EXPECT_TRUE(messages.empty()) << messages.front();
}

// ---- Findings of the first review

// A chunk size that would carry the answer past what is kept is refused, also
// when the sum of it and what is held would wrap.
TEST(QtHttpClient, AHugeChunkSizeIsRefusedNotWaitedFor) {
  Server server([](const Seen&) {
    return Reply{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\nffffffffffffffff\r\n", false};
  });
  QtHttpClient client;
  HttpRequest r = request("GET", server.url("/x"));
  r.timeout = 3000ms;
  const auto answer = client.send(r);
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, ErrorKind::Io) << answer.error().what;
  EXPECT_TRUE(has(answer.error().what, "too large")) << answer.error().what;
}

// Only `chunked`, alone, is a transfer coding this reads. Anything else is an
// answer that cannot be read: it came, so it is not "never sent".
TEST(QtHttpClient, ATransferCodingOtherThanChunkedIsAnErrorAnswer) {
  Server server([](const Seen& seen) {
    const std::string coding = seen.target == "/stacked" ? "gzip, chunked" : seen.target == "/spaced" ? "  Chunked " : "gzip";
    return Reply{"HTTP/1.1 200 OK\r\nTransfer-Encoding: " + coding + "\r\nConnection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n", true};
  });
  QtHttpClient client;
  for (const char* target : {"/stacked", "/gzip"}) {
    const auto answer = client.send(request("GET", server.url(target)));
    ASSERT_FALSE(answer) << target;
    EXPECT_EQ(answer.error().kind, ErrorKind::Io) << target;
    EXPECT_NE(answer.error().code, kHttpNotSent) << target;
    EXPECT_TRUE(request_may_have_arrived(answer.error())) << target;
    EXPECT_TRUE(has(answer.error().what, "transfer coding")) << answer.error().what;
  }
  const auto spaced = client.send(request("GET", server.url("/spaced")));
  ASSERT_TRUE(spaced) << spaced.error().what;
  EXPECT_EQ(spaced->body, "hello");
}

TEST(QtHttpClient, AStatusOfMoreThanThreeDigitsIsNotAStatus) {
  Server server([](const Seen& seen) {
    if (seen.target == "/four") return Reply{"HTTP/1.1 2000 OK\r\nContent-Length: 0\r\n\r\n", true};
    if (seen.target == "/bare") return Reply{"HTTP/1.1 204\r\n\r\n", true};
    return Reply{"HTTP/1.1 20x OK\r\nContent-Length: 0\r\n\r\n", true};
  });
  QtHttpClient client;
  EXPECT_FALSE(client.send(request("GET", server.url("/four"))));
  EXPECT_FALSE(client.send(request("GET", server.url("/x"))));
  const auto bare = client.send(request("GET", server.url("/bare")));
  ASSERT_TRUE(bare) << bare.error().what;
  EXPECT_EQ(bare->status, 204);
}

// A body that runs to the end of the connection, in many pieces.
TEST(QtHttpClient, AnAnswerToTheEndOfTheConnectionIsRead) {
  const std::string big(3 * 1024 * 1024, 'z');
  Server server([&](const Seen&) { return Reply{"HTTP/1.1 200 OK\r\n\r\n" + big, true}; });
  QtHttpClient client;
  const auto answer = client.send(request("GET", server.url("/big")));
  ASSERT_TRUE(answer) << answer.error().what;
  EXPECT_EQ(answer->body.size(), big.size());
}

TEST(QtHttpClient, ANegativeTimeoutIsATimeoutAndNoWarning) {
  Server server([](const Seen&) { return Reply{}; });
  std::vector<std::string> messages;
  g_messages = &messages;
  const QtMessageHandler before = qInstallMessageHandler(collect);
  QtHttpClient client;
  HttpRequest r = request("GET", server.url("/x"));
  r.timeout = std::chrono::milliseconds(-5);
  const auto answer = client.send(r);
  qInstallMessageHandler(before);
  g_messages = nullptr;
  ASSERT_FALSE(answer);
  EXPECT_EQ(answer.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(messages.empty()) << messages.front();
}

// QCoreApplication reads the locale from the environment on Unix; elctl and
// the reductions format and read numbers with the C library, and a comma for
// a decimal point must not reach them.
TEST(QtHttpClient, TheApplicationItMakesLeavesNumbersInTheCLocale) {
#ifdef _WIN32
  GTEST_SKIP() << "Unix locales only";
#else
  if (QCoreApplication::instance() != nullptr) GTEST_SKIP() << "another test made the application first; run alone";
  setenv("LC_ALL", "de_DE.UTF-8", 1);
  if (std::setlocale(LC_ALL, "de_DE.UTF-8") == nullptr) GTEST_SKIP() << "no de_DE.UTF-8 locale here";
  std::setlocale(LC_ALL, "C");
  QtHttpClient client;
  (void)client.send(request("GET", "http://127.0.0.1:1/x"));
  ASSERT_NE(QCoreApplication::instance(), nullptr);
  char text[32];
  std::snprintf(text, sizeof text, "%.1f", 1.5);
  EXPECT_STREQ(text, "1.5");
  EXPECT_EQ(std::strtod("1.5", nullptr), 1.5);
#endif
}

#if defined(PYCHRON_TEST_HAS_SSL_SERVER) && defined(PYCHRON_TEST_TLS_DIR)
// A server that speaks TLS with a certificate made for the test (cmake, in the
// build tree). Nobody trusts it: the client must not either.
class TlsServer {
 public:
  TlsServer(const char* name) {
    ensure_application();
    QFile crt(QString::fromStdString(std::string(PYCHRON_TEST_TLS_DIR) + "/" + name + ".crt"));
    QFile key(QString::fromStdString(std::string(PYCHRON_TEST_TLS_DIR) + "/" + name + ".key"));
    if (!crt.open(QIODevice::ReadOnly) || !key.open(QIODevice::ReadOnly)) {
      ADD_FAILURE() << "no test certificate";
      return;
    }
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(QSslCertificate(&crt, QSsl::Pem));
    config.setPrivateKey(QSslKey(&key, QSsl::Rsa, QSsl::Pem));
    server_.setSslConfiguration(config);
    QObject::connect(&server_, &QSslServer::pendingConnectionAvailable, &server_, [this] {
      while (QTcpSocket* socket = server_.nextPendingConnection()) {
        ++connections_;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          socket->readAll();
          const std::string reply = response(200, "OK", "reached");
          socket->write(reply.data(), static_cast<qint64>(reply.size()));
          socket->disconnectFromHost();
        });
      }
    });
    if (!server_.listen(QHostAddress::LocalHost, 0)) ADD_FAILURE() << server_.errorString().toStdString();
  }
  unsigned short port() const { return server_.serverPort(); }
  int connections() const { return connections_; }

 private:
  QSslServer server_;
  int connections_ = 0;
};

// Certificates are checked: one nobody vouches for is refused, and so is one
// for another name. Nothing of the request is sent to either.
TEST(QtHttpClient, ACertificateNobodyVouchesForIsRefused) {
  TlsServer server("ip");
  QtHttpClient client;
  HttpRequest r = request("GET", "https://127.0.0.1:" + std::to_string(server.port()) + "/api/account?size=2");
  r.headers = {{"Authorization", "Bearer not-a-real-token"}};
  r.timeout = 5000ms;
  const auto answer = client.send(r);
  ASSERT_FALSE(answer) << "a self-signed certificate was accepted";
  EXPECT_EQ(answer.error().kind, ErrorKind::Io) << answer.error().what;
  EXPECT_EQ(answer.error().code, kHttpNotSent) << answer.error().what;
  const std::string what = answer.error().what;
  EXPECT_TRUE(has(what, "self-signed") || has(what, "self signed") || has(what, "not trusted") || has(what, "untrusted") ||
              has(what, "issuer") || has(what, "authority"))
      << what;
  EXPECT_FALSE(has(what, "size=2")) << what;
  EXPECT_FALSE(has(what, "not-a-real-token")) << what;
}

TEST(QtHttpClient, ACertificateForAnotherNameIsRefused) {
  TlsServer server("name");
  QtHttpClient client;
  HttpRequest r = request("GET", "https://127.0.0.1:" + std::to_string(server.port()) + "/x");
  r.timeout = 5000ms;
  const auto answer = client.send(r);
  ASSERT_FALSE(answer) << "a certificate for earthbank.test was accepted for 127.0.0.1";
  EXPECT_EQ(answer.error().kind, ErrorKind::Io) << answer.error().what;
  EXPECT_EQ(answer.error().code, kHttpNotSent) << answer.error().what;
  EXPECT_TRUE(has(answer.error().what, "host name") || has(answer.error().what, "hostname") ||
              has(answer.error().what, "does not match") || has(answer.error().what, "did not match"))
      << answer.error().what;
}
#else
TEST(QtHttpClient, ACertificateNobodyVouchesForIsRefused) {
  GTEST_SKIP() << "no openssl program at configure time (or Qt older than 6.4): no test certificate";
}
TEST(QtHttpClient, ACertificateForAnotherNameIsRefused) {
  GTEST_SKIP() << "no openssl program at configure time (or Qt older than 6.4): no test certificate";
}
#endif

TEST(QtHttpClient, MakeHttpClientGivesOne) {
  const std::unique_ptr<IHttpClient> client = make_http_client();
  ASSERT_NE(client, nullptr);
  EXPECT_NE(dynamic_cast<QtHttpClient*>(client.get()), nullptr);
}

}  // namespace pychron::earthbank
