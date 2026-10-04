#include "pychron/entry/sample_search.hpp"

#include <cctype>

namespace pychron::entry {

std::string sample_key(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (c == ' ' || c == '-' || c == '_' || c == '\t') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::vector<persistence::SampleRow> near_duplicates(std::string_view name,
                                                    const std::vector<persistence::SampleRow>& samples) {
  const std::string key = sample_key(name);
  std::vector<persistence::SampleRow> out;
  if (key.empty()) return out;
  for (const auto& s : samples)
    if (sample_key(s.name) == key) out.push_back(s);
  return out;
}

}  // namespace pychron::entry
