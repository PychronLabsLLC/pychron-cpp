#pragma once

// Files the application writes for the user to open elsewhere: a data
// report, a figure, a CSV template, a level sheet.
//
// On macOS a file written by an application that is itself quarantined (one
// downloaded and not notarized, as a pychron release is) inherits the
// quarantine attribute, and Gatekeeper then greets the file with "Apple could
// not verify ... is free of malware" when it is opened. The file is the
// application's own output, not something downloaded, so the attribute is
// cleared. Elsewhere this does nothing.

#include <filesystem>

namespace pychron {

// Clears the quarantine attribute of `path`. True when the file carries none
// afterwards (including when it never did, or on other platforms); false when
// the attribute could not be removed or the file does not exist.
bool mark_as_user_file(const std::filesystem::path& path) noexcept;

}  // namespace pychron
