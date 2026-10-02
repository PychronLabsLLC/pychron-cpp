#include "pychron/experiment/conditionals/metrics.hpp"

namespace pychron::experiment {

std::optional<std::vector<double>> ChainContext::series(const MetricRef& m) const {
  for (const auto* c : chain_)
    if (c != nullptr)
      if (auto v = c->series(m)) return v;
  return std::nullopt;
}

std::optional<double> ChainContext::scalar(const MetricRef& m) const {
  for (const auto* c : chain_)
    if (c != nullptr)
      if (auto v = c->scalar(m)) return v;
  return std::nullopt;
}

std::optional<double> ChainContext::elapsed() const {
  for (const auto* c : chain_)
    if (c != nullptr)
      if (auto v = c->elapsed()) return v;
  return std::nullopt;
}

const record::DataSeries* RecordMetrics::signal(const std::string& iso) const {
  const record::DataSeries* best = nullptr;
  for (const auto& s : rec_.data.series) {
    if (s.kind != "signal" || s.iso != iso) continue;
    if (best == nullptr || s.trace.v.size() > best->trace.v.size()) best = &s;
  }
  return best;
}

const record::InterceptResult* RecordMetrics::intercept(const std::string& iso, std::string* det) const {
  const auto* s = signal(iso);
  if (s == nullptr) return nullptr;
  if (det != nullptr) *det = s->det;
  const auto& ints = rec_.results.intercepts;
  if (auto it = ints.find(iso + ":" + s->det); it != ints.end()) return &it->second;
  if (auto it = ints.find(iso); it != ints.end()) return &it->second;
  return nullptr;
}

std::optional<double> RecordMetrics::corrected(const std::string& iso) const {
  std::string det;
  const auto* i = intercept(iso, &det);
  if (i == nullptr) return std::nullopt;
  const auto b = rec_.results.baselines.find(det);
  const double bs = b == rec_.results.baselines.end() ? 0.0 : b->second.value;
  return (i->intercept.value - bs) * record::icfactor(rec_.results, det);
}

std::optional<std::vector<double>> RecordMetrics::series(const MetricRef& m) const {
  using K = MetricRef::Kind;
  auto floats = [](const std::vector<float>& v) { return std::vector<double>(v.begin(), v.end()); };
  switch (m.kind) {
    case K::Isotope:
      if (const auto* s = signal(m.a)) return floats(s->trace.v);
      break;
    case K::IsotopeField: {
      const auto* s = signal(m.a);
      if (s == nullptr) break;
      if (m.field == "bs") {
        std::vector<double> out;
        for (const auto& b : rec_.data.series)
          if (b.kind == "baseline" && b.det == s->det) out.insert(out.end(), b.trace.v.begin(), b.trace.v.end());
        if (out.empty()) break;
        return out;
      }
      return floats(s->trace.v);
    }
    case K::Ratio: {
      const auto* a = signal(m.a);
      const auto* b = signal(m.b);
      if (a == nullptr || b == nullptr) break;
      const std::size_t n = std::min(a->trace.v.size(), b->trace.v.size());
      std::vector<double> out;
      for (std::size_t i = 0; i < n; ++i) {
        const double den = b->trace.v[b->trace.v.size() - n + i];
        if (den == 0) return std::nullopt;
        out.push_back(a->trace.v[a->trace.v.size() - n + i] / den);
      }
      return out;
    }
    default: break;
  }
  return std::nullopt;
}

std::optional<double> RecordMetrics::scalar(const MetricRef& m) const {
  using K = MetricRef::Kind;
  switch (m.kind) {
    case K::Isotope: return corrected(m.a);
    case K::Ratio: {
      auto a = corrected(m.a), b = corrected(m.b);
      if (a && b && *b != 0) return *a / *b;
      return std::nullopt;
    }
    case K::IsotopeField: {
      if (m.field == "bs") return std::nullopt;
      if (m.field == "cur") {
        const auto* s = signal(m.a);
        if (s == nullptr || s->trace.v.empty()) return std::nullopt;
        return s->trace.v.back();
      }
      std::string det;
      const auto* i = intercept(m.a, &det);
      if (i == nullptr) return std::nullopt;
      if (m.field == "intercept") return i->intercept.value;
      if (m.field == "std_dev") return i->intercept.error;
      const auto b = rec_.results.baselines.find(det);
      const double bs = i->intercept.value - (b == rec_.results.baselines.end() ? 0.0 : b->second.value);
      return m.field == "ic_corrected" ? bs * record::icfactor(rec_.results, det) : bs;
    }
    case K::Computed: {
      if (!arar_ || m.a == "instant_age") return std::nullopt;
      reduction::ArArIntensities in{corrected("Ar36"), corrected("Ar37"), corrected("Ar38"), corrected("Ar39"),
                                    corrected("Ar40")};
      auto all = reduction::compute_arar(in, *arar_);
      if (auto it = all.find(m.a); it != all.end()) return it->second;
      return std::nullopt;
    }
    default: return std::nullopt;
  }
}

}  // namespace pychron::experiment
