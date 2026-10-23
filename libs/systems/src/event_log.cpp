#include "pychron/systems/event_log.hpp"

#include <array>
#include <optional>

#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/detectors.hpp"
#include "pychron/systems/jobs/job.hpp"
#include "pychron/systems/jobs/peak_center.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::systems {
namespace {

constexpr std::array<EventClass, 11> kSystemsClasses{{
    {"JobStarted", LogLevel::Debug},
    {"JobFinished", LogLevel::Debug},
    {"PeakCenterTryStarted", LogLevel::Debug},
    {"PeakCenterDone", LogLevel::Debug},
    {"DetectorState", LogLevel::Debug},
    {"FieldTableChanged", LogLevel::Debug},
    {"ScanStatus", LogLevel::Debug},     // warn when it carries an error
    {"JobProgress", std::nullopt},       // every step of a job
    {"PeakCenterPoint", std::nullopt},   // every step of a sweep
    {"IntensityReading", std::nullopt},  // every integration
    {"MagnetMoved", std::nullopt},       // every hop and every step of a sweep
}};

}  // namespace

SystemsEventRecorder::SystemsEventRecorder(SignalBus& bus) {
  using jobs::JobFinished;
  using jobs::JobStarted;
  using jobs::PeakCenterDone;
  using jobs::PeakCenterTryStarted;
  using spectrometer::DetectorState;
  using spectrometer::FieldTableChanged;
  using spectrometer::ScanStatus;

  // `before`, the spectrometer's whole state, is left out.
  subscriptions_.push_back(
      record_event<JobStarted>(bus, "JobStarted", LogLevel::Debug, [](const JobStarted& e, EventLine& l) {
        l.count("id", e.id).text("kind", e.kind);
      }));
  // The job's record, flattened to what says how it ended.
  subscriptions_.push_back(
      record_event<JobFinished>(bus, "JobFinished", LogLevel::Debug, [](const JobFinished& e, EventLine& l) {
        l.count("id", e.job.id).text("kind", e.job.kind).word("state", jobs::to_string(e.job.state));
        if (e.job.error) l.text("error", to_string(*e.job.error));
      }));
  subscriptions_.push_back(record_event<PeakCenterTryStarted>(
      bus, "PeakCenterTryStarted", LogLevel::Debug, [](const PeakCenterTryStarted& e, EventLine& l) {
        l.text("detector", e.detector)
            .text("isotope", e.isotope)
            .list("additional", e.additional)
            .count("try_index", e.try_index)
            .count("n_tries", e.n_tries)
            .number("start", e.start)
            .number("end", e.end)
            .number("initial_center", e.initial_center);
      }));
  // The result, flattened: its config, shapes and sweeps are left out (tries
  // is their count); notes only when there are any.
  subscriptions_.push_back(
      record_event<PeakCenterDone>(bus, "PeakCenterDone", LogLevel::Debug, [](const PeakCenterDone& e, EventLine& l) {
        const auto& r = e.result;
        l.flag("ok", r.ok)
            .text("message", r.message)
            .text("detector", r.detector)
            .text("isotope", r.isotope)
            .number("initial_center", r.initial_center)
            .number("center", r.center)
            .number("table_value", r.table_value)
            .flag("table_updated", r.table_updated)
            .count("tries", r.tries.size());
        if (!r.notes.empty()) l.list("notes", r.notes);
      }));
  subscriptions_.push_back(
      record_event<DetectorState>(bus, "DetectorState", LogLevel::Debug, [](const DetectorState& e, EventLine& l) {
        l.text("detector", e.detector)
            .flag("active", e.active)
            .text("isotope", e.isotope)
            .flag("protected", e.protected_)
            .number("deflection", e.deflection)
            .number("gain", e.gain)
            .number("cdd_voltage", e.cdd_voltage);
      }));
  subscriptions_.push_back(record_event<FieldTableChanged>(
      bus, "FieldTableChanged", LogLevel::Debug,
      [](const FieldTableChanged& e, EventLine& l) { l.text("name", e.name).text("version", e.version); }));
  subscriptions_.push_back(record_event<ScanStatus>(
      bus, "ScanStatus",
      [](const ScanStatus& e) -> std::optional<LogLevel> { return e.error.empty() ? LogLevel::Debug : LogLevel::Warn; },
      [](const ScanStatus& e, EventLine& l) {
        l.text("spectrometer", e.spectrometer)
            .flag("running", e.running)
            .flag("paused", e.paused)
            .seconds("integration", e.integration)
            .text("error", e.error);
      }));
}

std::span<const EventClass> systems_event_classes() noexcept { return kSystemsClasses; }

}  // namespace pychron::systems
