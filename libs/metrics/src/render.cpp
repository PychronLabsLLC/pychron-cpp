// The Prometheus text exposition format, version 0.0.4.

#include <charconv>
#include <cmath>
#include <string>

#include "registry_impl.hpp"

namespace pychron::metrics::detail {

namespace {

// The shortest text that reads back as the same double.
void number(std::string& out, double v) {
  if (std::isnan(v)) {
    out += "NaN";
  } else if (std::isinf(v)) {
    out += v > 0 ? "+Inf" : "-Inf";
  } else {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
  }
}

void escaped(std::string& out, std::string_view text, bool quotes) {
  for (const char c : text) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '"' && quotes) {
      out += "\\\"";
    } else {
      out += c;
    }
  }
}

// `{a="1",b="2"}`, with `le` last when given; nothing when there are no labels.
void label_set(std::string& out, const Labels& labels, const std::string* le = nullptr) {
  if (labels.empty() && le == nullptr) return;
  out += '{';
  bool first = true;
  for (const auto& [name, value] : labels) {
    if (!first) out += ',';
    first = false;
    out += name;
    out += "=\"";
    escaped(out, value, true);
    out += '"';
  }
  if (le != nullptr) {
    if (!first) out += ',';
    out += "le=\"" + *le + "\"";
  }
  out += '}';
}

void sample(std::string& out, const std::string& name, const char* suffix, const Labels& labels, double v,
            const std::string* le = nullptr) {
  out += name;
  out += suffix;
  label_set(out, labels, le);
  out += ' ';
  number(out, v);
  out += '\n';
}

const char* type_name(MetricType t) {
  switch (t) {
    case MetricType::Counter: return "counter";
    case MetricType::Gauge: return "gauge";
    case MetricType::Histogram: return "histogram";
  }
  return "untyped";
}

}  // namespace

std::string render_text(const std::map<std::string, Family, std::less<>>& families) {
  std::string out;
  for (const auto& [name, family] : families) {
    bool headed = false;
    for (const auto& [labels, series] : family.series) {
      if (series.hidden) continue;
      if (!headed) {
        headed = true;
        out += "# HELP " + name + ' ';
        escaped(out, family.help, false);
        out += "\n# TYPE " + name + ' ' + type_name(family.type) + '\n';
      }
      if (series.counter) {
        sample(out, name, "", labels, series.counter->value());
      } else if (series.gauge) {
        sample(out, name, "", labels, series.gauge->value());
      } else if (series.histogram) {
        // The count is the sum of the buckets read here, so +Inf and _count
        // agree even while another thread observes.
        const Histogram& h = *series.histogram;
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < h.bounds().size(); ++i) {
          cumulative += h.in_bucket(i);
          std::string le;
          number(le, h.bounds()[i]);
          sample(out, name, "_bucket", labels, static_cast<double>(cumulative), &le);
        }
        cumulative += h.in_bucket(h.bounds().size());
        const std::string inf = "+Inf";
        sample(out, name, "_bucket", labels, static_cast<double>(cumulative), &inf);
        sample(out, name, "_sum", labels, h.sum());
        sample(out, name, "_count", labels, static_cast<double>(cumulative));
      }
    }
  }
  return out;
}

}  // namespace pychron::metrics::detail
