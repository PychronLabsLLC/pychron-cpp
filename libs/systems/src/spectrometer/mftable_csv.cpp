#include "pychron/systems/spectrometer/mftable_csv.hpp"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pychron::spectrometer {

namespace {

std::string trim(std::string_view s) {
  const auto ws = " \t\r\"";
  auto b = s.find_first_not_of(ws);
  if (b == std::string_view::npos) return {};
  auto e = s.find_last_not_of(ws);
  return std::string(s.substr(b, e - b + 1));
}

std::vector<std::string> split_row(std::string_view line) {
  std::vector<std::string> cells;
  size_t start = 0;
  while (true) {
    auto comma = line.find(',', start);
    cells.push_back(trim(line.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return cells;
}

std::optional<double> to_double(const std::string& s) {
  if (s.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  double d = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size()) return std::nullopt;
  return d;
}

Unexpected<Error> bad(size_t line_no, const std::string& what) {
  return fail(ErrorKind::Config, "mftable.csv line " + std::to_string(line_no) + ": " + what);
}

}  // namespace

Result<FieldTable> read_mftable_csv(std::string_view csv_text, const MolecularWeights& weights, TableAxis axis) {
  std::vector<std::string> header;  // detector names; "" marks the mass column
  std::optional<size_t> mass_col;
  std::vector<std::optional<FitKind>> fits;
  std::vector<ControlPoint> points;
  std::set<std::string> seen;

  size_t line_no = 0;
  size_t pos = 0;
  while (pos <= csv_text.size()) {
    auto nl = csv_text.find('\n', pos);
    std::string_view line = csv_text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? csv_text.size() + 1 : nl + 1;
    ++line_no;
    if (trim(line).empty()) continue;
    auto cells = split_row(line);

    if (header.empty()) {
      if (cells.size() < 2) return bad(line_no, "header needs an isotope column and at least one detector");
      for (size_t i = 1; i < cells.size(); ++i) {
        if (cells[i].empty()) return bad(line_no, "empty detector name in header");
        if (cells[i] == "mass") mass_col = i;
        header.push_back(cells[i] == "mass" ? std::string{} : cells[i]);
      }
      if (mass_col && header.size() < 2) return bad(line_no, "header has no detector columns");
      fits.assign(header.size(), std::nullopt);
      continue;
    }

    if (!cells[0].empty() && cells[0].front() == '#') {
      // "#,<fit>,<fit>..." declares per-detector fits; other comments skipped.
      if (cells[0] != "#" || cells.size() < 2) continue;
      if (cells.size() - 1 > header.size()) return bad(line_no, "more fit cells than header columns");
      for (size_t i = 1; i < cells.size(); ++i) {
        if (header[i - 1].empty() || cells[i].empty()) continue;
        auto k = parse_fit_kind(cells[i]);
        if (!k) return bad(line_no, k.error().what);
        fits[i - 1] = *k;
      }
      continue;
    }

    if (cells.size() - 1 > header.size()) return bad(line_no, "more cells than header columns");
    ControlPoint cp;
    cp.isotope = cells[0];
    if (cp.isotope.empty()) return bad(line_no, "missing isotope");
    if (!seen.insert(cp.isotope).second) return bad(line_no, "duplicate isotope " + cp.isotope);
    std::optional<double> mass;
    for (size_t i = 1; i < cells.size(); ++i) {
      if (cells[i].empty()) continue;
      auto d = to_double(cells[i]);
      if (!d) return bad(line_no, "'" + cells[i] + "' is not a number");
      if (header[i - 1].empty()) {
        mass = *d;
      } else {
        cp.values.emplace(header[i - 1], *d);
      }
    }
    if (!mass) {
      auto m = weights.mass(cp.isotope);
      if (!m) return bad(line_no, "unknown isotope " + cp.isotope + " (no mass column, not in molecular weights)");
      mass = *m;
    }
    cp.mass = *mass;
    points.push_back(std::move(cp));
  }

  if (header.empty()) return fail(ErrorKind::Config, "mftable.csv: empty file");

  FieldTable table(FitKind::Quadratic, axis, std::move(points));
  for (size_t i = 0; i < header.size(); ++i) {
    if (!header[i].empty() && fits[i] && *fits[i] != table.default_fit()) table.set_fit(header[i], *fits[i]);
  }
  return table;
}

}  // namespace pychron::spectrometer
