#pragma once

// A recording IRehearsal for the Rehearse tests: every step is kept, answered
// from `answers` (by command) or by `respond`, and formatted as a line.

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "pychron/scripting/rehearsal.hpp"

namespace pychron::scripting::testing {

inline std::string format_value(const Value& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "None";
        else if constexpr (std::is_same_v<T, bool>) return x ? "True" : "False";
        else if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(x);
        else if constexpr (std::is_same_v<T, double>) return num(x);
        else return x;
      },
      v);
}

class RecordingRehearsal final : public IRehearsal {
 public:
  Result<Value> step(const StepCall& call) override {
    std::function<Result<Value>(const StepCall&)> handler;
    Value answer;
    {
      std::lock_guard lock(mutex_);
      calls.push_back(call);
      handler = respond;
      if (auto it = answers.find(call.command); it != answers.end()) answer = it->second;
    }
    if (handler) return handler(call);
    return answer;
  }

  // "<command> <args joined by space>", one per step.
  std::vector<std::string> lines() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(calls.size());
    for (const auto& c : calls) {
      std::string line = c.command;
      for (const auto& a : c.args) line += " " + format_value(a);
      out.push_back(std::move(line));
    }
    return out;
  }

  std::vector<StepCall> calls;
  std::map<std::string, Value> answers;
  std::function<Result<Value>(const StepCall&)> respond;

 private:
  std::mutex mutex_;
};

}  // namespace pychron::scripting::testing
