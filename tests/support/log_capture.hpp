// A process log hub writing to a temporary directory for the life of the
// object (ProcessLogScope), and the lines it wrote, without their timestamp
// and thread field: `[debug] trace.ValveChanged: valve=V1, state=open`.
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/process_log.hpp"
#include "temp_path.hpp"

namespace pychron::testing {

class LogCapture {
 public:
  LogCapture() : dir_(unique_temp_path("pychron_log_capture_")) {
    std::filesystem::create_directories(dir_);
    config::LoggingConfig cfg;
    cfg.dir = dir_;
    cfg.default_level = LogLevel::Debug;
    if (auto hub = LogHub::create(cfg, clock_)) hub_ = std::move(*hub);
    scope_.emplace(hub_);
  }
  ~LogCapture() {
    scope_.reset();
    hub_.reset();  // closes the file before its directory goes
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
  LogCapture(LogCapture&&) = delete;
  LogCapture& operator=(LogCapture&&) = delete;

  [[nodiscard]] bool ok() const noexcept { return hub_ != nullptr; }
  [[nodiscard]] LogHub& hub() const noexcept { return *hub_; }

  // Every line written so far (flushed first), stripped as above.
  [[nodiscard]] std::vector<std::string> lines() const {
    std::vector<std::string> out;
    if (!hub_) return out;
    hub_->flush();
    std::ifstream in(hub_->file_path(), std::ios::binary);
    static const std::regex shape(R"(^\S+Z (\[\w+\]) \[[^\]]*\] (?:run=\S+ )?(.*)$)");
    for (std::string line; std::getline(in, line);) {
      std::smatch m;
      out.push_back(std::regex_match(line, m, shape) ? m[1].str() + " " + m[2].str() : line);
    }
    return out;
  }

  // Every line written so far, whole: timestamp, thread tag and run field kept.
  [[nodiscard]] std::vector<std::string> raw_lines() const {
    std::vector<std::string> out;
    if (!hub_) return out;
    hub_->flush();
    std::ifstream in(hub_->file_path(), std::ios::binary);
    for (std::string line; std::getline(in, line);) out.push_back(std::move(line));
    return out;
  }

  // The lines of loggers named trace.*.
  [[nodiscard]] std::vector<std::string> trace_lines() const {
    std::vector<std::string> out;
    for (auto& line : lines()) {
      if (line.find("] trace.") != std::string::npos) out.push_back(std::move(line));
    }
    return out;
  }

 private:
  std::filesystem::path dir_;
  SteadyClock clock_;
  std::shared_ptr<LogHub> hub_;
  std::optional<ProcessLogScope> scope_;
};

}  // namespace pychron::testing
