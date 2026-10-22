#include <chrono>
#include <string>

#include "pychron/core/http.hpp"
#include "pychron/update/update.hpp"

namespace pychron::update {

Result<Fetched> fetch_latest(http::IHttpClient& client, std::string_view repository) {
  http::HttpRequest request;
  request.method = "GET";
  request.url = "https://api.github.com/repos/" + std::string(repository) + "/releases/latest";
  request.headers = {{"Accept", "application/vnd.github+json"}, {"User-Agent", PYCHRON_UPDATE_USER_AGENT}};
  request.timeout = std::chrono::seconds(10);

  const auto response = client.send(request);
  if (!response) return fail(response.error());
  if (response->status == 404) return Fetched{FetchOutcome::NoRelease, std::nullopt};
  if (response->status != 200) {
    return fail(ErrorKind::Protocol, "GitHub answered " + std::to_string(response->status));
  }
  auto release = parse_latest_release(response->body);
  if (!release) return fail(release.error());
  return Fetched{FetchOutcome::Found, std::move(*release)};
}

}  // namespace pychron::update
