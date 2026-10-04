#include "pychron/laser/tray_map.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "pychron/codecs/codec.hpp"
#include "pychron/core/sha256.hpp"

namespace pychron::laser {

namespace {

namespace fs = std::filesystem;

std::string_view trim(std::string_view s) {
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!s.empty() && blank(s.front())) s.remove_prefix(1);
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> out;
  for (;;) {
    const auto comma = s.find(',');
    out.push_back(trim(s.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    s.remove_prefix(comma + 1);
  }
  return out;
}

struct Line {
  int number = 0;
  std::string_view text;  // trimmed
};

}  // namespace

Result<TrayMap> TrayMap::parse(std::string_view text, std::string name) {
  TrayMap map;
  map.name_ = std::move(name);
  map.sha256_ = to_hex(pychron::sha256(text));

  const auto bad = [&map](int line, const std::string& what) {
    return fail(ErrorKind::Config, map.name_ + ":" + std::to_string(line) + ": " + what);
  };

  std::string_view rest = text;
  if (rest.starts_with("\xEF\xBB\xBF")) rest.remove_prefix(3);
  std::vector<Line> lines;  // every line that is not a comment
  for (int number = 1; !rest.empty(); ++number) {
    const auto end = rest.find('\n');
    const std::string_view line = trim(rest.substr(0, end));
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
    if (!line.starts_with('#')) lines.push_back({number, line});
  }
  if (lines.size() < 3) {
    return fail(ErrorKind::Config, map.name_ + ": a tray map starts with three header lines (shape and dimension, "
                                                "valid holes, calibration holes)");
  }

  // shape,dimension
  {
    const auto f = split(lines[0].text);
    if (f.size() != 2) return bad(lines[0].number, "expected '<circle|square>,<dimension in mm>'");
    if (f[0] == "circle") map.shape_ = HoleShape::Circle;
    else if (f[0] == "square") map.shape_ = HoleShape::Square;
    else return bad(lines[0].number, "unknown hole shape '" + std::string(f[0]) + "' (circle or square)");
    const auto d = codec::parse_decimal(f[1]);
    if (!d || *d <= 0) return bad(lines[0].number, "the hole dimension must be a number above 0");
    map.dimension_ = *d;
  }
  // The valid-holes line (lines[1]) is read and not enforced: legacy never did.
  if (!lines[2].text.empty()) {
    const auto f = split(lines[2].text);
    if (f.size() != 5 || std::any_of(f.begin(), f.end(), [](std::string_view s) { return s.empty(); })) {
      return bad(lines[2].number, "expected five calibration holes: north, east, south, west, centre");
    }
    for (auto s : f) map.calibration_.emplace_back(s);
  }

  for (std::size_t i = 3; i < lines.size(); ++i) {
    std::string_view row = lines[i].text;
    const int number = lines[i].number;
    if (row.empty()) continue;

    // A trailing "(...)" lists associated holes: read past, not kept.
    if (const auto open = row.find('('); open != std::string_view::npos) {
      if (row.back() != ')') return bad(number, "an association must end the row: '(...)'");
      row = trim(row.substr(0, open));
      if (row.empty() || row.back() != ',') return bad(number, "expected ',' before '('");
      row = trim(row.substr(0, row.size() - 1));
    }
    auto f = split(row);

    Hole hole;
    hole.dimension = map.dimension_;
    if (f.size() == 3 && f[2].starts_with('r')) {
      const auto d = codec::parse_decimal(f[2].substr(1));
      if (!d || *d <= 0) return bad(number, "'" + std::string(f[2]) + "' is not a hole size ('r<mm>', above 0)");
      hole.dimension = *d;
      f.pop_back();
    }
    if (f.size() == 3) {
      if (f[0].empty()) return bad(number, "empty hole id");
      hole.id = std::string(f[0]);
      f.erase(f.begin());
    } else if (f.size() == 2) {
      hole.id = std::to_string(map.holes_.size() + 1);
    } else {
      return bad(number, "expected 'x,y' or 'id,x,y', optionally followed by 'r<mm>' or '(associated holes)'");
    }
    const auto x = codec::parse_decimal(f[0]);
    const auto y = codec::parse_decimal(f[1]);
    if (!x) return bad(number, "'" + std::string(f[0]) + "' is not a number");
    if (!y) return bad(number, "'" + std::string(f[1]) + "' is not a number");
    hole.x = *x;
    hole.y = *y;
    if (!map.index_.emplace(hole.id, map.holes_.size()).second) {
      return bad(number, "hole '" + hole.id + "' is on the map twice");
    }
    map.holes_.push_back(std::move(hole));
  }
  if (map.holes_.empty()) return fail(ErrorKind::Config, map.name_ + ": the tray map has no holes");
  for (const auto& id : map.calibration_) {
    if (!map.index_.contains(id)) return bad(lines[2].number, "calibration hole '" + id + "' is not on the map");
  }
  return map;
}

Result<TrayMap> TrayMap::load(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Config, "cannot read tray map " + file.string());
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str(), file.stem().string());
}

const Hole* TrayMap::find(std::string_view id) const {
  const auto it = index_.find(id);
  return it == index_.end() ? nullptr : &holes_[it->second];
}

std::optional<std::string> TrayMap::center_hole() const {
  if (calibration_.empty()) return std::nullopt;
  return calibration_[4];
}

std::optional<std::string> TrayMap::right_hole() const {
  if (calibration_.empty()) return std::nullopt;
  return calibration_[1];
}

TrayLibrary TrayLibrary::load(const fs::path& dir) {
  TrayLibrary lib;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return lib;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".txt") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());  // directory order is unspecified
  for (const auto& file : files) {
    auto map = TrayMap::load(file);
    if (!map) {
      lib.problems_.push_back(map.error().what);
      continue;
    }
    std::string name = map->name();
    lib.maps_.insert_or_assign(std::move(name), std::move(*map));
  }
  return lib;
}

const TrayMap* TrayLibrary::find(std::string_view name) const {
  const auto it = maps_.find(name);
  return it == maps_.end() ? nullptr : &it->second;
}

std::vector<std::string> TrayLibrary::names() const {
  std::vector<std::string> out;
  for (const auto& [name, map] : maps_) out.push_back(name);
  return out;
}

}  // namespace pychron::laser
