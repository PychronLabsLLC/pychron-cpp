// The password store on macOS: the user's login keychain, through the
// Security framework's C interface (no Objective-C is needed for it).
//
// An item is a generic password with the service and the account as its two
// names. It is not shared with other programs' access groups and is not
// synchronized to iCloud. The login keychain is a file keychain, which gives
// up a password only while it is unlocked, and to the program that put it
// there: another program, or this one built again and signed differently,
// makes the system ask the user first. Nothing here needs an entitlement
// under the hardened runtime: those (keychain-access-groups) are for the
// data protection keychain, which this does not use.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "secret_store_detail.hpp"

namespace pychron::earthbank {
namespace {

constexpr std::string_view kBackend = "keychain";

struct Release {
  void operator()(CFTypeRef ref) const {
    if (ref != nullptr) CFRelease(ref);
  }
};
// A Core Foundation object this owns: released at the end of the scope.
template <class Ref>
using Owned = std::unique_ptr<std::remove_pointer_t<Ref>, Release>;

Owned<CFStringRef> cf_string(std::string_view text) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): CoreFoundation takes text as UInt8 bytes
  return Owned<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                                    static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false));
}

std::string utf8(CFStringRef text) {
  if (text == nullptr) return {};
  const CFIndex length = CFStringGetLength(text);
  CFIndex bytes = 0;
  CFStringGetBytes(text, CFRangeMake(0, length), kCFStringEncodingUTF8, '?', false, nullptr, 0, &bytes);
  std::string out(static_cast<std::size_t>(bytes), '\0');
  CFStringGetBytes(text, CFRangeMake(0, length), kCFStringEncodingUTF8, '?', false,
                   // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): CoreFoundation writes text as UInt8 bytes
                   reinterpret_cast<UInt8*>(out.data()), bytes, nullptr);
  return out;
}

// What the keychain said, as an error: the doing, the account, the system's
// words for the status and its number. Never a password.
Unexpected<Error> refused(std::string_view doing, std::string_view account, OSStatus status) {
  const Owned<CFStringRef> words(SecCopyErrorMessageString(status, nullptr));
  std::string what = std::string(kBackend) + ": could not " + std::string(doing) + " the password of '" +
                     std::string(account) + "': ";
  const std::string said = utf8(words.get());
  what += said.empty() ? "the keychain refused" : said;
  what += " (OSStatus " + std::to_string(status) + ")";
  return fail(ErrorKind::Io, std::move(what));
}

class KeychainSecretStore final : public ISecretStore {
 public:
  explicit KeychainSecretStore(std::string_view service) : service_(service) {}

  std::string backend() const override { return std::string(kBackend); }
  bool persistent() const override { return true; }

  Result<std::optional<std::string>> get(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return fail(ok.error());
    const auto query = item(account);
    if (!query) return refused("read", account, errSecAllocate);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(query.get(), &found);
    const Owned<CFTypeRef> owned(found);
    if (status == errSecItemNotFound) return std::optional<std::string>{};
    if (status != errSecSuccess) return refused("read", account, status);
    if (found == nullptr || CFGetTypeID(found) != CFDataGetTypeID()) return refused("read", account, errSecDecode);
    const auto data = static_cast<CFDataRef>(found);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the keychain gives the password as UInt8 bytes
    return std::optional<std::string>{std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                                                  static_cast<std::size_t>(CFDataGetLength(data)))};
  }

  Result<void> set(std::string_view account, std::string_view password) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    if (auto ok = detail::check_password(kBackend, password); !ok) return ok;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the keychain takes the password as UInt8 bytes
    const Owned<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(password.data()),
                                             static_cast<CFIndex>(password.size())));
    const auto adding = item(account);
    if (!data || !adding) return refused("store", account, errSecAllocate);
    CFDictionarySetValue(adding.get(), kSecValueData, data.get());
    CFDictionarySetValue(adding.get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked);
    OSStatus status = SecItemAdd(adding.get(), nullptr);
    if (status == errSecDuplicateItem) {
      // The account has one: the same item, with the new password.
      const auto query = item(account);
      const Owned<CFMutableDictionaryRef> change(dictionary());
      if (!query || !change) return refused("store", account, errSecAllocate);
      CFDictionarySetValue(change.get(), kSecValueData, data.get());
      status = SecItemUpdate(query.get(), change.get());
    }
    if (status != errSecSuccess) return refused("store", account, status);
    return {};
  }

  Result<void> erase(std::string_view account) override {
    if (auto ok = detail::check_account(kBackend, account); !ok) return ok;
    const auto query = item(account);
    if (!query) return refused("remove", account, errSecAllocate);
    const OSStatus status = SecItemDelete(query.get());
    if (status != errSecSuccess && status != errSecItemNotFound) return refused("remove", account, status);
    return {};
  }

  // Whether there is a keychain to ask: a search for this service's items
  // that returns nothing of them. It unlocks nothing and asks the user
  // nothing.
  bool usable() const {
    const auto query = item("pychron.probe");
    if (!query) return false;
    const OSStatus status = SecItemCopyMatching(query.get(), nullptr);
    return status == errSecSuccess || status == errSecItemNotFound;
  }

 private:
  static CFMutableDictionaryRef dictionary() {
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
  }

  // The item of `account`, as a query finds it and as it is added: a generic
  // password of this service, on this machine only. Null when out of memory.
  Owned<CFMutableDictionaryRef> item(std::string_view account) const {
    Owned<CFMutableDictionaryRef> query(dictionary());
    const auto service = cf_string(service_);
    const auto name = cf_string(account);
    if (!query || !service || !name) return nullptr;
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, service.get());
    CFDictionarySetValue(query.get(), kSecAttrAccount, name.get());
    CFDictionarySetValue(query.get(), kSecAttrSynchronizable, kCFBooleanFalse);
    return query;
  }

  std::string service_;
};

}  // namespace

std::unique_ptr<ISecretStore> detail::make_system_secret_store(std::string_view service) {
  auto store = std::make_unique<KeychainSecretStore>(service);
  if (!store->usable()) return nullptr;
  return store;
}

}  // namespace pychron::earthbank
