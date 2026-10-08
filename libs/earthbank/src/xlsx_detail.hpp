#pragma once

// Two small parts of the workbook exporter (xlsx.cpp) that a test cannot
// reach through export_xlsx: where the templates are looked for when they are
// nowhere, and what happens to the files already written when one cannot be
// taken back. Internal to pychron::earthbank.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pychron::earthbank::detail {

// Where the templates were looked for, in order, each place in words a user
// can follow, and the directory taken: the first that applies. `found` is
// empty when none does.
struct TemplateSearch {
  std::filesystem::path found;
  std::vector<std::string> tried;
};

// `named` is what PYCHRON_EARTHBANK_TEMPLATES holds (taken as it is: the user
// said so); `program_dir` the running program's directory, beside which an
// install keeps the templates (empty: not known); `source_dir` the source
// tree's templates, taken only when the directory is there. On a machine
// that does not have that tree (an installed release) its path is the build
// machine's: it is not taken, and not named.
TemplateSearch find_templates(const std::optional<std::string>& named, const std::filesystem::path& program_dir,
                              const std::filesystem::path& source_dir);

// "earthbank: the workbook templates were not found. Looked in: ...", every
// place of `search.tried`, in order.
std::string templates_not_found(const TemplateSearch& search);

// Removes the files a failed export had already written. Those that could
// not be removed are returned: they are still there, and the error says so.
std::vector<std::filesystem::path> take_back(const std::vector<std::filesystem::path>& written);

}  // namespace pychron::earthbank::detail
