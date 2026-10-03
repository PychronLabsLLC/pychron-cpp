#pragma once

// Reads the byte-exact legacy files under tests/dvc/fixtures (README.md there).

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace pychron::dvc::testing {

inline std::string fixture(const std::string& relative) {
  const std::string path = std::string(PYCHRON_DVC_FIXTURES_DIR) + "/" + relative;
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("missing fixture " + path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

inline const std::string kUnknown = "project/IR1010/";       // 66052-01E
inline const std::string kBlank = "project/Felix_blank180/";  // bu-FD-F-789

}  // namespace pychron::dvc::testing
