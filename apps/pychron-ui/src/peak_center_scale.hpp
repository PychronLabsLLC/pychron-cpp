#pragma once

#include <optional>
#include <span>
#include <vector>

namespace pychron::ui {

// y of `other` mapped onto the range of `reference`: (y - min_o) * R / r + min_ref,
// legacy's formula. Empty when either range is zero or either input is
// empty: the caller then draws the raw values. For display only: the fit, the
// field table and the record read raw values.
[[nodiscard]] std::optional<std::vector<double>> scale_to_reference(std::span<const double> reference,
                                                                    std::span<const double> other);

}  // namespace pychron::ui
