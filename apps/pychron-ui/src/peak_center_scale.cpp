#include "peak_center_scale.hpp"

#include <algorithm>

namespace pychron::ui {

std::optional<std::vector<double>> scale_to_reference(std::span<const double> reference,
                                                      std::span<const double> other) {
  if (reference.empty() || other.empty()) return std::nullopt;
  const auto ref_range = std::ranges::minmax(reference);
  const auto other_range = std::ranges::minmax(other);
  const double ref_span = ref_range.max - ref_range.min;
  const double other_span = other_range.max - other_range.min;
  if (ref_span == 0.0 || other_span == 0.0) return std::nullopt;
  std::vector<double> out(other.size());
  std::ranges::transform(other, out.begin(), [&](double y) {
    return (y - other_range.min) * ref_span / other_span + ref_range.min;
  });
  return out;
}

}  // namespace pychron::ui
