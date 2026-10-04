#include "pychron/dvc/meta_files.hpp"

#include "meta_layout.hpp"

namespace pychron::dvc {

Result<persistence::HolderValue> parse_holder_text(std::string_view text) {
  auto parsed = parse_holder(text);
  if (!parsed) return fail(parsed.error());
  return std::move(parsed->value);
}

Result<persistence::ChronologyValue> parse_chronology_text(std::string_view text, std::string_view lab_time_zone) {
  auto parsed = parse_chronology(text, lab_time_zone);
  if (!parsed) return fail(parsed.error());
  return std::move(parsed->value);
}

}  // namespace pychron::dvc
