#include "pychron/earthbank/preflight.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/earthbank/vocab.hpp"
#include "text_detail.hpp"

namespace pychron::earthbank {
namespace {

using namespace std::string_view_literals;

// The names of a record's fields, from the list the struct and to_json are
// made from (model.hpp).
#define PYCHRON_EARTHBANK_FIELD_NAME(kind, name) std::string_view(#name),
constexpr std::array kSampleFields = {PYCHRON_EARTHBANK_SAMPLE_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::array kLocationFields = {PYCHRON_EARTHBANK_LOCATION_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::array kDataPointFields = {PYCHRON_EARTHBANK_DATA_POINT_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::array kAliquotFields = {PYCHRON_EARTHBANK_ALIQUOT_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::array kMeasurementFields = {PYCHRON_EARTHBANK_MEASUREMENT_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::array kAgeCalcFields = {PYCHRON_EARTHBANK_AGE_CALC_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
constexpr std::array kAgeSummaryFields = {PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS(PYCHRON_EARTHBANK_FIELD_NAME)};
#undef PYCHRON_EARTHBANK_FIELD_NAME

constexpr std::string_view kSample = "Sample";
constexpr std::string_view kLocation = "Location";
constexpr std::string_view kDataPoint = "ArArDataPoint";
constexpr std::string_view kAliquot = "ArArAliquot";
constexpr std::string_view kMeasurement = "ArArMeasurement";
constexpr std::string_view kAgeCalc = "ArArAgeCalc";
constexpr std::string_view kAgeSummary = "ArArAgeSummary";

// Names with an id beside them that are not terms. `locationName` is the
// name given to the sample's place, and `locationId` the place's own row.
// `lithologyName` is sent as it is: the legacy integration never looked it
// up, and no list for it is known.
constexpr std::array kFreeText = {"locationName"sv, "lithologyName"sv};

constexpr std::string_view kNameEnd = "Name";

// What is sent and cannot be checked (PreflightReport::not_checked).
constexpr std::string_view kLithology = "lithologyName";
constexpr std::string_view kNoSetUpList =
    "EarthBank's list of analytical set-ups cannot be asked: whether it has one of this name is not checked.";
constexpr std::string_view kNoIrradiationList =
    "EarthBank's list of irradiations cannot be asked: whether it has one of this name is not checked.";
constexpr std::string_view kLithologyByName =
    "The lithology is sent by its name, and no list for it is known: whether EarthBank takes the name is not checked.";

// The terms of one record: its `<term>Name` fields that have `<term>Id`.
void add_terms(std::vector<TermField>& out, std::string_view record, std::span<const std::string_view> fields) {
  for (const std::string_view field : fields) {
    if (field.size() <= kNameEnd.size() || !field.ends_with(kNameEnd)) continue;
    const std::string id = std::string(field.substr(0, field.size() - kNameEnd.size())) + "Id";
    const auto partner = std::find(fields.begin(), fields.end(), std::string_view(id));
    if (partner == fields.end()) continue;
    TermField term{record, field, *partner, TermKind::Unlisted, {}};
    const auto named = [&](const LookupSpec& spec) { return spec.name_field == field; };
    const auto looked_up = lookup_specs();
    const auto fixed = uncertainty_type_specs();
    if (const auto spec = std::find_if(looked_up.begin(), looked_up.end(), named); spec != looked_up.end()) {
      term.kind = spec->endpoint.empty() ? TermKind::ByName : TermKind::Listed;
      term.endpoint = spec->endpoint;
    } else if (std::any_of(fixed.begin(), fixed.end(), named)) {
      term.kind = TermKind::Fixed;
    } else if (std::find(kFreeText.begin(), kFreeText.end(), field) != kFreeText.end()) {
      term.kind = TermKind::FreeText;
    }
    out.push_back(term);
  }
}

// Checks the terms of one record, as it would be sent.
class Walk {
 public:
  Walk(LookupResolver& resolver, PreflightReport& report, const std::vector<TermField>& terms)
      : resolver_(resolver), misses_(report.misses), resolved_(report.resolved), terms_(terms) {}

  // Some record had a lithology.
  bool lithology() const { return lithology_; }

  void start(std::string sample) {
    sample_ = std::move(sample);
    first_of_sample_ = misses_.size();
  }

  void check(std::string_view record, const nlohmann::json& sent) {
    for (const TermField& term : terms_) {
      if (term.record != record) continue;
      if (term.name_field == kLithology && sent.contains(term.name_field)) lithology_ = true;
      if (term.kind == TermKind::ByName || term.kind == TermKind::FreeText) continue;
      // to_json holds what is set: a name that is empty is not there, and an
      // id beside the name means there is nothing left to look up.
      const auto name = sent.find(term.name_field);
      if (name == sent.end() || !name->is_string() || sent.contains(term.id_field)) continue;
      const auto& value = name->get_ref<const std::string&>();
      if (again(record, term.name_field, value)) continue;

      Miss miss;
      miss.sample = sample_;
      miss.record = record;
      miss.field = term.name_field;
      miss.value = value;
      miss.endpoint = term.endpoint;
      if (term.kind == TermKind::Unlisted) {
        // Never a silent pass: the field is new to the model and nobody has
        // said where it is looked up.
        miss.outcome = LookupOutcome::CouldNotAsk;
        miss.detail = "pychron-cpp does not know which list of EarthBank's this field is of.";
      } else {
        const LookupResult result = resolver_.resolve(term.name_field, value);
        if (result.outcome == LookupOutcome::Found) found(record, term.name_field, value, result);
        if (result.outcome == LookupOutcome::Found || result.outcome == LookupOutcome::NoList) continue;
        miss.outcome = result.outcome;
        miss.detail = result.detail;
      }
      misses_.push_back(std::move(miss));
    }
  }

 private:
  // The same miss on another record of this kind of this sample: counted.
  bool again(std::string_view record, std::string_view field, std::string_view value) {
    for (std::size_t i = first_of_sample_; i < misses_.size(); ++i) {
      Miss& miss = misses_[i];
      if (miss.record != record || miss.field != field || miss.value != value) continue;
      ++miss.count;
      return true;
    }
    return false;
  }

  // A term that was found: said in the report when the entry is not the value
  // as written, or was not found by its one name. Once for each value.
  void found(std::string_view record, std::string_view field, const std::string& value, const LookupResult& result) {
    if (detail::same_but_case(detail::trimmed(value), result.matched_name) && result.detail.empty()) return;
    for (Resolved& one : resolved_) {
      if (one.record != record || one.field != field || one.value != value) continue;
      ++one.count;
      return;
    }
    resolved_.push_back({sample_, std::string(record), std::string(field), value, result.matched_name, result.detail, 1});
  }

  LookupResolver& resolver_;
  std::vector<Miss>& misses_;
  std::vector<Resolved>& resolved_;
  const std::vector<TermField>& terms_;
  bool lithology_ = false;
  std::string sample_;
  std::size_t first_of_sample_ = 0;
};

std::vector<Miss> with_outcome(const std::vector<Miss>& misses, LookupOutcome outcome) {
  std::vector<Miss> out;
  std::copy_if(misses.begin(), misses.end(), std::back_inserter(out),
               [&](const Miss& miss) { return miss.outcome == outcome; });
  return out;
}

}  // namespace

std::vector<TermField> term_fields() {
  std::vector<TermField> out;
  add_terms(out, kSample, kSampleFields);
  add_terms(out, kLocation, kLocationFields);
  add_terms(out, kDataPoint, kDataPointFields);
  add_terms(out, kAliquot, kAliquotFields);
  add_terms(out, kMeasurement, kMeasurementFields);
  add_terms(out, kAgeCalc, kAgeCalcFields);
  add_terms(out, kAgeSummary, kAgeSummaryFields);
  return out;
}

std::vector<Miss> PreflightReport::unknown() const { return with_outcome(misses, LookupOutcome::Unknown); }
std::vector<Miss> PreflightReport::could_not_ask() const { return with_outcome(misses, LookupOutcome::CouldNotAsk); }

PreflightReport preflight(const std::vector<Submission>& submissions, LookupResolver& resolver) {
  PreflightReport report;
  const std::vector<TermField> terms = term_fields();
  Walk walk(resolver, report, terms);
  bool set_up = false, irradiation = false;
  for (const Submission& submission : submissions) {
    set_up = set_up || !detail::trimmed(submission.setup_name).empty();
    irradiation = irradiation || !detail::trimmed(submission.irradiation_name).empty();
    std::string sample = submission.sample.name.value_or(std::string());
    if (sample.empty()) sample = submission.datapoint_name;
    if (!submission.notes.empty()) report.notes.push_back({sample, submission.notes});

    // The records in the order an upload sends them.
    walk.start(std::move(sample));
    walk.check(kSample, to_json(submission.sample));
    if (submission.location) walk.check(kLocation, to_json(*submission.location));
    walk.check(kDataPoint, to_json(submission.data_point));
    for (const Aliquot& aliquot : submission.aliquots) walk.check(kAliquot, to_json(aliquot));
    for (const Measurement& measurement : submission.measurements) walk.check(kMeasurement, to_json(measurement));
    walk.check(kAgeCalc, to_json(submission.age_calc));
    walk.check(kAgeSummary, to_json(submission.age_summary));
  }
  if (set_up) report.not_checked.push_back({"setup_name", std::string(kNoSetUpList)});
  if (irradiation) report.not_checked.push_back({"irradiation_name", std::string(kNoIrradiationList)});
  if (walk.lithology()) report.not_checked.push_back({std::string(kLithology), std::string(kLithologyByName)});
  return report;
}

}  // namespace pychron::earthbank
