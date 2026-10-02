#pragma once

// ProcessingBridge (data browsing and visualization design, section 11.1):
// runs processing pipelines on one worker thread so the UI never blocks on
// loading or reducing analyses. Each caller submits on its own channel; a new
// submission cancels the channel's running job and replaces its pending one,
// so only the latest result reaches the callback, on the GUI thread.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QObject>

#include "pychron/processing/units.hpp"

namespace pychron::ui {

struct PipelineResult {
  // One entry per requested target, in order.
  std::vector<Result<std::vector<processing::PortValue>>> outputs;
  std::vector<std::string> diagnostics;
  double seconds = 0.0;
};

class ProcessingBridge : public QObject {
  Q_OBJECT

 public:
  // `source` must outlive the bridge; it is used from the worker thread.
  explicit ProcessingBridge(processing::IAnalysisSource& source, QObject* parent = nullptr);
  ~ProcessingBridge() override;

  processing::IAnalysisSource& source() noexcept { return source_; }

  using Callback = std::function<void(const PipelineResult&)>;
  // Channels: one per window (new_channel()). The callback runs on the GUI
  // thread unless a newer submission on the same channel superseded it.
  // Targets are evaluated in order with the channel's runner, so they share
  // its cache.
  void submit(int channel, processing::Pipeline pipeline, std::vector<std::string> targets, Callback done);
  int new_channel() { return next_channel_++; }
  // Drops the channel's cached results and pending work (window closed).
  void release(int channel);

  // For tests: true when nothing is queued or running.
  bool idle() const;
  // For tests: processes posted callbacks until idle or `timeout_ms` passes.
  bool wait_idle(int timeout_ms = 5000);

 private:
  struct Job {
    int channel = 0;
    std::uint64_t serial = 0;
    processing::Pipeline pipeline;
    std::vector<std::string> targets;
    Callback done;
  };

  void loop();

  processing::IAnalysisSource& source_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> queue_;
  std::map<int, std::uint64_t> latest_;  // channel -> newest serial
  std::map<int, std::unique_ptr<processing::Runner>> runners_;
  std::shared_ptr<std::atomic<bool>> running_cancel_;
  int running_channel_ = -1;
  std::atomic<int> in_flight_{0};  // queued + running + posted-but-not-delivered
  std::uint64_t serial_ = 0;
  int next_channel_ = 1;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace pychron::ui
