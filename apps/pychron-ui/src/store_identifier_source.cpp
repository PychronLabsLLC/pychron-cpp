#include "store_identifier_source.hpp"

#include <utility>

#include <QMetaObject>
#include <QPointer>

#include "entry_actions.hpp"
#include "entry_bridge.hpp"

namespace pychron::ui {

namespace {

namespace ps = persistence;

// An answer that needed no store: still given later, as every other is.
template <class R>
void refuse(QObject* context, QObject* receiver, std::function<void(Result<R>)> done, Error error) {
  QMetaObject::invokeMethod(
      context,
      [guard = QPointer<QObject>(receiver), done = std::move(done), error = std::move(error)] {
        if (guard) done(Unexpected<Error>(error));
      },
      Qt::QueuedConnection);
}

}  // namespace

StoreIdentifierSource::StoreIdentifierSource(EntryActions& entry) : IdentifierSource(&entry), entry_(entry) {}

Result<EntryBridge*> StoreIdentifierSource::bridge() {
  if (auto opened = entry_.open_bridge(); !opened) return Unexpected<Error>(opened.error());
  EntryBridge* b = entry_.bridge();
  connect(b, &EntryBridge::changed, this, &IdentifierSource::changed, Qt::UniqueConnection);
  return b;
}

void StoreIdentifierSource::packages(QObject* receiver, std::function<void(Result<std::vector<PackageChoice>>)> done) {
  auto b = bridge();
  if (!b) {
    refuse(this, receiver, std::move(done), b.error());
    return;
  }
  (*b)->run<std::vector<PackageChoice>>(
      receiver,
      [](ps::IStore& store, const ps::Actor&) -> Result<std::vector<PackageChoice>> {
        auto rows = store.irradiations();
        if (!rows) return Unexpected<Error>(rows.error());
        std::vector<PackageChoice> out;
        out.reserve(rows->size());
        for (const auto& row : *rows) out.push_back({row.uuid.str(), row.name});
        return out;
      },
      std::move(done));
}

void StoreIdentifierSource::contents(QObject* receiver, const std::string& package,
                                     std::function<void(Result<PackageContents>)> done) {
  const auto uuid = ps::Uuid::parse(package);
  if (!uuid) {
    refuse(this, receiver, std::move(done), Error{ErrorKind::Config, "not a package id: " + package, {}});
    return;
  }
  auto b = bridge();
  if (!b) {
    refuse(this, receiver, std::move(done), b.error());
    return;
  }
  (*b)->run<PackageContents>(
      receiver,
      [id = *uuid](ps::IStore& store, const ps::Actor&) -> Result<PackageContents> {
        auto levels = store.levels(id);
        if (!levels) return Unexpected<Error>(levels.error());
        PackageContents out;
        for (const auto& level : *levels) {
          auto sheet = store.level_sheet(level.uuid);
          if (!sheet) return Unexpected<Error>(sheet.error());
          if (!*sheet) continue;  // the level went away meanwhile
          out.levels.push_back(level.name);
          for (const auto& p : (*sheet)->positions) {
            if (p.identifier) out.choices.push_back({*p.identifier, p.sample_name, level.name, p.position});
          }
        }
        return out;
      },
      std::move(done));
}

}  // namespace pychron::ui
