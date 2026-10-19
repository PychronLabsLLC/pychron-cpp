#pragma once

// The New release dialog of the Publish window (data publishing spec,
// section 6.3): what a release says about itself. The analyses are the Data
// browser's selection, counted at the top with how many of them are under
// embargo now; the reduction options are the defaults of elctl publish
// create.

#include <cstddef>
#include <optional>

#include <QStringList>

class QWidget;

namespace pychron::publish {
struct ReleaseSpec;
}

namespace pychron::ui {

// Shows the dialog; nullopt when cancelled. The spec has everything the
// dialog asks for; its analyses, options and versions are the caller's to
// fill. `releases` are the names a release may supersede.
std::optional<publish::ReleaseSpec> ask_release(QWidget* parent, std::size_t analyses, std::size_t embargoed,
                                                const QStringList& releases);

}  // namespace pychron::ui
