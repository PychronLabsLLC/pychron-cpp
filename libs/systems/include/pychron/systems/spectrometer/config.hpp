#pragma once

// Typed model of `spectrometer.toml` (spectrometer spec section 7).
//
// One schema covers both composition shapes: an integrated vendor box binds
// every role to one driver on one transport; a legacy split binds one driver
// per role. Only the bindings differ. The model is plain data: parsing lives in
// config_loader.hpp, assembler rules in config_validate.hpp.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/config/system_config.hpp"

namespace pychron::spectrometer::cfg {

using config::Located;
using config::SourceLoc;

// Role a driver can play (spec section 2.1).
enum class Role { Positioner, Source, Acquirer, DetectorControl, BeamBlank, Optics };

// "positioner", "source", "acquirer", "detector_control", "beam_blank", "optics".
std::string_view to_string(Role role) noexcept;
std::optional<Role> role_from_string(std::string_view name) noexcept;

// What the positioner natively accepts (mirrors IMassPositioner::Axis).
enum class Axis { Dac, Field, Mass };
std::string_view to_string(Axis axis) noexcept;
std::optional<Axis> axis_from_string(std::string_view name) noexcept;

enum class DetectorKind { Faraday, Counter, Cdd, Atona };
std::string_view to_string(DetectorKind kind) noexcept;

struct SystemSection : Located {
  std::string name;
  std::string reference_detector;
  double integration_time_s = 1.0;
};

enum class TransportKind { Tcp, Udp, Serial, ModbusTcp, ModbusRtu, LabjackU3, Sim, Link };

// `[transports.<name>]`. `port` is a TCP port number for network kinds and a
// device path for serial kinds, so both forms are kept.
struct TransportConfig : Located {
  std::string name;
  TransportKind kind = TransportKind::Sim;
  std::string host;
  std::int64_t tcp_port = 0;
  std::string serial_port;
  std::string link;  // kind "link": the shared connection's name (NGX)
  std::int64_t baud = 9600;
  std::int64_t timeout_ms = 1000;
  std::int64_t retries = 0;  // extra attempts per request; 0 = none
  bool trace = false;        // record the wire to <trace_dir>/<name>.trace
  // `[transports.<name>.recovery]`. Not on kind "link": that transport is
  // recovered by the connection it names.
  config::RecoveryConfig recovery;
};

// `[drivers.<name>]`. Keys other than kind/transport/roles/channels are kept
// verbatim in `options` for the driver factory (e.g. `sample_hz`, `channel`).
struct DriverConfig : Located {
  std::string name;
  std::string kind;
  std::string transport;
  std::vector<Role> roles;
  std::vector<std::string> channels;  // acquirer channel names, "H1", "EM", ...
  toml::table options;

  bool has_role(Role r) const;
};

struct Limits {
  double min = 0.0;
  double max = 0.0;
};

struct Corrections {
  bool deflection = false;
  bool hv = false;
};

struct MagnetProtection {
  std::vector<std::string> detectors;
  std::optional<double> beam_blank_threshold;
};

struct AfDemag {
  bool enabled = false;
  double period_s = 0.5;
  double duration_s = 10.0;
  double start_amplitude = 0.5;
  double threshold = 0.5;
};

struct MagnetSection : Located {
  std::string positioner;  // driver name
  Axis native_axis = Axis::Dac;
  std::optional<Limits> limits;  // native units; enforced by Spectrometer on top of the positioner's own
  std::int64_t settle_ms = 0;
  std::string field_table;
  std::string hv_table;  // optional; empty when absent
  bool propagate = false;  // FieldTable::update default (spec 4.3)
  Corrections corrections;
  MagnetProtection protection;
  AfDemag af_demag;
};

struct SourceSection : Located {
  std::string driver;
  std::optional<double> nominal_hv;
  std::map<std::string, double> ramp;  // canonical param name -> rate per second
};

enum class BinEpoch { Shared, PerAcquirer };

struct AcquisitionSection : Located {
  std::vector<std::string> acquirers;  // driver names
  bool stale_frame_guard = true;
  double timeout_factor = 2.0;
  bool host_integration = false;
  BinEpoch bin_epoch = BinEpoch::Shared;
  std::vector<std::string> ignored_channels;  // "<driver>:<channel>"
};

struct DetectorControlSection : Located {
  std::string driver;
};

struct Deflection {
  bool control = false;
  std::vector<double> correction;  // polynomial coefficients, low order first
  int sign = 1;
  std::optional<double> max;
  std::optional<double> per_volt;
};

struct DetectorProtection {
  double threshold = 0.0;
  bool on_move = false;
};

// `[[detectors]]` (spec section 5.2). Static config only; runtime state lives
// in DetectorSet.
struct DetectorConfig : Located {
  std::string name;
  DetectorKind kind = DetectorKind::Faraday;
  std::string channel;  // "<acquirer driver>:<channel>"
  std::string units;
  double software_gain = 1.0;
  std::string isotope;
  std::string color;  // "#rrggbb" lower-case; empty = unset
  bool active = true;
  std::optional<Deflection> deflection;
  std::optional<DetectorProtection> protection;
  std::optional<double> saturation;
  std::optional<double> dead_time_ns;
  std::optional<double> cdd_voltage;
  // Measurement timing (measurement plan v2 spec, 3.6). A signal reading above
  // `large_beam_threshold` makes the next move onto this detector settle at
  // least `settle_after_large_beam_s`.
  std::optional<double> large_beam_threshold;
  double settle_after_large_beam_s = 0;
  // Logical detector modes: two detectors naming one `collector` are one cup
  // in two modes (`H2` and `H2(CDD)` with collector "H2"); empty means the
  // detector is its own collector. Changing a collector's mode waits
  // `mode_switch_s`.
  std::string collector;
  std::string mode;
  double mode_switch_s = 0;
};

// `[optics.<name>]` (measurement plan v2 spec 3.6): opaque to the plan;
// each key and its value as text, in file order.
struct OpticsPreset {
  std::vector<std::pair<std::string, std::string>> params;
  friend bool operator==(const OpticsPreset&, const OpticsPreset&) = default;
};

// `[interferences] "<monitor>" = "<corrected>"`: the monitor's signal
// corrects the other's (Ar40++ on Ne20), reduction metadata (spec 6.2).
struct Interference {
  std::string monitor, corrects;
  friend bool operator==(const Interference&, const Interference&) = default;
};

struct SpectrometerConfig {
  std::string source_file;
  SystemSection system;
  std::map<std::string, TransportConfig> transports;
  std::map<std::string, DriverConfig> drivers;
  MagnetSection magnet;
  SourceSection source;
  AcquisitionSection acquisition;
  std::optional<DetectorControlSection> detector_control;
  std::vector<DetectorConfig> detectors;
  std::map<std::string, OpticsPreset> optics;
  std::vector<Interference> interferences;

  const DriverConfig* driver(const std::string& name) const;
  const DetectorConfig* detector(const std::string& name) const;
};

// Splits "driver:channel"; nullopt when there is no ':' or either side is empty.
struct ChannelRef {
  std::string driver;
  std::string channel;
};
std::optional<ChannelRef> parse_channel_ref(std::string_view text);

}  // namespace pychron::spectrometer::cfg
