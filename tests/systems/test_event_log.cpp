#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "log_capture.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/spectrometer/detectors.hpp"
#include "pychron/systems/event_log.hpp"
#include "pychron/systems/jobs/job.hpp"
#include "pychron/systems/jobs/peak_center.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::systems {
namespace {

using pychron::testing::LogCapture;
using Lines = std::vector<std::string>;

// Every struct libs/systems publishes on a SignalBus (jobs/job.hpp,
// jobs/peak_center.hpp, spectrometer/acquisition.hpp, spectrometer.hpp,
// scan_service.hpp; DetectorState is declared in libs/devices and published
// by the Spectrometer). Kept by hand: a new event fails EveryEventClassified
// until systems_event_classes() has it.
const std::set<std::string_view> kSystemsEvents{
    "JobStarted",       "JobProgress",       "JobFinished",   "PeakCenterTryStarted", "PeakCenterPoint",
    "PeakCenterDone",   "IntensityReading",  "DetectorState", "MagnetMoved",          "FieldTableChanged",
    "ScanStatus",
};

TEST(SystemsEventLog, EveryEventClassified) {
  std::set<std::string_view> classified;
  for (const auto& c : systems_event_classes()) EXPECT_TRUE(classified.insert(c.name).second) << c.name << " twice";
  EXPECT_EQ(classified, kSystemsEvents);
}

TEST(SystemsEventLog, JobStartedAndFinished) {
  LogCapture log;
  ASSERT_TRUE(log.ok());
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(jobs::JobStarted{7, "sweep", {}});
  jobs::Job job;
  job.id = 7;
  job.kind = "sweep";
  job.state = jobs::JobState::Failed;
  job.error = Error{ErrorKind::Cancelled, "stopped", "", ""};
  bus.publish(jobs::JobFinished{job});
  job.state = jobs::JobState::Succeeded;
  job.error.reset();
  bus.publish(jobs::JobFinished{job});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.JobStarted: id=7, kind=sweep",
                                      "[debug] trace.JobFinished: id=7, kind=sweep, state=failed, error=cancelled: stopped",
                                      "[debug] trace.JobFinished: id=7, kind=sweep, state=succeeded"}));
}

TEST(SystemsEventLog, PeakCenterTryStarted) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(jobs::PeakCenterTryStarted{"H1", "Ar40", {"L1", "CDD"}, 0, 2, 4.9, 5.1, 5.0});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.PeakCenterTryStarted: detector=H1, isotope=Ar40, additional=[L1,CDD], try_index=0, "
                   "n_tries=2, start=4.9, end=5.1, initial_center=5"}));
}

TEST(SystemsEventLog, PeakCenterDone) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  jobs::PeakCenterResult r;
  r.ok = true;
  r.message = "centered";
  r.detector = "H1";
  r.isotope = "Ar40";
  r.initial_center = 5.0;
  r.center = 5.002;
  r.table_value = 5.001;
  r.table_updated = true;
  r.tries.resize(2);
  r.notes = {"field table not saved: disk full"};
  bus.publish(jobs::PeakCenterDone{r});
  r.ok = false;
  r.message = "no peak";
  r.center.reset();
  r.table_value.reset();
  r.table_updated = false;
  r.tries.resize(1);
  r.notes.clear();
  bus.publish(jobs::PeakCenterDone{r});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.PeakCenterDone: ok=true, message=centered, detector=H1, isotope=Ar40, "
                   "initial_center=5, center=5.002, table_value=5.001, table_updated=true, tries=2, "
                   "notes=[field table not saved: disk full]",
                   "[debug] trace.PeakCenterDone: ok=false, message=no peak, detector=H1, isotope=Ar40, "
                   "initial_center=5, table_updated=false, tries=1"}));
}

TEST(SystemsEventLog, DetectorState) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  spectrometer::DetectorState s;
  s.detector = "CDD";
  s.isotope = "Ar36";
  s.protected_ = true;
  s.deflection = 50;
  bus.publish(s);
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.DetectorState: detector=CDD, active=true, isotope=Ar36, protected=true, deflection=50"}));
}

TEST(SystemsEventLog, FieldTableChanged) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(spectrometer::FieldTableChanged{"argon", "3"});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.FieldTableChanged: name=argon, version=3"}));
}

TEST(SystemsEventLog, ScanStatusWarnsOnError) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(spectrometer::ScanStatus{"argus", true, false, std::chrono::seconds(1), "", {}});
  bus.publish(spectrometer::ScanStatus{"argus", false, false, std::chrono::seconds(1), "link down", {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.ScanStatus: spectrometer=argus, running=true, paused=false, integration=1s, error=",
                   "[warn] trace.ScanStatus: spectrometer=argus, running=false, paused=false, integration=1s, "
                   "error=link down"}));
}

TEST(SystemsEventLog, NeverLoggedEventsWriteNothing) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  const auto before = log.lines();
  bus.publish(jobs::JobProgress{1, "sweep", {}});
  bus.publish(jobs::PeakCenterPoint{0, {}});
  bus.publish(spectrometer::IntensityReading{});
  bus.publish(spectrometer::MagnetMoved{});
  EXPECT_EQ(log.lines(), before);
}

}  // namespace
}  // namespace pychron::systems
