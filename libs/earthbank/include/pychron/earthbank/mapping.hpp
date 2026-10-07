// From pychron-cpp's analyses to what EarthBank is sent: one Submission
// (model.hpp) per sample.
//
// build_submissions reduces each analysis, groups them by sample and fills
// the records from the reduction, with no I/O and nothing asked of the
// service: the controlled terms are left as names for the lookup to resolve.
// Everything a later step sends or writes into a workbook is decided here.
//
// Where each value comes from (the service's own words for a field are in the
// `Field Definitions` sheet of its Ar-Ar template):
//
//   Sample, Location   the analyses' sample name, material and catalog entry
//                      (processing::SampleInfo); a location needs both a
//                      latitude and a longitude.
//   DataPoint          the earliest analysis's UTC date, the method (vocab.hpp
//                      ar_method), the material as mineral, the lithology.
//   Aliquot            `<identifier>-<aliquot, two digits>`, in the order the
//                      aliquots first appear.
//   Measurement        one per analysis that has an age, in time order.
//     ar{36..40}Corr       Stage::InterferenceCorrected: 40Ar less 40ArK,
//                          39ArK, 38Ar, 37Ar decay corrected, 36Ar atmospheric.
//     ar{36..40}ExcInt     corrected for blank, discrimination and detector
//                          factors and, 39Ar only, for decay, as the service
//                          defines each: Stage::DecayCorrected, but
//                          Stage::IcCorrected for 37Ar.
//     ar{36..40}blank      Stage::Blank.
//     ratioNNArx39ArCorr   the Corr values over 39ArK, errors correlated.
//     ratio40Arx39ArK      F, 40Ar*/39ArK.
//     radiogenicArgon      40Ar* in percent of the 40Ar.
//     ar39KReleased, cumulative39ArK
//                          percent of the 39ArK of the step's own aliquot,
//                          alone and summed in step order; absent on a
//                          fusion, and when a step of the aliquot did not
//                          reduce (the total is then not known).
//     caK, kCa, clK        reduction::ArArResult.
//     jValue               J and its uncertainty.
//     apparentAge          the analytical age (no J uncertainty), in Ma.
//   AgeCalc            the names of the decay constant and atmospheric ratio
//                      the reduction used, found from their values, and the
//                      flux monitor as a query.
//   AgeSummary         the one age reported for the sample: see PreferredAge.
//
// Ages are in Ma whatever the reduction's age unit; every uncertainty is one
// sigma and absolute; intensities are in fA.
//
// What is missing stays missing. A number that is not finite is left unset
// and named in the submission's notes, an analysis without an age is left out
// and counted there, and a sample none of whose analyses has an age is an
// error. Nothing is taken from the first analysis on trust: analyses of one
// sample that disagree about what the sample is (material, lithology, place,
// constants, monitor) are refused with both values. The fields nothing in
// pychron-cpp can fill (grain size, aliquot weight, beam size, K content,
// funding, literature, the sample's kind and collection) are left unset.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/report.hpp"

namespace pychron::earthbank {

// The age reported for a sample. Left to the rule, a sample with heating
// steps reports its plateau when one exists under PublishOptions::ages and
// its integrated (total gas) age when none does, and a sample of fusion
// analyses reports the weighted mean. Each is computed as the publication
// report computes it (processing group_ages.hpp, arar_groups.hpp):
//   Plateau       over the steps of the one heated aliquot; a sample with
//                 several heated aliquots has no plateau.
//   WeightedMean  of every analysis with an age.
//   Isochron      inverse isochron of every analysis with an age.
//   Integrated    all the gas of every analysis with an age.
using PreferredAge = AgeKind;

// What the lab says about one sample; every part may be left empty.
struct SampleOptions {
  std::string interpretation;                 // an LArArInterpretation name; empty: `Unknown`
  std::optional<PreferredAge> preferred_age;  // empty: the rule above. One that cannot be computed is an error.
  std::string flux_monitor;                   // replaces the monitor found on the analyses
  std::string datapoint_name;                 // empty: "<sample> <material>", or "<sample>"
};

struct PublishOptions {
  processing::ReductionSettings reduction;  // as the report
  // The plateau criteria and weighting, the error kind of the plateau and
  // the weighted mean, and the isochron's regression. The rest (the sigma
  // level, the laboratory, the notes) is the report's own and is not read:
  // what is sent is always at one sigma.
  processing::ReportOptions ages;
  // The reported age's uncertainty with the J uncertainty of the irradiation
  // position added, as the report's "with J" column and as legacy pychron
  // quotes a mean; false gives the analytical uncertainty alone. The apparent
  // age of a single analysis is always analytical: its J is sent beside it.
  bool age_includes_j_error = true;
  // The service's name for the decay constant and for the atmospheric
  // 40Ar/36Ar, or a citation (`Min (2008)`; vocab.hpp normalize_citation).
  // Empty: named from the value the reduction used; a value no name matches
  // leaves the field unset, with a note.
  std::string decay_constant;
  std::string air_ratio;
  std::map<std::string, SampleOptions> samples;  // by sample name
  std::string software = "pychron-cpp";          // name and version, for the sample's description
};

// One submission per sample (Analysis::sample), in order of sample name. The
// caller has already left out what it does not want published (analyses
// tagged invalid, as `elctl export` does): every analysis given is used.
// Errors (Config, "earthbank: ..." naming the sample): an analysis without a
// sample name; a sample with no analysis that has an age; analyses of one
// sample that disagree, with the field and the two values; a preferred age
// asked for that cannot be computed; two samples given the same data point
// name.
Result<std::vector<Submission>> build_submissions(const std::vector<processing::AnalysisPtr>& analyses,
                                                  const PublishOptions& options);

}  // namespace pychron::earthbank
