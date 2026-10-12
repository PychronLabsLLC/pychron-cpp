// One whole upload against a service, start to finish, and its clean-up: the
// body of the live test (tests/earthbank_qt/test_live.cpp), which a person
// with an EarthBank login runs by hand. It is written against IHttpClient so
// that the same steps are run against the fake on every build (test_live.cpp
// here): what a person runs once a year has been run by the suite that day.
//
// It makes a data package of its own, `pychron-test-<UTC date-time>`, says
// its name and id before anything else so that it can be removed by hand if
// the process dies, uploads the two fixture samples into it, checks what came
// back, uploads again to see that nothing is sent twice, and deletes the
// package whatever happened in between.
//
// What it cannot do: read a data point or its measurements back. No way to
// read one is known (legacy never did), so the counts are those the service
// acknowledged, record by record, and the comparison of values is the
// runbook's (docs/earthbank.md): by eye, on the website.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "analyses.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/mapping.hpp"
#include "pychron/earthbank/preflight.hpp"
#include "pychron/earthbank/session.hpp"
#include "pychron/earthbank/uploader.hpp"
#include "upload_support.hpp"

namespace pychron::earthbank::testing {

struct RoundTrip {
  std::string base_url, username, password;
  std::optional<std::int64_t> institution_id;
  // Where what the person needs to read goes.
  std::ostream* out = &std::cout;
};

// `pychron-test-20311005-142233`, UTC.
inline std::string test_package_name(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) {
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  std::string stamp(32, '\0');
  stamp.resize(std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &utc));
  return "pychron-test-" + stamp;
}

// Removes the package when it goes, pass or fail. A package that could not
// be removed is a failure that says so, with its id: it is a real package on
// a real service, and somebody has to take it away.
class PackageGuard {
 public:
  PackageGuard(Session& session, DataPackage package, std::ostream& out)
      : session_(session), package_(std::move(package)), out_(out) {}
  PackageGuard(const PackageGuard&) = delete;
  PackageGuard& operator=(const PackageGuard&) = delete;
  ~PackageGuard() {
    HttpRequest request;
    request.method = "DELETE";
    request.url = session_.url("/api/management/data-packages/" + std::to_string(package_.id));
    const auto answer = session_.send(request, true);
    const bool gone = answer && answer->status >= 200 && answer->status < 300;
    if (gone) {
      out_ << "removed data package " << package_.id << " '" << package_.name << "'\n";
      return;
    }
    const std::string why = answer ? "status " + std::to_string(answer->status) : to_string(answer.error());
    out_ << "data package " << package_.id << " '" << package_.name << "' was NOT removed (" << why
         << "): remove it on EarthBank by hand\n";
    ADD_FAILURE() << "the test's data package " << package_.id << " '" << package_.name << "' was not removed (" << why
                  << "): remove it on EarthBank by hand";
  }

 private:
  Session& session_;
  DataPackage package_;
  std::ostream& out_;
};

inline void run_round_trip(IHttpClient& http, const RoundTrip& with) {
  std::ostream& out = *with.out;
  Session session(http, with.base_url);
  const auto in = session.sign_in(with.username, with.password);
  ASSERT_TRUE(in) << to_string(in.error());
  const auto account = session.account();
  ASSERT_TRUE(account) << to_string(account.error());
  out << "signed in to " << session.base_url() << " as " << account->name << " (" << account->login << ")\n";

  const auto made = session.create_package(test_package_name(), with.institution_id);
  ASSERT_TRUE(made) << to_string(made.error())
                    << "\n(a package may have been made all the same: look for one named pychron-test-...)";
  // Said before anything is put in it, so that it can be found if this dies.
  out << "made data package " << made->id << " '" << made->name << "'" << std::endl;
  const PackageGuard guard(session, *made, out);

  PublishOptions options;
  options.software = "pychron-cpp live test";
  const auto submissions = build_submissions(both_samples(), options);
  ASSERT_TRUE(submissions) << to_string(submissions.error());
  ASSERT_EQ(submissions->size(), 2u);

  // The terms of the fixture samples that this service does not know are
  // said, and left out of what is sent; one it could not be asked about
  // stops the test, since nothing can be concluded from it.
  LookupResolver resolver(session);
  const PreflightReport checked = preflight(*submissions, resolver);
  for (const Miss& miss : checked.misses) {
    out << "pre-flight: " << miss.sample << ": " << miss.record << '.' << miss.field << ": '" << miss.value << "' "
        << (miss.outcome == LookupOutcome::CouldNotAsk ? "could not be asked" : "is not known to the service") << ": "
        << miss.detail << '\n';
  }
  for (const Resolved& found : checked.resolved) {
    out << "pre-flight: " << found.record << '.' << found.field << ": '" << found.value << "' is '" << found.matched_name
        << "'\n";
  }
  ASSERT_TRUE(checked.could_not_ask().empty()) << "some terms could not be asked about";

  const TempDir journals;
  UploadOptions upload_options;
  upload_options.data_package_id = made->id;
  upload_options.allow_unresolved = true;
  upload_options.journal_dir = journals.path();
  upload_options.on_event = [&out](const UploadEvent& event) {
    if (!event.record.empty()) out << event.sample << ": " << event.record << ": " << event.detail << '\n';
  };

  const auto first = upload(*submissions, session, resolver, upload_options);
  ASSERT_TRUE(first) << to_string(first.error());
  ASSERT_EQ(first->samples.size(), submissions->size());
  std::vector<std::int64_t> data_points;
  for (std::size_t i = 0; i < submissions->size(); ++i) {
    const Submission& sent = (*submissions)[i];
    const SampleReport& got = first->samples[i];
    std::string said;
    for (const std::string& message : got.messages) said += "\n  " + message;
    ASSERT_EQ(got.status, SampleStatus::Complete) << got.sample << said;
    ASSERT_TRUE(got.data_point_id.has_value()) << got.sample;
    EXPECT_TRUE(got.missing.empty()) << got.sample;
    // The sample, the data point, each aliquot and measurement, the age
    // calculation and the age summary: every one acknowledged with an id.
    EXPECT_EQ(got.written.size(), 2u + sent.aliquots.size() + sent.measurements.size() + 2u) << got.sample;
    data_points.push_back(*got.data_point_id);
    out << got.sample << ": complete, data point " << *got.data_point_id << ", " << sent.aliquots.size()
        << " aliquots, " << sent.measurements.size() << " measurements, age " << sent.age_summary.age.value_or(0.0)
        << " +/- " << sent.age_summary.ageUncertainty.value_or(0.0) << " Ma ("
        << sent.age_summary.ageCalcTypeName.value_or("") << ")\n";
  }

  // Again, with the same journal: everything is there and nothing is sent.
  const auto second = upload(*submissions, session, resolver, upload_options);
  ASSERT_TRUE(second) << to_string(second.error());
  ASSERT_EQ(second->samples.size(), submissions->size());
  for (std::size_t i = 0; i < second->samples.size(); ++i) {
    const SampleReport& got = second->samples[i];
    EXPECT_EQ(got.status, SampleStatus::Complete) << got.sample;
    EXPECT_TRUE(got.written.empty()) << got.sample << ": a second upload wrote " << got.written.size() << " records";
    EXPECT_EQ(got.data_point_id, std::optional<std::int64_t>(data_points[i])) << got.sample;
  }
  out << "a second upload sent nothing new\n";
}

}  // namespace pychron::earthbank::testing
