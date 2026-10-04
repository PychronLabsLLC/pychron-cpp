#pragma once

// The store and entry headers, for UI sources. Qt's `signals` keyword macro
// would rewrite persistence::CollectionRoots::signals, so it is set aside
// while they are read.

#pragma push_macro("signals")
#undef signals
#include "pychron/entry/csv.hpp"
#include "pychron/entry/export.hpp"
#include "pychron/entry/holder_import.hpp"
#include "pychron/entry/identifier_plan.hpp"
#include "pychron/entry/level_sheet.hpp"
#include "pychron/entry/names.hpp"
#include "pychron/entry/package_edit.hpp"
#include "pychron/entry/positions_import.hpp"
#include "pychron/entry/sample_fields.hpp"
#include "pychron/entry/sample_import.hpp"
#include "pychron/entry/sample_search.hpp"
#include "pychron/entry/settings.hpp"
#include "pychron/persistence/store.hpp"
#pragma pop_macro("signals")
