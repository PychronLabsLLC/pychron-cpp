#pragma once

#include <string_view>

namespace pychron::experiment::lab {

// The actor a script's valve calls are made as. The live line (session.cpp)
// and the rehearsal's copy of it both use this one name, so a rehearsal's
// verdict on an owner or interlock cannot drift from the line's.
inline constexpr std::string_view kScriptActor = "script";

}  // namespace pychron::experiment::lab
