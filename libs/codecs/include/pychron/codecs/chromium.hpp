#pragma once

// Photon Machines "Chromium" laser-ablation software, remote command
// interface. See CONVENTIONS.md and
// docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md.
//
//   host     -> "Laser.Output?\n"     a query: <component>.<command>? [values]
//   chromium -> "12.5\r"              its reply, CR-terminated
//   host     -> "Laser.Fire\n"        an action: nothing comes back...
//   chromium -> "?4\r"                ...unless it fails: ?<n>, n = 0..4
//
// Commands end in LF over TCP. Replies are framed on CR or LF, so a build
// that answers CRLF frames the same way: the LF left over is absorbed into
// the next frame and trimmed by the decoders. Action commands carry no
// ReadSpec; the driver decides how to learn whether one failed.
//
// Stage values are microns; speeds microns per second.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::chromium {

inline constexpr std::string_view kCommandTerminator = "\n";
ReadSpec reply_frame();  // until CR or LF

struct Microns {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;
  friend bool operator==(const Microns&, const Microns&) = default;
};

// --- queries (reply framed by reply_frame()) --------------------------------------
codec::Command sys_id();
codec::Command laser_status();        // 0 when every interlock is satisfied
codec::Command laser_interlocks();    // the tripped ones, comma separated
codec::Command laser_enabled();
codec::Command laser_output_query();
codec::Command stage_position();
codec::Command stage_limits();        // limit switches per axis: 0, -1, +1
codec::Command scans_count();
// Scans are numbered from 1; anything less is a Config error.
Result<codec::Command> scan_in_position(int scan);

// --- actions (no reply) -------------------------------------------------------------
codec::Command laser_enable(bool on);
// Percent, 0..100, at most three decimals. Outside that, or not finite, is Config.
Result<codec::Command> laser_output(double percent);
codec::Command laser_fire();
codec::Command laser_stop();
codec::Command stage_move_to(Microns target, Microns speed);
codec::Command stage_stop();
Result<codec::Command> scan_move_to(int scan);
codec::Command scans_stop();
codec::Command scans_status_verbosity(int value);

// --- replies --------------------------------------------------------------------------
// Each decoder takes one framed reply. A "?<n>" reply is the error to_error(n)
// gives; anything else it cannot read is Protocol.

// n of a "?<n>" reply (n = 0..4); nullopt for any other reply.
std::optional<int> error_code(const Bytes& reply);
// ?1, ?2, ?0 -> Protocol (Chromium does not know what was sent); ?3 -> Config
// (a bad value); ?4 -> Io (failed, or not supported by this hardware).
// Error::code is "chromium?<n>". `command` names what was refused, if known.
Error to_error(int code, std::string_view command = {});

Result<std::string> decode_text(const Bytes& reply);  // trimmed
Result<bool> decode_flag(const Bytes& reply);         // "1" or "0"
Result<double> decode_number(const Bytes& reply);
Result<Microns> decode_position(const Bytes& reply);  // "x,y,z"; decimals rounded
Result<std::vector<std::string>> decode_interlocks(const Bytes& reply);

struct LimitStatus {
  int x = 0;
  int y = 0;
  int z = 0;
};
Result<LimitStatus> decode_limits(const Bytes& reply);
// The program's name and version; Protocol unless it starts "CHROMIUM".
Result<std::string> decode_id(const Bytes& reply);

}  // namespace pychron::codec::chromium
