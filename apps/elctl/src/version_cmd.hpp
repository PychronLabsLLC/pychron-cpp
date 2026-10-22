#pragma once

// elctl version [--check]: the running version and, only when asked, the
// latest release on GitHub (libs/update). elctl never checks on its own.
//
// Exit status: 0 up to date, no release published or this build newer;
// 10 a newer release exists; 2 a failure, no network client in this build,
// or a usage error.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pychron/core/http.hpp"

namespace elctl {

struct VersionDeps {
  // Empty: pychron::earthbank::make_http_client (null without Qt Network).
  std::function<std::unique_ptr<pychron::http::IHttpClient>()> make_client;
};

int version_command(const std::vector<std::string>& args, Io io, const VersionDeps& deps = {});

}  // namespace elctl
