#pragma once

// Spectrometer facade (spectrometer spec sections 2.2, 4, 5). Managers, jobs
// and UI see only this class, never the role interfaces behind it. It is
// built by SpectrometerAssembler (assembler.hpp) from config, or directly
// from role pointers in tests.
//
// Position pipeline (4.2):
//   target -> mass (MolecularWeights) -> table value (FieldTable, detector `on`)
//          -> corrections (deflection, HV) -> native -> move protocol (4.4)
// uncorrect()/mass_at() run it backwards exactly.
//
// Every hardware operation is serialized by one mutex; events (MagnetMoved,
// DetectorState) are published on the SignalBus after the operation. A caller
// that finds it taken (a move settling, say) waits for it in the context
// clock's time.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/clock_mutex.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/spectrometer/detectors.hpp"
#include "pychron/devices/spectrometer/roles.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/corrections.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"
#include "pychron/systems/spectrometer/move_protocol.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron::spectrometer {

// ---- position request -----------------------------------------------------

struct Isotope {
  std::string name;
};
struct Mass {
  double amu = 0.0;
};
struct NativeUnits {
  double value = 0.0;
};

struct PositionTarget {
  std::variant<Isotope, Mass, NativeUnits> target;
  DetectorId on;  // empty: the reference detector
};

// Auto: protect detectors whose peaks lie on the path, and everything (plus
// blank) for moves above beam_blank_threshold. Always: protect every listed
// detector and blank. Never: no protection.
enum class ProtectPolicy { Auto, Always, Never };

struct PositionOptions {
  // Default [magnet].settle_ms. A move that fails once a set went out waits at
  // least [magnet].settle_ms before its cleanup, whatever is given here.
  std::optional<Duration> settle;
  ProtectPolicy protect = ProtectPolicy::Auto;
  bool wait_moving = true;
  // Large-move confirmation is a UI concern; the core never blocks on it.
  bool confirmed = false;
};

struct PositionResult {
  std::optional<double> mass;         // amu; nullopt for NativeUnits targets
  std::optional<double> table_value;  // before corrections
  double native = 0.0;                // value sent to the positioner / HV supply
  MoveOutcome move;
};

// ---- events ---------------------------------------------------------------

struct MagnetMoved {
  double from = 0.0;
  double to = 0.0;
  IMassPositioner::Axis axis = IMassPositioner::Axis::Dac;
  std::optional<double> mass_on_reference;  // amu on the reference detector
  Duration elapsed{};
  TimePoint ts{};
};

// A field table got a new current version in the store (save_table, and the
// editor's replace and restore). `version` is the store's bare version.
struct FieldTableChanged {
  std::string name;
  std::string version;
};

// ---- snapshot -------------------------------------------------------------

struct SpectrometerState {
  std::string name;
  IMassPositioner::Axis axis = IMassPositioner::Axis::Dac;
  std::optional<double> magnet;             // native value
  std::optional<double> hv;
  std::map<std::string, Readback> params;   // to_string(ParamId) -> readback
  std::vector<DetectorState> detectors;     // ts zeroed
  Duration integration{};
  std::string field_table;                  // active table name
  TimePoint ts{};                           // not hashed
  std::uint64_t hash = 0;                   // content_hash() of the rest

  std::string hash_hex() const;
};

// FNV-1a over every field except `ts` and `hash`, doubles bit-exact, so equal
// instrument states dedupe in persistence.
std::uint64_t content_hash(const SpectrometerState& state);

// ---- composition ----------------------------------------------------------

// Role bindings plus ownership of whatever backs them. Only `positioner` and
// at least one acquirer are required.
struct SpectrometerRoles {
  IMassPositioner* positioner = nullptr;
  IBeamSource* source = nullptr;
  std::vector<std::pair<std::string, IIntensityAcquirer*>> acquirers;  // driver name -> acquirer
  IDetectorControl* detector_control = nullptr;
  IBeamBlank* beam_blank = nullptr;

  std::vector<std::unique_ptr<Transport>> transports;  // kept alive, destroyed last
  // One per transport but those of kind "link" ([transports.<name>.recovery]),
  // shared by the drivers on it: destroyed after them, before the transports.
  std::vector<std::unique_ptr<Link>> links;
  std::vector<std::unique_ptr<Device>> devices;
};

struct SpectrometerContext {
  const Clock& clock;
  Scheduler& scheduler;
  SignalBus& bus;
};

struct SpectrometerOptions {
  // Waits (settle, moving() polls, demag steps). Default blocks on the
  // context clock; tests inject one that advances a ManualClock.
  std::function<void(Duration)> sleep;
  Duration max_wait = std::chrono::seconds(30);
  Duration poll_interval = std::chrono::milliseconds(50);
  double epsilon = 1e-6;
  Duration hv_cache = std::chrono::seconds(1);  // HV readback age for the correction
  // Auto protection treats a detector's table peaks within this many native
  // units of the path as "on the path".
  double protect_margin = 0.05;
  // integration and timeout_factor are taken from config.
  AcquisitionEngine::Options acquisition;
  // Data directory root; enables save_table() and with_table() of tables
  // not loaded at startup.
  std::filesystem::path data_root;
  // Where transports with `trace = true` write <name>.trace; created on demand.
  std::filesystem::path trace_dir = "traces";
};

class Spectrometer {
 public:
  using Options = SpectrometerOptions;

  // Config error when a required role is missing or the pieces disagree.
  static Result<std::unique_ptr<Spectrometer>> create(cfg::SpectrometerConfig config, MolecularWeights weights,
                                                      std::map<std::string, FieldTable> tables,
                                                      SpectrometerRoles roles, SpectrometerContext context,
                                                      Options options = {});
  ~Spectrometer();
  Spectrometer(const Spectrometer&) = delete;
  Spectrometer& operator=(const Spectrometer&) = delete;

  const std::string& name() const noexcept { return config_.system.name; }
  const cfg::SpectrometerConfig& config() const noexcept { return config_; }
  const MolecularWeights& weights() const noexcept { return weights_; }
  IMassPositioner::Axis native_axis() const noexcept { return axis_; }
  const DetectorId& reference_detector() const noexcept { return config_.system.reference_detector; }

  // Capability flags for UI.
  bool has_source() const noexcept { return roles_.source != nullptr; }
  bool has_detector_control() const noexcept { return roles_.detector_control != nullptr; }
  bool has_beam_blank() const noexcept { return roles_.beam_blank != nullptr; }
  // Its links, one per transport but those of kind "link" (spec 4.3).
  std::vector<Link*> links() const {
    std::vector<Link*> out;
    out.reserve(roles_.links.size());
    for (const auto& l : roles_.links) out.push_back(l.get());
    return out;
  }
  Caps detector_caps() const;

  // ---- positioning ----
  Result<PositionResult> position(const PositionTarget& target, PositionOptions options = {});
  // Move protocol only (section 4.4); `value` is native. Like position(), a
  // value outside the positioner's limits or [magnet].limits (the stricter
  // bound on each side) is Config with nothing read or written. If detector
  // protection cannot be planned (a correction fails, e.g. the HV read behind
  // it), the move fails with that error and nothing is set, protected or
  // blanked.
  Result<MoveOutcome> move_native(double value, PositionOptions options = {});
  // HV table + IBeamSource::set_hv (section 4.5).
  Result<PositionResult> position_hv(double mass, const DetectorId& det, PositionOptions options = {});
  Result<PositionResult> position_hv(const PositionTarget& target, PositionOptions options = {});
  Result<double> magnet_native();

  // Pipeline pieces, exposed for jobs (peak center uncorrects its result).
  Result<double> mass_of(const PositionTarget& target) const;
  Result<double> correct(double table_value, const DetectorId& det);
  Result<double> uncorrect(double native, const DetectorId& det);
  Result<double> native_for(double mass, const DetectorId& det);  // table + corrections
  Result<double> mass_at(double native, const DetectorId& det);   // inverse

  // ---- field tables ----
  const std::string& active_table() const;
  FieldTable field_table() const;  // copy of the active table
  Result<void> update_table(const DetectorId& det, const std::string& isotope, double table_value,
                            std::optional<bool> propagate = std::nullopt);
  // Saves the active table as a new version under <data_root>/tables and
  // publishes FieldTableChanged. Not refused while a TableHold is out.
  Result<std::string> save_table(std::chrono::system_clock::time_point when);
  // The store's current version of table `name`; Config error without a
  // data_root.
  Result<std::string> table_version(const std::string& name) const;
  const std::filesystem::path& data_root() const noexcept { return options_.data_root; }

  // The editor's Save (field table editor spec, section 5): writes `table` as
  // a new version of `name`, makes it current, swaps it in when `name` is
  // loaded, and publishes FieldTableChanged. Refused, with nothing written,
  // in this order: a TableHold is out (code "table_held"); the table's axis
  // is not the positioner's ("table_axis"); check_field_table reports an
  // error ("table_invalid", one line per error); the store's current version
  // is not `base_version` ("table_conflict"; an empty `base_version` skips
  // this check). Config error without a data_root. Returns the new version.
  Result<std::string> replace_table(const std::string& name, FieldTable table, std::string_view base_version,
                                    std::chrono::system_clock::time_point when);
  // Make Current: points `name`'s current at an existing `version`, reloads
  // it when `name` is loaded, and publishes FieldTableChanged. Refused while
  // a TableHold is out ("table_held"), and when the version does not parse or
  // check_field_table reports an error ("table_invalid").
  Result<void> restore_table_version(const std::string& name, const std::string& version);

  // RAII: while any is out, the table may not be replaced or restored to
  // another version (a queue or a peak center is using it). Move-only; a
  // default-constructed or moved-from hold holds nothing. Must not outlive
  // the Spectrometer that gave it.
  class TableHold {
   public:
    TableHold() = default;
    TableHold(TableHold&& other) noexcept;
    TableHold& operator=(TableHold&& other) noexcept;
    TableHold(const TableHold&) = delete;
    TableHold& operator=(const TableHold&) = delete;
    ~TableHold();
    void release();

   private:
    friend class Spectrometer;
    TableHold(Spectrometer* owner, std::uint64_t id) : owner_(owner), id_(id) {}
    Spectrometer* owner_ = nullptr;
    std::uint64_t id_ = 0;
  };
  [[nodiscard]] TableHold hold_table(std::string holder);
  // The first holder still out, for the UI's enable state.
  std::optional<std::string> table_held() const;

  // Waits `d` on the context clock (simulated time under a ManualClock), or
  // through SpectrometerOptions::sleep when injected. Jobs settle with it.
  void sleep(Duration d) const;

  // RAII: selects table `name` (loaded or read from data_root) for the
  // lifetime of the scope, then restores the previous one.
  class TableScope {
   public:
    TableScope() = default;
    TableScope(TableScope&& other) noexcept;
    TableScope& operator=(TableScope&& other) noexcept;
    TableScope(const TableScope&) = delete;
    TableScope& operator=(const TableScope&) = delete;
    ~TableScope();
    void release();

   private:
    friend class Spectrometer;
    TableScope(Spectrometer* owner, std::string previous) : owner_(owner), previous_(std::move(previous)) {}
    Spectrometer* owner_ = nullptr;
    std::string previous_;
  };
  Result<TableScope> with_table(const std::string& name);

  // ---- detectors ----
  const DetectorSet& detectors() const noexcept { return *detectors_; }
  Result<DetectorState> detector_state(const DetectorId& det) const;
  Result<void> set_active(const DetectorId& det, bool active);
  Result<void> set_isotope(const DetectorId& det, std::string isotope);
  // Guarded by caps(); unsupported ops fail with Config. Deflection is
  // clamped to the detector's `max`.
  Result<void> protect(const DetectorId& det, bool on);
  Result<double> set_deflection(const DetectorId& det, double value);  // returns the clamped value
  Result<void> set_gain(const DetectorId& det, double value);
  Result<void> set_cdd_voltage(const DetectorId& det, double volts);

  // ---- source ----
  Result<void> set_hv(double volts);
  Result<double> read_hv();
  Result<std::vector<ParamSpec>> source_params() const;
  Result<void> set_param(const ParamId& id, double value);
  Result<Readback> read_param(const ParamId& id);

  // ---- acquisition ----
  AcquisitionEngine& acquisition() noexcept { return *engine_; }
  Result<std::vector<Reading>> acquire(std::size_t n) { return engine_->acquire(n); }
  Result<std::vector<Reading>> acquire(Duration duration) { return engine_->acquire(duration); }

  // All readable params, detector states (control readbacks refreshed where
  // caps allow), magnet native value, integration time; content-hashed.
  SpectrometerState snapshot();

 private:
  Spectrometer(cfg::SpectrometerConfig config, MolecularWeights weights, std::map<std::string, FieldTable> tables,
               SpectrometerRoles roles, SpectrometerContext context, Options options);
  Result<void> build();

  // Unlocked implementations.
  Result<MoveOutcome> move_locked(double value, const PositionOptions& options);
  Result<CorrectionInputs> inputs_locked(const DetectorId& det);
  Result<double> hv_locked();
  const FieldTable& table_locked() const;
  Result<const FieldTable*> table_named_locked(const std::string& name);
  Result<ChannelId> control_channel(const DetectorId& det) const;
  // Channels to protect for a move and whether to blank. Fails when a
  // correction it needs cannot be computed (an HV read that fails, say): the
  // caller must then not move, since the path cannot be judged.
  Result<std::vector<ChannelId>> plan_protection(double from, double to, ProtectPolicy policy, bool& blank);
  void restore_table(const std::string& name);
  void release_hold(std::uint64_t id);
  // Error "table_held" naming the first holder, or nothing.
  Result<void> refuse_if_held() const;

  cfg::SpectrometerConfig config_;
  MolecularWeights weights_;
  SpectrometerRoles roles_;
  SpectrometerContext context_;
  Options options_;
  IMassPositioner::Axis axis_ = IMassPositioner::Axis::Dac;

  // Held across device calls and a move's settle, which wait in clock time:
  // a clock mutex. Recursive: the locked operations call one another.
  mutable RecursiveClockMutex mutex_;
  std::map<std::string, FieldTable> tables_;
  std::string active_table_;
  std::optional<DetectorSet> detectors_;
  std::map<DetectorId, ChannelId> local_channel_;  // detector -> driver-local channel
  std::optional<std::pair<double, TimePoint>> hv_cache_;

  // Table holds, in the order taken. A plain mutex: it guards these fields
  // only and is never held across clock time, so table_held() from the UI
  // thread does not wait behind a settling move. Taken after mutex_ when both
  // are needed, never before.
  mutable std::mutex holds_mutex_;
  std::vector<std::pair<std::uint64_t, std::string>> holds_;
  std::uint64_t next_hold_id_ = 1;

  std::vector<std::unique_ptr<IIntensityAcquirer>> adapters_;  // "driver:channel" views
  std::unique_ptr<AcquisitionEngine> engine_;
};

// cfg -> runtime model conversions shared with the assembler.
DetectorConfig to_detector_config(const cfg::DetectorConfig& d);
FieldTable to_field_table(const cfg::TableFile& table);
IMassPositioner::Axis to_axis(cfg::Axis axis) noexcept;

}  // namespace pychron::spectrometer
