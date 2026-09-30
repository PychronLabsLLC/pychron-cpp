#include "pychron/experiment/factory/increments.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>

#include "pychron/experiment/model/positions.hpp"

namespace pychron::experiment {
namespace {

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

}  // namespace

int step_index(std::string_view step) {
  if (step.empty()) return -1;
  long long n = 0;
  for (char c : step) {
    if (!is_alpha(c)) return -1;
    n = n * 26 + (std::toupper(static_cast<unsigned char>(c)) - 'A' + 1);
    if (n > INT32_MAX) return -1;
  }
  return static_cast<int>(n - 1);
}

std::string step_name(int index) {
  if (index < 0) return {};
  std::string out;
  for (long long n = static_cast<long long>(index) + 1; n > 0; n = (n - 1) / 26)
    out.insert(out.begin(), static_cast<char>('A' + (n - 1) % 26));
  return out;
}

std::string next_step(std::string_view step) { return step_name(step_index(step) + 1); }

Result<std::string> increment_identifier(std::string_view identifier, int by) {
  std::size_t start = identifier.size();
  while (start > 0 && is_digit(identifier[start - 1])) --start;
  const std::string_view digits = identifier.substr(start);
  if (digits.empty())
    return fail(ErrorKind::Config, "identifier '" + std::string(identifier) + "' has no trailing number to increment");
  std::int64_t n = 0;
  auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
  if (ec != std::errc{}) return fail(ErrorKind::Config, "identifier '" + std::string(identifier) + "' number out of range");
  n += by;
  if (n < 0) return fail(ErrorKind::Config, "identifier '" + std::string(identifier) + "' increment goes below zero");
  std::string num = std::to_string(n);
  if (num.size() < digits.size()) num.insert(0, digits.size() - num.size(), '0');
  return std::string(identifier.substr(0, start)) + num;
}

std::string format_runid(std::string_view identifier, int aliquot, std::string_view step) {
  std::string a = std::to_string(aliquot);
  if (a.size() < 2) a.insert(0, 2 - a.size(), '0');
  return std::string(identifier) + "-" + a + std::string(step);
}

Result<RunIdentity> parse_runid(std::string_view runid) {
  const auto dash = runid.rfind('-');
  if (dash == std::string_view::npos || dash == 0)
    return fail(ErrorKind::Config, "runid '" + std::string(runid) + "' is not <identifier>-<aliquot><step>");
  const std::string_view tail = runid.substr(dash + 1);
  std::size_t i = 0;
  while (i < tail.size() && is_digit(tail[i])) ++i;
  int aliquot = 0;
  auto [p, ec] = std::from_chars(tail.data(), tail.data() + i, aliquot);
  if (i == 0 || ec != std::errc{}) return fail(ErrorKind::Config, "runid '" + std::string(runid) + "' has no aliquot");
  const std::string_view step = tail.substr(i);
  for (char c : step)
    if (!is_alpha(c)) return fail(ErrorKind::Config, "runid '" + std::string(runid) + "' has a malformed step");
  RunIdentity id;
  id.identifier = std::string(runid.substr(0, dash));
  id.aliquot = aliquot;
  id.step = std::string(step);
  return id;
}

Result<RunSpec> next_template(const RunSpec& run, const IncrementOptions& inc, const IdentifierRules& ids) {
  RunSpec next = run;
  if (inc.identifier != 0 && !ids.is_special(run.id.identifier)) {
    auto id = increment_identifier(run.id.identifier, inc.identifier);
    if (!id) return fail(id.error());
    next.id.identifier = std::move(*id);
    next.id.aliquot.reset();
    next.id.step.clear();
  }
  if (inc.position != 0 && run.extraction.position) {
    auto pos = increment_position(*run.extraction.position, inc.position);
    if (!pos) return fail(pos.error());
    next.extraction.position = std::move(*pos);
  }
  return next;
}

std::vector<RunSpec> expand_positions(const RunSpec& tmpl, const Position& positions) {
  std::vector<RunSpec> out;
  out.reserve(positions.holes.size());
  for (auto& p : split_position(positions)) {
    out.push_back(tmpl);
    out.back().extraction.position = std::move(p);
  }
  return out;
}

Result<std::vector<RunSpec>> expand_positions(const RunSpec& tmpl, const Position& positions, int identifier_step) {
  auto out = expand_positions(tmpl, positions);
  for (std::size_t i = 1; i < out.size(); ++i) {
    auto id = increment_identifier(out[i - 1].id.identifier, identifier_step);
    if (!id) return fail(id.error());
    out[i].id.identifier = std::move(*id);
  }
  return out;
}

std::vector<RunSpec> make_step_heat(const RunSpec& tmpl, const std::vector<double>& values) {
  std::vector<RunSpec> out;
  out.reserve(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    out.push_back(tmpl);
    out.back().id.step = step_name(static_cast<int>(i));
    out.back().extraction.value = values[i];
  }
  return out;
}

std::vector<double> step_values(double start, double increment, int n) {
  std::vector<double> out;
  for (int i = 0; i < n; ++i) out.push_back(start + increment * i);
  return out;
}

}  // namespace pychron::experiment
