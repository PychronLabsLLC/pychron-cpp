#include "pychron/earthbank/uploader.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"
#include "pychron/core/path_text.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/journal.hpp"
#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/preflight.hpp"
#include "pychron/earthbank/session.hpp"

namespace pychron::earthbank {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view kSamples = "/api/core/sample-with-locations";
constexpr std::string_view kDataPointPath = "/api/arar/ArArDataPoint";
constexpr std::string_view kAliquotPath = "/api/arar/ArArAliquot";
constexpr std::string_view kMeasurementPath = "/api/arar/ArArMeasurement";
constexpr std::string_view kAgeCalcPath = "/api/arar/ArArAgeCalc";
constexpr std::string_view kAgeSummaryPath = "/api/arar/ArArAgeSummary";

// The record names the term table (preflight.hpp) uses.
constexpr std::string_view kSampleRecord = "Sample";
constexpr std::string_view kLocationRecord = "Location";
constexpr std::string_view kDataPointRecord = "ArArDataPoint";
constexpr std::string_view kAliquotRecord = "ArArAliquot";
constexpr std::string_view kMeasurementRecord = "ArArMeasurement";
constexpr std::string_view kAgeCalcRecord = "ArArAgeCalc";
constexpr std::string_view kAgeSummaryRecord = "ArArAgeSummary";

constexpr std::string_view kSampleLabel = "sample";

bool is_success(int status) { return status >= 200 && status < 300; }

bool all_digits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// An id as the service writes one: a number, or a string of digits.
std::optional<std::int64_t> id_of(const json& object, const char* key) {
  if (!object.is_object()) return std::nullopt;
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  if (it->is_number_integer()) {
    const auto id = it->get<std::int64_t>();
    return id >= 1 ? std::optional<std::int64_t>(id) : std::nullopt;
  }
  if (it->is_string()) {
    const std::string& text = it->get_ref<const std::string&>();
    if (text.size() <= 18 && all_digits(text) && std::stoll(text) >= 1) return std::stoll(text);
  }
  return std::nullopt;
}

std::string text_of(const json& object, const char* key) {
  if (!object.is_object()) return {};
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// `yyyymmdd-hhmmss`, UTC.
std::string utc_stamp() {
  const auto second = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const auto midnight = std::chrono::floor<std::chrono::days>(second);
  const std::chrono::year_month_day day(midnight);
  const std::chrono::hh_mm_ss clock(second - midnight);
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%04d%02u%02u-%02d%02d%02d", static_cast<int>(day.year()),
                static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()),
                static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
                static_cast<int>(clock.seconds().count()));
  return buffer;
}

// The host (and port) of the service, as the journal's directory is named.
std::string host_of(std::string_view base_url) {
  if (const auto scheme = base_url.find("://"); scheme != std::string_view::npos) base_url.remove_prefix(scheme + 3);
  return std::string(base_url.substr(0, base_url.find_first_of("/?#")));
}

std::string measurement_key(std::size_t number) {
  std::string digits = std::to_string(number);
  if (digits.size() < 4) digits.insert(0, 4 - digits.size(), '0');
  return digits;
}

// ---- The records of one sample, in the order they are sent -----------------

enum class Kind { Sample, DataPoint, Aliquot, Measurement, AgeCalc, AgeSummary };

struct Step {
  Kind kind = Kind::Sample;
  std::string label;  // as UploadEvent::record and Journal::in_doubt
  std::string key;    // the journal's, for an aliquot or a measurement
  std::size_t index = 0;
};

std::vector<Step> plan(const Submission& s) {
  std::vector<Step> out;
  out.push_back({Kind::Sample, std::string(kSampleLabel), {}, 0});
  out.push_back({Kind::DataPoint, "data point", {}, 0});
  for (std::size_t i = 0; i < s.aliquots.size(); ++i) {
    const std::string name = s.aliquots[i].aliquotName.value_or("");
    out.push_back({Kind::Aliquot, "aliquot " + name, name, i});
  }
  for (std::size_t i = 0; i < s.measurements.size(); ++i) {
    out.push_back({Kind::Measurement, "measurement " + std::to_string(i + 1), measurement_key(i + 1), i});
  }
  out.push_back({Kind::AgeCalc, "age calculation", {}, 0});
  out.push_back({Kind::AgeSummary, "age summary", {}, 0});
  return out;
}

std::optional<std::int64_t> recorded(const Journal& j, const Step& step) {
  const auto in = [&](const std::map<std::string, std::int64_t>& map) -> std::optional<std::int64_t> {
    const auto found = map.find(step.key);
    return found == map.end() ? std::nullopt : std::optional<std::int64_t>(found->second);
  };
  switch (step.kind) {
    case Kind::Sample: return j.sample_id;
    case Kind::DataPoint: return j.data_point_id;
    case Kind::Aliquot: return in(j.aliquots);
    case Kind::Measurement: return in(j.measurements);
    case Kind::AgeCalc: return j.age_calc_id;
    case Kind::AgeSummary: return j.age_summary_id;
  }
  return std::nullopt;
}

void record(Journal& j, const Step& step, std::int64_t id) {
  switch (step.kind) {
    case Kind::Sample: j.sample_id = id; break;
    case Kind::DataPoint: j.data_point_id = id; break;
    case Kind::Aliquot: j.aliquots[step.key] = id; break;
    case Kind::Measurement: j.measurements[step.key] = id; break;
    case Kind::AgeCalc: j.age_calc_id = id; break;
    case Kind::AgeSummary: j.age_summary_id = id; break;
  }
}

// ---- What the user said of a record in doubt -------------------------------

struct Resolution {
  bool written = false;
  std::int64_t id = 0;
};

Result<std::optional<Resolution>> parse_resolution(const std::optional<std::string>& said) {
  if (!said) return std::optional<Resolution>();
  if (*said == "not-written") return std::optional<Resolution>(Resolution{false, 0});
  constexpr std::string_view kWritten = "written:";
  if (std::string_view(*said).starts_with(kWritten)) {
    const std::string digits = said->substr(kWritten.size());
    if (digits.size() <= 18 && all_digits(digits) && std::stoll(digits) >= 1) {
      return std::optional<Resolution>(Resolution{true, std::stoll(digits)});
    }
  }
  return fail(ErrorKind::Config, "earthbank: resolve_in_doubt is '" + *said +
                                     "'; it is \"written:<id>\" (the record is on EarthBank, with that id) or "
                                     "\"not-written\"");
}

// What the user is told of a record whose write got no answer. The same
// words every time, for the run that met the doubt and the runs after it.
std::string in_doubt_sentence(const std::string& sample, const std::string& label) {
  if (label == kSampleLabel) {
    return sample + ": the sample may or may not have been saved on EarthBank (the request was sent and no usable answer "
                    "came back). Nothing more of " + sample +
           " was sent. Run the upload again: the data package is searched for a sample of that name, and the sample is "
           "used if it is there and made if it is not.";
  }
  return sample + ": the " + label +
         " may or may not have been saved on EarthBank (the request was sent and no usable answer came back). Nothing "
         "more of " + sample + " was sent, because a record sent twice is there twice and cannot be removed. Look for the " +
         label + " of " + sample + " on EarthBank, then run again with resolve_in_doubt set to \"written:<id>\" (it is "
         "there; <id> is the id EarthBank shows for it) or to \"not-written\" (it is not).";
}

// ---- One upload ------------------------------------------------------------

struct Env {
  Session& session;
  LookupResolver& resolver;
  const UploadOptions& options;
  std::vector<TermField> terms;
  std::string host;
  std::optional<Resolution> resolution;  // the one that may be used
  bool resolution_is_ambiguous = false;  // one was given and several samples are in doubt
  const std::vector<Miss>* misses = nullptr;
  bool cancelled = false;  // the user stopped the upload: no sample after it starts
};

class SampleRun {
 public:
  SampleRun(Env& env, const Submission& sub, std::string name) : env_(env), sub_(sub), name_(std::move(name)) {
    report_.sample = name_;
  }

  SampleReport run();

 private:
  struct Posted {
    enum class Fate { Written, NotDone, InDoubt } fate = Fate::NotDone;
    std::int64_t id = 0;
    std::string said;
    bool session_lost = false;
  };
  enum class Search { None, One, Many, Failed };

  SampleReport refused(std::string why) {
    report_.status = SampleStatus::Refused;
    report_.messages.push_back(std::move(why));
    return std::move(report_);
  }
  bool asked_to_stop() {
    if (env_.cancelled || (env_.options.cancelled && env_.options.cancelled())) env_.cancelled = true;
    return env_.cancelled;
  }
  void emit(const std::string& record, const std::string& detail) {
    if (env_.options.on_event) env_.options.on_event({name_, record, detail, done_, total_});
  }
  Result<void> save() { return save_journal(journal_, path_); }
  void stop(SampleStatus status, std::string why) {
    forced_ = status;
    report_.messages.push_back(std::move(why));
  }

  std::optional<SampleReport> open_journal();
  bool settle_doubt();
  Search find_sample(std::int64_t& id, std::string& said);
  json resolved(std::string_view record, json body) const;
  std::pair<std::string, json> request_of(const Step& step) const;
  Posted post(std::string_view path, const json& body, const std::string& label);
  void send(const Step& step);
  void finish();

  Env& env_;
  const Submission& sub_;
  std::string name_;
  SampleReport report_;
  std::vector<Step> steps_;
  Journal journal_;
  fs::path path_;
  std::optional<SampleStatus> forced_;
  std::size_t done_ = 0, total_ = 0;
  bool stopped_ = false;
};

// Opens the sample's journal: this one's, a fresh one, or a refusal.
std::optional<SampleReport> SampleRun::open_journal() {
  const std::string hash = content_hash(sub_);
  path_ = journal_file(env_.options.journal_dir, env_.host, env_.options.data_package_id, name_);
  auto loaded = load_journal(path_);
  if (!loaded) return refused(loaded.error().what);
  journal_ = Journal{};
  journal_.hash = hash;
  if (!*loaded) return std::nullopt;

  Journal& old = **loaded;
  if (old.hash == hash) {
    journal_ = std::move(old);
    return std::nullopt;
  }
  // Other data. A journal that holds nothing has nothing to protect.
  if (old.empty()) return std::nullopt;
  if (!env_.options.new_datapoint) {
    return refused(name_ + ": the data changed since the upload of it began, and EarthBank cannot be updated, so nothing was "
                           "sent. To send " + name_ + " again as new records, run again with new_datapoint (the old records "
                           "stay on EarthBank; remove them there). The journal is " + utf8(path_) + ".");
  }
  fs::path aside;
  const std::string stem = path_.stem().string();
  for (int n = 1;; ++n) {
    aside = path_.parent_path() / (stem + "." + utc_stamp() + (n == 1 ? "" : "-" + std::to_string(n)) + ".superseded.json");
    std::error_code ec;
    if (!fs::exists(aside, ec) && !ec) break;
    if (n > 1000) return refused(name_ + ": no name could be found to put the old journal aside under.");
  }
  std::error_code ec;
  fs::rename(path_, aside, ec);
  if (ec) {
    return refused(name_ + ": the old journal could not be put aside (" + ec.message() + "), so nothing was sent: " + utf8(path_));
  }
  report_.messages.push_back("The earlier upload of " + name_ + " is kept, superseded, as " + utf8(aside) + ".");
  if (old.in_doubt) {
    report_.messages.push_back("In that earlier upload the " + *old.in_doubt + " of " + name_ +
                               " may or may not have been saved on EarthBank; it was not settled, and the new upload does "
                               "not look for it. Look on EarthBank for a duplicate.");
  }
  return std::nullopt;
}

// A journal with a record in doubt: settled by the user's word, by search
// (the sample) or not at all. False: the sample stops here.
bool SampleRun::settle_doubt() {
  if (!journal_.in_doubt) return true;
  const std::string label = *journal_.in_doubt;
  if (label == kSampleLabel) {
    // The search that comes next is how a sample is settled; whatever it finds
    // or does not is recorded then.
    journal_.in_doubt.reset();
    return true;
  }
  const auto step = std::find_if(steps_.begin(), steps_.end(), [&](const Step& s) { return s.label == label; });
  if (step == steps_.end()) {
    stop(SampleStatus::InDoubt, in_doubt_sentence(name_, label) + " (The journal names a record this submission does not have.)");
    return false;
  }
  if (env_.resolution_is_ambiguous) {
    stop(SampleStatus::InDoubt, in_doubt_sentence(name_, label) +
                                    " More than one sample is in doubt and a resolution names none, so none was used: "
                                    "settle them one at a time, uploading only this sample.");
    return false;
  }
  if (!env_.resolution) {
    stop(SampleStatus::InDoubt, in_doubt_sentence(name_, label));
    return false;
  }
  if (env_.resolution->written) record(journal_, *step, env_.resolution->id);
  journal_.in_doubt.reset();
  if (const auto saved = save(); !saved) {
    stop(SampleStatus::InDoubt, in_doubt_sentence(name_, label) + " Your answer could not be kept: " + saved.error().what);
    journal_.in_doubt = label;
    return false;
  }
  report_.messages.push_back(env_.resolution->written
                                 ? "The " + label + " of " + name_ + " is taken as saved on EarthBank, with id " +
                                       std::to_string(env_.resolution->id) + "."
                                 : "The " + label + " of " + name_ + " is taken as not saved on EarthBank; it is sent again.");
  return true;
}

// The sample of that name in the data package. The service's own filter is
// not trusted: each row's package is read.
SampleRun::Search SampleRun::find_sample(std::int64_t& id, std::string& said) {
  const std::int64_t package = env_.options.data_package_id;
  HttpRequest request;
  request.method = "GET";
  request.url = with_query(env_.session.url(kSamples), {{"name.equals", name_},
                                                        {"dataPackageId.equals", std::to_string(package)},
                                                        {"size", "20"}});
  const std::string what = "sample '" + name_ + "'";
  const Result<HttpResponse> answer = env_.session.send(std::move(request), true);
  if (!answer) {
    said = name_ + ": " + env_.session.failure(answer.error(), "GET", kSamples, what).what;
    return Search::Failed;
  }
  if (!is_success(answer->status)) {
    said = name_ + ": " + env_.session.failure(*answer, "GET", kSamples, what).what;
    return Search::Failed;
  }
  const json rows = json::parse(answer->body, nullptr, false);
  if (!rows.is_array()) {
    said = name_ + ": EarthBank's answer to the search for the sample could not be read, so nothing was sent.";
    return Search::Failed;
  }
  std::set<std::int64_t> ids;
  for (const json& row : rows) {
    const auto dto = row.is_object() ? row.find("sampleDTO") : row.end();
    if (dto == row.end() || !dto->is_object() || text_of(*dto, "name") != name_ || id_of(*dto, "dataPackageId") != package) continue;
    const auto found = id_of(*dto, "id");
    if (!found) {
      said = name_ + ": a sample of that name is on EarthBank in this data package, but its id could not be read, so nothing was sent.";
      return Search::Failed;
    }
    ids.insert(*found);
  }
  if (ids.empty()) return Search::None;
  if (ids.size() > 1) {
    said = name_ + ": EarthBank has more than one sample of that name in this data package (ids";
    for (const std::int64_t one : ids) said += " " + std::to_string(one);
    said += "), and which is meant cannot be guessed, so nothing was sent. Remove the extra ones there, or rename them.";
    return Search::Many;
  }
  id = *ids.begin();
  return Search::One;
}

// A record as it is sent: a term as its id, the names of the terms gone. A
// term the lookups do not have is left out (allow_unresolved; otherwise the
// pre-flight check has stopped the upload before this). Terms that are sent by
// name stay as they are.
json SampleRun::resolved(std::string_view record, json body) const {
  for (const TermField& term : env_.terms) {
    if (term.record != record) continue;
    const auto name = body.find(std::string(term.name_field));
    if (name == body.end() || !name->is_string()) continue;
    if (body.contains(std::string(term.id_field))) {
      body.erase(std::string(term.name_field));
      continue;
    }
    if (term.kind == TermKind::ByName || term.kind == TermKind::FreeText || term.kind == TermKind::Unlisted) continue;
    const LookupResult result = env_.resolver.resolve(term.name_field, name->get<std::string>());
    if (result.outcome == LookupOutcome::NoList) continue;
    if (result.outcome == LookupOutcome::Found) body[std::string(term.id_field)] = result.id;
    body.erase(std::string(term.name_field));
  }
  return body;
}

std::pair<std::string, json> SampleRun::request_of(const Step& step) const {
  const std::int64_t package = env_.options.data_package_id;
  const auto under_point = [&](std::string_view record, json body) {
    body = resolved(record, std::move(body));
    body["arArDataPointId"] = *journal_.data_point_id;
    return body;
  };
  switch (step.kind) {
    case Kind::Sample: {
      json dto = resolved(kSampleRecord, to_json(sub_.sample));
      dto["dataPackageId"] = package;
      json wrapper = {{"sampleDTO", std::move(dto)},
                      {"locationDTO", sub_.location ? resolved(kLocationRecord, to_json(*sub_.location)) : json::object()},
                      {"dataPackageId", package},
                      {"shortName", name_}};
      return {std::string(kSamples), std::move(wrapper)};
    }
    case Kind::DataPoint: {
      // The umbrella names the package and the sample; the Ar-Ar record holds
      // the rest. Funding and literature are carried for the workbook and are
      // not fields of the service's Ar-Ar record.
      json ext = resolved(kDataPointRecord, to_json(sub_.data_point));
      for (const char* carried : {"fundingName", "fundingId", "literatureName", "literatureId"}) ext.erase(carried);
      json core = {{"dataStructure", "ARARDATAPOINT"}, {"dataPackageId", package}, {"sampleId", *journal_.sample_id}};
      core["name"] = sub_.datapoint_name.empty() ? name_ : sub_.datapoint_name;
      json wrapper = {{"dataPointDTO", std::move(core)}, {"extendingDataPointDTO", std::move(ext)}};
      if (!sub_.irradiation_name.empty()) wrapper["arArIrradiationName"] = sub_.irradiation_name;
      if (!sub_.setup_name.empty()) wrapper["arArSetUpName"] = sub_.setup_name;
      return {std::string(kDataPointPath), std::move(wrapper)};
    }
    case Kind::Aliquot: return {std::string(kAliquotPath), under_point(kAliquotRecord, to_json(sub_.aliquots[step.index]))};
    case Kind::Measurement:
      return {std::string(kMeasurementPath), under_point(kMeasurementRecord, to_json(sub_.measurements[step.index]))};
    case Kind::AgeCalc: return {std::string(kAgeCalcPath), under_point(kAgeCalcRecord, to_json(sub_.age_calc))};
    case Kind::AgeSummary: return {std::string(kAgeSummaryPath), under_point(kAgeSummaryRecord, to_json(sub_.age_summary))};
  }
  return {};
}

// One POST, and what is known of it afterwards. Written: answered 2xx with
// an id. NotDone: refused with an answer, or never left. InDoubt: anything
// else, which includes an answer that is not a clean success: a gateway
// fault, a 2xx without an id.
SampleRun::Posted SampleRun::post(std::string_view path, const json& body, const std::string& label) {
  Posted out;
  HttpRequest request;
  request.method = "POST";
  request.url = env_.session.url(path);
  request.headers.emplace_back("Content-Type", "application/json");
  request.body = body.dump();
  const std::string what = label + " of " + name_;

  const Result<HttpResponse> answer = env_.session.send(std::move(request), true);
  if (!answer) {
    const Error& error = answer.error();
    out.said = name_ + ": " + env_.session.failure(error, "POST", path, what).what;
    out.session_lost = error.code == kNotSignedIn;
    out.fate = error.code == kNotSignedIn || !request_may_have_arrived(error) ? Posted::Fate::NotDone : Posted::Fate::InDoubt;
    return out;
  }
  if (!is_success(answer->status)) {
    const Error error = env_.session.failure(*answer, "POST", path, what);
    out.said = name_ + ": " + error.what;
    out.session_lost = error.code == kLoginRejected;
    out.fate = error.code == kInDoubt ? Posted::Fate::InDoubt : Posted::Fate::NotDone;
    return out;
  }
  const json parsed = json::parse(answer->body, nullptr, false);
  std::optional<std::int64_t> id;
  if (path == kSamples && parsed.is_object()) {
    // Where legacy looked for a new sample's id: in `sampleDTO`, then in `sample`.
    for (const char* inner : {"sampleDTO", "sample"}) {
      if (const auto it = parsed.find(inner); it != parsed.end() && !id) id = id_of(*it, "id");
    }
  }
  if (!id) id = id_of(parsed, "id");
  if (!id) {
    out.said = name_ + ": EarthBank answered the " + label + " without an id that could be read.";
    out.fate = Posted::Fate::InDoubt;
    return out;
  }
  out.fate = Posted::Fate::Written;
  out.id = *id;
  return out;
}

// One record: intent saved, request, outcome saved.
void SampleRun::send(const Step& step) {
  const bool before_point = step.kind == Kind::Sample || step.kind == Kind::DataPoint;
  const auto [path, body] = request_of(step);

  journal_.in_doubt = step.label;
  if (const auto saved = save(); !saved) {
    // Nothing was sent.
    journal_.in_doubt.reset();
    stop(before_point ? SampleStatus::Stopped : SampleStatus::Partial,
         name_ + ": the journal could not be written, so the " + step.label + " was not sent: " + saved.error().what);
    stopped_ = true;
    return;
  }

  const Posted posted = post(path, body, step.label);
  switch (posted.fate) {
    case Posted::Fate::Written: {
      record(journal_, step, posted.id);
      journal_.in_doubt.reset();
      if (const auto saved = save(); !saved) {
        // The service has it and the file says "in doubt": the user can settle
        // that with the id, which is told here.
        journal_.in_doubt = step.label;
        stop(SampleStatus::InDoubt, name_ + ": the " + step.label + " was saved on EarthBank with id " + std::to_string(posted.id) +
                                        ", but the journal could not be updated (" + saved.error().what +
                                        "). Nothing more of " + name_ + " was sent. Run again with resolve_in_doubt set to "
                                        "\"written:" + std::to_string(posted.id) + "\".");
        stopped_ = true;
        return;
      }
      report_.written.push_back(step.label);
      ++done_;
      emit(step.label, "saved, id " + std::to_string(posted.id));
      return;
    }
    case Posted::Fate::NotDone: {
      journal_.in_doubt.reset();
      report_.messages.push_back(posted.said);
      if (const auto saved = save(); !saved) {
        journal_.in_doubt = step.label;
        stop(SampleStatus::Stopped, name_ + ": the " + step.label + " was not saved, but the journal could not be told (" +
                                        saved.error().what + "), so it still says it is in doubt. Run again with "
                                        "resolve_in_doubt set to \"not-written\".");
        stopped_ = true;
        return;
      }
      if (before_point || posted.session_lost) stopped_ = true;
      return;
    }
    case Posted::Fate::InDoubt:
      // The journal already says so.
      report_.messages.push_back(in_doubt_sentence(name_, step.label));
      report_.messages.push_back(posted.said);
      forced_ = SampleStatus::InDoubt;
      stopped_ = true;
      return;
  }
}

void SampleRun::finish() {
  for (const Step& step : steps_) {
    if (!recorded(journal_, step)) report_.missing.push_back(step.label);
  }
  report_.data_point_id = journal_.data_point_id;
  if (forced_) {
    report_.status = *forced_;
    return;
  }
  if (report_.missing.empty()) {
    report_.status = SampleStatus::Complete;
  } else if (journal_.data_point_id) {
    report_.status = SampleStatus::Partial;
    report_.messages.push_back(Session::partial_sentence(name_, report_.missing));
  } else {
    report_.status = SampleStatus::Stopped;
  }
}

SampleReport SampleRun::run() {
  if (env_.cancelled) {
    report_.status = SampleStatus::Stopped;
    report_.messages.push_back(name_ + ": not started, the upload was stopped at your request.");
    return std::move(report_);
  }
  if (sub_.sample.name.value_or("").empty()) return refused("A sample has no name, and EarthBank is searched for it by name: nothing was sent.");
  {
    std::set<std::string> names;
    for (const Aliquot& aliquot : sub_.aliquots) {
      const std::string aliquot_name = aliquot.aliquotName.value_or("");
      if (aliquot_name.empty()) return refused(name_ + ": an aliquot has no name, so nothing was sent.");
      if (!names.insert(aliquot_name).second) {
        return refused(name_ + ": two aliquots are named '" + aliquot_name + "', and the journal could not tell them apart, so nothing was sent.");
      }
    }
  }
  if (auto refusal = open_journal()) return std::move(*refusal);
  steps_ = plan(sub_);
  if (env_.options.allow_unresolved) {
    for (const Miss& miss : *env_.misses) {
      if (miss.sample != name_) continue;
      report_.messages.push_back(name_ + ": " + miss.field + " '" + miss.value + "' of the " + miss.record +
                                 " is not known to EarthBank, and the field was left out" +
                                 (miss.count > 1 ? " (" + std::to_string(miss.count) + " records)." : "."));
    }
  }
  if (!settle_doubt()) {
    finish();
    return std::move(report_);
  }

  // The sample: the journal's, or the package's, or made below.
  bool asked = false;
  if (!journal_.sample_id) {
    asked = true;
    if (asked_to_stop()) {
      stop(SampleStatus::Stopped, name_ + ": stopped at your request before anything was sent; run again to continue.");
      finish();
      return std::move(report_);
    }
    std::int64_t found = 0;
    std::string said;
    switch (find_sample(found, said)) {
      case Search::Failed:
      case Search::Many:
        stop(SampleStatus::Stopped, said);
        finish();
        return std::move(report_);
      case Search::One:
        journal_.sample_id = found;
        if (const auto saved = save(); !saved) {
          journal_.sample_id.reset();
          stop(SampleStatus::Stopped, name_ + ": the journal could not be written: " + saved.error().what);
          finish();
          return std::move(report_);
        }
        report_.messages.push_back("The sample " + name_ + " is already in this data package (id " + std::to_string(found) +
                                   "); it was not made again.");
        break;
      case Search::None: break;
    }
  }

  total_ = static_cast<std::size_t>(std::count_if(steps_.begin(), steps_.end(), [&](const Step& s) { return !recorded(journal_, s); }));
  emit("", "starting");
  for (const Step& step : steps_) {
    if (recorded(journal_, step)) continue;
    if (!(step.kind == Kind::Sample && asked) && asked_to_stop()) {
      stop(SampleStatus::Stopped, name_ + ": stopped at your request before the " + step.label +
                                      "; run again to continue where it left off.");
      break;
    }
    send(step);
    if (stopped_) break;
  }
  finish();
  return std::move(report_);
}

}  // namespace

Result<UploadReport> upload(const std::vector<Submission>& submissions, Session& session, LookupResolver& resolver,
                            const UploadOptions& options) {
  if (options.data_package_id < 1) return fail(ErrorKind::Config, Session::no_package_sentence());
  if (options.journal_dir.empty()) return fail(ErrorKind::Config, "earthbank: no directory was named for the upload's journals");
  const Result<std::optional<Resolution>> resolution = parse_resolution(options.resolve_in_doubt);
  if (!resolution) return fail(resolution.error());
  if (!session.signed_in()) {
    Error error{ErrorKind::NotConnected, {}, ""};
    error.code = std::string(kNotSignedIn);
    error.what = Session::sentence(error, "", "upload", false) + " Sign in before uploading.";
    return fail(std::move(error));
  }

  const auto name_of = [](const Submission& s) {
    std::string name = s.sample.name.value_or(std::string());
    return name.empty() ? s.datapoint_name : name;
  };

  // Check first: nothing is written while a term is unknown (or cannot be
  // asked about, which allow_unresolved does not cover).
  const PreflightReport checked = preflight(submissions, resolver);
  UploadReport report;
  if (!checked.clean() && (!options.allow_unresolved || !checked.could_not_ask().empty())) {
    report.misses = checked.misses;
    for (const Submission& s : submissions) {
      SampleReport out;
      out.sample = name_of(s);
      out.status = SampleStatus::Refused;
      std::size_t own = 0;
      for (const Miss& miss : checked.misses) own += miss.sample == out.sample ? 1 : 0;
      out.messages.push_back(own > 0 ? out.sample + ": nothing was sent, because EarthBank does not have " +
                                           std::to_string(own) + (own == 1 ? " term" : " terms") +
                                           " of this sample (listed with the upload)."
                                     : out.sample + ": nothing was sent, because terms of other samples in this upload "
                                                    "are not on EarthBank (listed with the upload).");
      report.samples.push_back(std::move(out));
    }
    return report;
  }

  Env env{session, resolver, options, term_fields(), host_of(session.base_url()), std::nullopt, false, &checked.misses, false};

  // A resolution names no sample. It is used when exactly one sample has a
  // record in doubt that the user is to settle.
  if (*resolution) {
    std::size_t in_doubt = 0;
    for (const Submission& s : submissions) {
      const std::string name = s.sample.name.value_or(std::string());
      if (name.empty()) continue;
      const auto loaded = load_journal(journal_file(options.journal_dir, env.host, options.data_package_id, name));
      if (!loaded || !*loaded) continue;
      const Journal& j = **loaded;
      if (j.hash == content_hash(s) && j.in_doubt && *j.in_doubt != kSampleLabel) ++in_doubt;
    }
    env.resolution = **resolution;
    env.resolution_is_ambiguous = in_doubt > 1;
  }

  std::set<std::string> seen;
  for (const Submission& s : submissions) {
    const std::string name = s.sample.name.value_or(std::string());
    if (!name.empty() && !seen.insert(name).second) {
      SampleReport out;
      out.sample = name;
      out.status = SampleStatus::Refused;
      out.messages.push_back(name + ": another submission of this upload is of a sample of the same name, and one journal cannot "
                                    "hold both, so this one was not sent.");
      report.samples.push_back(std::move(out));
      continue;
    }
    report.samples.push_back(SampleRun(env, s, name_of(s)).run());
  }
  return report;
}

}  // namespace pychron::earthbank
