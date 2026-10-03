#include "pychron/processing/isotope_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

namespace pychron::processing {

std::optional<double> isotope_mass(std::string_view isotope) {
  static const std::map<std::string, double, std::less<>> kMasses = {
      {"Ar40", 39.9624}, {"Ar39", 38.964}, {"Ar38", 37.9627}, {"Ar37", 36.9668}, {"Ar36", 35.9675},
      {"Ar35", 35.0},    {"Ar41", 40.962}, {"Ar33", 33.5}};
  auto it = kMasses.find(isotope);
  if (it == kMasses.end()) return std::nullopt;
  return it->second;
}

IsotopeSample make_isotope_sample(const RawData& raw, const std::string& isotope_key, const std::string& isotope_name) {
  IsotopeSample s;
  s.isotope = isotope_key;
  s.mass = isotope_mass(isotope_name).value_or(0.0);
  if (const RawSeries* sniff = raw.find(SeriesKind::Sniff, isotope_key)) {
    for (std::size_t i = 0; i < sniff->t.size() && i < sniff->v.size(); ++i) {
      // numpy.digitize(10 t, arange(12000)): index of the first bin edge > 10 t.
      const double x = sniff->t[i] * 10.0;
      if (!(x >= 0.0)) continue;
      const int bin = static_cast<int>(std::floor(x)) + 1;
      if (bin >= kClassifierBins) continue;
      if (sniff->v[i] != 0.0) {
        s.bins[bin] = sniff->v[i];
      } else {
        s.bins.erase(bin);  // a later point in the same bin overwrites, as numpy assignment
      }
    }
  }
  return s;
}

double sample_distance(const IsotopeSample& a, const IsotopeSample& b) {
  double d2 = (a.mass - b.mass) * (a.mass - b.mass);  // the placeholders cancel
  auto ia = a.bins.begin();
  auto ib = b.bins.begin();
  while (ia != a.bins.end() || ib != b.bins.end()) {
    if (ib == b.bins.end() || (ia != a.bins.end() && ia->first < ib->first)) {
      d2 += ia->second * ia->second;
      ++ia;
    } else if (ia == a.bins.end() || ib->first < ia->first) {
      d2 += ib->second * ib->second;
      ++ib;
    } else {
      const double d = ia->second - ib->second;
      d2 += d * d;
      ++ia;
      ++ib;
    }
  }
  return std::sqrt(d2);
}

std::optional<Classification> IsotopeClassifier::classify(const IsotopeSample& sample) const {
  if (samples_.empty()) return std::nullopt;
  std::vector<std::pair<double, int>> by_distance;  // (distance, klass)
  by_distance.reserve(samples_.size());
  for (const auto& s : samples_) by_distance.emplace_back(sample_distance(sample, s), s.klass);
  const std::size_t k = std::min<std::size_t>(kNeighbours, by_distance.size());
  // The k nearest, ties by training order (stable), as sklearn's brute search.
  std::stable_sort(by_distance.begin(), by_distance.end(),
                   [](const auto& x, const auto& y) { return x.first < y.first; });
  std::map<int, int> votes;
  for (std::size_t i = 0; i < k; ++i) ++votes[by_distance[i].second];
  int best = votes.begin()->first, count = 0;
  for (const auto& [klass, n] : votes)  // ascending klass: a tie keeps the smaller
    if (n > count) {
      best = klass;
      count = n;
    }
  return Classification{best, static_cast<double>(count) / static_cast<double>(k)};
}

std::string IsotopeClassifier::to_toml() const {
  toml::table root;
  toml::array samples;
  for (const auto& s : samples_) {
    toml::table t;
    t.insert("klass", s.klass);
    t.insert("mass", s.mass);
    toml::array bins, values;
    for (const auto& [b, v] : s.bins) {
      bins.push_back(b);
      values.push_back(v);
    }
    t.insert("bins", std::move(bins));
    t.insert("values", std::move(values));
    t.insert("runid", s.runid);
    t.insert("isotope", s.isotope);
    samples.push_back(std::move(t));
  }
  root.insert("schema", "isotope_classifier/1");
  root.insert("sample", std::move(samples));
  std::ostringstream out;
  out << root;
  return out.str();
}

Result<IsotopeClassifier> IsotopeClassifier::from_toml(std::string_view text) {
  auto parsed = toml::parse(text);
  if (!parsed) return fail(ErrorKind::Config, "isotope classifier: " + std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();
  IsotopeClassifier c;
  const auto* samples = root.get_as<toml::array>("sample");
  if (!samples) return c;
  for (const auto& node : *samples) {
    const auto* t = node.as_table();
    if (!t) return fail(ErrorKind::Config, "isotope classifier: a sample is not a table");
    IsotopeSample s;
    s.klass = static_cast<int>(t->get("klass") ? t->get("klass")->value<std::int64_t>().value_or(1) : 1);
    if (s.klass != 0 && s.klass != 1) return fail(ErrorKind::Config, "isotope classifier: klass must be 0 or 1");
    s.mass = t->get("mass") ? t->get("mass")->value<double>().value_or(0.0) : 0.0;
    const auto* bins = t->get_as<toml::array>("bins");
    const auto* values = t->get_as<toml::array>("values");
    if (bins && values) {
      if (bins->size() != values->size())
        return fail(ErrorKind::Config, "isotope classifier: bins and values differ in length");
      for (std::size_t i = 0; i < bins->size(); ++i) {
        const auto b = (*bins)[i].value<std::int64_t>();
        const auto v = (*values)[i].value<double>();
        if (!b || !v || *b < 0 || *b >= kClassifierBins)
          return fail(ErrorKind::Config, "isotope classifier: bad bin");
        s.bins[static_cast<int>(*b)] = *v;
      }
    }
    s.runid = t->get("runid") ? t->get("runid")->value<std::string>().value_or("") : "";
    s.isotope = t->get("isotope") ? t->get("isotope")->value<std::string>().value_or("") : "";
    c.samples_.push_back(std::move(s));
  }
  return c;
}

Result<IsotopeClassifier> IsotopeClassifier::load(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return IsotopeClassifier{};
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_toml(ss.str());
}

Result<void> IsotopeClassifier::save(const std::filesystem::path& path) const {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail(ErrorKind::Io, "cannot write " + tmp);
    out << to_toml();
    if (!out) return fail(ErrorKind::Io, "cannot write " + tmp);
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) return fail(ErrorKind::Io, "cannot replace " + path.string() + ": " + ec.message());
  return {};
}

}  // namespace pychron::processing
