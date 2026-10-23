#pragma once

// The summary ages of one group of analyses that are a mean of the analyses'
// own ages: the plateau of a heating schedule and the weighted mean. They are
// worked out here, once, for everything that reports them (the publication
// report, the EarthBank submission), so that two reports of the same group
// cannot give two plateaus. The group ages that run the age equation again
// (integrated, isochron) are in arar_groups.hpp.
//
// Ages are the analytical ones (reduction::AgeSet::age: no J error), in the
// age units of the reduction.

#include <optional>

#include "pychron/core/error.hpp"
#include "pychron/arar/arar_groups.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/reduction/stats.hpp"

namespace pychron::processing {

// The analysis reduced to an age.
bool has_ages(const DatasetItem& item);

// The 39ArK of the analysis; NaN when it did not reduce.
double k39_of(const DatasetItem& item);

// Step order within a group: increment, then time (as the spectrum). Stable.
GroupItems step_ordered(GroupItems items);

struct GroupPlateau {
  GroupItems steps;             // the steps the search ran over, in the order given
  reduction::StepRange range;   // the plateau, as indices into `steps`
  GroupItems used;              // the included steps in `range`: what the mean is of
  reduction::PlateauMean mean;
};

// The plateau of a heating schedule. `reduced` is the group's analyses with
// an age, in step order; the search runs over those with 39ArK > 0, each
// weighted by its 39ArK, an excluded one (DatasetItem::exclusion) taking part
// in neither the test nor the mean. Absent when no range meets `criteria` or
// its mean cannot be formed.
std::optional<GroupPlateau> group_plateau(const GroupItems& reduced, const reduction::PlateauCriteria& criteria,
                                          reduction::PlateauWeighting weighting, reduction::MeanErrorKind error);

// The inverse-variance weighted mean of the ages of `items`, every one of
// which has an age. Error as reduction::weighted_mean (no item, or none with
// a usable error).
Result<reduction::Mean> group_weighted_mean(const GroupItems& items, reduction::MeanErrorKind error);

}  // namespace pychron::processing
