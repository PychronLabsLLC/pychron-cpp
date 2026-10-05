#include "pychron/experiment/lab/lasers.hpp"

#include <utility>

#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/laser/tray_camera.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace pychron::experiment::lab {

Lasers::Lease& Lasers::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    release();
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

void Lasers::Lease::release() noexcept {
  if (Lasers* owner = std::exchange(owner_, nullptr)) owner->release();
}

Lasers::Lasers(const Lab& lab, systems::ExtractionLine& line, std::function<bool(std::string_view)> simulated) {
  for (const auto& [name, driver] : line.config().drivers) {
    auto* device = dynamic_cast<extraction::IExtractionDevice*>(line.device(name));
    if (device == nullptr) continue;
    auto system = std::make_unique<laser::LaserSystem>(name, *device, lab.trays, *lab.calibrations, &lab.patterns);
    // Where holes were found before, and (if the lab gives the device a
    // camera) the means to find them again.
    system->set_corrections(*lab.corrections);
    // A recording is for looking (elctl laser look): it never moves a
    // stage. Any other camera must be one that can centre holes here.
    const laser::CameraConfig* camera = lab.cameras.find(name);
    if (camera != nullptr && camera->source != laser::CameraSource::Recorded) {
      const bool sim = simulated ? simulated(name) : line.sim() != nullptr && line.sim()->chromium(name) != nullptr;
      Result<void> usable = laser::usable_for_autocenter(*camera, sim);
      if (usable) {
        auto frames = laser::make_frame_source(*camera, lab.paths.dir, system->sight(), line.clock());
        if (frames) usable = system->attach_camera(*camera, std::move(*frames), line.clock());
        else usable = fail(frames.error());
      }
      if (!usable) problems_.insert_or_assign(name, usable.error().what);
    }
    systems_.emplace(name, std::move(system));
  }
}

laser::LaserSystem* Lasers::find(std::string_view device) {
  const auto it = systems_.find(device);
  return it == systems_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Lasers::names() const {
  std::vector<std::string> out;
  for (const auto& [name, system] : systems_) out.push_back(name);
  return out;
}

std::vector<std::string> Lasers::problems() const {
  std::vector<std::string> out;
  for (const auto& [device, what] : problems_) out.push_back(what);
  return out;
}

const std::string* Lasers::problem_of(std::string_view device) const {
  const auto it = problems_.find(device);
  return it == problems_.end() ? nullptr : &it->second;
}

Result<Lasers::Lease> Lasers::drive(Driver who) {
  if (who == Driver::None) return fail(ErrorKind::Config, "nobody cannot drive a laser", "laser");
  std::lock_guard lock(mutex_);
  if (driver_ == Driver::Queue) {
    return fail(ErrorKind::Config, who == Driver::Queue ? "a queue is already running" : "a queue is running", "laser");
  }
  if (driver_ == Driver::Manual && who == Driver::Queue) {
    return fail(ErrorKind::Config, "the laser is being driven by hand", "laser");
  }
  driver_ = who;
  ++holders_;
  return Lease(this);
}

void Lasers::release() noexcept {
  std::lock_guard lock(mutex_);
  if (--holders_ <= 0) {
    holders_ = 0;
    driver_ = Driver::None;
  }
}

Lasers::Driver Lasers::driver() const {
  std::lock_guard lock(mutex_);
  return driver_;
}

std::vector<std::string> Lasers::firing() {
  std::vector<std::string> out;
  for (const auto& [name, system] : systems_) {
    if (system->laser() == nullptr) continue;
    if (system->is_firing().value_or(false)) out.push_back(name);
  }
  return out;
}

std::vector<std::string> Lasers::stopped() const {
  std::vector<std::string> out;
  for (const auto& [name, system] : systems_) {
    if (system->stopped()) out.push_back(name);
  }
  return out;
}

}  // namespace pychron::experiment::lab
