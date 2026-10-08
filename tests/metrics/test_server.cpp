// MetricsServer over a real socket on the loopback interface. Every server
// asks the OS for its port: tests run in parallel.
#include "pychron/metrics/server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "http_client.hpp"
#include "metrics_text.hpp"
#include "pychron/metrics/registry.hpp"

using namespace pychron;
using namespace pychron::metrics;
using namespace std::chrono_literals;
using http_client::body;
using http_client::get;
using http_client::http;

namespace {

MetricsServer::Options loopback() {
  MetricsServer::Options o;
  o.bind = "127.0.0.1";
  o.port = 0;
  return o;
}

double bad_requests(Registry& r) { return metrics_text::value(r.render(), "pychron_metrics_bad_requests_total"); }

// The counter is bumped on the server's thread just after it closes.
bool eventually(const std::function<bool()>& what) {
  for (int i = 0; i < 200; ++i) {
    if (what()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

}  // namespace

TEST(MetricsServer, ServesTheRegistry) {
  Registry registry;
  registry.gauge("pychron_pressure", "h", {{"gauge", "IG1"}, {"unit", "torr"}}).set(2);
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value()) << to_string(server.error());
  ASSERT_NE((*server)->port(), 0);

  const std::string response = get((*server)->port(), "/metrics");
  EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << response;
  EXPECT_NE(response.find("Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"), std::string::npos);
  EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
  const std::string b = body(response);
  EXPECT_NE(response.find("Content-Length: " + std::to_string(b.size()) + "\r\n"), std::string::npos);
  EXPECT_DOUBLE_EQ(metrics_text::value(b, "pychron_pressure{gauge=\"IG1\",unit=\"torr\"}"), 2.0);
}

TEST(MetricsServer, AnswersARequestThatArrivesInPieces) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  const std::string response = http((*server)->port(),
                                    "GET /metrics HTTP/1.1\r\n"
                                    "Host: labpc:9464\r\n"
                                    "User-Agent: Prometheus/2.53\r\n"
                                    "Accept: application/openmetrics-text;version=1.0.0,text/plain;version=0.0.4;q=0.5\r\n"
                                    "Accept-Encoding: gzip\r\n"
                                    "X-Prometheus-Scrape-Timeout-Seconds: 10\r\n\r\n",
                                    50ms);
  EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << response;
  EXPECT_EQ(response.find("Content-Encoding"), std::string::npos);  // gzip was asked for and is not given
}

TEST(MetricsServer, IgnoresAQueryString) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(get((*server)->port(), "/metrics?x=1").rfind("HTTP/1.1 200", 0), 0u);
}

TEST(MetricsServer, AnswersHttp10) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(http((*server)->port(), "GET /metrics HTTP/1.0\r\n\r\n").rfind("HTTP/1.1 200", 0), 0u);
}

TEST(MetricsServer, Healthz) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  const std::string response = get((*server)->port(), "/healthz");
  EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u);
  EXPECT_EQ(body(response), "ok\n");
}

TEST(MetricsServer, UnknownPathIs404) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(get((*server)->port(), "/").rfind("HTTP/1.1 404", 0), 0u);
  EXPECT_EQ(get((*server)->port(), "/metrics/extra").rfind("HTTP/1.1 404", 0), 0u);
}

TEST(MetricsServer, OtherMethodIs405) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  const std::string response = http((*server)->port(), "POST /metrics HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
  EXPECT_EQ(response.rfind("HTTP/1.1 405", 0), 0u);
  EXPECT_NE(response.find("Allow: GET\r\n"), std::string::npos);
}

TEST(MetricsServer, HeadersOverTheLimitCloseTheConnection) {
  Registry registry;
  MetricsServer::Options o = loopback();
  o.max_header_bytes = 256;
  auto server = MetricsServer::start(registry, o);
  ASSERT_TRUE(server.has_value());
  const std::string response =
      http((*server)->port(), "GET /metrics HTTP/1.1\r\nX-Big: " + std::string(1024, 'a') + "\r\n\r\n");
  EXPECT_EQ(response.find("200"), std::string::npos);
  EXPECT_TRUE(eventually([&] { return bad_requests(registry) == 1.0; }));
}

TEST(MetricsServer, ASilentClientIsDropped) {
  Registry registry;
  MetricsServer::Options o = loopback();
  o.read_timeout = 100ms;
  auto server = MetricsServer::start(registry, o);
  ASSERT_TRUE(server.has_value());
  asio::io_context io;
  asio::ip::tcp::socket socket = http_client::connect(io, (*server)->port());
  const auto got = http_client::read_all_within(socket, 2000ms);
  ASSERT_TRUE(got.has_value()) << "the server kept a silent connection open";
  EXPECT_EQ(*got, "");
  EXPECT_TRUE(eventually([&] { return bad_requests(registry) == 1.0; }));
}

// The deadline is for the request to arrive; one that arrived just in time is answered whole.
TEST(MetricsServer, ARequestThatArrivesAtTheDeadlineIsAnsweredWhole) {
  Registry registry;
  for (int i = 0; i < 200; ++i) registry.gauge("pychron_g", "h", {{"n", std::to_string(i)}}).set(i);
  MetricsServer::Options o = loopback();
  o.read_timeout = 60ms;
  auto server = MetricsServer::start(registry, o);
  ASSERT_TRUE(server.has_value());
  for (int i = 0; i < 40; ++i) {
    // Halves 55 to 65 ms apart: around the deadline, on either side of it.
    const std::string response =
        http((*server)->port(), "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n", std::chrono::milliseconds(55 + i % 11));
    if (response.empty()) continue;  // too late: dropped, and nothing was sent
    ASSERT_EQ(response.rfind("HTTP/1.1 200", 0), 0u);
    const std::string b = body(response);
    ASSERT_NE(response.find("Content-Length: " + std::to_string(b.size()) + "\r\n"), std::string::npos)
        << "a response was cut short at " << b.size() << " bytes";
  }
}

TEST(MetricsServer, GarbageIsABadRequest) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  const std::string response = http((*server)->port(), std::string("\x00\x01\x02\r\n\r\n", 7));
  EXPECT_EQ(response.find("200"), std::string::npos);
  EXPECT_TRUE(eventually([&] { return bad_requests(registry) == 1.0; }));
}

TEST(MetricsServer, AClientThatConnectsAndLeavesIsNotABadRequest) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  {
    asio::io_context io;
    asio::ip::tcp::socket socket = http_client::connect(io, (*server)->port());
  }
  // The server is one thread taking events in order: two requests answered
  // means it has long since seen the first client go.
  EXPECT_EQ(get((*server)->port(), "/healthz").rfind("HTTP/1.1 200", 0), 0u);
  EXPECT_EQ(get((*server)->port(), "/healthz").rfind("HTTP/1.1 200", 0), 0u);
  EXPECT_DOUBLE_EQ(bad_requests(registry), 0.0);
}

TEST(MetricsServer, RefusesConnectionsOverTheLimit) {
  Registry registry;
  MetricsServer::Options o = loopback();
  o.max_connections = 2;
  o.read_timeout = 5000ms;
  auto server = MetricsServer::start(registry, o);
  ASSERT_TRUE(server.has_value());
  const std::uint16_t port = (*server)->port();
  {
    asio::io_context io;
    asio::ip::tcp::socket a = http_client::connect(io, port);
    asio::ip::tcp::socket b = http_client::connect(io, port);
    // Both are counted once the server has accepted them; a third is then turned away.
    EXPECT_TRUE(eventually([&] {
      asio::ip::tcp::socket c = http_client::connect(io, port);
      const auto got = http_client::read_all_within(c, 300ms);
      return got.has_value() && got->empty();
    }));
  }
  // The two have gone: the next is served.
  EXPECT_TRUE(eventually([&] { return get(port, "/healthz").rfind("HTTP/1.1 200", 0) == 0; }));
}

TEST(MetricsServer, APortInUseIsAnError) {
  Registry registry;
  auto first = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(first.has_value());
  MetricsServer::Options o = loopback();
  o.port = (*first)->port();
  auto second = MetricsServer::start(registry, o);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, ErrorKind::Io);
  EXPECT_NE(second.error().what.find(std::to_string(o.port)), std::string::npos) << second.error().what;
  // The first still serves.
  EXPECT_EQ(get((*first)->port(), "/healthz").rfind("HTTP/1.1 200", 0), 0u);
}

TEST(MetricsServer, ABadBindAddressIsAnError) {
  Registry registry;
  MetricsServer::Options o = loopback();
  o.bind = "not-an-address";
  auto server = MetricsServer::start(registry, o);
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error().kind, ErrorKind::Config);
}

TEST(MetricsServer, StopsWithAConnectionOpen) {
  Registry registry;
  MetricsServer::Options o = loopback();
  o.read_timeout = 60000ms;
  auto server = MetricsServer::start(registry, o);
  ASSERT_TRUE(server.has_value());
  asio::io_context io;
  asio::ip::tcp::socket socket = http_client::connect(io, (*server)->port());
  const auto began = std::chrono::steady_clock::now();
  server->reset();
  EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
  const auto got = http_client::read_all_within(socket, 2000ms);
  EXPECT_TRUE(got.has_value()) << "the connection outlived the server";
}

TEST(MetricsServer, RecordsTheScrapeDuration) {
  Registry registry;
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  (void)get((*server)->port(), "/metrics");
  const std::string second = body(get((*server)->port(), "/metrics"));
  ASSERT_TRUE(metrics_text::has(second, "pychron_metrics_scrape_duration_seconds"));
  EXPECT_GE(metrics_text::value(second, "pychron_metrics_scrape_duration_seconds"), 0.0);
  EXPECT_LT(metrics_text::value(second, "pychron_metrics_scrape_duration_seconds"), 5.0);
}

TEST(MetricsServer, ManyScrapesInARow) {
  Registry registry;
  registry.gauge("pychron_g", "h").set(1);
  auto server = MetricsServer::start(registry, loopback());
  ASSERT_TRUE(server.has_value());
  for (int i = 0; i < 50; ++i) {
    ASSERT_EQ(get((*server)->port(), "/metrics").rfind("HTTP/1.1 200", 0), 0u) << "scrape " << i;
  }
}

TEST(MetricsServer, ListensOnIPv6) {
  Registry registry;
  MetricsServer::Options o = loopback();
  o.bind = "::1";
  auto server = MetricsServer::start(registry, o);
  if (!server.has_value()) {
    // A machine with no IPv6 loopback: the refusal must still be an error, not a crash.
    EXPECT_EQ(server.error().kind, ErrorKind::Io);
    return;
  }
  EXPECT_EQ(get((*server)->port(), "/healthz", "::1").rfind("HTTP/1.1 200", 0), 0u);
}
