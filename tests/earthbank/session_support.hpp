// What the tests of the session, the lookup and the pre-flight check share: a
// client that stands between a Session and the fake service and remembers
// every secret that passed, so that a test can look for one where none may
// be, and a few words for reading the fake's log.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_earthbank.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"

namespace pychron::earthbank::testing {

// Passes every request on to the fake. Keeps the value of each Authorization
// header it saw, the token inside it, and every token a login was answered
// with: the strings that must be in no message, error or log line.
class Spy final : public IHttpClient {
 public:
  explicit Spy(FakeEarthBank& bank) : bank_(bank) {}

  Result<HttpResponse> send(const HttpRequest& request) override {
    ++sent_;
    if (const auto header = find_header(request, "Authorization")) {
      remember(std::string(*header));
      if (header->starts_with("Bearer ")) remember(std::string(header->substr(7)));
    }
    Result<HttpResponse> answer = bank_.send(request);
    if (answer && url_path(request.url) == "/api/authenticate") {
      const nlohmann::json body = nlohmann::json::parse(answer->body, nullptr, false);
      if (body.is_object() && body.contains("id_token") && body["id_token"].is_string()) {
        remember(body["id_token"].get<std::string>());
      }
    }
    return answer;
  }

  const std::vector<std::string>& secrets() const { return secrets_; }
  // The token of the last login.
  std::string token() const { return tokens_.empty() ? std::string() : tokens_.back(); }
  std::size_t sent() const { return sent_; }

 private:
  void remember(std::string secret) {
    if (!secret.starts_with("Bearer ")) tokens_.push_back(secret);
    if (std::find(secrets_.begin(), secrets_.end(), secret) == secrets_.end()) secrets_.push_back(std::move(secret));
  }

  FakeEarthBank& bank_;
  std::vector<std::string> secrets_, tokens_;
  std::size_t sent_ = 0;
};

// The requests of `log` from index `from` on, as `GET /api/account`.
inline std::vector<std::string> lines(const std::vector<LoggedRequest>& log, std::size_t from = 0) {
  std::vector<std::string> out;
  for (std::size_t i = from; i < log.size(); ++i) out.push_back(log[i].method + " " + log[i].path);
  return out;
}

}  // namespace pychron::earthbank::testing
