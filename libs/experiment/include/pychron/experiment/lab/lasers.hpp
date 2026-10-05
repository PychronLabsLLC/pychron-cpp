#pragma once

// Lasers (laser window design, section 2): the lab's extraction devices as
// laser systems, built once and shared by whatever drives them: a queue
// (LabSession) and an operator (the laser window). Sharing them is what makes
// the tray, the calibration, the corrections, the camera and a centring the
// same for both.
//
// Only one of the two drives at a time, and that is the lease: a queue holds
// it for as long as it runs, a command made by hand for as long as it takes.
// It is one lease for the whole lab, not one per device: a queue names its
// device run by run. Watching (LaserSystem::snapshot, view) and the emergency
// stop need no lease.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/laser/laser_system.hpp"

namespace pychron::systems {
class ExtractionLine;
}

namespace pychron::experiment::lab {

class Lasers {
 public:
  enum class Driver { None, Queue, Manual };

  // Gives the lease back when it goes. Movable; an empty one holds nothing.
  class Lease {
   public:
    Lease() = default;
    Lease(Lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    Lease& operator=(Lease&& other) noexcept;
    ~Lease() { release(); }
    explicit operator bool() const noexcept { return owner_ != nullptr; }
    void release() noexcept;

   private:
    friend class Lasers;
    explicit Lease(Lasers* owner) : owner_(owner) {}
    Lasers* owner_ = nullptr;
  };

  // One LaserSystem per driver of `line` that is an extraction device, under
  // the driver's name (what a queue's extract_device names), with the lab's
  // corrections and, when the lab gives it one that may be used here, its
  // camera (one marked use = "view" as a picture only). `simulated` says whether a driver is simulated (a simulated
  // camera may only centre holes on a simulated stage); empty: asked of the
  // line. `lab` and `line` must outlive this.
  Lasers(const Lab& lab, systems::ExtractionLine& line, std::function<bool(std::string_view driver)> simulated = {});
  Lasers(const Lasers&) = delete;
  Lasers& operator=(const Lasers&) = delete;

  laser::LaserSystem* find(std::string_view device);  // null: no such device
  std::vector<std::string> names() const;             // sorted
  // What the lab asks of a device that cannot be done here: a camera that
  // cannot be used to centre holes on it. Fixed once built.
  std::vector<std::string> problems() const;
  const std::string* problem_of(std::string_view device) const;
  // What is wrong that stops no queue: a camera for looking that cannot be
  // looked through. Fixed once built.
  std::vector<std::string> notes() const { return notes_; }

  // Config error, saying who drives now, when it is the other. By hand may
  // be taken again while it is held by hand (those commands are serialised by
  // whoever makes them); a queue only once.
  Result<Lease> drive(Driver who);
  Driver driver() const;
  // The devices whose emergency stop is latched.
  std::vector<std::string> stopped() const;
  // The devices whose beam is on now (asked of each; one that cannot say is
  // not listed).
  std::vector<std::string> firing();

 private:
  void release() noexcept;

  // By device name; they refer to the line's drivers and the lab's trays and
  // calibrations.
  std::map<std::string, std::unique_ptr<laser::LaserSystem>, std::less<>> systems_;
  std::map<std::string, std::string, std::less<>> problems_;
  std::vector<std::string> notes_;

  mutable std::mutex mutex_;
  Driver driver_ = Driver::None;
  int holders_ = 0;
};

}  // namespace pychron::experiment::lab
