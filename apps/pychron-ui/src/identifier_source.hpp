#pragma once

// Where the run factory's Identifier select gets its choices (identifier-select
// design, section 3): the packages, and the levels and identifiers of one.
// The panel knows nothing of the store behind it, so it builds, and is
// tested, without one; StoreIdentifierSource is the one that reads the store.

#include <functional>
#include <string>
#include <vector>

#include <QObject>

#include "pychron/core/error.hpp"

namespace pychron::ui {

struct PackageChoice {
  std::string id;  // opaque to the panel (the store's uuid as text)
  std::string name;
  friend bool operator==(const PackageChoice&, const PackageChoice&) = default;
};

struct IdentifierChoice {
  std::string identifier;
  std::string sample;  // empty: the position has no sample
  std::string level;
  int position = 0;
  friend bool operator==(const IdentifierChoice&, const IdentifierChoice&) = default;
};

struct PackageContents {
  std::vector<std::string> levels;        // every level of the package, by name
  std::vector<IdentifierChoice> choices;  // by level name, then position
  friend bool operator==(const PackageContents&, const PackageContents&) = default;
};

class IdentifierSource : public QObject {
  Q_OBJECT

 public:
  using QObject::QObject;

  // Both answer on the GUI thread, later, and not at all once `receiver` is gone.
  virtual void packages(QObject* receiver, std::function<void(Result<std::vector<PackageChoice>>)> done) = 0;
  virtual void contents(QObject* receiver, const std::string& package,
                        std::function<void(Result<PackageContents>)> done) = 0;

 Q_SIGNALS:
  void changed();  // the catalog changed: what was listed may be stale
};

}  // namespace pychron::ui
