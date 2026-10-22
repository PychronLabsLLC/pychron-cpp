#include "version_cmd.hpp"

#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "pychron/setup/installer.hpp"
#include "pychron/update/update.hpp"

#ifdef PYCHRON_ELCTL_HAS_STORE
#include "pychron/earthbank/http.hpp"
#endif

namespace elctl {

namespace {

std::unique_ptr<pychron::http::IHttpClient> default_client() {
#ifdef PYCHRON_ELCTL_HAS_STORE
  return pychron::earthbank::make_http_client();
#else
  return nullptr;
#endif
}

int failed(Io io, const std::string& reason) {
  io.err << "elctl version: could not check for updates: " << reason << '\n';
  return kUsage;
}

}  // namespace

int version_command(const std::vector<std::string>& args, Io io, const VersionDeps& deps) {
  bool check = false;
  for (const std::string& a : args) {
    if (a == "--check") {
      check = true;
    } else {
      io.err << "elctl version: unknown argument '" << a << "'\nusage: elctl version [--check]\n";
      return kUsage;
    }
  }
  const std::string running_text(pychron::setup::version());
  io.out << "elctl " << running_text << '\n';
  if (!check) return kOk;

  auto client = deps.make_client ? deps.make_client() : default_client();
  if (!client) return failed(io, "this build has no network client");
  const auto running = pychron::update::parse_version(running_text);
  if (!running) return failed(io, "the running version is not readable: " + running.error().what);

  const auto fetched = pychron::update::fetch_latest(*client);
  if (!fetched) return failed(io, fetched.error().what);
  if (fetched->outcome == pychron::update::FetchOutcome::NoRelease || !fetched->release) {
    io.out << "latest: none published\n";
    return kOk;
  }
  const pychron::update::Release& latest = *fetched->release;
  const std::string latest_text = pychron::update::to_string(latest.version);
  if (latest.version > *running) {
    io.out << "latest: " << latest_text << "\ndownload: " << latest.html_url << '\n';
    return kUpdateAvailable;
  }
  if (latest.version == *running) {
    io.out << "latest: " << latest_text << " (up to date)\n";
  } else {
    io.out << "latest: " << latest_text << " (this build is newer)\n";
  }
  return kOk;
}

}  // namespace elctl
