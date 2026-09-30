#include "pychron/systems/spectrometer/config_validate.hpp"

#include <algorithm>
#include <set>

namespace pychron::spectrometer::cfg {
namespace {

void add(Diagnostics& out, const SourceLoc& loc, std::string field, std::string message) {
  out.push_back({loc, std::move(field), std::move(message)});
}

std::string q(std::string_view s) { return "'" + std::string(s) + "'"; }

std::string indexed(std::string_view path, std::size_t i) { return std::string(path) + "[" + std::to_string(i) + "]"; }

// Checks that `name` is a driver with `role`; reports against `field`.
void need_role(Diagnostics& out, const SpectrometerConfig& c, const std::string& name, Role role, const SourceLoc& loc,
               std::string field) {
  const auto* d = c.driver(name);
  if (d == nullptr) {
    add(out, loc, std::move(field), "unknown driver " + q(name));
  } else if (!d->has_role(role)) {
    add(out, loc, std::move(field), "driver " + q(name) + " does not have role " + q(to_string(role)));
  }
}

}  // namespace

Diagnostics check_references(const SpectrometerConfig& c) {
  Diagnostics out;
  for (const auto& [name, d] : c.drivers) {
    if (!c.transports.contains(d.transport))
      add(out, d.where("transport"), d.path + ".transport", "unknown transport " + q(d.transport));
    std::set<Role> seen;
    for (auto r : d.roles) {
      if (!seen.insert(r).second) add(out, d.where("roles"), d.path + ".roles", "role " + q(to_string(r)) + " listed twice");
    }
  }
  if (c.detector(c.system.reference_detector) == nullptr) {
    add(out, c.system.where("reference_detector"), "system.reference_detector",
        "unknown detector " + q(c.system.reference_detector));
  }
  std::set<std::string> names;
  for (const auto& d : c.detectors) {
    if (!names.insert(d.name).second) add(out, d.where("name"), d.path + ".name", "duplicate detector " + q(d.name));
  }
  return out;
}

Diagnostics check_roles(const SpectrometerConfig& c) {
  Diagnostics out;
  need_role(out, c, c.magnet.positioner, Role::Positioner, c.magnet.where("positioner"), "magnet.positioner");
  need_role(out, c, c.source.driver, Role::Source, c.source.where("driver"), "source.driver");
  const auto& acq = c.acquisition;
  for (std::size_t i = 0; i < acq.acquirers.size(); ++i) {
    need_role(out, c, acq.acquirers[i], Role::Acquirer, acq.where("acquirers"), indexed("acquisition.acquirers", i));
  }
  if (c.detector_control) {
    need_role(out, c, c.detector_control->driver, Role::DetectorControl, c.detector_control->where("driver"),
              "detector_control.driver");
  }
  for (const auto& [name, d] : c.drivers) {
    if (d.has_role(Role::Acquirer) && d.channels.empty())
      add(out, d.loc, d.path + ".channels", "a driver with role 'acquirer' must declare its channels");
  }
  if (c.magnet.protection.beam_blank_threshold) {
    const bool any = std::any_of(c.drivers.begin(), c.drivers.end(),
                                 [](const auto& kv) { return kv.second.has_role(Role::BeamBlank); });
    if (!any) {
      add(out, c.magnet.where("protection"), "magnet.protection.beam_blank_threshold",
          "requires a driver with role 'beam_blank'");
    }
  }
  return out;
}

Diagnostics check_channels(const SpectrometerConfig& c) {
  Diagnostics out;
  const auto& acq = c.acquisition;

  // "driver:channel" -> bound detector name ("" while unbound).
  std::map<std::string, std::string> channels;
  std::set<std::string> acquirers;
  for (const auto& name : acq.acquirers) {
    const auto* d = c.driver(name);
    if (d == nullptr || !d->has_role(Role::Acquirer) || !acquirers.insert(name).second) continue;
    for (std::size_t j = 0; j < d->channels.size(); ++j) {
      if (!channels.emplace(name + ":" + d->channels[j], std::string()).second) {
        add(out, d->where("channels"), indexed(d->path + ".channels", j), "duplicate channel " + q(d->channels[j]));
      }
    }
  }

  for (const auto& det : c.detectors) {
    const auto ref = parse_channel_ref(det.channel);
    if (!ref) continue;  // the loader already reported the format
    const auto field = det.path + ".channel";
    if (!acquirers.contains(ref->driver)) {
      add(out, det.where("channel"), field, q(ref->driver) + " is not an acquirer (not in acquisition.acquirers)");
      continue;
    }
    auto it = channels.find(det.channel);
    if (it == channels.end()) {
      add(out, det.where("channel"), field, "no acquirer channel " + q(det.channel));
    } else if (!it->second.empty()) {
      add(out, det.where("channel"), field, "channel " + q(det.channel) + " already bound to detector " + q(it->second));
    } else {
      it->second = det.name;
    }
  }

  std::set<std::string> ignored;
  for (std::size_t i = 0; i < acq.ignored_channels.size(); ++i) {
    const auto& ch = acq.ignored_channels[i];
    const auto field = indexed("acquisition.ignored_channels", i);
    auto it = channels.find(ch);
    if (it == channels.end()) {
      add(out, acq.where("ignored_channels"), field, "no acquirer channel " + q(ch));
    } else if (!it->second.empty()) {
      add(out, acq.where("ignored_channels"), field, q(ch) + " is bound to detector " + q(it->second));
    } else {
      ignored.insert(ch);
    }
  }

  for (const auto& [ch, det] : channels) {
    if (!det.empty() || ignored.contains(ch)) continue;
    const auto* d = c.driver(parse_channel_ref(ch)->driver);
    add(out, d->where("channels"), d->path + ".channels",
        "acquirer channel " + q(ch) + " is not bound to a detector or listed in acquisition.ignored_channels");
  }

  if (acq.acquirers.size() > 1 && !acq.host_integration) {
    add(out, acq.where("host_integration"), "acquisition.host_integration",
        "more than one acquirer requires host_integration = true (frames are merged and integrated by the host)");
  }
  return out;
}

Diagnostics check_field_tables(const SpectrometerConfig& c, const Tables& tables) {
  Diagnostics out;
  const auto& m = c.magnet;
  auto check = [&](const std::string& key, const std::string& name) {
    const auto field = "magnet." + key;
    const auto& loc = m.where(key);
    auto it = tables.find(name);
    if (it == tables.end()) {
      add(out, loc, field, "table " + q(name) + " not found (expected tables/" + name + "/current)");
      return;
    }
    const auto& t = it->second;
    if (t.axis != m.native_axis) {
      add(out, loc, field,
          "table " + q(name) + " axis " + q(to_string(t.axis)) + " does not match positioner native_axis " +
              q(to_string(m.native_axis)));
    }
    for (const auto& d : c.detectors) {
      if (d.active && !t.has_column(d.name))
        add(out, loc, field, "table " + q(name) + " has no column for active detector " + q(d.name));
    }
  };
  check("field_table", m.field_table);
  if (!m.hv_table.empty()) check("hv_table", m.hv_table);
  return out;
}

Diagnostics check_protection(const SpectrometerConfig& c) {
  Diagnostics out;
  const auto& dets = c.magnet.protection.detectors;
  for (std::size_t i = 0; i < dets.size(); ++i) {
    const auto field = indexed("magnet.protection.detectors", i);
    const auto* d = c.detector(dets[i]);
    if (d == nullptr) {
      add(out, c.magnet.where("protection"), field, "unknown detector " + q(dets[i]));
    } else if (!d->protection) {
      add(out, c.magnet.where("protection"), field, "detector " + q(dets[i]) + " has no protection config");
    }
  }
  if (c.detector_control) return out;
  for (const auto& d : c.detectors) {
    if (d.deflection && d.deflection->control) {
      add(out, d.where("deflection"), d.path + ".deflection", "deflection control requires [detector_control]");
    }
    if (d.protection) add(out, d.where("protection"), d.path + ".protection", "protection requires [detector_control]");
  }
  return out;
}

Diagnostics check_hv_correction(const SpectrometerConfig& c) {
  Diagnostics out;
  if (!c.magnet.corrections.hv) return out;
  if (c.magnet.native_axis == Axis::Mass) {
    add(out, c.magnet.where("corrections"), "magnet.corrections.hv",
        "HV correction cannot be enabled for native_axis = \"mass\" positioners");
  }
  if (!c.source.nominal_hv) {
    add(out, c.source.loc, "source.nominal_hv", "required when magnet.corrections.hv = true");
  }
  return out;
}

Diagnostics validate(const SpectrometerConfig& c, const Tables& tables) {
  Diagnostics out;
  for (auto part : {check_references(c), check_roles(c), check_channels(c), check_field_tables(c, tables),
                    check_protection(c), check_hv_correction(c)}) {
    out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
  }
  return out;
}

}  // namespace pychron::spectrometer::cfg
