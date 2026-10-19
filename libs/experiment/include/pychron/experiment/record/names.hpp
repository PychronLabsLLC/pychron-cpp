#pragma once

// The names of a record's series and fitted values where an analysis is read
// (processing) and where it is stored (the store adapter). One place, so that
// an analysis loaded from the store is the analysis the record itself gives.
//
// The record keeps the acquisition's own names (measurement::fit_results):
// an isotope's intercept is under the isotope, or, when its signal was
// measured on more than one detector, under "<isotope>:<detector>". An
// analysis names such an isotope "<detector>:<isotope>" ("H1:Ar40"), as the
// processing library and imported analyses do; an isotope on one detector
// keeps its plain name. A detector's baseline may be in several series (a
// baseline block and a baseline hop); an analysis has one.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::record {

// "H1:Ar40": an analysis' key of an isotope measured on several detectors.
std::string detector_isotope_key(std::string_view detector, std::string_view isotope);

// "Ar40:H1": the key the acquisition fits such an isotope's intercept under,
// and so the key it has in a record's results.
std::string acquisition_isotope_key(std::string_view isotope, std::string_view detector);

// "Ar40@H1": a v3 record's key of a label's intercept on one detector.
std::string qualified_key(std::string_view label, std::string_view detector);

// A v2 record's intercept key ("Ar40", "Ar40:H1") as v3 has it ("Ar40@H1"),
// from the record's series; a key with no series stays as it is.
std::string v3_intercept_key(const Data& data, std::string_view v2_key);

// A v3 key ("Ar40@H1") as a v2 record had it: "Ar40", or "Ar40:H1" when the
// label's signal is on several detectors. Any other key stays as it is.
std::string v2_intercept_key(const Data& data, std::string_view v3_key);

// What an analysis (and the store) calls the intercept a record keeps under
// `result_key`: "Ar40@H1" -> "Ar40" when Ar40's signal is on one detector,
// "H1:Ar40" when on several; a v2 key likewise. A key with no series stays.
std::string analysis_name(const Data& data, std::string_view result_key);

// The keys a complete record's results have an intercept under: one for each
// isotope with a signal series, or, for an isotope whose signal is on several
// detectors, one for each of those detectors. Sorted.
std::vector<std::string> intercept_keys(const Data& data);

// One series as an analysis has it.
struct NamedSeries {
  std::string kind;      // as in the record: signal, baseline, sniff, whiff
  std::string key;       // the isotope's key; the detector for a baseline
  std::string detector;
  Trace trace;
  friend bool operator==(const NamedSeries&, const NamedSeries&) = default;
};

// The record's series under an analysis' names, in the record's order.
//
// A signal, sniff or whiff series is keyed by its isotope; by
// "<detector>:<isotope>" when that isotope's signal is on more than one
// detector, or when the series' own kind has it on more than one.
//
// The series of one isotope and kind on one detector, read at several places
// in the plan (v3), are one series, standing where the first of them stood,
// their points in time order.
//
// The baseline series of one detector are one series, keyed by the detector
// and standing where the first of them stood: their points one after the
// other in the record's order. The stored baseline is the record's
// Results::baselines entry for the detector (in a v3 record the baseline
// chosen for its first signal, spec 6.2). When only
// some of them carry a sigma the others' points have a sigma of NaN: none was
// measured.
std::vector<NamedSeries> named_series(const Data& data);

struct InterceptName {
  std::string key;       // the analysis' isotope key: "Ar40", or "H1:Ar40"
  std::string isotope;   // "Ar40"
  std::string detector;  // "H1"
  friend bool operator==(const InterceptName&, const InterceptName&) = default;
};

// What an analysis calls the isotope whose intercept the record keeps under
// `result_key`. A v3 key "<label>@<detector>" is that detector's (nullopt
// without a series of it there). "<isotope>:<detector>" of a signal series of an isotope
// measured on several detectors is that detector's; any other key is an
// isotope, on the detector of its signal series, or, with no signal series,
// on the detector of lowest name among its other series. nullopt: the record
// has no series of it.
std::optional<InterceptName> intercept_name(const Data& data, std::string_view result_key);

}  // namespace pychron::experiment::record
