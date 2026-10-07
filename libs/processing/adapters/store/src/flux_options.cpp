// The options JSON of a saved flux fit (flux fitting design, section 6.4):
// the legacy keys with the legacy model strings, so a revision written here
// and one imported from a meta repo read alike, plus the keys that are new.

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "pychron/processing/flux_store.hpp"

namespace pychron::processing {

using Json = nlohmann::json;
using reduction::Axis;
using reduction::Interpolation;
using reduction::MeanErrorKind;

namespace {

std::string lower(std::string_view text) {
  std::string s(text);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// "sem", "SD", "msem" as parse_mean_error_kind reads them, and the legacy
// long forms: "SE but if MSWD>1 use SE * sqrt(MSWD)" and
// "SEM, but if MSWD>1 use SEM * sqrt(MSWD)".
std::optional<MeanErrorKind> error_kind_of(std::string_view text) {
  if (auto kind = reduction::parse_mean_error_kind(text)) return kind;
  if (lower(text).find("mswd") != std::string::npos) return MeanErrorKind::Msem;
  return std::nullopt;
}

std::string_view interpolation_name(Interpolation i) {
  switch (i) {
    case Interpolation::WeightedMean: return "Weighted Mean";
    case Interpolation::Average: return "Average";
    case Interpolation::Linear: return "Linear";
  }
  return "Weighted Mean";
}

std::optional<Interpolation> interpolation_of(std::string_view text) {
  const std::string s = lower(text);
  if (s == "weighted mean") return Interpolation::WeightedMean;
  if (s == "average") return Interpolation::Average;
  if (s == "linear") return Interpolation::Linear;
  return std::nullopt;
}

std::optional<std::string> text_at(const Json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

std::optional<bool> bool_at(const Json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_boolean()) return std::nullopt;
  return it->get<bool>();
}

std::optional<int> int_at(const Json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_number_integer()) return std::nullopt;
  const auto v = it->get<std::int64_t>();
  if (v < -1'000'000 || v > 1'000'000) return std::nullopt;
  return static_cast<int>(v);
}

}  // namespace

FluxOptionsDoc parse_flux_options(std::string_view options_json) {
  FluxOptionsDoc doc;
  const Json j = Json::parse(options_json, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return doc;

  doc.monitor_set = text_at(j, "monitor_reference").value_or("");
  doc.monitor_sample = text_at(j, "monitor_sample").value_or("");
  doc.used_in_fit = bool_at(j, "used_in_fit");
  doc.excluded = bool_at(j, "excluded");

  const auto model = text_at(j, "model_kind");
  const auto kind = model ? parse_model_kind(*model) : std::nullopt;
  if (!kind) return doc;  // none, or a model this version does not have (an imported RBF fit)

  FluxOptions o;
  o.fit.kind = *kind;
  o.fit.weighted = bool_at(j, "use_weighted_fit").value_or(o.fit.weighted);
  if (auto text = text_at(j, "predicted_j_error_type"))
    if (auto error = error_kind_of(*text)) o.fit.error = *error;
  if (auto text = text_at(j, "error_kind"))
    if (auto error = error_kind_of(*text)) o.mean_error = *error;
  if (auto text = text_at(j, "mean_kind"))
    if (auto mean = reduction::parse_mean_kind(*text)) o.mean = *mean;
  o.fit.n_neighbors = int_at(j, "n_neighbors").value_or(o.fit.n_neighbors);
  if (auto text = text_at(j, "interpolation_style"))
    if (auto interpolation = interpolation_of(*text)) o.fit.interpolation = *interpolation;
  if (auto text = text_at(j, "one_d_axis")) {
    const std::string axis = lower(*text);
    if (axis == "x") o.fit.axis = Axis::X;
    else if (axis == "y") o.fit.axis = Axis::Y;
  }
  o.fit.degree = int_at(j, "degree").value_or(o.fit.degree);

  // F13: SD is no error kind of the least-squares models.
  if (reduction::is_least_squares(o.fit.kind) && o.fit.error == MeanErrorKind::Sd) {
    o.fit.error = MeanErrorKind::Msem;
    doc.sd_replaced = true;
  }
  doc.options = o;
  return doc;
}

std::string flux_options_json(const FluxOptions& options, const MonitorSet& monitor_set, bool used_in_fit,
                              bool excluded, double fit_mswd, int fit_dof, std::string_view software) {
  Json j = Json::object();
  j["model_kind"] = legacy_model_name(options.fit.kind);
  j["use_weighted_fit"] = options.fit.weighted;
  j["predicted_j_error_type"] = reduction::to_string(options.fit.error);
  j["error_kind"] = reduction::to_string(options.mean_error);
  j["mean_kind"] = reduction::to_string(options.mean);
  j["n_neighbors"] = options.fit.n_neighbors;
  j["interpolation_style"] = interpolation_name(options.fit.interpolation);
  j["one_d_axis"] = options.fit.axis == Axis::Y ? "Y" : "X";
  j["degree"] = options.fit.degree;
  j["monitor_reference"] = monitor_set.name;
  j["monitor_sample"] = monitor_set.sample;
  j["used_in_fit"] = used_in_fit;
  j["excluded"] = excluded;
  j["fit_mswd"] = fit_mswd;  // not finite: null
  j["fit_dof"] = fit_dof;
  j["software"] = software;
  return j.dump();
}

namespace {

// The options without the version that wrote them; discarded when the text
// is not JSON.
Json fit_of(const std::string& options_json) {
  Json j = Json::parse(options_json, nullptr, false);
  if (j.is_object()) j.erase("software");
  return j;
}

bool same_options(const std::optional<std::string>& a, const std::optional<std::string>& b) {
  if (!a || !b) return a.has_value() == b.has_value();
  if (*a == *b) return true;
  const Json ja = fit_of(*a), jb = fit_of(*b);
  return !ja.is_discarded() && !jb.is_discarded() && ja == jb;
}

}  // namespace

bool same_flux_value(const persistence::FluxValue& a, const persistence::FluxValue& b) {
  if (!same_options(a.options_json, b.options_json)) return false;
  persistence::FluxValue rest_a = a, rest_b = b;  // every other field, whatever FluxValue grows
  rest_a.options_json.reset();
  rest_b.options_json.reset();
  return rest_a == rest_b;
}

}  // namespace pychron::processing
