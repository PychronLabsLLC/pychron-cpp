#pragma once

// Legacy files store naive local timestamps. These convert them to UTC with
// the lab's IANA zone and report the two DST cases a naive time cannot
// resolve, so the importer can flag them instead of guessing silently.

#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/persistence/ids.hpp"

namespace pychron::ingest {

// True when `iana` names a zone in the platform's time zone database.
bool known_zone(std::string_view iana);

enum class LocalKind { Unique, Ambiguous, Nonexistent };

struct LocalToUtc {
  persistence::UtcTime utc;
  LocalKind kind = LocalKind::Unique;
};

// `naive_local`: "YYYY-MM-DD HH:MM:SS[.f{1,6}]", or with 'T' in place of the
// space. Anything else, including a 'Z' or an offset, is an error (offsets are
// handled elsewhere). Ambiguous (clocks set back): the earlier instant.
// Nonexistent (clocks set forward): the UTC instant at which the gap begins.
// An unknown zone is an error, never an exception.
Result<LocalToUtc> local_to_utc(std::string_view naive_local, std::string_view iana);

}  // namespace pychron::ingest
