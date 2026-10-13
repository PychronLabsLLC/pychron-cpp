#pragma once

// The run factory's identifier choices read from the DVC store
// (identifier-select design, section 4), on the EntryBridge that EntryActions
// owns. It only reads; the bridge is opened by the first question, so a
// session that never opens the experiment window opens no connection for it.

#include "identifier_source.hpp"

namespace pychron::ui {

class EntryActions;
class EntryBridge;

class StoreIdentifierSource : public IdentifierSource {
  Q_OBJECT

 public:
  // A child of `entry`, whose bridge it uses.
  explicit StoreIdentifierSource(EntryActions& entry);

  void packages(QObject* receiver, std::function<void(Result<std::vector<PackageChoice>>)> done) override;
  void contents(QObject* receiver, const std::string& package,
                std::function<void(Result<PackageContents>)> done) override;

 private:
  // The bridge, opened without a message box; changed() follows it from then on.
  Result<EntryBridge*> bridge();

  EntryActions& entry_;
};

}  // namespace pychron::ui
