#pragma once

// One vendor connection shared by several drivers, possibly from different
// config files (NGX, Qtegra). The driver on the real transport owns the link
// and registers it by name; a driver whose transport is a LinkTransport
// ([transports.x] kind = "link", link = "<name>") borrows it by that name.
//
// `Link` provides `static constexpr std::string_view kLabel` ("NGX"), used in
// messages.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/transport/link_transport.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// Process-wide links of one kind, by name.
template <class Link>
class LinkRegistry {
 public:
  static LinkRegistry& global() {
    static LinkRegistry registry;
    return registry;
  }

  // Config when `name` is already registered to a live link.
  Result<void> add(const std::string& name, const std::shared_ptr<Link>& link) {
    std::lock_guard lock(mutex_);
    auto it = links_.find(name);
    if (it != links_.end() && !it->second.expired()) {
      return fail(ErrorKind::Config, std::string(Link::kLabel) + " link '" + name +
                                         "' is already open; declare its transport once and use kind = \"link\" "
                                         "elsewhere");
    }
    links_[name] = link;
    return {};
  }

  void remove(const std::string& name, const Link* link) {
    std::lock_guard lock(mutex_);
    auto it = links_.find(name);
    if (it == links_.end()) return;
    auto live = it->second.lock();
    if (!live || live.get() == link) links_.erase(it);
  }

  // NotConnected when no live link has that name.
  Result<std::shared_ptr<Link>> find(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = links_.find(name);
    if (it != links_.end()) {
      if (auto live = it->second.lock()) return live;
    }
    return fail(ErrorKind::NotConnected, std::string(Link::kLabel) + " link '" + name +
                                             "' is not running (is the config that owns it loaded?)");
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::weak_ptr<Link>> links_;
};

// The link a driver should use: its own on a real transport (made by `make`
// and registered under `name`), or the registered one when the transport is a
// LinkTransport. Owned links unregister when the handle is released.
template <class Link>
class LinkHandle {
 public:
  using Factory = std::function<std::shared_ptr<Link>(Transport&)>;

  static Result<LinkHandle> make(Transport& transport, const std::string& name, const Factory& factory) {
    LinkHandle h;
    if (const auto* link = dynamic_cast<const LinkTransport*>(&transport)) {
      h.name_ = link->link();
      return h;
    }
    h.name_ = name;
    auto owned = factory(transport);
    if (auto added = LinkRegistry<Link>::global().add(name, owned); !added) return fail(std::move(added).error());
    h.owned_ = std::move(owned);
    return h;
  }

  LinkHandle(LinkHandle&& other) noexcept = default;
  LinkHandle& operator=(LinkHandle&& other) noexcept {
    if (this != &other) {
      release();
      name_ = std::move(other.name_);
      owned_ = std::move(other.owned_);
    }
    return *this;
  }
  LinkHandle(const LinkHandle&) = delete;
  LinkHandle& operator=(const LinkHandle&) = delete;
  ~LinkHandle() { release(); }

  // The link to use now (a borrowed one is looked up on every call).
  Result<std::shared_ptr<Link>> get() const {
    if (owned_) return owned_;
    return LinkRegistry<Link>::global().find(name_);
  }
  bool owner() const noexcept { return static_cast<bool>(owned_); }
  const std::string& name() const noexcept { return name_; }

 private:
  LinkHandle() = default;
  void release() {
    if (owned_) LinkRegistry<Link>::global().remove(name_, owned_.get());
    owned_.reset();
  }

  std::string name_;
  std::shared_ptr<Link> owned_;
};

}  // namespace pychron
