#include "pychron/devices/types.hpp"

#include <charconv>

namespace pychron {

std::string_view to_string(ValveState state) noexcept {
  switch (state) {
    case ValveState::Unknown: return "unknown";
    case ValveState::Open: return "open";
    case ValveState::Closed: return "closed";
  }
  return "unknown";
}

Result<std::int64_t> ValveAddress::as_index() const {
  std::string_view s = value;
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);

  std::int64_t index = 0;
  const char* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(s.data(), end, index);
  if (s.empty() || ec != std::errc{} || ptr != end || index < 0) {
    return fail(ErrorKind::Config, "valve address '" + value + "' is not a non-negative integer");
  }
  return index;
}

}  // namespace pychron
