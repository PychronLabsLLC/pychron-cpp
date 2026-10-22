// A scripted HTTP client for tests of code that checks for updates.
//
// Answers are queued with answer() / fail() and given out in order; every
// request is kept for sent(). Safe to call from a worker thread while the
// test thread inspects it. Included by the update, elctl and UI tests.
#pragma once

#include <gtest/gtest.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/http.hpp"

namespace pychron::update::testing {

// fixtures/latest.json with its tag_name replaced by `tag`.
inline std::string release_json(std::string_view tag) {
  std::ifstream in(std::string(PYCHRON_UPDATE_FIXTURE_DIR) + "/latest.json");
  std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const std::string from = "\"v0.4.0\",";
  const auto at = text.find(from);
  EXPECT_NE(at, std::string::npos);
  if (at != std::string::npos) text.replace(at, from.size(), "\"" + std::string(tag) + "\",");
  return text;
}

class FakeHttp final : public http::IHttpClient {
 public:
  Result<http::HttpResponse> send(const http::HttpRequest& request) override {
    std::unique_lock lock(mutex_);
    sent_.push_back(request);
    cv_.wait(lock, [this] { return !held_; });
    if (answers_.empty()) {
      ADD_FAILURE() << "FakeHttp: no answer queued for " << request.url;
      return pychron::fail(ErrorKind::Io, "no answer queued");
    }
    auto next = std::move(answers_.front());
    answers_.pop_front();
    return next;
  }

  std::vector<http::HttpRequest> sent() const {
    const std::lock_guard lock(mutex_);
    return sent_;
  }

  void answer(int status, std::string body) {
    const std::lock_guard lock(mutex_);
    answers_.emplace_back(http::HttpResponse{status, std::move(body)});
  }

  void fail(ErrorKind kind) {
    const std::lock_guard lock(mutex_);
    answers_.emplace_back(pychron::fail(kind, "fake failure"));
  }

  // While held, send() records its request and blocks until release().
  void hold() {
    const std::lock_guard lock(mutex_);
    held_ = true;
  }

  void release() {
    {
      const std::lock_guard lock(mutex_);
      held_ = false;
    }
    cv_.notify_all();
  }

  // A client that forwards to this fake, for a factory that hands out ownership.
  std::unique_ptr<http::IHttpClient> borrow() { return std::make_unique<Forward>(*this); }

 private:
  class Forward final : public http::IHttpClient {
   public:
    explicit Forward(FakeHttp& fake) : fake_(fake) {}
    Result<http::HttpResponse> send(const http::HttpRequest& request) override { return fake_.send(request); }

   private:
    FakeHttp& fake_;
  };

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Result<http::HttpResponse>> answers_;
  std::vector<http::HttpRequest> sent_;
  bool held_ = false;
};

}  // namespace pychron::update::testing
